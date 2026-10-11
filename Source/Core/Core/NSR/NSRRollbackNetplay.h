// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Frame-keyed rollback input exchange and match-handoff agreement state,
// independent of Dolphin and ENet.  The emulator adapter supplies local
// samples and decoded datagrams; this component owns prediction,
// acknowledgement, retransmit history, confirmed-state checksums, and
// transition proposals.  Keeping both state machines free of sockets lets
// peers be exercised deterministically in one process.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace NSRNetplay::RollbackNet
{
constexpr std::uint32_t MAGIC = 0x4E535242;  // 'NSRB'
constexpr std::uint32_t HANDOFF_MAGIC = 0x4E535248;  // 'NSRH'
constexpr std::uint8_t VERSION = 4;
constexpr std::uint8_t HANDOFF_VERSION = 1;
constexpr std::size_t MAX_PLAYERS = 4;
constexpr std::size_t PAD_SIZE = 12;
constexpr std::size_t MAX_HISTORY = 64;
constexpr std::uint32_t NO_FRAME = UINT32_MAX;
// Extra frames of remote input/checksum lookahead tolerated beyond what this
// peer's own delay and prediction settings permit.  Delay and prediction are
// per-process settings (the adapter clamps them to <= 8 and <= 60), so a
// legitimate peer may be configured more permissively than we are.  Two
// datagram windows cover that whole clamp range while still bounding how much
// attacker-chosen future input a single sender can make us retain.
constexpr std::uint32_t FUTURE_INPUT_SLACK = static_cast<std::uint32_t>(2 * MAX_HISTORY);

using Pad = std::array<std::uint8_t, PAD_SIZE>;
using Pads = std::array<Pad, MAX_PLAYERS>;

// The decoded form of an NSRB datagram.  Encode/Decode use explicit little
// endian fields so recordings and loopback tests do not inherit host ABI
// padding or alignment.
struct Packet
{
  std::uint8_t sender = 0;
  std::uint8_t players = 0;
  std::uint8_t flags = 0;
  std::uint32_t session = 0;
  std::uint32_t sender_frame = 0;
  std::uint32_t send_us = 0;
  // Each peer reflects the newest timestamp seen from every other peer and
  // how long it held that timestamp.  The originator can therefore measure
  // an end-to-end RTT without synchronized host clocks, including a star
  // relay hop.  Unsigned subtraction deliberately handles the 32-bit clock
  // wrapping about every 71 minutes.
  std::array<std::uint32_t, MAX_PLAYERS> echo_us{};
  std::array<std::uint32_t, MAX_PLAYERS> echo_age_us{};
  std::array<std::uint32_t, MAX_PLAYERS> ack{NO_FRAME, NO_FRAME, NO_FRAME, NO_FRAME};
  std::uint32_t checksum_frame = NO_FRAME;
  std::uint64_t checksum = 0;
  std::uint32_t first_frame = 0;
  std::vector<Pad> pads;
};

// Match transitions use a separate fixed-size message because the entry
// barrier exists before an NSRB rollback epoch does.  The absolute frame is
// assigned by the confirmed lockstep timeline; the semantic hash proves that
// every peer reached the same deterministic game state at that boundary.
enum class HandoffKind : std::uint8_t
{
  EnterRollback = 1,
  LeaveRollback = 2,
};

struct HandoffPacket
{
  std::uint8_t sender = 0;
  std::uint8_t players = 0;
  HandoffKind kind = HandoffKind::EnterRollback;
  std::uint32_t transition = 0;
  std::uint32_t frame = 0;
  std::uint64_t semantic_hash = 0;
};

std::vector<std::uint8_t> EncodeHandoff(const HandoffPacket& packet);
std::optional<HandoffPacket> DecodeHandoff(std::span<const std::uint8_t> bytes);

// HandoffAgreement is deliberately transport-free.  Each peer proposes the
// same kind/frame/hash tuple and accepts duplicate, reordered announcements;
// the first conflicting tuple is sticky so the adapter can abort loudly.
class HandoffAgreement
{
public:
  HandoffAgreement(std::uint8_t players, std::uint8_t me, HandoffKind kind,
                   std::uint32_t transition, std::uint32_t frame,
                   std::uint64_t semantic_hash);

  bool Accept(const HandoffPacket& packet);
  bool Complete() const;
  bool Mismatched() const { return m_mismatch.has_value(); }
  const HandoffPacket& Local() const { return m_local; }
  const std::optional<HandoffPacket>& Mismatch() const { return m_mismatch; }

private:
  std::uint8_t m_players;
  std::uint8_t m_me;
  HandoffPacket m_local;
  std::array<bool, MAX_PLAYERS> m_seen{};
  std::optional<HandoffPacket> m_mismatch;
};

std::vector<std::uint8_t> Encode(const Packet& packet);
std::optional<Packet> Decode(std::span<const std::uint8_t> bytes);

// A well-formed datagram for this epoch whose contents no honest peer can
// produce.  These are distinct from stale or foreign packets (wrong session,
// player count, or self-addressed), which unsequenced delivery makes normal
// and which Accept therefore rejects without recording a violation.
enum class ViolationKind : std::uint8_t
{
  // The sender acknowledged local input we have not produced yet (or a frame
  // before the epoch start).  Honoring it would stop retransmission.
  ImpossibleAck = 1,
  // The sender restated an already-confirmed frame with different input.
  // Local samples are immutable once submitted, so this is a cheat or a
  // corrupted peer, and silently keeping either value would hide a desync.
  ConflictingInput = 2,
  // Input lies beyond FutureInputLimit(); see FUTURE_INPUT_SLACK.
  InputOutOfWindow = 3,
  // A finalized checksum frame lies beyond FutureInputLimit().
  ChecksumOutOfWindow = 4,
};

struct ProtocolViolation
{
  std::uint8_t player = 0;
  ViolationKind kind = ViolationKind::ImpossibleAck;
  std::uint32_t frame = NO_FRAME;
};

// Exchange is CPU-thread-owned.  A transport may deliver packets in any
// order and may duplicate them; insertion and acknowledgements are
// idempotent.  Assemble records exactly what a frame ran with so a later
// confirmed input can identify the earliest wrong prediction.
//
// Accept validates a whole datagram before applying any of it: a packet that
// carries an impossible acknowledgement, a conflicting restatement of a
// confirmed input, or input/checksums outside the plausible window returns
// false, leaves acknowledgement, input, timing, and checksum state untouched,
// and is recorded in ViolationCount()/FirstViolation() for the adapter.
class Exchange
{
public:
  Exchange(std::uint32_t start, std::uint8_t players, std::uint8_t me,
           std::uint32_t delay, std::uint32_t max_prediction, std::uint32_t session);

  void SubmitLocal(std::uint32_t live_frame, Pad pad);
  Packet BuildPacket(std::uint32_t live_frame, std::uint32_t send_us = 0) const;
  bool Accept(const Packet& packet, std::uint32_t receive_us = 0);
  Pads Assemble(std::uint32_t frame);

  bool ShouldStall(std::uint32_t frame) const;
  std::uint32_t ConfirmedThrough() const;
  std::optional<std::uint32_t> PendingRollback() const;
  void BeginRollback();

  void SetStateChecksum(std::uint32_t frame, std::uint64_t checksum);
  void UpdateFinalized();
  void TrimBefore(std::uint32_t frame);
  std::optional<std::uint32_t> FirstDesync() const;
  std::optional<std::uint32_t> OldestUnackedLocal() const;
  // Highest remote input or checksum frame Accept will store.  A peer's
  // ShouldStall keeps its live frame within max_prediction of its contiguous
  // view of our input (plus one, because the adapter submits before it
  // checks the stall), that view can never pass our own contiguous local
  // prefix, and its samples land delay frames later.  Our own delay and
  // prediction plus FUTURE_INPUT_SLACK therefore bound every honest sender,
  // including senders whose datagrams the star host relays unchanged.
  std::uint32_t FutureInputLimit() const;
  std::uint32_t ViolationCount() const { return m_violations; }
  const std::optional<ProtocolViolation>& FirstViolation() const { return m_first_violation; }
  // Retained confirmed input for one player, for diagnostics and tests.
  std::size_t HeldInputs(std::uint8_t player) const;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> HeldInputRange(std::uint8_t player) const;
  // Positive frame advantage means this peer is genuinely ahead after
  // projecting each reported remote frame forward by half of the measured
  // RTT.  TimeSyncWaitUs applies a wide deadband and a 30-frame cadence so
  // only the faster peer occasionally stretches a frame.
  double FrameAdvantage() const;
  double FrameAdvantage(std::uint32_t) const { return FrameAdvantage(); }
  std::uint32_t TimeSyncWaitUs(std::uint32_t local_frame);
  std::optional<std::uint32_t> SmoothedRttUs(std::uint8_t player) const;

  std::uint32_t Start() const { return m_start; }
  std::uint32_t Session() const { return m_session; }
  std::uint32_t Delay() const { return m_delay; }
  std::uint32_t MaxPrediction() const { return m_max_prediction; }
  const std::array<std::uint32_t, MAX_PLAYERS>& Contiguous() const { return m_contiguous; }

private:
  static Pad Canonical(Pad pad);
  Pad Predict(std::uint8_t player, std::uint32_t frame) const;
  void AdvanceContiguous(std::uint8_t player);
  void CompareChecksum(std::uint8_t player, std::uint32_t frame);
  void ObserveTiming(const Packet& packet, std::uint32_t receive_us);
  std::optional<ProtocolViolation> Validate(const Packet& packet) const;

  struct PeerTiming
  {
    bool have_remote_timestamp = false;
    bool have_rtt = false;
    std::uint32_t remote_send_us = 0;
    std::uint32_t remote_received_us = 0;
    std::uint32_t rtt_us = 0;
    std::uint32_t sampled_frame = NO_FRAME;
    std::array<double, 30> advantage{};
    std::size_t advantage_count = 0;
    std::size_t advantage_next = 0;
  };

  std::uint32_t m_start;
  std::uint8_t m_players;
  std::uint8_t m_me;
  std::uint32_t m_delay;
  std::uint32_t m_max_prediction;
  std::uint32_t m_session;
  std::array<std::map<std::uint32_t, Pad>, MAX_PLAYERS> m_confirmed;
  std::array<std::uint32_t, MAX_PLAYERS> m_contiguous{NO_FRAME, NO_FRAME, NO_FRAME,
                                                      NO_FRAME};
  std::array<std::uint32_t, MAX_PLAYERS> m_peer_ack{NO_FRAME, NO_FRAME, NO_FRAME,
                                                    NO_FRAME};
  std::array<PeerTiming, MAX_PLAYERS> m_timing;
  std::uint32_t m_local_frame = NO_FRAME;
  std::uint32_t m_last_sync_frame = NO_FRAME;
  std::map<std::uint32_t, Pads> m_ran;
  std::map<std::uint32_t, std::uint8_t> m_predicted;
  std::optional<std::uint32_t> m_pending;
  std::map<std::uint32_t, std::uint64_t> m_local_checksums;
  std::array<std::map<std::uint32_t, std::uint64_t>, MAX_PLAYERS> m_remote_checksums;
  std::uint32_t m_finalized = NO_FRAME;
  std::optional<std::uint32_t> m_first_desync;
  std::uint32_t m_violations = 0;
  std::optional<ProtocolViolation> m_first_violation;
};
}  // namespace NSRNetplay::RollbackNet
