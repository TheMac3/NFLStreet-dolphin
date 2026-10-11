// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// In-process two-peer test for the socket-free rollback exchange core.  It
// covers wire validation, delayed delivery and correction, prediction-window
// stalling, acknowledgement-driven history, and confirmed-state desyncs.

#include "Core/NSR/NSRRollbackNetplay.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
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

// Sender frame reports drive the coarse frame-advantage wait, and reordered
// unsequenced packets cannot make a peer appear to move backward.
static void TestFrameAdvantage()
{
  RN::Exchange a(600, 2, 0, 0, 7, 12);
  RN::Exchange b(600, 2, 1, 0, 7, 12);
  assert(a.Accept(b.BuildPacket(605)));
  assert(a.FrameAdvantage(608) == 3);
  assert(a.Accept(b.BuildPacket(604)));
  assert(a.FrameAdvantage(606) == 1);
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
  TestPredictionAndCorrection();
  TestPredictionWindowAndAcks();
  TestLongLossRecovery();
  TestFrameAdvantage();
  TestChecksumMismatch();
  TestDelayedChecksumFinalization();
  TestWireValidation();
  std::puts("NSR rollback exchange tests passed");
  return 0;
}
