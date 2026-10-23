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
#include "Core/NSR/NSRBootAgreement.h"
#include "Core/NSR/NSRQuickstart.h"
#include "Core/NSR/NSRRollbackLogic.h"
#include "Core/NSR/NSRTelemetryLogic.h"
#include "Core/NSR/NSRGameStatsRead.h"
#include "Core/NSR/NSRDraftLog.h"
#include "Core/NSR/NSRPadSource.h"
#include "Core/NSR/NSRContentRead.h"
#include "Core/NSR/NSRDraft.h"
#include "Core/NSR/NSRGameStatsIdentity.h"

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
#include <string_view>
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
// every peer. Per-peer clocks may differ, player 0's survives wire encoding,
// duplicates are harmless, and the first conflict stays visible.
static void TestHandoffAgreement()
{
  RN::HandoffAgreement agreement(3, 0, RN::HandoffKind::EnterRollback, 2, 1234, 0xaabbccdd, {},
                                 100, 1000);
  RN::HandoffPacket peer1{1, 3, RN::HandoffKind::EnterRollback, 2, 1234, 0xaabbccdd, {}, 111,
                          1110};
  RN::HandoffPacket peer2{2, 3, RN::HandoffKind::EnterRollback, 2, 1234, 0xaabbccdd, {}, 122,
                          1220};
  const auto bytes = RN::EncodeHandoff(peer1);
  const auto decoded = RN::DecodeHandoff(bytes);
  assert(decoded);
  assert(decoded->sync_clock == 111);
  assert(decoded->sync_aux_clock == 1110);
  assert(agreement.Accept(*decoded));
  assert(agreement.Accept(*decoded));
  assert(!agreement.Complete());
  assert(agreement.Accept(peer2));
  assert(agreement.Complete());
  assert(agreement.HostClock() == 100);
  assert(agreement.HostAuxClock() == 1000);
  // A retransmission may not change its sender's clock.
  RN::HandoffPacket moved = peer2;
  moved.sync_clock = 123;
  assert(!agreement.Accept(moved));
  assert(agreement.Mismatched() && !agreement.Complete());

  RN::HandoffAgreement leaf(3, 2, RN::HandoffKind::EnterRollback, 2, 1234, 0xaabbccdd, {},
                            122, 1220);
  assert(leaf.Accept(
      {0, 3, RN::HandoffKind::EnterRollback, 2, 1234, 0xaabbccdd, {}, 100, 1000}));
  assert(leaf.HostClock() == 100);
  assert(leaf.HostAuxClock() == 1000);
  assert(leaf.Accept(peer1));
  assert(leaf.Complete());

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
// fix/lifecycle: transition states hold confirmed-input mode for a grace
// window; normal play never waits.
static void TestLifecycleConfirmFrames()
{
  std::uint32_t hold = 0;
  const RL::LifecycleSignals play{};
  assert(!RL::LifecycleConfirmFrame(100, play, 8, hold) && hold == 0);
  RL::LifecycleSignals paused{};
  paused.pause_depth = 1;
  assert(RL::LifecycleConfirmFrame(101, paused, 8, hold) && hold == 110);
  assert(RL::LifecycleConfirmFrame(109, play, 8, hold));
  assert(!RL::LifecycleConfirmFrame(110, play, 8, hold));
  RL::LifecycleSignals ending{};
  ending.in_game_running = false;  // the teardown frame itself
  assert(RL::LifecycleConfirmFrame(500, ending, 0, hold));
  assert(!RL::LifecycleConfirmFrame(501, play, 0, hold));
  RL::LifecycleSignals dialog{};
  dialog.modal_dialog = true;
  assert(RL::LifecycleConfirmFrame(0xFFFFFFFE, dialog, 8, hold));
  assert(RL::LifecycleConfirmFrame(3, play, 8, hold));  // window crosses wrap
  assert(!RL::LifecycleConfirmFrame(7, play, 8, hold));
}

// MispredictedPlayers names the player whose late input differs from the
// one the frame ran with (handoff diagnostics).
static void TestMispredictedPlayers()
{
  RN::Exchange a(100, 2, 0, 1, 3, 0x1234);
  RN::Exchange b(100, 2, 1, 1, 3, 0x1234);
  RN::Pad neutral{};
  RN::Pad right{};
  right[0] = 1;
  a.SubmitLocal(100, neutral);
  b.SubmitLocal(100, right);
  a.Assemble(101);
  assert(a.MispredictedPlayers(101) == 0);
  Deliver(b, &a, 101);
  assert(a.PendingRollback() == 101 && a.MispredictedPlayers(101) == 0x2);
  assert(a.MispredictedPlayers(5) == 0);
}

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

// ---------------------------------------------------------------------------
// Config agreement (final review M2) and final-only digest comparison (H4).
// ---------------------------------------------------------------------------

static RN::ConfigFingerprint TestConfig(std::uint64_t salt)
{
  RN::ConfigFingerprint config;
  for (std::size_t i = 0; i < RN::CONFIG_FIELDS; ++i)
    config.fields[i] = 0x1000 * (i + 1) + salt;
  return config;
}

// Every field survives the wire; a proposal whose configuration differs is
// reported as a config mismatch naming exactly the differing fields, takes
// precedence over a state-hash difference, and blocks completion.  Stale
// transitions are still ignored, whatever their configuration.
static void TestHandoffConfigAgreement()
{
  const RN::ConfigFingerprint local = TestConfig(0);
  RN::HandoffPacket same{1, 2, RN::HandoffKind::EnterRollback, 4, 3080, 0x55, local};
  const auto bytes = RN::EncodeHandoff(same);
  const auto decoded = RN::DecodeHandoff(bytes);
  assert(decoded && decoded->config == local && decoded->semantic_hash == 0x55);

  RN::HandoffAgreement ok(2, 0, RN::HandoffKind::EnterRollback, 4, 3080, 0x55, local);
  assert(ok.Accept(*decoded));
  assert(ok.Complete() && !ok.ConfigMismatch());

  RN::ConfigFingerprint remote = local;
  remote[RN::ConfigField::VtimeBase] ^= 1;
  remote[RN::ConfigField::AudioRollback] = 0;
  const auto differs = RN::ConfigDifferences(local, remote);
  assert(differs.size() == 2);
  assert(differs[0] == RN::ConfigField::VtimeBase && differs[1] == RN::ConfigField::AudioRollback);
  assert(std::string_view(RN::ConfigFieldName(differs[0])) == "vtime_base");
  assert(std::string_view(RN::ConfigFieldName(differs[1])) == "audio_rollback");
  assert(RN::ConfigDigest(local) != RN::ConfigDigest(remote));

  RN::HandoffAgreement refuse(2, 0, RN::HandoffKind::EnterRollback, 4, 3080, 0x55, local);
  RN::HandoffPacket stale{1, 2, RN::HandoffKind::LeaveRollback, 3, 2000, 0x99, remote};
  assert(!refuse.Accept(stale));
  assert(!refuse.Mismatched());  // an older transition says nothing about this one
  RN::HandoffPacket wrong{1, 2, RN::HandoffKind::EnterRollback, 4, 3080, 0x66, remote};
  const auto wire = RN::DecodeHandoff(RN::EncodeHandoff(wrong));
  assert(wire);
  assert(!refuse.Accept(*wire));
  assert(refuse.Mismatched() && !refuse.Complete());
  assert(refuse.ConfigMismatch() && !refuse.Mismatch());  // config reported first
  assert(RN::ConfigDifferences(local, refuse.ConfigMismatch()->config) == differs);
  // A later matching proposal cannot clear it.
  assert(!refuse.Accept(*decoded) || refuse.Mismatched());
  assert(!refuse.Complete());

  // Field count and size are part of the format.
  std::vector<std::uint8_t> malformed = bytes;
  malformed[24] = static_cast<std::uint8_t>(RN::CONFIG_FIELDS - 1);
  assert(!RN::DecodeHandoff(malformed));
  malformed = bytes;
  malformed.push_back(0);
  assert(!RN::DecodeHandoff(malformed));
}

// A datagram from a differently configured peer in the same epoch changes
// nothing, is not a protocol violation, and is reported once with the peer.
static void TestExchangeConfigMismatch()
{
  constexpr std::uint32_t session = 0x77;
  RN::Exchange local(600, 2, 0, 1, 7, session, 0xAAAA);
  RN::Exchange other(600, 2, 1, 1, 7, session, 0xBBBB);
  local.SubmitLocal(600, {});
  other.SubmitLocal(600, ValidationPad(1, 600));
  const auto before = Fingerprint(local, 600);
  const auto packet = RN::Decode(RN::Encode(other.BuildPacket(600, 1000)));
  assert(packet && packet->config == 0xBBBB);
  assert(!local.Accept(*packet, 2000));
  assert(Fingerprint(local, 600) == before);
  assert(local.ViolationCount() == 0);
  assert(local.ConfigMismatch() && local.ConfigMismatch()->player == 1 &&
         local.ConfigMismatch()->remote == 0xBBBB);

  RN::Exchange agreeing(600, 2, 1, 1, 7, session, 0xAAAA);
  agreeing.SubmitLocal(600, {});
  Deliver(agreeing, &local, 600);
}

// The digest of a frame start computed with mispredicted remote input must
// not be compared (a sticky false desync), nor advertised during the
// re-simulation that replaces it.  Once recomputed with confirmed input the
// same frame is compared and agrees; a real difference is still caught.
static void TestDigestComparedOnlyWhenFinal()
{
  RN::Exchange a(500, 2, 0, 0, 7, 21);
  RN::Exchange b(500, 2, 1, 0, 7, 21);
  const RN::Pad pad_b = ValidationPad(1, 500);
  // State digest model: start of frame f+1 = f(inputs through f).
  const auto digest = [](const RN::Pads& pads, std::uint64_t prior) {
    std::uint64_t h = prior * 1099511628211ULL;
    for (const auto& pad : pads)
      for (const std::uint8_t byte : pad)
        h = (h ^ byte) * 1099511628211ULL;
    return h;
  };
  a.SubmitLocal(500, {});
  b.SubmitLocal(500, pad_b);
  Deliver(a, &b, 500);
  // a runs 500 and 501 predicting b's (unknown) input as neutral.
  const std::uint64_t a501 = digest(a.Assemble(500), 1);
  a.SetStateChecksum(501, a501);
  a.SubmitLocal(501, {});
  const std::uint64_t a502 = digest(a.Assemble(501), a501);
  a.SetStateChecksum(502, a502);
  a.UpdateFinalized();
  // b ran 500 with a's confirmed input and finalizes its start of 501.
  const std::uint64_t b501 = digest(b.Assemble(500), 1);
  assert(b501 != a501);
  b.SetStateChecksum(501, b501);
  b.SubmitLocal(501, pad_b);
  b.UpdateFinalized();
  Deliver(b, &a, 501);  // b's real input for 500/501 and its digest of 501
  assert(a.PendingRollback() == 500u);
  assert(!a.FirstDesync());  // a's 501 digest is speculative: not compared
  a.UpdateFinalized();
  assert(!a.FirstDesync());
  assert(a.BuildPacket(501).checksum_frame == RN::NO_FRAME);  // nothing final yet

  // Rollback to 500 and re-simulate 500 only (501 is the next unit).
  a.BeginRollback();
  a.DiscardChecksumsAfter(500);
  const std::uint64_t r501 = digest(a.Assemble(500), 1);
  assert(r501 == b501);
  a.SetStateChecksum(501, r501);
  a.UpdateFinalized();
  // The stale 502 digest from the abandoned timeline is never advertised.
  const RN::Packet out = a.BuildPacket(501);
  assert(out.checksum_frame == 501 && out.checksum == b501);
  assert(!a.FirstDesync());

  // A genuine difference between two final digests is still reported.
  RN::Exchange c(700, 2, 0, 0, 7, 22);
  RN::Exchange d(700, 2, 1, 0, 7, 22);
  c.SubmitLocal(700, {});
  d.SubmitLocal(700, {});
  Deliver(c, &d, 700);
  Deliver(d, &c, 700);
  d.SetStateChecksum(701, 0xD);
  d.UpdateFinalized();
  Deliver(d, &c, 700);  // arrives before c's own digest exists
  assert(!c.FirstDesync());
  c.SetStateChecksum(701, 0xC);
  c.UpdateFinalized();  // now final locally: compared
  assert(c.FirstDesync() == 701u);
  assert(c.FirstDesyncInfo() && c.FirstDesyncInfo()->local == 0xC &&
         c.FirstDesyncInfo()->remote == 0xD && c.FirstDesyncInfo()->player == 1);
}

// Synthetic MEM1 verifies that the Crush rule hides exactly its 18 padding
// bytes and fails closed for another mode, stale type tags or damaged roots.
// No game data is needed; meaningful score/ID/flag bytes stay observable.
static void TestStreetEventRankingPadding()
{
  constexpr std::uint32_t BASE = 0x80000000;
  constexpr std::uint32_t MODE = 0x80010000;
  constexpr std::uint32_t STATE = 0x80011000;
  constexpr std::uint32_t RECORDS = 0x80012000;
  std::vector<std::uint8_t> ram(0x400000);
  // Write big-endian synthetic roots/fields without a Dolphin dependency.
  auto write = [&](std::uint32_t address, std::uint32_t value) {
    for (std::uint32_t i = 0; i < 4; ++i)
      ram[address - BASE + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
  };
  write(0x8031BDBC, MODE);
  write(MODE + 0x1C, STATE);
  write(STATE + 0x34, RECORDS);
  for (std::uint32_t i = 0; i < 6; ++i)
    write(RECORDS + i * 0x38 + 0x34, 0x802A1F78);
  const auto ranges = RL::StreetEventRankingPadding(ram);
  assert(ranges.size() == 6);
  for (std::uint32_t i = 0; i < 6; ++i)
  {
    const std::uint32_t record = RECORDS - BASE + i * 0x38;
    assert((ranges[i] == RL::Range{record + 9, record + 12}));
    for (std::uint32_t offset = 0; offset < 0x38; ++offset)
      assert((record + offset >= ranges[i].lo && record + offset < ranges[i].hi) ==
             (offset >= 9 && offset < 12));
  }
  write(0x803EB2AC, 1);
  assert(RL::StreetEventRankingPadding(ram).empty());
  write(0x803EB2AC, 0);
  write(RECORDS + 5 * 0x38 + 0x34, 0x802A1F90);
  assert(RL::StreetEventRankingPadding(ram).empty());
  write(RECORDS + 5 * 0x38 + 0x34, 0x802A1F78);
  for (const std::uint32_t root : {0x8031BDBCu, MODE + 0x1C, STATE + 0x34})
  {
    const std::uint32_t good = root == 0x8031BDBC ? MODE : root == MODE + 0x1C ? STATE : RECORDS;
    for (const std::uint32_t bad : {0u, 0x80000000u, 0x803FFFFCu, 0xFFFFFFFCu, good + 1})
    {
      write(root, bad);
      assert(RL::StreetEventRankingPadding(ram).empty());
    }
    write(root, good);
  }
  assert(RL::StreetEventRankingPadding({}).empty());
  assert(RL::StreetEventRankingPadding(std::span<const std::uint8_t>(ram).first(0x3100)).empty());
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
  assert(!RL::PausedFrame(0) && RL::PausedFrame(1) && RL::PausedFrame(2));
  // fix/modal: the poll feeds the game's pad buffer only outside a frame-mode
  // session (menus/loading), never while RbStep owns it.
  assert(RL::PollFeedsPadBuffer(false, true, false, false));
  assert(!RL::PollFeedsPadBuffer(false, true, false, true));
  assert(!RL::PollFeedsPadBuffer(true, true, false, false));
  assert(!RL::PollFeedsPadBuffer(false, false, false, false));
  assert(!RL::PollFeedsPadBuffer(false, true, true, false));
  const RL::SongTimeline song = RL::ScheduleSong(100, 380, 10800);
  assert(song.start == 480 && song.end == 11280);
  const RL::SongTimeline held = RL::HoldSongOneFrame(song);
  assert(held.start == 481 && held.end == 11281);
  const RL::SongTimeline no_start = RL::HoldSongOneFrame({0, 7});
  assert(no_start.start == 0 && no_start.end == 8);

  std::map<const void*, std::uint32_t> synced{{pages.data(), 7}};
  std::map<const void*, std::uint64_t> sequences{{pages.data(), 9}};
  RL::ClearSessionSlotMetadata(&synced, &sequences);
  assert(synced.empty() && sequences.empty());
  assert(RL::PageNeedsCopy(7, std::nullopt));
}

// Whole-session telemetry must preserve the legacy wire hash with collection
// on or off, while a changed region must be visible in the diagnostic list.
static void TestTelemetryLockstepEvidence()
{
  namespace TL = NSRTelemetry::Logic;
  std::array<unsigned char, 96> ram{};
  for (size_t i = 0; i < ram.size(); ++i)
    ram[i] = static_cast<unsigned char>(i);
  const auto skip = [](std::uint32_t off) { return off >= 32 && off < 48; };
  std::uint64_t original = 1469598103934665603ULL;
  for (std::uint32_t off = 0; off + 8 <= ram.size(); off += 8)
  {
    if (skip(off))
      continue;
    std::uint64_t word;
    std::memcpy(&word, ram.data() + off, 8);
    original = (original ^ word) * 1099511628211ULL;
  }
  std::vector<std::uint64_t> regions;
  assert(TL::LockstepMem1Hash(ram.data(), 96, skip) == original);
  assert(TL::LockstepMem1Hash(ram.data(), 96, skip, &regions, 32) == original);
  const auto before = TL::FoldUnits(regions, 1);
  assert(before.size() == 3);
  ram[80] ^= 1;
  assert(TL::LockstepMem1Hash(ram.data(), 96, skip, &regions, 32) != original);
  const auto after = TL::FoldUnits(regions, 1);
  assert(before[0] == after[0] && before[1] == after[1] && before[2] != after[2]);
  const auto changed = regions;
  ram[40] ^= 1;  // An excluded word changes neither wire nor region hashes.
  TL::LockstepMem1Hash(ram.data(), 96, skip, &regions, 32);
  assert(regions == changed);
  assert(TL::FoldUnits({}, 16).empty());
  assert(TL::FoldUnits(regions, 2).size() == 2);  // Keep the short final unit.
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

// Synthetic memory only: packed fields cross bytes, signed counters preserve
// their sign, deleted rows are omitted, and hostile descriptors fail closed.
static void TestGameStatsMemoryReader()
{
  namespace GS = NSRGameStats;
  std::vector<std::uint8_t> ram(0x400000), vmem(0x1000);
  const auto put = [](std::vector<std::uint8_t>& bytes, std::uint32_t offset,
                      std::uint32_t value, unsigned size = 4) {
    for (unsigned i = 0; i < size; ++i)
      bytes[offset + i] = static_cast<std::uint8_t>(value >> ((size - i - 1) * 8));
  };
  const auto tag = [](std::vector<std::uint8_t>& bytes, std::uint32_t offset,
                      std::string_view name) {
    std::copy(name.begin(), name.end(), bytes.begin() + offset);
  };
  put(ram, 0x3ebe88, 0x7e000100);
  put(vmem, 0x104, 0x7e000200);
  tag(vmem, 0x108, "EMAG");
  tag(vmem, 0x21c, "TEST");
  put(vmem, 0x214, 0x7e000400);
  put(vmem, 0x220, 8);
  put(vmem, 0x224, 63);
  put(vmem, 0x240, 3, 2);
  put(vmem, 0x242, 3, 2);
  put(vmem, 0x244, 3, 2);
  put(vmem, 0x24a, 3, 1);
  put(vmem, 0x24c, 3);
  put(vmem, 0x250, 3);
  tag(vmem, 0x254, "DIGP");
  put(vmem, 0x258, 8);
  put(vmem, 0x25c, 2);
  put(vmem, 0x260, 11);
  tag(vmem, 0x264, "SIGN");
  put(vmem, 0x268, 5);
  put(vmem, 0x26c, 0);
  put(vmem, 0x270, 16);
  tag(vmem, 0x274, "NAME");
  put(vmem, 0x278, 32);
  // 8-bit value 0x91 starting at bit 3; 5-bit -4 starting at bit 11.
  vmem[0x400] = 0x12;
  vmem[0x401] = 0x3c;
  tag(vmem, 0x402, "Name");
  vmem[0x40f] = 1;  // deleted row 1
  tag(vmem, 0x412, "Zero");
  const GS::Memory memory{ram, vmem};
  const auto rows = GS::ReadTable(memory, "EMAG", "TEST");
  assert(rows && rows->size() == 2);
  assert(GS::Integer((*rows)[0], "DIGP") == 0x91);
  assert(GS::Integer((*rows)[0], "SIGN") == -4);
  assert(GS::String((*rows)[0], "NAME") == "Name");
  assert(GS::Integer((*rows)[1], "DIGP") == 0);
  assert(GS::UniqueIntegerKeys(*rows, "DIGP"));
  assert(!GS::UniqueIntegerKeys(*rows, "NAME"));
  auto duplicate_rows = *rows;
  duplicate_rows.push_back((*rows)[0]);
  assert(!GS::UniqueIntegerKeys(duplicate_rows, "DIGP"));
  assert(!GS::Integer((*rows)[0], "NAME"));
  assert(!GS::String((*rows)[0], "DIGP"));
  assert(!memory.Bytes(0xfffffffc, 8));
  assert(!memory.Bytes(0x7dffffff, 4));
  assert(!memory.Bytes(0x80000000, 0xffffffff));
  assert(!GS::ReadTable(memory, "NONE", "TEST"));
  const std::array<std::string_view, 1> missing{"NONE"};
  assert(!GS::ReadTable(memory, "EMAG", "TEST", missing));
  const std::array<std::string_view, 1> wrong_type{"NAME"};
  assert(!GS::ReadTable(memory, "EMAG", "TEST", wrong_type));
  put(vmem, 0x270, 8);  // overlapping fields
  assert(!GS::ReadTable(memory, "EMAG", "TEST"));
  put(vmem, 0x270, 16);
  put(vmem, 0x244, 4, 2);  // high-water exceeds allocation
  assert(!GS::ReadTable(memory, "EMAG", "TEST"));
  put(vmem, 0x244, 3, 2);
  put(vmem, 0x214, 0x7e000ffc);  // record buffer overrun
  assert(!GS::ReadTable(memory, "EMAG", "TEST"));
  put(vmem, 0x100, 0x7e000100);  // cycle must terminate
  assert(!GS::FindTable(memory, "NONE", "TEST"));
  assert(ram[0x3ebe88] == 0x7e);  // all reads preserve synthetic guest memory
}

// Packed writes must update every roster copy atomically, preserve neighbors,
// reject stale/missing/duplicate edits and never write a deleted physical row.
static void TestRuntimeContent()
{
  namespace C = NSRContent;
  std::vector<std::uint8_t> ram(0x400000), vmem(0x1000);
  const auto put = [&](std::uint32_t offset, std::uint32_t value, unsigned size = 4) {
    for (unsigned i = 0; i < size; ++i)
      vmem[offset + i] = static_cast<std::uint8_t>(value >> ((size - i - 1) * 8));
  };
  const auto tag = [&](std::uint32_t offset, std::string_view name) {
    std::copy(name.begin(), name.end(), vmem.begin() + offset);
  };
  ram[0x3ebe88] = 0x7e;
  ram[0x3ebe8a] = 1;
  put(0x104, 0x7e000200);
  tag(0x108, "TATS");
  tag(0x21c, "YALP");
  put(0x214, 0x7e000400);
  put(0x220, 16);
  put(0x224, 127);
  put(0x240, 3, 2);
  put(0x242, 3, 2);
  put(0x244, 3, 2);
  put(0x24a, 3, 1);
  for (unsigned i = 0; i < 3; ++i)
  {
    put(0x24c + i * 16, i == 2 ? 3 : 0);
    put(0x250 + i * 16, i == 0 ? 0 : (i == 1 ? 32 : 67));
    put(0x258 + i * 16, i == 2 ? 7 : 32);
  }
  tag(0x254, "ANFP");
  tag(0x264, "ANLP");
  tag(0x274, "DPSP");
  for (unsigned i = 0; i < 3; ++i)
  {
    tag(0x400 + i * 16, "Ada");
    tag(0x404 + i * 16, "Test");
    vmem[0x408 + i * 16] = 0xb1;
    vmem[0x409 + i * 16] = 0x9a;  // rating 70, surrounding bits nonzero
  }
  vmem[0x41f] = 1;  // middle copy is deleted
  const NSRGameStats::Memory memory{ram, vmem};
  const auto original = vmem;
  const std::vector<C::Edit> edits{{" ada ", "TEST", "PSPD", 17, {70}}};
  const auto writes = C::Plan(memory, edits);
  assert(vmem == original && writes && writes->size() == 4);
  for (const auto& [address, value] : *writes)
    vmem[address - 0x7e000000] = value;
  assert(vmem[0x408] == 0xa4 && vmem[0x409] == 0x5a);
  assert(vmem[0x428] == 0xa4 && vmem[0x429] == 0x5a);
  assert(vmem[0x418] == original[0x418] && vmem[0x419] == original[0x419]);
  const auto rejects = [&](std::vector<C::Edit> bad) {
    vmem = original;
    assert(!C::Plan(memory, bad));
    assert(vmem == original);
  };
  rejects({{"Ada", "Test", "PSPD", 17, {71}}});
  rejects({{"Absent", "Test", "PSPD", 17, {70}}});
  rejects({edits[0], edits[0]});
  rejects({{"Ada", "Test", "PXYZ", 17, {70}}});
  rejects({{"Ada", "Test", "PSPD", 128, {70}}});
  rejects({edits[0], {"Absent", "Test", "PSPD", 17, {70}}});
  vmem = original;
  put(0x214, 0x7e000fff);
  assert(!C::Plan(memory, edits));
  // Each content identity field participates in wire agreement and diagnostics.
  NSRNetplay::RollbackNet::ConfigFingerprint a, b;
  for (auto field : {NSRNetplay::RollbackNet::ConfigField::ContentSha0,
                     NSRNetplay::RollbackNet::ConfigField::ContentSha3,
                     NSRNetplay::RollbackNet::ConfigField::ContentVersion,
                     NSRNetplay::RollbackNet::ConfigField::ContentPolicy})
  {
    b = a;
    b[field] = 1;
    assert(NSRNetplay::RollbackNet::ConfigDigest(a) != NSRNetplay::RollbackNet::ConfigDigest(b));
    assert(NSRNetplay::RollbackNet::ConfigDifferences(a, b).size() == 1);
  }
}

// Identity validation is independent of sockets; attribution to the established
// connection remains the adapter's existing PacketSenderMatchesPeer contract.
static void TestGameStatsIdentity()
{
  namespace GS = NSRGameStats;
  const GS::PeerIdentity identity{0, 2, 1700000000, "Synthetic \xc3\xa9"};
  const auto bytes = GS::EncodeIdentity(identity);
  const auto decoded = GS::DecodeIdentity(bytes);
  assert(decoded && decoded->pad == 0 && decoded->players == 2);
  assert(decoded->started_at == identity.started_at && decoded->name == identity.name);
  for (std::size_t length = 0; length < bytes.size(); ++length)
    assert(!GS::DecodeIdentity(std::span(bytes).first(length)));
  auto bad = bytes;
  bad.push_back(0);
  assert(!GS::DecodeIdentity(bad));
  bad = bytes;
  bad[5] = 2;
  assert(!GS::DecodeIdentity(bad));
  bad[5] = 1;  // a leaf cannot set the shared timestamp
  assert(!GS::DecodeIdentity(bad));
  assert(!GS::ValidIdentityName("bad\nname"));
  assert(!GS::ValidIdentityName("\xc0\xaf"));
  assert(!GS::ValidIdentityName("\xed\xa0\x80"));
  assert(!GS::ValidIdentityName("\xf4\x90\x80\x80"));
  assert(!GS::ValidIdentityName(std::string(65, 'x')));
  assert(GS::EncodeIdentity({1, 2, 0, "Second"}).size() == 18);
  const auto anchor = GS::EncodeCaptureAnchor({2, 7, 0x12345678});
  const auto decoded_anchor = GS::DecodeCaptureAnchor(anchor);
  assert(decoded_anchor && decoded_anchor->players == 2 && decoded_anchor->ordinal == 7 &&
         decoded_anchor->frame == 0x12345678);
  for (std::size_t length = 0; length < anchor.size(); ++length)
    assert(!GS::DecodeCaptureAnchor(std::span(anchor).first(length)));
  auto bad_anchor = anchor;
  bad_anchor[6] = 1;
  assert(!GS::DecodeCaptureAnchor(bad_anchor));
  bad_anchor = anchor;
  bad_anchor[4] = 2;
  assert(!GS::DecodeCaptureAnchor(bad_anchor));
  assert(GS::EncodeCaptureAnchor({2, 0, 7}).empty());
}

// The boot protocol must reject malformed messages and report actual card or
// emulation-setting differences, including missing fields from older builds.
static void TestBootAgreementWire()
{
  using namespace NSRBootSync;
  const BootMessage local{BootKind::Settings, 0, 3,
      {{"Dolphin.Core.CPUThread", "False"}, {"MemoryCard.A.contents", "empty-v1"}}};
  const auto bytes = EncodeBoot(local);
  const auto decoded = DecodeBoot(bytes);
  assert(decoded && decoded->fields == local.fields);
  for (std::size_t n = 0; n < 8; ++n)
    assert(!DecodeBoot(std::span(bytes).first(n)));
  auto bad = bytes;
  bad[4] = 2;
  assert(!DecodeBoot(bad));
  bad = bytes;
  bad[6] = 3;
  assert(!DecodeBoot(bad));
  bad = bytes;
  bad.pop_back();
  assert(!DecodeBoot(bad));
  bad = bytes;
  bad.insert(bad.end(), bytes.begin() + 8, bytes.end());
  assert(!DecodeBoot(bad));
  bad.assign(MAX_BOOT_PACKET + 1, 0);
  assert(!DecodeBoot(bad));
  auto other = local.fields;
  other["MemoryCard.A.contents"] = "different-save";
  assert(BootDifferences(local.fields, other) ==
         std::vector<std::string>{"MemoryCard.A.contents"});
  other = local.fields;
  other.erase("Dolphin.Core.CPUThread");
  assert(BootDifferences(local.fields, other) ==
         std::vector<std::string>{"Dolphin.Core.CPUThread"});
  assert(DecodeBoot(EncodeBoot({BootKind::Ready, 1, 3, {}})));
  assert(DecodeBoot(EncodeBoot({BootKind::Go, 0, 3, {}})));
  assert(!DecodeBoot(EncodeBoot({BootKind::Ready, 1, 3, local.fields})));
  assert(!DecodeBoot(EncodeBoot({BootKind::Settings, 1, 3, {}})));
}

// Pickup draft recorder (NSRDraftLog.h): synthetic GN7E69 rev-0 draft table and
// pool exercise the live reader, ordered pick capture, gaps and restarts.
static void TestDraftLog()
{
  namespace GS = NSRGameStats;
  std::vector<std::uint8_t> ram(0x500000), vmem(0x100);
  const auto put = [&ram](std::uint32_t address, std::uint32_t value, unsigned size = 4) {
    for (unsigned i = 0; i < size; ++i)
      ram[(address & 0x1ffffff) + i] = static_cast<std::uint8_t>(value >> ((size - i - 1) * 8));
  };
  const std::uint32_t table = 0x80400000, counts = 0x80400100, handles = 0x80400110,
                      pool = 0x80401000, pool_size = 20;
  put(GS::DRAFT_MODE_ADDRESS, GS::DRAFT_MODE_PICKUP);
  put(GS::DRAFT_TABLE_ADDRESS, table);
  put(table + 4, counts);
  put(table + 8, handles);
  put(table + 0x14, 2, 1);
  put(table + 0x16, 7, 1);
  put(handles, 1, 1);  // drafter 0 uses registry slot 1, drafter 1 slot 0
  put(handles + 1, 0, 1);
  put(GS::PAD_MAP_ADDRESS, 0);
  put(GS::PAD_MAP_ADDRESS + 4, 1);
  put(GS::DRAFT_POOL_ADDRESS, pool);
  put(GS::DRAFT_POOL_COUNT_ADDRESS, pool_size);
  for (std::uint32_t i = 0; i < pool_size; ++i)
  {
    put(pool + i * 0x18, 100 + i);
    put(pool + i * 0x18 + 4, 0xffffffff);
  }
  std::array<std::uint32_t, 2> team_count{}, drafter_count{};
  // Commit like fn_8001AA8C: team pick index, team and drafter into the record.
  const auto commit = [&](std::uint32_t index, std::uint32_t team, std::uint32_t drafter) {
    put(pool + index * 0x18 + 4, team_count[team]);
    put(pool + index * 0x18 + 0x14, team, 1);
    put(pool + index * 0x18 + 0x15, drafter, 1);
    ++team_count[team];
    ++drafter_count[drafter];
    put(table + 0x0c + team, team_count[team], 1);
    put(counts + drafter, drafter_count[drafter], 1);
  };
  const GS::Memory memory{ram, vmem};
  GS::DraftLog log;
  auto view = GS::ReadDraft(memory);
  assert(view && view->total == 0 && view->picked.empty());
  log.Observe(*view);
  // Snake from team 1: B, A, A, B, B, ... with drafter == team here.
  const std::array<std::uint32_t, 14> teams{1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0};
  for (std::uint32_t n = 0; n < 14; ++n)
  {
    commit(19 - n, teams[n], teams[n]);
    view = GS::ReadDraft(memory);
    assert(view && view->total == n + 1);
    log.Observe(*view);
    log.Observe(*view);  // an unchanged frame adds nothing
  }
  assert(log.Complete() && log.Picks().size() == 14);
  for (std::uint32_t n = 0; n < 14; ++n)
  {
    const auto& pick = log.Picks()[n];
    assert(pick.pick == n + 1 && pick.pgid == 119 - n && pick.team == teams[n]);
    assert(pick.controller == (teams[n] == 0 ? 1 : 0));
    assert(pick.pad == static_cast<std::int32_t>(teams[n] == 0 ? 1 : 0));
  }
  assert(log.Picks()[0].round == 1 && log.Picks()[2].round == 2 && log.Picks()[13].round == 7);

  // Torn/stale states are rejected: other mode, count mismatch, duplicate ids.
  put(GS::DRAFT_MODE_ADDRESS, 3);
  assert(!GS::ReadDraft(memory));
  put(GS::DRAFT_MODE_ADDRESS, GS::DRAFT_MODE_PICKUP);
  put(table + 0x0c, 6, 1);
  assert(!GS::ReadDraft(memory));
  put(table + 0x0c, 7, 1);
  put(pool + 6 * 0x18, 119);  // pool id 119 now twice
  assert(!GS::ReadDraft(memory));
  put(pool + 6 * 0x18, 106);
  put(GS::DRAFT_TABLE_ADDRESS, 0x81fffff0);  // outside MEM1 view
  assert(!GS::ReadDraft(memory));
  put(GS::DRAFT_TABLE_ADDRESS, table);

  // A new draft (zero picks) restarts; two picks in one frame are a gap.
  for (std::uint32_t i = 0; i < pool_size; ++i)
    put(pool + i * 0x18 + 4, 0xffffffff);
  team_count = {};
  drafter_count = {};
  put(table + 0x0c, 0, 2);
  put(counts, 0, 2);
  log.Observe(*GS::ReadDraft(memory));
  assert(log.Picks().empty() && !log.Complete());
  commit(0, 0, 0);
  commit(1, 1, 1);
  log.Observe(*GS::ReadDraft(memory));
  assert(log.Picks().size() == 2 && log.Picks()[0].pick == 0 && !log.Complete());
}

// USB GameCube adapter as a standard controller's input (NSRPadSource.h): the
// guest-visible origin/error bits never pass, rest positions recentre to the
// standard controller's fixed origin, and the port is always connected.
static void TestPadSourceAdapter()
{
  using namespace NSRPadSource;
  // Netplay ports never read as unplugged; a connected pad passes untouched;
  // offline keeps the unplugged pad (the game re-probes when it appears).
  GCPadStatus unplugged{};  // what GCPad::GetInput returns without its device
  unplugged.isConnected = false;
  const GCPadStatus held = NetplayPad(unplugged, true);
  assert(held.isConnected && held.button == 0 && held.stickX == 0x80 && held.stickY == 0x80 &&
         held.substickX == 0x80 && held.substickY == 0x80 && held.triggerLeft == 0);
  assert(!NetplayPad(unplugged, false).isConnected);
  GCPadStatus live = NeutralPad();
  live.button = PAD_BUTTON_START;
  live.stickX = 0xf0;
  const GCPadStatus passed = NetplayPad(live, true);
  assert(passed.button == PAD_BUTTON_START && passed.stickX == 0xf0 && passed.isConnected);
  assert(ParseChannel("0") == 0 && ParseChannel("3") == 3);
  assert(!ParseChannel("") && !ParseChannel("4") && !ParseChannel("off") && !ParseChannel("01"));
  assert(PickChannel(2, {false, true, true, false}) == 2);
  assert(PickChannel(0, {false, false, false, true}) == 3);
  assert(!PickChannel(0, {false, false, false, false}));

  GCPadStatus origin{};
  origin.button = PAD_GET_ORIGIN;
  origin.stickX = 0x7a;
  origin.stickY = 0x84;
  origin.substickX = 0x80;
  origin.substickY = 0x7e;
  origin.triggerLeft = 0x20;
  origin.triggerRight = 0x18;
  GCPadStatus raw = origin;  // first report after plugging in: at rest + GET_ORIGIN
  raw.button = PAD_GET_ORIGIN | PAD_ERR_STATUS | PAD_USE_ORIGIN | PAD_BUTTON_A | PAD_TRIGGER_Z;
  const GCPadStatus rest = StandardControllerPad(raw, origin);
  assert(rest.button == (PAD_BUTTON_A | PAD_TRIGGER_Z));
  assert(rest.stickX == 0x80 && rest.stickY == 0x80 && rest.substickX == 0x80 &&
         rest.substickY == 0x80);
  assert(rest.triggerLeft == 0 && rest.triggerRight == 0);
  assert(rest.analogA == 0xff && rest.analogB == 0 && rest.isConnected);

  raw.button = PAD_BUTTON_B;
  raw.stickX = 0xff;   // full right from a left-of-centre rest: clamps, never wraps
  raw.stickY = 0x00;
  raw.triggerLeft = 0x10;  // below rest
  raw.triggerRight = 0xf0;
  raw.isConnected = false;
  const GCPadStatus moved = StandardControllerPad(raw, origin);
  assert(moved.stickX == 0xff && moved.stickY == 0x00);
  assert(moved.triggerLeft == 0 && moved.triggerRight == 0xd8);
  assert(moved.analogA == 0 && moved.analogB == 0xff && moved.isConnected);

  const GCPadStatus neutral = NeutralPad();
  assert(neutral.button == 0 && neutral.stickX == 0x80 && neutral.substickY == 0x80 &&
         neutral.triggerLeft == 0 && neutral.isConnected);
}

// Navigation must ignore wall-clock gaps and duplicate polls, wait on screens,
// release buttons between edges, fail closed and stay finished on menu return.
static void TestQuickstartNavigation()
{
  using namespace NSRQuickstart;
  assert(Parse("") == Mode::Manual && Parse("manual") == Mode::Manual);
  assert(!Parse("quick-play;other") && !Parse("unknown"));
  Navigator manual;
  assert(!manual.Poll(0, 3));
  for (const auto mode : {Mode::QuickPlay, Mode::Pickup})
  {
    Navigator nav(mode);
    std::uint32_t frame = 1;
    const std::array<std::uint32_t, 8> screens{3, 0x10003, 0x10003, 0x10003,
        1, 1, 1, 0x11};
    for (std::uint32_t step = 0; step < screens.size(); ++step)
    {
      for (int i = 0; i < 75; ++i)
      {
        const auto result = nav.Poll(frame, 0xffffffff);
        assert(result && (*result == std::array<std::uint16_t, 4>{}));
        assert(nav.StepIndex() == step);
        ++frame;
      }
      // A transient controller screen must not finish the macro. Stability
      // restarts after an unrelated screen; every waiting pad remains neutral.
      if (step == 7)
      {
        for (std::uint32_t elapsed = 0; elapsed < 60; ++elapsed)
        {
          const auto result = nav.Poll(frame++, screens[step]);
          assert(nav.Active() && result && (*result == std::array<std::uint16_t, 4>{}));
        }
        nav.Poll(frame++, 0xffffffff);
        assert(nav.Active());
      }
      const auto begin = frame;
      for (std::uint32_t elapsed = 0; elapsed <= (step == 7 ? 60U : 120U); ++elapsed)
      {
        // A different next screen acknowledges START without reissuing it.
        const auto screen = step == 0 && elapsed >= 70 ? 0x10003U : screens[step];
        const auto result = nav.Poll(frame, screen);
        assert(nav.Poll(frame, screen) == result);
        assert(result);
        if (step == 7)
          assert(nav.Active() == (elapsed < 60));
        if (step == 7 || elapsed < 60 || elapsed >= 66)
          assert((*result == std::array<std::uint16_t, 4>{}));
        else
        {
          const std::array<std::uint16_t, 7> presses{0x1000, 4, 4, 0x100,
              static_cast<std::uint16_t>(mode == Mode::Pickup ? 0 : 0x200),
              static_cast<std::uint16_t>(mode == Mode::Pickup ? 8 : 1), 0x100};
          assert((*result == std::array<std::uint16_t, 4>{presses[step], 0, 0, 0}));
        }
        ++frame;
      }
      assert(frame > begin);
    }
    assert(!nav.Active() && !nav.Failed());
    assert(!nav.Poll(frame, 0x11));  // humans own the still-visible controller screen
    assert(!nav.Poll(frame + 100, 3));
  }
  Navigator slow(Mode::QuickPlay);
  for (std::uint32_t frame = 1; frame < 500; ++frame) slow.Poll(frame, 3);
  assert(slow.StepIndex() == 0);  // title hasn't accepted START yet
  Navigator stalled(Mode::Pickup);
  stalled.Poll(1, 0xffffffff);
  stalled.Poll(18002, 0xffffffff);
  assert(stalled.Failed());
}

// Runtime draft plans use synthetic MEM1 only: validate the full fixed recipe
// before writes, reject changed native instructions and tolerate exact reloads.
static void TestRuntimeDraft()
{
  std::vector<std::uint8_t> ram(0x420000);
  std::copy_n("GN7E69", 6, ram.begin());
  const auto put = [&](std::uint32_t address, std::uint32_t value) {
    for (std::uint32_t i = 0; i < 4; ++i)
      ram[address - 0x80000000 + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
  };
  put(0x8001abac, 0x816d9768);
  put(0x80018fc0, 0x48063bad);
  const NSRGameStats::Memory memory{ram, {}};
  std::string error;
  for (std::uint32_t order = 0; order < 3; ++order)
    for (std::uint32_t first = 0; first < 3; ++first)
    {
      const auto before = ram;
      const auto plan = NSRContent::PlanDraft(memory, order, first, &error);
      assert(plan && ram == before);
    }
  auto plan = NSRContent::PlanDraft(memory, 1, 2, &error);
  assert(plan && !plan->empty());
  for (const auto& [address, value] : *plan)
    put(address, value);
  assert(NSRContent::PlanDraft(memory, 1, 2, &error) == plan);
  assert(!NSRContent::PlanDraft(memory, 2, 2, &error));
  put(0x80411604, 0xdeadbeef);
  assert(!NSRContent::PlanDraft(memory, 1, 2, &error));
  assert(!NSRContent::PlanDraft(memory, 3, 0, &error));
  ram[7] = 1;
  assert(!NSRContent::PlanDraft(memory, 0, 0, &error));
}

int main()
{
  TestPadSourceAdapter();
  TestDraftLog();
  TestQuickstartNavigation();
  TestBootAgreementWire();
  TestRuntimeContent();
  TestRuntimeDraft();
  TestGameStatsMemoryReader();
  TestGameStatsIdentity();
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
  TestHandoffConfigAgreement();
  TestExchangeConfigMismatch();
  TestDigestComparedOnlyWhenFinal();
  TestPureRollbackHelpers();
  TestLifecycleConfirmFrames();
  TestMispredictedPlayers();
  TestStreetEventRankingPadding();
  TestWindowUndoLog();
  TestTelemetryLockstepEvidence();
  TestTelemetryFinalFlushIsBounded();
  std::puts("NSR rollback exchange tests passed");
  return 0;
}
