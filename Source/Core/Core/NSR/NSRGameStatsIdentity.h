// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Optional, transport-neutral stats identities. NSRI never affects simulation
// agreement; ENet's adapter must bind the decoded pad to the sending connection.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace NSRGameStats
{
struct PeerIdentity
{
  std::uint8_t pad = 0, players = 1;
  std::uint32_t started_at = 0;
  std::string name;
};

struct CaptureAnchor
{
  std::uint8_t players = 1;
  std::uint32_t ordinal = 0, frame = 0;
};

// NSRG is a host-only publication anchor, not an input or a simulation barrier.
// Peers can accept the same corrected result at different live frames. Sharing
// the host's anchor keeps capture metadata/UTC identical without delaying play.
inline std::optional<CaptureAnchor> DecodeCaptureAnchor(std::span<const std::uint8_t> packet)
{
  if (packet.size() != 16 || packet[0] != 'N' || packet[1] != 'S' || packet[2] != 'R' ||
      packet[3] != 'G' || packet[4] != 1 || packet[5] < 1 || packet[5] > 4 ||
      packet[6] != 0 || packet[7] != 0)
    return std::nullopt;
  CaptureAnchor result;
  result.players = packet[5];
  for (unsigned i = 0; i < 4; ++i)
  {
    result.ordinal = (result.ordinal << 8) | packet[8 + i];
    result.frame = (result.frame << 8) | packet[12 + i];
  }
  return result.ordinal ? std::optional{result} : std::nullopt;
}

inline std::vector<std::uint8_t> EncodeCaptureAnchor(const CaptureAnchor& anchor)
{
  if (anchor.players < 1 || anchor.players > 4 || anchor.ordinal == 0)
    return {};
  std::vector<std::uint8_t> packet{'N', 'S', 'R', 'G', 1, anchor.players, 0, 0};
  for (const auto value : {anchor.ordinal, anchor.frame})
  {
    for (unsigned i = 0; i < 4; ++i)
      packet.push_back(static_cast<std::uint8_t>(value >> ((3 - i) * 8)));
  }
  return packet;
}

// UTF-8 must survive JSON without replacement, malformed byte sequences or
// control characters. Bounds reject overlong encodings and surrogate scalars.
inline bool ValidIdentityName(std::string_view name)
{
  if (name.size() > 64)
    return false;
  std::uint32_t scalar = 0, remaining = 0, minimum = 0;
  for (const auto character : name)
  {
    const auto byte = static_cast<std::uint8_t>(character);
    if (remaining)
    {
      if ((byte & 0xc0) != 0x80)
        return false;
      scalar = (scalar << 6) | (byte & 0x3f);
      if (--remaining == 0 &&
          (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)))
        return false;
    }
    else if (byte < 0x80)
    {
      if (byte < 0x20 || byte == 0x7f)
        return false;
    }
    else if (byte >= 0xc2 && byte <= 0xf4)
    {
      remaining = byte < 0xe0 ? 1 : byte < 0xf0 ? 2 : 3;
      scalar = byte & (remaining == 1 ? 0x1f : remaining == 2 ? 0xf : 7);
      minimum = remaining == 1 ? 0x80 : remaining == 2 ? 0x800 : 0x10000;
    }
    else
    {
      return false;
    }
  }
  return remaining == 0;
}

// Exact encoding: "NSRI", version, pad, players, name length, four big-endian
// Unix-second bytes, then UTF-8. Non-host peers cannot supply the shared clock.
inline std::optional<PeerIdentity> DecodeIdentity(std::span<const std::uint8_t> packet)
{
  if (packet.size() < 12 || packet.size() > 76 || packet[0] != 'N' || packet[1] != 'S' ||
      packet[2] != 'R' || packet[3] != 'I' || packet[4] != 1 || packet[6] < 1 || packet[6] > 4 ||
      packet[5] >= packet[6] || packet[7] > 64 || packet.size() != 12u + packet[7])
    return std::nullopt;
  PeerIdentity result;
  result.pad = packet[5];
  result.players = packet[6];
  for (unsigned i = 8; i < 12; ++i)
    result.started_at = (result.started_at << 8) | packet[i];
  result.name.assign(packet.begin() + 12, packet.end());
  if ((result.pad != 0 && result.started_at != 0) || !ValidIdentityName(result.name))
    return std::nullopt;
  return result;
}

// Local input uses the same validation as received packets. No native struct
// layout, host endianness, padding or unbounded strings cross the wire.
inline std::vector<std::uint8_t> EncodeIdentity(const PeerIdentity& identity)
{
  if (identity.players < 1 || identity.players > 4 || identity.pad >= identity.players ||
      (identity.pad != 0 && identity.started_at != 0) || !ValidIdentityName(identity.name))
    return {};
  std::vector<std::uint8_t> packet{'N', 'S', 'R', 'I', 1, identity.pad, identity.players,
                                   static_cast<std::uint8_t>(identity.name.size())};
  for (unsigned i = 0; i < 4; ++i)
    packet.push_back(static_cast<std::uint8_t>(identity.started_at >> ((3 - i) * 8)));
  packet.insert(packet.end(), identity.name.begin(), identity.name.end());
  return packet;
}
}  // namespace NSRGameStats
