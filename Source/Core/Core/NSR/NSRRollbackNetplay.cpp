// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Socket-free implementation of rollback input exchange and match-handoff
// agreement.  Wire parsing is deliberately strict because ENet datagrams are
// untrusted input even when the lobby supplied the peer.

#include "Core/NSR/NSRRollbackNetplay.h"

#include <algorithm>
#include <limits>

namespace NSRNetplay::RollbackNet
{
namespace
{
constexpr std::size_t HEADER_SIZE = 56;
constexpr std::size_t HANDOFF_SIZE = 24;

// Append one integer in protocol byte order without depending on packed C++
// layouts, which keeps the same datagram on x86-64 and ARM hosts.
template <typename T>
void Put(std::vector<std::uint8_t>& out, T value)
{
  for (std::size_t i = 0; i < sizeof(T); ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

// Consume one little-endian integer, failing closed on a truncated packet.
template <typename T>
bool Get(std::span<const std::uint8_t> bytes, std::size_t& at, T* value)
{
  if (bytes.size() - std::min(bytes.size(), at) < sizeof(T))
    return false;
  *value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i)
    *value |= static_cast<T>(bytes[at++]) << (8 * i);
  return true;
}
}  // namespace

std::vector<std::uint8_t> EncodeHandoff(const HandoffPacket& packet)
{
  std::vector<std::uint8_t> out;
  out.reserve(HANDOFF_SIZE);
  Put(out, HANDOFF_MAGIC);
  Put(out, HANDOFF_VERSION);
  Put(out, packet.sender);
  Put(out, packet.players);
  Put(out, static_cast<std::uint8_t>(packet.kind));
  Put(out, packet.transition);
  Put(out, packet.frame);
  Put(out, packet.semantic_hash);
  return out;
}

std::optional<HandoffPacket> DecodeHandoff(std::span<const std::uint8_t> bytes)
{
  if (bytes.size() != HANDOFF_SIZE)
    return std::nullopt;
  std::size_t at = 0;
  std::uint32_t magic = 0;
  std::uint8_t version = 0;
  std::uint8_t kind = 0;
  HandoffPacket packet;
  if (!Get(bytes, at, &magic) || !Get(bytes, at, &version) ||
      !Get(bytes, at, &packet.sender) || !Get(bytes, at, &packet.players) ||
      !Get(bytes, at, &kind) || !Get(bytes, at, &packet.transition) ||
      !Get(bytes, at, &packet.frame) ||
      !Get(bytes, at, &packet.semantic_hash) || magic != HANDOFF_MAGIC ||
      version != HANDOFF_VERSION || packet.players < 2 || packet.players > MAX_PLAYERS ||
      packet.sender >= packet.players ||
      (kind != static_cast<std::uint8_t>(HandoffKind::EnterRollback) &&
       kind != static_cast<std::uint8_t>(HandoffKind::LeaveRollback)))
  {
    return std::nullopt;
  }
  packet.kind = static_cast<HandoffKind>(kind);
  return packet;
}

HandoffAgreement::HandoffAgreement(std::uint8_t players, std::uint8_t me, HandoffKind kind,
                                   std::uint32_t transition, std::uint32_t frame,
                                   std::uint64_t semantic_hash)
    : m_players(std::clamp<std::uint8_t>(players, 2, MAX_PLAYERS)),
      m_me(std::min<std::uint8_t>(me, m_players - 1)),
      m_local{m_me, m_players, kind, transition, frame, semantic_hash}
{
  m_seen[m_me] = true;
}

bool HandoffAgreement::Accept(const HandoffPacket& packet)
{
  if (packet.players != m_players || packet.sender >= m_players || packet.sender == m_me)
    return false;
  // Packets from an earlier/later match can survive unsequenced delivery.
  // They are not evidence of disagreement about this transition.
  if (packet.transition != m_local.transition)
    return false;
  if (packet.kind != m_local.kind || packet.frame != m_local.frame ||
      packet.semantic_hash != m_local.semantic_hash)
  {
    if (!m_mismatch)
      m_mismatch = packet;
    return false;
  }
  m_seen[packet.sender] = true;
  return true;
}

bool HandoffAgreement::Complete() const
{
  return !m_mismatch &&
         std::all_of(m_seen.begin(), m_seen.begin() + m_players, [](bool seen) { return seen; });
}

std::vector<std::uint8_t> Encode(const Packet& packet)
{
  const std::size_t count = std::min(packet.pads.size(), MAX_HISTORY);
  std::vector<std::uint8_t> out;
  out.reserve(HEADER_SIZE + count * PAD_SIZE);
  Put(out, MAGIC);
  Put(out, VERSION);
  Put(out, packet.sender);
  Put(out, packet.players);
  Put(out, packet.flags);
  Put(out, packet.session);
  Put(out, packet.sender_frame);
  Put(out, packet.send_us);
  for (const std::uint32_t ack : packet.ack)
    Put(out, ack);
  Put(out, packet.checksum_frame);
  Put(out, packet.checksum);
  Put(out, packet.first_frame);
  Put(out, static_cast<std::uint16_t>(count));
  Put(out, static_cast<std::uint16_t>(PAD_SIZE));
  for (std::size_t i = 0; i < count; ++i)
    out.insert(out.end(), packet.pads[i].begin(), packet.pads[i].end());
  return out;
}

std::optional<Packet> Decode(std::span<const std::uint8_t> bytes)
{
  if (bytes.size() < HEADER_SIZE)
    return std::nullopt;
  std::size_t at = 0;
  std::uint32_t magic = 0;
  std::uint8_t version = 0;
  std::uint16_t count = 0;
  std::uint16_t pad_size = 0;
  Packet packet;
  if (!Get(bytes, at, &magic) || !Get(bytes, at, &version) ||
      !Get(bytes, at, &packet.sender) || !Get(bytes, at, &packet.players) ||
      !Get(bytes, at, &packet.flags) || !Get(bytes, at, &packet.session) ||
      !Get(bytes, at, &packet.sender_frame) || !Get(bytes, at, &packet.send_us))
  {
    return std::nullopt;
  }
  for (std::uint32_t& ack : packet.ack)
  {
    if (!Get(bytes, at, &ack))
      return std::nullopt;
  }
  if (!Get(bytes, at, &packet.checksum_frame) || !Get(bytes, at, &packet.checksum) ||
      !Get(bytes, at, &packet.first_frame) || !Get(bytes, at, &count) ||
      !Get(bytes, at, &pad_size) || magic != MAGIC || version != VERSION ||
      packet.players < 2 || packet.players > MAX_PLAYERS || packet.sender >= packet.players ||
      count > MAX_HISTORY || pad_size != PAD_SIZE ||
      (count > 0 &&
       packet.first_frame > std::numeric_limits<std::uint32_t>::max() - (count - 1)) ||
      bytes.size() != HEADER_SIZE + count * PAD_SIZE)
  {
    return std::nullopt;
  }
  packet.pads.resize(count);
  for (Pad& pad : packet.pads)
  {
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), PAD_SIZE, pad.begin());
    at += PAD_SIZE;
  }
  return packet;
}

Exchange::Exchange(std::uint32_t start, std::uint8_t players, std::uint8_t me,
                   std::uint32_t delay, std::uint32_t max_prediction,
                   std::uint32_t session)
    : m_start(start), m_players(std::clamp<std::uint8_t>(players, 2, MAX_PLAYERS)),
      m_me(std::min<std::uint8_t>(me, m_players - 1)), m_delay(delay),
      m_max_prediction(max_prediction), m_session(session)
{
  // Delay slots are real confirmed local inputs.  Sending the neutral prefix
  // lets peers acknowledge it exactly like later controller samples.
  for (std::uint32_t frame = start; frame < start + delay; ++frame)
    m_confirmed[m_me].emplace(frame, Pad{});
  AdvanceContiguous(m_me);
}

Pad Exchange::Canonical(Pad pad)
{
  pad[10] = 0;  // connected
  pad[11] = 0;  // never put process-local padding/control data on the wire
  return pad;
}

void Exchange::SubmitLocal(std::uint32_t live_frame, Pad pad)
{
  m_confirmed[m_me].try_emplace(live_frame + m_delay, Canonical(pad));
  AdvanceContiguous(m_me);
}

std::optional<std::uint32_t> Exchange::OldestUnackedLocal() const
{
  const auto& local = m_confirmed[m_me];
  if (local.empty())
    return std::nullopt;
  std::uint32_t first = NO_FRAME;
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    if (player == m_me)
      continue;
    const std::uint32_t ack = m_peer_ack[player];
    if (ack == NO_FRAME)
      return local.begin()->first;
    first = std::min(first, ack + 1);
  }
  return std::max(local.begin()->first, first);
}

Packet Exchange::BuildPacket(std::uint32_t live_frame, std::uint32_t send_us) const
{
  Packet packet;
  packet.sender = m_me;
  packet.players = m_players;
  packet.session = m_session;
  packet.sender_frame = live_frame;
  packet.send_us = send_us;
  packet.flags = m_first_desync ? 0x02 : 0;
  packet.ack = m_contiguous;
  if (m_finalized != NO_FRAME)
  {
    if (const auto it = m_local_checksums.find(m_finalized); it != m_local_checksums.end())
    {
      packet.checksum_frame = m_finalized;
      packet.checksum = it->second;
    }
  }

  const auto& local = m_confirmed[m_me];
  if (local.empty())
    return packet;
  const std::uint32_t newest = local.rbegin()->first;
  const std::uint32_t first = OldestUnackedLocal().value_or(newest);
  if (first > newest)
    return packet;
  // Send the oldest unacknowledged window, not merely the newest window.
  // Otherwise a loss burst longer than MAX_HISTORY creates a permanent hole
  // in the receiver's contiguous prefix and both peers eventually stall.
  const std::uint32_t last = newest - first + 1 > MAX_HISTORY ?
                                 first + static_cast<std::uint32_t>(MAX_HISTORY) - 1 :
                                 newest;
  packet.first_frame = first;
  for (std::uint32_t frame = first; frame <= last; ++frame)
  {
    const auto it = local.find(frame);
    if (it == local.end())
      break;
    packet.pads.push_back(it->second);
    if (frame == std::numeric_limits<std::uint32_t>::max())
      break;
  }
  return packet;
}

bool Exchange::Accept(const Packet& packet)
{
  if (packet.session != m_session || packet.players != m_players || packet.sender >= m_players ||
      packet.sender == m_me || packet.pads.size() > MAX_HISTORY)
  {
    return false;
  }
  if (m_remote_frame[packet.sender] == NO_FRAME ||
      packet.sender_frame > m_remote_frame[packet.sender])
  {
    m_remote_frame[packet.sender] = packet.sender_frame;
  }
  const std::uint32_t ack = packet.ack[m_me];
  if (ack != NO_FRAME &&
      (m_peer_ack[packet.sender] == NO_FRAME || ack > m_peer_ack[packet.sender]))
  {
    m_peer_ack[packet.sender] = ack;
  }
  for (std::size_t i = 0; i < packet.pads.size(); ++i)
  {
    const std::uint32_t frame = packet.first_frame + static_cast<std::uint32_t>(i);
    const Pad pad = Canonical(packet.pads[i]);
    const auto [it, inserted] = m_confirmed[packet.sender].try_emplace(frame, pad);
    if (!inserted || frame < m_start)
      continue;
    if (const auto ran = m_ran.find(frame); ran != m_ran.end() &&
        (m_predicted[frame] & (1U << packet.sender)) != 0)
    {
      if (ran->second[packet.sender] != pad)
        m_pending = m_pending ? std::min(*m_pending, frame) : frame;
      else
        m_predicted[frame] &= static_cast<std::uint8_t>(~(1U << packet.sender));
    }
  }
  AdvanceContiguous(packet.sender);
  if (packet.checksum_frame != NO_FRAME)
  {
    m_remote_checksums[packet.sender][packet.checksum_frame] = packet.checksum;
    CompareChecksum(packet.sender, packet.checksum_frame);
  }
  return true;
}

void Exchange::AdvanceContiguous(std::uint8_t player)
{
  std::uint32_t next = m_contiguous[player] == NO_FRAME ? m_start : m_contiguous[player] + 1;
  while (m_confirmed[player].contains(next))
  {
    m_contiguous[player] = next;
    if (next == std::numeric_limits<std::uint32_t>::max())
      break;
    ++next;
  }
}

Pad Exchange::Predict(std::uint8_t player, std::uint32_t frame) const
{
  const auto& inputs = m_confirmed[player];
  auto it = inputs.upper_bound(frame);
  if (it == inputs.begin())
    return {};
  return std::prev(it)->second;
}

Pads Exchange::Assemble(std::uint32_t frame)
{
  Pads pads{};
  std::uint8_t predicted = 0;
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    if (const auto it = m_confirmed[player].find(frame); it != m_confirmed[player].end())
      pads[player] = it->second;
    else
    {
      pads[player] = Predict(player, frame);
      if (player != m_me)
        predicted |= static_cast<std::uint8_t>(1U << player);
    }
  }
  m_ran[frame] = pads;
  m_predicted[frame] = predicted;
  return pads;
}

std::uint32_t Exchange::ConfirmedThrough() const
{
  std::uint32_t confirmed = NO_FRAME;
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    if (player == m_me)
      continue;
    if (m_contiguous[player] == NO_FRAME)
      return NO_FRAME;
    confirmed = confirmed == NO_FRAME ? m_contiguous[player] :
                                       std::min(confirmed, m_contiguous[player]);
  }
  return confirmed;
}

bool Exchange::ShouldStall(std::uint32_t frame) const
{
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    if (player == m_me)
      continue;
    const std::uint32_t confirmed = m_contiguous[player];
    const std::uint64_t predicted = confirmed == NO_FRAME ?
                                        std::uint64_t{frame} - m_start + 1 :
                                        std::uint64_t{frame} - std::min(frame, confirmed);
    if (predicted > m_max_prediction)
      return true;
  }
  return false;
}

std::optional<std::uint32_t> Exchange::PendingRollback() const
{
  return m_pending;
}

void Exchange::BeginRollback()
{
  m_pending.reset();
}

void Exchange::SetStateChecksum(std::uint32_t frame, std::uint64_t checksum)
{
  m_local_checksums[frame] = checksum;
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    if (player != m_me)
      CompareChecksum(player, frame);
  }
}

void Exchange::UpdateFinalized()
{
  const std::uint32_t confirmed = ConfirmedThrough();
  if (confirmed == NO_FRAME)
    return;
  std::uint32_t limit = confirmed == std::numeric_limits<std::uint32_t>::max() ?
                            confirmed : confirmed + 1;
  // Input for frame f changes the state after f, so the start state of f is
  // still final while f itself awaits correction.
  if (m_pending)
    limit = std::min(limit, *m_pending);
  auto candidate = m_local_checksums.upper_bound(limit);
  if (candidate == m_local_checksums.begin())
    return;
  --candidate;
  if (m_finalized == NO_FRAME || candidate->first > m_finalized)
    m_finalized = candidate->first;
}

void Exchange::CompareChecksum(std::uint8_t player, std::uint32_t frame)
{
  const auto local = m_local_checksums.find(frame);
  const auto remote = m_remote_checksums[player].find(frame);
  if (local != m_local_checksums.end() && remote != m_remote_checksums[player].end() &&
      local->second != remote->second && (!m_first_desync || frame < *m_first_desync))
  {
    m_first_desync = frame;
  }
}

void Exchange::TrimBefore(std::uint32_t frame)
{
  // Keep one input before the cutoff because repeat-last prediction needs it.
  // Local history also remains available until every peer has acknowledged it.
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    auto& inputs = m_confirmed[player];
    std::uint32_t cutoff = frame;
    if (player == m_me)
    {
      if (const auto unacked = OldestUnackedLocal())
        cutoff = std::min(cutoff, *unacked);
    }
    auto keep = inputs.lower_bound(cutoff);
    if (keep != inputs.begin())
      --keep;
    inputs.erase(inputs.begin(), keep);
  }

  m_ran.erase(m_ran.begin(), m_ran.lower_bound(frame));
  m_predicted.erase(m_predicted.begin(), m_predicted.lower_bound(frame));
  m_local_checksums.erase(m_local_checksums.begin(), m_local_checksums.lower_bound(frame));
  for (auto& checksums : m_remote_checksums)
    checksums.erase(checksums.begin(), checksums.lower_bound(frame));
}

std::optional<std::uint32_t> Exchange::FirstDesync() const
{
  return m_first_desync;
}

int Exchange::FrameAdvantage(std::uint32_t local_frame) const
{
  std::optional<std::uint32_t> slowest;
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    if (player == m_me || m_remote_frame[player] == NO_FRAME)
      continue;
    slowest = slowest ? std::min(*slowest, m_remote_frame[player]) : m_remote_frame[player];
  }
  if (!slowest)
    return 0;
  const std::int64_t difference = std::int64_t{local_frame} - *slowest;
  return static_cast<int>(std::clamp<std::int64_t>(difference, -100000, 100000));
}
}  // namespace NSRNetplay::RollbackNet
