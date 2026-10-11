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
#include <vector>

namespace NSRNetplay::RollbackNet
{
constexpr std::uint32_t MAGIC = 0x4E535242;  // 'NSRB'
constexpr std::uint32_t HANDOFF_MAGIC = 0x4E535248;  // 'NSRH'
constexpr std::uint8_t VERSION = 3;
constexpr std::uint8_t HANDOFF_VERSION = 1;
constexpr std::size_t MAX_PLAYERS = 4;
constexpr std::size_t PAD_SIZE = 12;
constexpr std::size_t MAX_HISTORY = 64;
constexpr std::uint32_t NO_FRAME = UINT32_MAX;

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

// Exchange is CPU-thread-owned.  A transport may deliver packets in any
// order and may duplicate them; insertion and acknowledgements are
// idempotent.  Assemble records exactly what a frame ran with so a later
// confirmed input can identify the earliest wrong prediction.
class Exchange
{
public:
  Exchange(std::uint32_t start, std::uint8_t players, std::uint8_t me,
           std::uint32_t delay, std::uint32_t max_prediction, std::uint32_t session);

  void SubmitLocal(std::uint32_t live_frame, Pad pad);
  Packet BuildPacket(std::uint32_t live_frame, std::uint32_t send_us = 0) const;
  bool Accept(const Packet& packet);
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
  int FrameAdvantage(std::uint32_t local_frame) const;

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
  std::array<std::uint32_t, MAX_PLAYERS> m_remote_frame{NO_FRAME, NO_FRAME, NO_FRAME,
                                                        NO_FRAME};
  std::map<std::uint32_t, Pads> m_ran;
  std::map<std::uint32_t, std::uint8_t> m_predicted;
  std::optional<std::uint32_t> m_pending;
  std::map<std::uint32_t, std::uint64_t> m_local_checksums;
  std::array<std::map<std::uint32_t, std::uint64_t>, MAX_PLAYERS> m_remote_checksums;
  std::uint32_t m_finalized = NO_FRAME;
  std::optional<std::uint32_t> m_first_desync;
};
}  // namespace NSRNetplay::RollbackNet
