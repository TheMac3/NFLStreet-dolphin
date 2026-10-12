// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Socket-free implementation of rollback input exchange and match-handoff
// agreement.  Wire parsing is deliberately strict because ENet datagrams are
// untrusted input even when the lobby supplied the peer.

#include "Core/NSR/NSRRollbackNetplay.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NSRNetplay::RollbackNet
{
namespace
{
constexpr std::size_t HEADER_SIZE = 96;
constexpr std::size_t HANDOFF_SIZE = 25 + 8 * CONFIG_FIELDS;
constexpr double FRAME_US = 16683.0;
constexpr std::uint32_t MAX_TIMING_RTT_US = 2'000'000;
constexpr std::size_t MIN_TIMING_SAMPLES = 6;
constexpr std::uint32_t TIME_SYNC_INTERVAL = 30;
constexpr double TIME_SYNC_DEADBAND_FRAMES = 0.60;
constexpr std::uint32_t MAX_TIME_SYNC_WAIT_US = 10'000;

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

const char* ConfigFieldName(ConfigField field)
{
  static constexpr std::array<const char*, CONFIG_FIELDS> NAMES{
      "protocol",       "state_digest",    "vtime_base",     "vmem_bytes",
      "resim_speed",    "instant_disc",    "io_idle",        "audio_rollback",
      "game_image",     "launcher_image",  "seed",           "restore_ranges",
      "audio_clock",    "resim_gates",     "session_rules",  "dolphin_core",
  };
  const auto index = static_cast<std::size_t>(field);
  return index < NAMES.size() ? NAMES[index] : "unknown";
}

std::vector<ConfigField> ConfigDifferences(const ConfigFingerprint& a, const ConfigFingerprint& b)
{
  std::vector<ConfigField> out;
  for (std::size_t i = 0; i < CONFIG_FIELDS; ++i)
  {
    if (a.fields[i] != b.fields[i])
      out.push_back(static_cast<ConfigField>(i));
  }
  return out;
}

std::uint64_t ConfigDigest(const ConfigFingerprint& config)
{
  std::uint64_t hash = 1469598103934665603ULL;
  for (const std::uint64_t value : config.fields)
  {
    for (int shift = 0; shift < 64; shift += 8)
      hash = (hash ^ ((value >> shift) & 0xFF)) * 1099511628211ULL;
  }
  return hash;
}

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
  Put(out, static_cast<std::uint8_t>(CONFIG_FIELDS));
  for (const std::uint64_t value : packet.config.fields)
    Put(out, value);
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
  std::uint8_t fields = 0;
  HandoffPacket packet;
  if (!Get(bytes, at, &magic) || !Get(bytes, at, &version) ||
      !Get(bytes, at, &packet.sender) || !Get(bytes, at, &packet.players) ||
      !Get(bytes, at, &kind) || !Get(bytes, at, &packet.transition) ||
      !Get(bytes, at, &packet.frame) ||
      !Get(bytes, at, &packet.semantic_hash) || !Get(bytes, at, &fields) ||
      fields != CONFIG_FIELDS || magic != HANDOFF_MAGIC ||
      version != HANDOFF_VERSION || packet.players < 2 || packet.players > MAX_PLAYERS ||
      packet.sender >= packet.players ||
      (kind != static_cast<std::uint8_t>(HandoffKind::EnterRollback) &&
       kind != static_cast<std::uint8_t>(HandoffKind::LeaveRollback)))
  {
    return std::nullopt;
  }
  for (std::uint64_t& value : packet.config.fields)
  {
    if (!Get(bytes, at, &value))
      return std::nullopt;
  }
  packet.kind = static_cast<HandoffKind>(kind);
  return packet;
}

HandoffAgreement::HandoffAgreement(std::uint8_t players, std::uint8_t me, HandoffKind kind,
                                   std::uint32_t transition, std::uint32_t frame,
                                   std::uint64_t semantic_hash, const ConfigFingerprint& config)
    : m_players(std::clamp<std::uint8_t>(players, 2, static_cast<std::uint8_t>(MAX_PLAYERS))),
      m_me(std::min<std::uint8_t>(me, m_players - 1)),
      m_local{m_me, m_players, kind, transition, frame, semantic_hash, config}
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
  if (packet.config != m_local.config)
  {
    if (!m_config_mismatch)
      m_config_mismatch = packet;
    return false;
  }
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
  return !Mismatched() &&
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
  Put(out, packet.config);
  Put(out, packet.sender_frame);
  Put(out, packet.send_us);
  for (const std::uint32_t echo : packet.echo_us)
    Put(out, echo);
  for (const std::uint32_t age : packet.echo_age_us)
    Put(out, age);
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
      !Get(bytes, at, &packet.config) || !Get(bytes, at, &packet.sender_frame) || !Get(bytes, at, &packet.send_us))
  {
    return std::nullopt;
  }
  for (std::uint32_t& echo : packet.echo_us)
  {
    if (!Get(bytes, at, &echo))
      return std::nullopt;
  }
  for (std::uint32_t& age : packet.echo_age_us)
  {
    if (!Get(bytes, at, &age))
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
                   std::uint32_t session, std::uint64_t config)
    : m_start(start),
      m_players(std::clamp<std::uint8_t>(players, 2, static_cast<std::uint8_t>(MAX_PLAYERS))),
      m_me(std::min<std::uint8_t>(me, m_players - 1)), m_delay(delay),
      m_max_prediction(max_prediction), m_session(session), m_config(config)
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
  m_local_frame = live_frame;
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
  packet.config = m_config;
  packet.sender_frame = live_frame;
  packet.send_us = send_us;
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    const PeerTiming& timing = m_timing[player];
    if (player == m_me || !timing.have_remote_timestamp)
      continue;
    packet.echo_us[player] = timing.remote_send_us;
    packet.echo_age_us[player] = send_us - timing.remote_received_us;
  }
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

std::uint32_t Exchange::FutureInputLimit() const
{
  // See the header for the derivation.  Before any local input exists a
  // peer may predict at most max_prediction frames from the epoch start.
  const std::uint32_t contiguous = m_contiguous[m_me];
  const std::uint64_t base =
      contiguous == NO_FRAME ? std::uint64_t{m_start} : std::uint64_t{contiguous} + 1;
  const std::uint64_t limit =
      base + m_max_prediction + m_delay + std::uint64_t{FUTURE_INPUT_SLACK};
  // NO_FRAME is a sentinel, never a storable frame.
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(limit, NO_FRAME - 1));
}

// Check every field that would mutate protocol state, without mutating any.
// Accept applies a datagram only after this returns nullopt, so a rejected
// packet cannot half-apply (for example, advance an ack but drop its pads).
std::optional<ProtocolViolation> Exchange::Validate(const Packet& packet) const
{
  // An honest ack is the sender's contiguous view of our input, which is
  // anchored at m_start and can never pass our own contiguous local prefix.
  // Stale (lower) acks remain valid; Accept simply does not regress on them.
  const std::uint32_t ack = packet.ack[m_me];
  const std::uint32_t local = m_contiguous[m_me];
  if (ack != NO_FRAME && (local == NO_FRAME || ack > local || ack < m_start))
    return ProtocolViolation{packet.sender, ViolationKind::ImpossibleAck, ack};

  const std::uint32_t limit = FutureInputLimit();
  const auto& known = m_confirmed[packet.sender];
  for (std::size_t i = 0; i < packet.pads.size(); ++i)
  {
    // Decode guarantees first_frame + count - 1 does not wrap.
    const std::uint32_t frame = packet.first_frame + static_cast<std::uint32_t>(i);
    if (frame > limit)
      return ProtocolViolation{packet.sender, ViolationKind::InputOutOfWindow, frame};
    // Redundant retransmission is the normal case and must be identical.
    // Frames already trimmed from history cannot be compared; Accept skips
    // them instead of resurrecting them.
    if (const auto it = known.find(frame);
        it != known.end() && it->second != Canonical(packet.pads[i]))
    {
      return ProtocolViolation{packet.sender, ViolationKind::ConflictingInput, frame};
    }
  }

  // A finalized checksum can only cover a state whose inputs, including
  // ours, the sender has confirmed, so the same window applies.
  if (packet.checksum_frame != NO_FRAME && packet.checksum_frame > limit)
  {
    return ProtocolViolation{packet.sender, ViolationKind::ChecksumOutOfWindow,
                             packet.checksum_frame};
  }
  return std::nullopt;
}

bool Exchange::Accept(const Packet& packet, std::uint32_t receive_us)
{
  // Stale epochs and misaddressed datagrams are expected under unsequenced
  // delivery; they are dropped without counting as a violation.
  if (packet.session != m_session || packet.players != m_players || packet.sender >= m_players ||
      packet.sender == m_me || packet.pads.size() > MAX_HISTORY)
  {
    return false;
  }
  // Same epoch, different configuration: an honest peer that will not
  // simulate the same game.  Nothing it says is usable; the adapter stops.
  if (packet.config != m_config)
  {
    if (!m_config_mismatch)
      m_config_mismatch = ConfigMismatchInfo{packet.sender, packet.config};
    return false;
  }
  if (const auto violation = Validate(packet))
  {
    // Saturate rather than wrap so a flood cannot make the count look clean.
    if (m_violations != std::numeric_limits<std::uint32_t>::max())
      ++m_violations;
    if (!m_first_violation)
      m_first_violation = violation;
    return false;
  }
  if (receive_us != 0)
    ObserveTiming(packet, receive_us);
  const std::uint32_t ack = packet.ack[m_me];
  if (ack != NO_FRAME &&
      (m_peer_ack[packet.sender] == NO_FRAME || ack > m_peer_ack[packet.sender]))
  {
    m_peer_ack[packet.sender] = ack;
  }
  auto& inputs = m_confirmed[packet.sender];
  const std::uint32_t contiguous = m_contiguous[packet.sender];
  for (std::size_t i = 0; i < packet.pads.size(); ++i)
  {
    const std::uint32_t frame = packet.first_frame + static_cast<std::uint32_t>(i);
    // Nothing precedes the epoch, and a frame at or below the contiguous
    // prefix that is no longer held was trimmed after confirmation.  A
    // reordered retransmission must not resurrect either into history.
    if (frame < m_start ||
        (contiguous != NO_FRAME && frame <= contiguous && !inputs.contains(frame)))
    {
      continue;
    }
    const Pad pad = Canonical(packet.pads[i]);
    const auto [it, inserted] = inputs.try_emplace(frame, pad);
    if (!inserted)
      continue;  // Validate proved the stored value is identical.
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

// Derive RTT from a reflected local timestamp, then compare progress at the
// receive boundary.  The remote report is projected forward by one-way
// latency; unlike raw frame subtraction, equal-speed peers therefore do not
// both look ahead merely because every report is old when it arrives.
void Exchange::ObserveTiming(const Packet& packet, std::uint32_t receive_us)
{
  PeerTiming& timing = m_timing[packet.sender];
  const std::uint32_t echoed = packet.echo_us[m_me];
  if (echoed != 0)
  {
    const std::uint32_t elapsed = receive_us - echoed;
    const std::uint32_t held = packet.echo_age_us[m_me];
    if (elapsed >= held && elapsed - held <= MAX_TIMING_RTT_US)
    {
      const std::uint32_t sample = elapsed - held;
      timing.rtt_us = timing.have_rtt ? (timing.rtt_us * 7 + sample) / 8 : sample;
      timing.have_rtt = true;
    }
  }

  const bool newer_frame = timing.sampled_frame == NO_FRAME ||
                           packet.sender_frame > timing.sampled_frame;
  if (timing.have_rtt && m_local_frame != NO_FRAME && newer_frame)
  {
    const double projected_remote =
        static_cast<double>(packet.sender_frame) + timing.rtt_us / (2.0 * FRAME_US);
    timing.advantage[timing.advantage_next] =
        static_cast<double>(m_local_frame) - projected_remote;
    timing.advantage_next = (timing.advantage_next + 1) % timing.advantage.size();
    timing.advantage_count = std::min(timing.advantage_count + 1, timing.advantage.size());
    timing.sampled_frame = packet.sender_frame;
  }

  timing.have_remote_timestamp = true;
  timing.remote_send_us = packet.send_us;
  timing.remote_received_us = receive_us;
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

void Exchange::DiscardChecksumsAfter(std::uint32_t frame)
{
  m_local_checksums.erase(m_local_checksums.upper_bound(frame), m_local_checksums.end());
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
  if (m_finalized != NO_FRAME && candidate->first <= m_finalized)
    return;
  const std::uint32_t previous = m_finalized;
  m_finalized = candidate->first;
  // Remote digests that arrived before our own state at that frame was
  // final are compared now.
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    if (player == m_me)
      continue;
    const auto& remote = m_remote_checksums[player];
    auto it = previous == NO_FRAME ? remote.begin() : remote.upper_bound(previous);
    for (; it != remote.end() && it->first <= m_finalized; ++it)
      CompareChecksum(player, it->first);
  }
}

void Exchange::CompareChecksum(std::uint8_t player, std::uint32_t frame)
{
  // A local digest beyond our finalized frame may still describe a
  // mispredicted state (a correction can be pending, or not yet received).
  if (m_finalized == NO_FRAME || frame > m_finalized)
    return;
  const auto local = m_local_checksums.find(frame);
  const auto remote = m_remote_checksums[player].find(frame);
  if (local == m_local_checksums.end() || remote == m_remote_checksums[player].end() ||
      local->second == remote->second)
  {
    return;
  }
  const DesyncInfo info{player, frame, local->second, remote->second};
  if (m_desyncs.size() < 256 &&
      std::none_of(m_desyncs.begin(), m_desyncs.end(), [&](const DesyncInfo& seen) {
        return seen.frame == frame && seen.player == player;
      }))
  {
    m_desyncs.push_back(info);
  }
  if (!m_first_desync || frame < *m_first_desync)
  {
    m_first_desync = frame;
    m_first_desync_info = info;
  }
}

void Exchange::TrimBefore(std::uint32_t frame)
{
  // Keep one input before the cutoff because repeat-last prediction needs it.
  // Local history also remains available until every peer has acknowledged it.
  //
  // Remote history above FutureInputLimit() is also dropped.  Accept never
  // stores such input and the limit only grows, so this is a no-op for honest
  // peers; it is defense in depth so that retention is bounded on both sides
  // of the live frame rather than relying on a single insertion check.
  const std::uint32_t limit = FutureInputLimit();
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    auto& inputs = m_confirmed[player];
    std::uint32_t cutoff = frame;
    if (player == m_me)
    {
      if (const auto unacked = OldestUnackedLocal())
        cutoff = std::min(cutoff, *unacked);
    }
    else
    {
      inputs.erase(inputs.upper_bound(limit), inputs.end());
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
  {
    checksums.erase(checksums.begin(), checksums.lower_bound(frame));
    checksums.erase(checksums.upper_bound(limit), checksums.end());
  }
}

std::size_t Exchange::HeldInputs(std::uint8_t player) const
{
  return player < m_players ? m_confirmed[player].size() : 0;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>>
Exchange::HeldInputRange(std::uint8_t player) const
{
  if (player >= m_players || m_confirmed[player].empty())
    return std::nullopt;
  return std::pair{m_confirmed[player].begin()->first, m_confirmed[player].rbegin()->first};
}

std::optional<std::uint32_t> Exchange::FirstDesync() const
{
  return m_first_desync;
}

double Exchange::FrameAdvantage() const
{
  std::optional<double> slowest;
  for (std::uint8_t player = 0; player < m_players; ++player)
  {
    if (player == m_me)
      continue;
    const PeerTiming& timing = m_timing[player];
    if (timing.advantage_count < MIN_TIMING_SAMPLES)
      return 0.0;

    std::vector<double> samples(timing.advantage.begin(),
                                timing.advantage.begin() + timing.advantage_count);
    std::sort(samples.begin(), samples.end());
    const std::size_t trim = samples.size() / 3;
    const auto begin = samples.begin() + static_cast<std::ptrdiff_t>(trim);
    const auto end = samples.end() - static_cast<std::ptrdiff_t>(trim);
    double sum = 0.0;
    for (auto it = begin; it != end; ++it)
      sum += *it;
    const double peer_advantage = sum / static_cast<double>(end - begin);
    slowest = slowest ? std::min(*slowest, peer_advantage) : peer_advantage;
  }
  return slowest.value_or(0.0);
}

std::uint32_t Exchange::TimeSyncWaitUs(std::uint32_t local_frame)
{
  if (local_frame < m_start || (local_frame - m_start) % TIME_SYNC_INTERVAL != 0 ||
      m_last_sync_frame == local_frame)
  {
    return 0;
  }
  m_last_sync_frame = local_frame;

  const double advantage = FrameAdvantage();
  if (advantage <= TIME_SYNC_DEADBAND_FRAMES)
    return 0;

  // Remove only part of the measured lead.  A capped sub-frame stretch is
  // less visible than a full repeated frame and the next 30-sample window
  // verifies the direction before another correction is allowed.
  const double requested = (advantage - 0.25) * FRAME_US;
  return std::min(MAX_TIME_SYNC_WAIT_US,
                  static_cast<std::uint32_t>(std::max(1.0, std::round(requested))));
}

std::optional<std::uint32_t> Exchange::SmoothedRttUs(std::uint8_t player) const
{
  if (player >= m_players || player == m_me || !m_timing[player].have_rtt)
    return std::nullopt;
  return m_timing[player].rtt_us;
}
}  // namespace NSRNetplay::RollbackNet
