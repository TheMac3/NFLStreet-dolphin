// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// In-process test for the socket-free rollback exchange and handoff cores. It
// covers transition agreement, wire validation, delayed delivery and
// correction, prediction-window stalling, acknowledgement-driven history,
// confirmed-state desyncs, and latency-compensated time-sync convergence.
// A separate protocol-validation section covers hostile datagrams (forged
// acknowledgements, conflicting restatements, far-future input) and proves
// honest lossy/reordered/relayed traffic is never rejected. The telemetry
// sender's bounded shutdown flush (NSRTelemetryLogic.h) is checked with a fake
// clock and a fake slow server at the end.
//
// Standalone build from the repository root (no Dolphin, ENet, or gtest):
//   g++ -std=c++20 -Wall -Wextra -Werror -I Source/Core
//     Tools/NSR/NSRRollbackNetplayTest.cpp Source/Core/Core/NSR/NSRRollbackNetplay.cpp
//     -o nsr-rollback-netplay-test && ./nsr-rollback-netplay-test

#include "Core/NSR/NSRRollbackNetplay.h"
#include "Core/NSR/NSRRollbackLogic.h"
#include "Core/NSR/NSRTelemetryLogic.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace RN = NSRNetplay::RollbackNet;
namespace RL = NSRNetplay::RollbackLogic;

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

// ---------------------------------------------------------------------------
// Protocol validation.  Self-contained: these helpers and tests are used only
// by the functions in this section so the section can be merged or moved
// independently of the behavioral tests above.
// ---------------------------------------------------------------------------

// A deterministic, distinguishable sample for (player, live frame).  Bytes 10
// and 11 stay zero because Exchange canonicalizes them off the wire.
static RN::Pad ValidationPad(std::uint8_t player, std::uint32_t live)
{
  RN::Pad pad{};
  pad[0] = static_cast<std::uint8_t>(live * 7U + player * 13U);
  pad[1] = static_cast<std::uint8_t>(player + 1U);
  pad[2] = static_cast<std::uint8_t>(live >> 8);
  return pad;
}

// A hand-built datagram from `sender`, as an attacker (or a buggy peer)
// could produce one.  Fields default to "nothing to say".
static RN::Packet ForgedPacket(std::uint8_t sender, std::uint8_t players, std::uint32_t session)
{
  RN::Packet packet;
  packet.sender = sender;
  packet.players = players;
  packet.session = session;
  return packet;
}

// Everything a rejected datagram could have disturbed, captured through the
// public surface: the outgoing datagram (acks, retransmit window, checksum,
// timing echoes), held input ranges, contiguous prefixes, and pending work.
static std::vector<std::uint8_t> Fingerprint(const RN::Exchange& exchange, std::uint32_t frame)
{
  std::vector<std::uint8_t> out = RN::Encode(exchange.BuildPacket(frame, 4242));
  auto put = [&out](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
      out.push_back(static_cast<std::uint8_t>(value >> shift));
  };
  for (std::uint8_t player = 0; player < RN::MAX_PLAYERS; ++player)
  {
    put(static_cast<std::uint32_t>(exchange.HeldInputs(player)));
    const auto range = exchange.HeldInputRange(player);
    put(range ? range->first : RN::NO_FRAME);
    put(range ? range->second : RN::NO_FRAME);
    put(exchange.Contiguous()[player]);
    put(exchange.SmoothedRttUs(player).value_or(RN::NO_FRAME));
  }
  put(exchange.OldestUnackedLocal().value_or(RN::NO_FRAME));
  put(exchange.PendingRollback().value_or(RN::NO_FRAME));
  put(exchange.FirstDesync().value_or(RN::NO_FRAME));
  return out;
}

// A forged acknowledgement beyond our own contiguous local input would
// otherwise stop retransmission of every held local frame.  It, an ack before
// the epoch start, and any ack before we have produced input are rejected
// whole, including the timestamp and pads riding in the same datagram.
static void TestRejectsImpossibleAck()
{
  constexpr std::uint32_t session = 0x1234;
  RN::Exchange victim(100, 2, 0, 0, 7, session);

  RN::Packet early = ForgedPacket(1, 2, session);
  early.ack[0] = 100;  // nothing local exists yet
  assert(!victim.Accept(early));
  assert(victim.FirstViolation()->kind == RN::ViolationKind::ImpossibleAck);

  RN::Pad local{};
  local[0] = 1;
  victim.SubmitLocal(100, local);
  const auto before = Fingerprint(victim, 100);
  assert(victim.BuildPacket(100).pads.size() == 1);

  RN::Packet forged = ForgedPacket(1, 2, session);
  forged.ack[0] = 1'000'000;
  forged.send_us = 77;
  forged.echo_us[0] = 1;
  forged.first_frame = 100;
  forged.pads = {RN::Pad{}};
  forged.checksum_frame = 100;
  forged.checksum = 0xdead;
  assert(!victim.Accept(forged, 5000));
  assert(Fingerprint(victim, 100) == before);
  assert(victim.BuildPacket(100).pads.size() == 1);  // retransmission continues
  assert(victim.ViolationCount() == 2);

  RN::Packet ancient = ForgedPacket(1, 2, session);
  ancient.ack[0] = 99;  // before the epoch start: never a contiguous prefix
  assert(!victim.Accept(ancient));
  assert(Fingerprint(victim, 100) == before);
  assert(victim.ViolationCount() == 3);
  // The first violation is sticky for deterministic reporting.
  assert(victim.FirstViolation()->frame == 100 && victim.FirstViolation()->player == 1);

  // The exact boundary (an ack of everything we produced) is legitimate and
  // ends retransmission; NO_FRAME means "nothing received yet".
  RN::Packet honest = ForgedPacket(1, 2, session);
  assert(victim.Accept(honest));
  honest.ack[0] = 100;
  assert(victim.Accept(honest));
  assert(victim.BuildPacket(100).pads.empty());
  assert(victim.ViolationCount() == 3);
}

// Identical restatements are the normal redundancy path.  A restatement with
// different content for a confirmed frame is a cheat/corruption signal: it is
// rejected whole, so a new frame or ack bundled with it is not applied either.
static void TestConflictingDuplicateInput()
{
  constexpr std::uint32_t session = 0x2345;
  RN::Exchange victim(100, 2, 0, 0, 7, session);
  victim.SubmitLocal(100, {});

  RN::Packet first = ForgedPacket(1, 2, session);
  first.first_frame = 100;
  RN::Pad a{};
  a[0] = 0x11;
  first.pads = {a};
  assert(victim.Accept(first));
  assert(victim.Accept(first));  // exact duplicate
  RN::Packet noisy = first;
  noisy.pads[0][10] = 1;  // process-local bytes are canonicalized away
  noisy.pads[0][11] = 0xff;
  assert(victim.Accept(noisy));
  assert(victim.ViolationCount() == 0);

  const auto before = Fingerprint(victim, 100);
  RN::Packet conflict = first;
  conflict.pads[0][0] = 0x22;
  conflict.pads.push_back(a);  // a new frame 101 must not be half-applied
  conflict.ack[0] = 100;
  assert(!victim.Accept(conflict));
  assert(Fingerprint(victim, 100) == before);
  assert(victim.Contiguous()[1] == 100);
  assert(victim.Assemble(100)[1][0] == 0x11);
  assert(victim.ViolationCount() == 1);
  assert(victim.FirstViolation()->kind == RN::ViolationKind::ConflictingInput);
  assert(victim.FirstViolation()->frame == 100);
}

// Input and checksums beyond FutureInputLimit() are rejected, the boundary
// itself is accepted, and TrimBefore leaves only the prediction predecessor
// below its cutoff without letting stale retransmissions resurrect history.
static void TestFutureInputWindowAndTrim()
{
  constexpr std::uint32_t session = 0x3456;
  RN::Exchange victim(100, 2, 0, 1, 7, session);
  // Delay prefix 100 is local input; the limit is anchored past it.
  assert(victim.FutureInputLimit() == 101 + 7 + 1 + RN::FUTURE_INPUT_SLACK);
  victim.SubmitLocal(100, {});  // occupies frame 101
  const std::uint32_t limit = victim.FutureInputLimit();
  assert(limit == 102 + 7 + 1 + RN::FUTURE_INPUT_SLACK);

  const auto before = Fingerprint(victim, 100);
  RN::Packet distant = ForgedPacket(1, 2, session);
  distant.first_frame = 1'000'000;
  distant.pads = {RN::Pad{}};
  assert(!victim.Accept(distant));
  RN::Packet straddle = ForgedPacket(1, 2, session);
  straddle.first_frame = limit;  // last pad lands one past the limit
  straddle.pads = {RN::Pad{}, RN::Pad{}};
  assert(!victim.Accept(straddle));
  RN::Packet checksum = ForgedPacket(1, 2, session);
  checksum.checksum_frame = limit + 1;
  assert(!victim.Accept(checksum));
  assert(Fingerprint(victim, 100) == before);
  assert(victim.ViolationCount() == 3);
  assert(victim.FirstViolation()->kind == RN::ViolationKind::InputOutOfWindow);
  assert(victim.FirstViolation()->frame == 1'000'000);

  RN::Packet edge = ForgedPacket(1, 2, session);
  edge.first_frame = limit;
  edge.pads = {RN::Pad{}};
  assert(victim.Accept(edge));
  victim.TrimBefore(500);
  const auto range = victim.HeldInputRange(1);
  assert(range && range->first == limit && range->second == limit);

  // Confirm a long remote prefix and fully acknowledged local history, then
  // trim: exactly one predecessor survives below the cutoff on both sides.
  RN::Exchange a(1000, 2, 0, 0, 60, session);
  RN::Exchange b(1000, 2, 1, 0, 60, session);
  for (std::uint32_t frame = 1000; frame < 1060; ++frame)
  {
    a.SubmitLocal(frame, ValidationPad(0, frame));
    b.SubmitLocal(frame, ValidationPad(1, frame));
  }
  const RN::Packet stale = b.BuildPacket(1059);  // frames 1000..1059
  assert(a.Accept(stale));
  assert(b.Accept(a.BuildPacket(1059)));
  assert(a.Accept(b.BuildPacket(1059)));  // b acknowledges a through 1059
  a.TrimBefore(1050);
  assert(a.HeldInputRange(0)->first == 1049);
  assert(a.HeldInputRange(1)->first == 1049);
  assert(a.HeldInputs(1) == 11);
  // A reordered pre-trim retransmission is still honest, but it must not
  // resurrect trimmed frames.
  assert(a.Accept(stale));
  assert(a.HeldInputRange(1)->first == 1049);
  assert(a.ViolationCount() == 0);
}

// Legitimate acknowledgement progress keeps advancing the retransmission
// window and lets TrimBefore release local history.
static void TestAckAdvanceTrimsRetransmission()
{
  RN::Exchange a(100, 2, 0, 2, 7, 0x4567);
  RN::Exchange b(100, 2, 1, 2, 7, 0x4567);
  for (std::uint32_t live = 100; live < 108; ++live)
    a.SubmitLocal(live, ValidationPad(0, live));  // frames 100..109
  assert(a.BuildPacket(107).first_frame == 100);
  assert(a.BuildPacket(107).pads.size() == 10);
  assert(b.Accept(a.BuildPacket(107)));
  assert(a.Accept(b.BuildPacket(100)));  // acknowledges 100..109
  assert(a.OldestUnackedLocal() == 110);
  assert(a.BuildPacket(107).pads.empty());
  a.SubmitLocal(108, ValidationPad(0, 108));  // frame 110
  const RN::Packet next = a.BuildPacket(108);
  assert(next.first_frame == 110 && next.pads.size() == 1);
  a.TrimBefore(200);  // capped by the oldest unacknowledged local frame
  assert(a.HeldInputRange(0)->first == 109);
}

struct ValidationNetConfig
{
  std::uint8_t players = 2;
  bool star = false;
  std::array<std::uint32_t, RN::MAX_PLAYERS> delay{1, 1, 1, 1};
  std::array<std::uint32_t, RN::MAX_PLAYERS> max_prediction{7, 7, 7, 7};
  std::uint32_t loss_percent = 25;
  std::uint32_t max_latency_ticks = 3;
  std::uint32_t seed = 1;
};

struct ValidationDatagram
{
  std::uint32_t due;
  std::uint8_t to;
  std::vector<std::uint8_t> bytes;
};

// Honest peers over a lossy, reordering, duplicating-by-retransmit network.
// In star mode leaves link only to player 0, which relays each leaf's
// original datagram unchanged to the other leaves, exactly as HandlePacket
// does.  Every honest datagram must be accepted, every peer must converge on
// identical inputs for every retained frame, and no violation may be
// recorded -- including under asymmetric delay/prediction settings.
static void SimulateValidatedNetwork(const ValidationNetConfig& config)
{
  constexpr std::uint32_t start = 700;
  constexpr std::uint32_t target = start + 240;
  constexpr std::uint32_t session = 0x5678;
  std::vector<RN::Exchange> peers;
  peers.reserve(config.players);
  for (std::uint8_t player = 0; player < config.players; ++player)
  {
    peers.emplace_back(start, config.players, player, config.delay[player],
                       config.max_prediction[player], session);
  }
  std::vector<std::uint32_t> live(config.players, start);
  std::vector<ValidationDatagram> network;
  std::uint32_t rng = config.seed;
  auto next_random = [&rng]() {
    rng = rng * 1664525U + 1013904223U;
    return rng >> 8;
  };
  auto linked = [&config](std::uint8_t lhs, std::uint8_t rhs) {
    return lhs != rhs && (!config.star || lhs == 0 || rhs == 0);
  };
  auto send = [&](std::uint32_t now, std::uint8_t to, const std::vector<std::uint8_t>& bytes) {
    if (next_random() % 100 < config.loss_percent)
      return;
    network.push_back({now + 1 + next_random() % (config.max_latency_ticks + 1), to, bytes});
  };

  std::uint32_t verify_from = start;
  std::uint32_t tick = 0;
  auto converged = [&]() {
    for (const RN::Exchange& peer : peers)
    {
      const std::uint32_t confirmed = peer.ConfirmedThrough();
      if (confirmed == RN::NO_FRAME || confirmed < target)
        return false;
    }
    return true;
  };
  for (; tick < 20'000 && !converged(); ++tick)
  {
    for (std::uint8_t player = 0; player < config.players; ++player)
    {
      RN::Exchange& peer = peers[player];
      // Adapter order: submit, then stall-check, then (if allowed) run.
      peer.SubmitLocal(live[player], ValidationPad(player, live[player]));
      if (!peer.ShouldStall(live[player]))
      {
        peer.Assemble(live[player]);
        ++live[player];
      }
      const auto bytes = RN::Encode(peer.BuildPacket(live[player]));
      for (std::uint8_t to = 0; to < config.players; ++to)
      {
        if (linked(player, to))
          send(tick, to, bytes);
      }
    }

    std::vector<ValidationDatagram> due;
    for (auto it = network.begin(); it != network.end();)
    {
      if (it->due <= tick)
      {
        due.push_back(std::move(*it));
        it = network.erase(it);
      }
      else
      {
        ++it;
      }
    }
    for (const ValidationDatagram& datagram : due)
    {
      const auto packet = RN::Decode(datagram.bytes);
      assert(packet);
      if (config.star && datagram.to == 0)
      {
        for (std::uint8_t leaf = 1; leaf < config.players; ++leaf)
        {
          if (leaf != packet->sender)
            send(tick, leaf, datagram.bytes);
        }
      }
      const bool accepted = peers[datagram.to].Accept(*packet);
      assert(accepted);
    }

    if (tick % 25 == 0)
    {
      for (std::uint8_t player = 0; player < config.players; ++player)
      {
        if (live[player] > start + 80)
        {
          peers[player].TrimBefore(live[player] - 80);
          verify_from = std::max(verify_from, live[player] - 80);
        }
      }
    }
  }
  assert(converged());
  assert(verify_from + 40 < target);

  for (std::uint8_t player = 0; player < config.players; ++player)
  {
    RN::Exchange& peer = peers[player];
    assert(peer.ViolationCount() == 0);
    // Acknowledgements kept the retransmission window moving.
    assert(peer.OldestUnackedLocal().value_or(0) > start + 100);
    for (std::uint32_t frame = verify_from; frame <= target; ++frame)
    {
      const RN::Pads pads = peer.Assemble(frame);
      for (std::uint8_t owner = 0; owner < config.players; ++owner)
      {
        const std::uint32_t delay = config.delay[owner];
        const RN::Pad expected =
            frame < start + delay ? RN::Pad{} : ValidationPad(owner, frame - delay);
        assert(pads[owner] == expected);
      }
    }
  }
}

static void TestHonestNetworksConverge()
{
  ValidationNetConfig two;
  SimulateValidatedNetwork(two);

  // Lockstep epoch (max prediction 0) with a larger shared delay.
  ValidationNetConfig lockstep;
  lockstep.delay = {2, 2, 2, 2};
  lockstep.max_prediction = {0, 0, 0, 0};
  lockstep.seed = 2;
  SimulateValidatedNetwork(lockstep);

  // Per-process settings may differ; the widest allowed peer (delay 8,
  // prediction 60) must still fit the narrowest peer's acceptance window.
  ValidationNetConfig asymmetric;
  asymmetric.delay = {0, 8, 1, 1};
  asymmetric.max_prediction = {0, 60, 7, 7};
  asymmetric.loss_percent = 10;
  asymmetric.seed = 3;
  SimulateValidatedNetwork(asymmetric);

  ValidationNetConfig star3;
  star3.players = 3;
  star3.star = true;
  star3.seed = 4;
  SimulateValidatedNetwork(star3);

  ValidationNetConfig star4;
  star4.players = 4;
  star4.star = true;
  star4.delay = {2, 1, 0, 3};
  star4.max_prediction = {7, 4, 7, 10};
  star4.seed = 5;
  SimulateValidatedNetwork(star4);

  ValidationNetConfig mesh4;
  mesh4.players = 4;
  mesh4.loss_percent = 35;
  mesh4.max_latency_ticks = 6;
  mesh4.seed = 6;
  SimulateValidatedNetwork(mesh4);
}

// A relayed datagram is validated against its original author, not the
// relaying host: a leaf's restatement conflicting with its own earlier input
// is rejected even when it arrives through player 0, while the host's own and
// other leaves' identical relays remain accepted.
static void TestRelayedConflictAttributedToAuthor()
{
  constexpr std::uint32_t session = 0x6789;
  RN::Exchange leaf1(100, 3, 1, 0, 7, session);
  RN::Exchange leaf2(100, 3, 2, 0, 7, session);
  leaf1.SubmitLocal(100, {});
  leaf2.SubmitLocal(100, ValidationPad(2, 100));
  const auto relayed = RN::Decode(RN::Encode(leaf2.BuildPacket(100)));
  assert(relayed && relayed->sender == 2);
  assert(leaf1.Accept(*relayed));
  assert(leaf1.Accept(*relayed));

  RN::Packet tampered = *relayed;
  tampered.pads[0][0] ^= 0x40;
  assert(!leaf1.Accept(tampered));
  assert(leaf1.FirstViolation()->player == 2);
  assert(leaf1.FirstViolation()->kind == RN::ViolationKind::ConflictingInput);
  assert(leaf1.Assemble(100)[2] == ValidationPad(2, 100));
}

// Thousands of arbitrary datagrams exercise every strict-length and field
// validation branch. Accepted packets must remain stable when re-encoded.
static void TestMalformedDatagramFuzz()
{
  std::mt19937 rng(0x4e535242U);
  std::uniform_int_distribution<std::size_t> size_dist(0, 1024);
  std::uniform_int_distribution<unsigned int> byte_dist(0, 255);
  for (int iteration = 0; iteration < 20000; ++iteration)
  {
    std::vector<std::uint8_t> bytes(size_dist(rng));
    for (std::uint8_t& byte : bytes)
      byte = static_cast<std::uint8_t>(byte_dist(rng));
    if (const auto packet = RN::Decode(bytes))
    {
      const auto roundtrip = RN::Decode(RN::Encode(*packet));
      assert(roundtrip);
      assert(roundtrip->sender == packet->sender);
      assert(roundtrip->players == packet->players);
      assert(roundtrip->pads == packet->pads);
    }
    (void)RN::DecodeHandoff(bytes);
  }

  RN::Exchange exchange(10, 4, 0, 1, 7, 99);
  exchange.SubmitLocal(10, {});
  const std::vector<std::uint8_t> valid = RN::Encode(exchange.BuildPacket(10));
  for (std::size_t cut = 0; cut < valid.size(); ++cut)
    assert(!RN::Decode(std::span(valid).first(cut)));
}

// Multiple late corrections must schedule the earliest affected frame and a
// matching confirmation must not erase an older pending mismatch.
static void TestEarliestCorrectionIsSticky()
{
  RN::Exchange local(100, 3, 0, 0, 7, 1);
  RN::Exchange p1(100, 3, 1, 0, 7, 1);
  RN::Exchange p2(100, 3, 2, 0, 7, 1);
  RN::Pad one{};
  RN::Pad two{};
  one[0] = 1;
  two[0] = 2;
  for (std::uint32_t frame = 100; frame <= 106; ++frame)
  {
    local.SubmitLocal(frame, {});
    local.Assemble(frame);
  }
  p1.SubmitLocal(104, one);
  p2.SubmitLocal(102, two);
  assert(local.Accept(p1.BuildPacket(104)));
  assert(local.PendingRollback() == 104);
  assert(local.Accept(p2.BuildPacket(102)));
  assert(local.PendingRollback() == 102);
  assert(local.Accept(p1.BuildPacket(104)));  // duplicate
  assert(local.PendingRollback() == 102);
}

// Loss, reordered delivery, and duplicates may delay acknowledgement but
// cannot corrupt the contiguous history or make acknowledged inputs reappear.
static void TestHistoryUnderLossReorderAndDuplication()
{
  RN::Exchange sender(700, 2, 0, 0, 120, 4);
  RN::Exchange receiver(700, 2, 1, 0, 120, 4);
  for (std::uint32_t frame = 700; frame < 850; ++frame)
  {
    RN::Pad pad{};
    pad[0] = static_cast<std::uint8_t>(frame);
    sender.SubmitLocal(frame, pad);
  }

  RN::Packet first = sender.BuildPacket(849);
  assert(first.first_frame == 700 && first.pads.size() == RN::MAX_HISTORY);
  RN::Packet later = first;
  later.first_frame += 32;
  later.pads.erase(later.pads.begin(), later.pads.begin() + 32);
  assert(receiver.Accept(later));  // reordered suffix first
  assert(receiver.Contiguous()[0] == RN::NO_FRAME);
  assert(receiver.Accept(later));  // duplicate is idempotent
  assert(receiver.Accept(first));  // closes the gap
  assert(receiver.Contiguous()[0] == 763);
  Deliver(receiver, &sender, 849);  // ack through 763
  RN::Packet second = sender.BuildPacket(849);
  assert(second.first_frame == 764);
  assert(receiver.Accept(second));
  assert(receiver.Contiguous()[0] == 827);
  Deliver(receiver, &sender, 850);
  RN::Packet third = sender.BuildPacket(849);
  assert(third.first_frame == 828 && third.pads.size() == 22);
  assert(receiver.Accept(third));
  assert(receiver.Contiguous()[0] == 849);
}

// Equal checksums never produce a false positive; once a mismatch is found,
// later agreement or a newer mismatch cannot hide the first desync.
static void TestChecksumAgreementAndStickyDesync()
{
  RN::Exchange a(900, 2, 0, 0, 7, 5);
  RN::Exchange b(900, 2, 1, 0, 7, 5);
  for (std::uint32_t frame = 900; frame <= 904; ++frame)
  {
    a.SubmitLocal(frame, {});
    b.SubmitLocal(frame, {});
    Deliver(a, &b, frame);
    Deliver(b, &a, frame);
    a.SetStateChecksum(frame + 1, frame == 902 ? 11U : frame);
    b.SetStateChecksum(frame + 1, frame == 902 ? 12U : frame);
    a.UpdateFinalized();
    b.UpdateFinalized();
    Deliver(a, &b, frame);
    Deliver(b, &a, frame);
  }
  assert(a.FirstDesync() == 903);
  assert(b.FirstDesync() == 903);
  a.SetStateChecksum(905, 904);  // later agreement cannot clear it
  assert(a.FirstDesync() == 903);
}

// Stage-4 transitions are independent generations. Reordered stale packets
// and a dropped first announcement do not contaminate leave or re-entry.
static void TestRepeatedHandoffTransitions()
{
  const auto run = [](RN::HandoffKind kind, std::uint32_t generation, std::uint32_t frame) {
    RN::HandoffAgreement agreement(4, 0, kind, generation, frame, 0xfeed0000U + generation);
    const std::array<RN::HandoffPacket, 3> peers{{
        {1, 4, kind, generation, frame, 0xfeed0000U + generation},
        {2, 4, kind, generation, frame, 0xfeed0000U + generation},
        {3, 4, kind, generation, frame, 0xfeed0000U + generation},
    }};
    assert(agreement.Accept(peers[2]));
    assert(agreement.Accept(peers[0]));
    assert(agreement.Accept(peers[2]));  // duplicate
    assert(!agreement.Complete());       // peer 2's first packet was lost
    assert(agreement.Accept(peers[1]));
    assert(agreement.Complete());
  };
  run(RN::HandoffKind::EnterRollback, 1, 4521);
  run(RN::HandoffKind::LeaveRollback, 2, 149407);
  run(RN::HandoffKind::EnterRollback, 3, 153925);

  RN::HandoffAgreement reentry(2, 0, RN::HandoffKind::EnterRollback, 3, 153925, 9);
  assert(!reentry.Accept({1, 2, RN::HandoffKind::LeaveRollback, 2, 149407, 8}));
  assert(!reentry.Mismatched());  // stale generation is ignored
}

// Production helper decisions cover the non-I/O portions of snapshotting,
// target selection, sidecar normalization, and audio virtual time.
static void TestPureRollbackHelpers()
{
  assert(RL::PageNeedsCopy(7, std::nullopt));
  assert(!RL::PageNeedsCopy(7, 7));
  assert(RL::PageNeedsCopy(8, 7));
  for (std::uint32_t frame = 0; frame < 1000; ++frame)
    assert(RL::ForcedDepth(7, 123, frame) <= 7);
  assert(RL::ForcedDepth(7, std::nullopt, 20) == 7);
  assert(RL::ForcedDepth(0, 123, 20) == 0);

  assert(RL::RollbackLowerBound(100, 24, 80, 90) == 90);
  std::set<std::uint32_t> idle{92, 95};
  const auto target = RL::FindIdleTarget(96, 90, true, [&](std::uint32_t frame) {
    return idle.contains(frame);
  });
  assert(target == 95);
  assert(!RL::FindIdleTarget(89, 90, true, [&](std::uint32_t) { return true; }));
  assert(!RL::FindIdleTarget(95, 90, false, [&](std::uint32_t) { return true; }));

  std::uint32_t cursor = 0;
  const std::array<std::uint32_t, 3> pages{4, 8, 12};
  std::vector<std::uint32_t> audited;
  for (int i = 0; i < 5; ++i)
    audited.push_back(*RL::NextRotatingAuditPage(pages, &cursor));
  assert((audited == std::vector<std::uint32_t>{4, 8, 12, 4, 8}));
  assert(cursor == 5);
  assert(RL::NextRotatingAuditPage(pages, &cursor) == 12);
  assert(RL::NextRotatingAuditPage(pages, &cursor) == 4);
  const std::span<const std::uint32_t> no_pages;
  assert(!RL::NextRotatingAuditPage(no_pages, &cursor));

  const auto merged = RL::MergeRanges({{30, 40}, {10, 20}, {20, 25}, {35, 50}, {7, 7}});
  assert((merged == std::vector<RL::Range>{{10, 25}, {30, 50}}));
  assert(RL::AudioNow(100, 3, 2) == 105);
  assert(RL::FramesToMs(75) == 1250);
  assert(RL::BusyPollCatchups(999, 1000) == 0);
  assert(RL::BusyPollCatchups(2001, 1000) == 2);
  assert(RL::Before(9, 10) && !RL::Before(10, 10));
  const RL::SongTimeline song = RL::ScheduleSong(100, 380, 10800);
  assert(song.start == 480 && song.end == 11280);

  std::map<const void*, std::uint32_t> synced{{pages.data(), 7}};
  std::map<const void*, std::uint64_t> sequences{{pages.data(), 9}};
  RL::ClearSessionSlotMetadata(&synced, &sequences);
  assert(synced.empty() && sequences.empty());
  assert(RL::PageNeedsCopy(7, std::nullopt));
}

// Telemetry shutdown (final review L1): every pending batch shares ONE
// deadline. Fake clock in milliseconds; the fake server takes `latency` per
// request and, like the sender's curl progress-callback abort, gives up with
// no response (0) when the deadline arrives first.
static void TestTelemetryFinalFlushIsBounded()
{
  namespace TL = NSRTelemetry::Logic;
  struct FakeServer
  {
    explicit FakeServer(int latency_ms, std::vector<int> codes_ = {})
        : latency(latency_ms), codes(std::move(codes_))
    {
    }
    int now = 0;
    int latency = 0;
    std::vector<int> codes;  // per request, then 200
    size_t requests = 0;
    std::vector<int> spilled;
    int Post(int deadline)
    {
      const int code = requests < codes.size() ? codes[requests] : 200;
      ++requests;
      if (now + latency > deadline)
      {
        now = deadline;
        return 0;
      }
      now += latency;
      return code;
    }
  };
  auto run = [](FakeServer& srv, std::deque<int>& pending, int budget) {
    const int deadline = srv.now + budget;
    return TL::FinalFlush(
        pending, deadline, [&] { return srv.now; }, [&](int) { return srv.Post(deadline); },
        [&](int b) { srv.spilled.push_back(b); });
  };
  auto queue = [](int n) {
    std::deque<int> q;
    for (int i = 0; i < n; ++i)
      q.push_back(i);
    return q;
  };

  // The review's scenario: 64 batches, a server that answers each in 1.9 s.
  // Old behavior: 64 x 1.9 s. Now: one delivered, the second aborted at 2 s.
  {
    FakeServer srv(1900);
    auto pending = queue(64);
    const auto r = run(srv, pending, 2000);
    assert(r.sent == 1 && r.deadline_hit && r.stop_code == 0);
    assert(srv.now == 2000 && srv.requests == 2);
    assert(pending.size() == 63 && pending.front() == 1);  // left for the caller to spill
  }
  // A fast server drains everything within the budget.
  {
    FakeServer srv(10);
    auto pending = queue(64);
    const auto r = run(srv, pending, 2000);
    assert(r.sent == 64 && !r.deadline_hit && pending.empty() && srv.now == 640);
  }
  // Budget used up exactly by two answers: no third request is started.
  {
    FakeServer srv(1000);
    auto pending = queue(5);
    const auto r = run(srv, pending, 2000);
    assert(r.sent == 2 && r.deadline_hit && srv.requests == 2 && pending.size() == 3);
  }
  // 400/413: kept locally (reject) and skipped; 503 and 401 stop at once.
  {
    FakeServer srv(10, {400, 200, 413, 503});
    auto pending = queue(6);
    const auto r = run(srv, pending, 2000);
    assert(r.sent == 1 && r.rejected == 2 && r.stop_code == 503 && !r.deadline_hit);
    assert((srv.spilled == std::vector<int>{0, 2}) && pending.size() == 3 && pending.front() == 3);
    FakeServer auth(10, {401});
    auto pending2 = queue(3);
    const auto r2 = run(auth, pending2, 2000);
    assert(r2.sent == 0 && r2.stop_code == 401 && pending2.size() == 3 && auth.requests == 1);
  }
  // Zero budget: nothing is sent, nothing is lost.
  {
    FakeServer srv(10);
    auto pending = queue(3);
    const auto r = run(srv, pending, 0);
    assert(r.sent == 0 && r.deadline_hit && srv.requests == 0 && pending.size() == 3);
  }
}

// ARAM replay-ring undo log (final-review M1): snapshots taken at frame
// boundaries, writes inside and across the window edge, a rollback, a
// re-simulation that writes different bytes, a second (deeper) rollback that
// must undo re-simulated writes too, trimming, and the session clear.
static void TestWindowUndoLog()
{
  std::vector<std::uint8_t> mem(64, 0);
  RL::WindowUndoLog log;
  constexpr std::uint32_t lo = 16, hi = 48;
  auto write = [&](std::uint32_t address, std::uint32_t length, std::uint8_t value) {
    log.Record(mem, lo, hi, address, length);
    std::fill(mem.begin() + address, mem.begin() + address + length, value);
  };
  auto window = [&] { return std::vector<std::uint8_t>(mem.begin() + lo, mem.begin() + hi); };

  // Frame 0..3 snapshots (mark + expected window at each frame start).
  std::vector<std::uint64_t> marks;
  std::vector<std::vector<std::uint8_t>> states;
  auto save = [&] {
    marks.push_back(log.Mark());
    states.push_back(window());
  };
  save();                // frame 0
  write(20, 8, 1);       // inside
  write(8, 16, 2);       // straddles lo: only [16, 24) is recorded
  save();                // frame 1
  write(44, 12, 3);      // straddles hi: only [44, 48) is recorded
  write(0, 8, 4);        // entirely outside: not recorded
  assert(log.Records() == 3);
  assert(log.Bytes() == 8 + 8 + 4);
  save();                // frame 2
  write(16, 32, 5);      // whole window
  save();                // frame 3
  write(30, 2, 6);

  // Roll back to frame 1 and re-simulate frames 1..2 with different writes.
  assert(log.UndoTo(marks[1], mem) == 3);
  assert(window() == states[1]);
  assert(mem[0] == 4);  // outside bytes are not the log's business
  marks.resize(2);
  states.resize(2);
  write(40, 4, 7);  // re-simulated frame 1
  save();           // frame 2 (re-saved)
  write(16, 4, 8);  // re-simulated frame 2
  save();           // frame 3 (re-saved)

  // A deeper rollback undoes re-simulated and original writes in order.
  assert(log.UndoTo(marks[2], mem) == 1);
  assert(window() == states[2]);
  assert(log.UndoTo(marks[0], mem) == 3);
  assert(window() == states[0]);
  assert(std::all_of(mem.begin() + lo, mem.begin() + hi, [](std::uint8_t b) { return b == 0; }));

  // Trimming keeps exactly what restorable marks still need.
  marks.assign(1, log.Mark());
  write(24, 4, 9);
  marks.push_back(log.Mark());
  write(28, 4, 10);
  log.TrimThrough(marks[0]);
  assert(log.Records() == 2);
  log.TrimThrough(marks[1]);
  assert(log.Records() == 1 && log.Bytes() == 4);
  assert(log.UndoTo(marks[1], mem) == 1);
  assert(mem[28] == 0 && mem[24] == 9);

  log.Clear();
  assert(log.Records() == 0 && log.Bytes() == 0 && log.Mark() == 0);
  assert(log.Record(mem, lo, hi, 60, 16) == 0);  // past the backing store
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
  // Protocol validation section.
  TestRejectsImpossibleAck();
  TestConflictingDuplicateInput();
  TestFutureInputWindowAndTrim();
  TestAckAdvanceTrimsRetransmission();
  TestHonestNetworksConverge();
  TestRelayedConflictAttributedToAuthor();
  TestMalformedDatagramFuzz();
  TestEarliestCorrectionIsSticky();
  TestHistoryUnderLossReorderAndDuplication();
  TestChecksumAgreementAndStickyDesync();
  TestRepeatedHandoffTransitions();
  TestPureRollbackHelpers();
  TestWindowUndoLog();
  TestTelemetryFinalFlushIsBounded();
  std::puts("NSR rollback exchange tests passed");
  return 0;
}
