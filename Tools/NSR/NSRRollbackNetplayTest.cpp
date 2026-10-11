// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// In-process test for the socket-free rollback exchange and handoff cores. It
// covers transition agreement, wire validation, delayed delivery and
// correction, prediction-window stalling, acknowledgement-driven history,
// confirmed-state desyncs, and latency-compensated time-sync convergence.

#include "Core/NSR/NSRRollbackNetplay.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace RN = NSRNetplay::RollbackNet;

// Serialize between peers just as ENet does, so the test exercises the wire
// boundary rather than passing trusted Packet objects directly.
static void Deliver(const RN::Exchange& from, RN::Exchange* to, std::uint32_t frame)
{
  const std::vector<std::uint8_t> bytes = RN::Encode(from.BuildPacket(frame, frame * 1000));
  const auto packet = RN::Decode(bytes);
  assert(packet);
  assert(to->Accept(*packet));
}

// Match entry/leave proposals are independently encoded and require the same
// transition kind, absolute lockstep frame, and deterministic-state hash from
// every peer.  Duplicates are harmless and the first conflict stays visible.
static void TestHandoffAgreement()
{
  RN::HandoffAgreement agreement(3, 0, RN::HandoffKind::EnterRollback, 2, 1234, 0xaabbccdd);
  RN::HandoffPacket peer1{1, 3, RN::HandoffKind::EnterRollback, 2, 1234, 0xaabbccdd};
  RN::HandoffPacket peer2{2, 3, RN::HandoffKind::EnterRollback, 2, 1234, 0xaabbccdd};
  const auto bytes = RN::EncodeHandoff(peer1);
  const auto decoded = RN::DecodeHandoff(bytes);
  assert(decoded);
  assert(agreement.Accept(*decoded));
  assert(agreement.Accept(*decoded));
  assert(!agreement.Complete());
  assert(agreement.Accept(peer2));
  assert(agreement.Complete());

  RN::HandoffAgreement mismatch(2, 0, RN::HandoffKind::LeaveRollback, 3, 2000, 0x1234);
  RN::HandoffPacket stale{1, 2, RN::HandoffKind::EnterRollback, 2, 1234, 0xaabbccdd};
  assert(!mismatch.Accept(stale));
  assert(!mismatch.Mismatched());
  RN::HandoffPacket wrong{1, 2, RN::HandoffKind::LeaveRollback, 3, 2001, 0x1234};
  assert(!mismatch.Accept(wrong));
  assert(mismatch.Mismatched());
  assert(mismatch.Mismatch()->frame == 2001);

  std::vector<std::uint8_t> malformed = bytes;
  malformed[4] = 99;  // protocol version
  assert(!RN::DecodeHandoff(malformed));
  malformed = bytes;
  malformed.pop_back();
  assert(!RN::DecodeHandoff(malformed));
}

// A changed remote input arrives after its frame ran predicted, selecting the
// earliest affected frame and then disappearing once rollback begins.
static void TestPredictionAndCorrection()
{
  RN::Exchange a(100, 2, 0, 1, 3, 0x1234);
  RN::Exchange b(100, 2, 1, 1, 3, 0x1234);
  RN::Pad neutral{};
  RN::Pad right{};
  right[0] = 1;

  a.SubmitLocal(100, neutral);
  b.SubmitLocal(100, right);  // applies to frame 101
  Deliver(a, &b, 100);
  a.Assemble(100);
  a.Assemble(101);  // player 1 is still predicted neutral
  Deliver(b, &a, 101);
  assert(a.PendingRollback() == 101);
  a.BeginRollback();
  assert(!a.PendingRollback());
  assert(a.Assemble(101)[1] == right);
}

// A peer may run only max_prediction frames beyond the remote contiguous
// prefix; receiving a redundant history packet releases the stall.
static void TestPredictionWindowAndAcks()
{
  RN::Exchange a(200, 2, 0, 0, 2, 7);
  RN::Exchange b(200, 2, 1, 0, 2, 7);
  RN::Pad pad{};
  for (std::uint32_t frame = 200; frame <= 203; ++frame)
  {
    a.SubmitLocal(frame, pad);
    b.SubmitLocal(frame, pad);
  }
  assert(!a.ShouldStall(201));
  assert(a.ShouldStall(202));
  Deliver(b, &a, 203);
  assert(!a.ShouldStall(203));
  const RN::Packet stale = b.BuildPacket(202);  // no acknowledgement for a yet
  Deliver(a, &b, 203);  // carries a's ack of b through 203
  Deliver(b, &a, 204);  // carries b's ack of a through 203
  const auto oldest = a.OldestUnackedLocal();
  assert(oldest && *oldest == 204);
  assert(a.Accept(stale));  // unsequenced delivery must not regress that ack
  assert(a.OldestUnackedLocal() == 204);
}

// A receiver that misses more than one datagram's history must first receive
// the oldest gap, acknowledge it, and then receive the remaining prefix.
// Sending only the newest 64 inputs would leave frame 500 missing forever.
static void TestLongLossRecovery()
{
  RN::Exchange a(500, 2, 0, 0, 60, 8);
  RN::Exchange b(500, 2, 1, 0, 60, 8);
  for (std::uint32_t frame = 500; frame < 570; ++frame)
    a.SubmitLocal(frame, {});
  RN::Packet first = a.BuildPacket(569);
  assert(first.first_frame == 500);
  assert(first.pads.size() == RN::MAX_HISTORY);
  assert(b.Accept(first));
  Deliver(b, &a, 569);  // acknowledge through frame 563
  RN::Packet second = a.BuildPacket(569);
  assert(second.first_frame == 564);
  assert(second.pads.size() == 6);
  assert(b.Accept(second));
  assert(b.Contiguous()[0] == 569);
}

struct InFlight
{
  double deliver_us;
  std::uint8_t receiver;
  RN::Packet packet;
};

struct TimeSyncResult
{
  std::uint32_t frames[2]{};
  std::uint32_t waits[2]{};
  std::uint64_t waited_us[2]{};
  double advantage[2]{};
  std::uint32_t rtt_us[2]{};
};

// Simulate two independent emulation clocks and a datagram path without
// sockets. Host-clock offsets are intentionally different; RTT reflection
// must cancel them. Jitter follows a deterministic sawtooth so failures are
// reproducible while arrivals still cross frame boundaries.
static TimeSyncResult SimulateTimeSync(double rate_a, double rate_b, double latency_us,
                                       double jitter_us, double duration_us)
{
  constexpr double frame_us = 16683.0;
  constexpr std::uint32_t start = 1000;
  RN::Exchange a(start, 2, 0, 0, 7, 0x55aa);
  RN::Exchange b(start, 2, 1, 0, 7, 0x55aa);
  std::vector<InFlight> network;
  std::uint32_t frames[2]{start, start};
  double next_frame[2]{0.0, 0.0};
  const double rates[2]{rate_a, rate_b};
  const std::uint32_t clock_offset[2]{300'000'000U, 1'700'000'000U};
  std::uint32_t sequence[2]{};
  TimeSyncResult result;

  auto local_time = [&](std::uint8_t player, double wall_us) {
    return clock_offset[player] + static_cast<std::uint32_t>(wall_us);
  };

  while (true)
  {
    auto delivery = std::min_element(network.begin(), network.end(), [](const auto& lhs,
                                                                        const auto& rhs) {
      return lhs.deliver_us < rhs.deliver_us;
    });
    const double delivery_time = delivery == network.end() ?
                                     std::numeric_limits<double>::infinity() :
                                     delivery->deliver_us;
    const std::uint8_t frame_player = next_frame[0] <= next_frame[1] ? 0 : 1;
    const double frame_time = next_frame[frame_player];
    const double now = std::min(delivery_time, frame_time);
    if (now > duration_us)
      break;

    if (delivery_time <= frame_time)
    {
      const InFlight event = *delivery;
      network.erase(delivery);
      RN::Exchange& receiver = event.receiver == 0 ? a : b;
      assert(receiver.Accept(event.packet, local_time(event.receiver, now)));
      continue;
    }

    RN::Exchange& sender = frame_player == 0 ? a : b;
    sender.SubmitLocal(frames[frame_player], {});
    const std::uint32_t send_us = local_time(frame_player, now);
    const auto encoded = RN::Encode(sender.BuildPacket(frames[frame_player], send_us));
    const auto decoded = RN::Decode(encoded);
    assert(decoded);
    const int jitter_step = static_cast<int>((sequence[frame_player] * 7 + frame_player * 5) % 17) -
                            8;
    ++sequence[frame_player];
    const double path_us = latency_us + jitter_us * jitter_step / 8.0;
    network.push_back({now + path_us, static_cast<std::uint8_t>(1 - frame_player), *decoded});

    const std::uint32_t wait_us = sender.TimeSyncWaitUs(frames[frame_player]);
    if (wait_us != 0)
    {
      ++result.waits[frame_player];
      result.waited_us[frame_player] += wait_us;
    }
    ++frames[frame_player];
    next_frame[frame_player] += frame_us / rates[frame_player] + wait_us;
  }

  result.frames[0] = frames[0];
  result.frames[1] = frames[1];
  result.advantage[0] = a.FrameAdvantage();
  result.advantage[1] = b.FrameAdvantage();
  result.rtt_us[0] = a.SmoothedRttUs(1).value_or(0);
  result.rtt_us[1] = b.SmoothedRttUs(0).value_or(0);
  return result;
}

// Forty milliseconds of one-way latency used to make both peers report a
// two-to-three-frame lead and wait almost every frame. RTT projection should
// instead leave equal-rate peers inside the deadband without throttling both.
static void TestLatencyDoesNotCreateTwoLeaders()
{
  const TimeSyncResult result = SimulateTimeSync(1.0, 1.0, 40'000, 8'000, 20'000'000);
  assert(result.rtt_us[0] > 65'000 && result.rtt_us[0] < 95'000);
  assert(result.rtt_us[1] > 65'000 && result.rtt_us[1] < 95'000);
  assert(result.waits[0] == 0 || result.waits[1] == 0);
  assert(std::abs(static_cast<std::int64_t>(result.frames[0]) - result.frames[1]) <= 1);
}

// A 1.2% faster peer accumulates a real lead, so only that side should apply
// occasional sub-frame stretches. The estimates and frame counts converge
// despite latency, jitter, and unrelated host-clock epochs.
static void TestClockRateConvergence()
{
  const TimeSyncResult fast_a = SimulateTimeSync(1.012, 1.0, 40'000, 8'000, 60'000'000);
  assert(fast_a.waits[0] > 0);
  assert(fast_a.waits[1] == 0);
  assert(fast_a.waits[0] < (fast_a.frames[0] - 1000) / 20);
  assert(fast_a.waited_us[0] <= static_cast<std::uint64_t>(fast_a.waits[0]) * 10'000);
  assert(std::abs(static_cast<std::int64_t>(fast_a.frames[0]) - fast_a.frames[1]) <= 1);
  assert(std::abs(fast_a.advantage[0]) < 0.75);

  const TimeSyncResult fast_b = SimulateTimeSync(1.0, 1.012, 40'000, 8'000, 60'000'000);
  assert(fast_b.waits[0] == 0);
  assert(fast_b.waits[1] > 0);
  assert(fast_b.waits[1] < (fast_b.frames[1] - 1000) / 20);
  assert(fast_b.waited_us[1] <= static_cast<std::uint64_t>(fast_b.waits[1]) * 10'000);
  assert(std::abs(static_cast<std::int64_t>(fast_b.frames[0]) - fast_b.frames[1]) <= 1);
  assert(std::abs(fast_b.advantage[1]) < 0.75);
}

// Checksums are advertised only after their start state is confirmed.  The
// first unequal finalized frame remains sticky for deterministic reporting.
static void TestChecksumMismatch()
{
  RN::Exchange a(300, 2, 0, 0, 3, 9);
  RN::Exchange b(300, 2, 1, 0, 3, 9);
  RN::Pad pad{};
  a.SubmitLocal(300, pad);
  b.SubmitLocal(300, pad);
  Deliver(a, &b, 300);
  Deliver(b, &a, 300);
  a.SetStateChecksum(301, 0xaaaa);
  b.SetStateChecksum(301, 0xbbbb);
  a.UpdateFinalized();
  b.UpdateFinalized();
  Deliver(a, &b, 301);
  Deliver(b, &a, 301);
  assert(a.FirstDesync() == 301);
  assert(b.FirstDesync() == 301);
}

// With local input delay, the confirmed input prefix can lead the most recent
// state hash.  Finalization must advertise the newest available safe hash,
// rather than waiting for an exact confirmed+1 entry that does not exist yet.
static void TestDelayedChecksumFinalization()
{
  RN::Exchange a(350, 2, 0, 2, 3, 10);
  RN::Exchange b(350, 2, 1, 2, 3, 10);
  a.SetStateChecksum(350, 0xcafe);
  Deliver(b, &a, 350);  // b's neutral delay prefix confirms 350 and 351
  a.UpdateFinalized();
  const RN::Packet packet = a.BuildPacket(350);
  assert(packet.checksum_frame == 350);
  assert(packet.checksum == 0xcafe);

  // Retention keeps prediction's predecessor and never discards unacked local
  // history merely because the diagnostic window advanced.
  a.SubmitLocal(350, {});
  a.TrimBefore(1000);
  assert(a.OldestUnackedLocal() == 350);
}

// Strict decoding rejects truncation and a forged player count before state
// reaches Exchange::Accept.
static void TestWireValidation()
{
  RN::Exchange a(400, 2, 0, 1, 7, 11);
  a.SubmitLocal(400, {});
  std::vector<std::uint8_t> bytes = RN::Encode(a.BuildPacket(400));
  bytes.pop_back();
  assert(!RN::Decode(bytes));
  bytes = RN::Encode(a.BuildPacket(400));
  bytes[6] = 5;  // players
  assert(!RN::Decode(bytes));
}

int main()
{
  TestHandoffAgreement();
  TestPredictionAndCorrection();
  TestPredictionWindowAndAcks();
  TestLongLossRecovery();
  TestLatencyDoesNotCreateTwoLeaders();
  TestClockRateConvergence();
  TestChecksumMismatch();
  TestDelayedChecksumFinalization();
  TestWireValidation();
  std::puts("NSR rollback exchange tests passed");
  return 0;
}
