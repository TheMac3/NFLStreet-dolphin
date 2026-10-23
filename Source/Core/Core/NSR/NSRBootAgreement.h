// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Bounded, endian-independent preboot messages. Kept socket-free so malformed
// input and named setting/card mismatch reporting can be tested without Dolphin.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace NSRBootSync
{
using Manifest = std::map<std::string, std::string>;
constexpr std::size_t MAX_BOOT_PACKET = 32768;
enum class BootKind : std::uint8_t { Settings, Ready, Go };
struct BootMessage
{
  BootKind kind{};
  std::uint8_t sender = 0;
  std::uint8_t players = 2;
  Manifest fields;
};

// Field values use printable ASCII; paths/secrets never belong in a manifest.
inline bool ValidBootText(const std::string& text)
{
  if (text.size() > 256)
    return false;
  for (const unsigned char ch : text)
    if (ch < 32 || ch > 126 || ch == '=')
      return false;
  return true;
}

// Encode sorted named fields so a digest covers the same bytes on every OS.
inline std::vector<std::uint8_t> EncodeBoot(const BootMessage& message)
{
  std::vector<std::uint8_t> bytes{'N', 'S', 'R', 'C', 1,
      static_cast<std::uint8_t>(message.kind), message.sender, message.players};
  for (const auto& [key, value] : message.fields)
  {
    bytes.insert(bytes.end(), key.begin(), key.end());
    bytes.push_back('=');
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back('\n');
  }
  return bytes;
}

// Reject truncation, duplicate fields, unknown versions and unbounded payloads.
inline std::optional<BootMessage> DecodeBoot(std::span<const std::uint8_t> bytes)
{
  if (bytes.size() < 8 || bytes.size() > MAX_BOOT_PACKET ||
      bytes[0] != 'N' || bytes[1] != 'S' || bytes[2] != 'R' || bytes[3] != 'C' ||
      bytes[4] != 1 || bytes[5] > 2 || bytes[7] < 2 || bytes[7] > 4 || bytes[6] >= bytes[7])
    return std::nullopt;
  BootMessage message{static_cast<BootKind>(bytes[5]), bytes[6], bytes[7], {}};
  std::size_t pos = 8;
  while (pos < bytes.size())
  {
    std::size_t end = pos;
    while (end < bytes.size() && bytes[end] != '\n')
      ++end;
    if (end == bytes.size())
      return std::nullopt;
    const std::string line(bytes.begin() + pos, bytes.begin() + end);
    const auto eq = line.find('=');
    if (eq == std::string::npos || eq == 0)
      return std::nullopt;
    auto key = line.substr(0, eq);
    auto value = line.substr(eq + 1);
    if (!ValidBootText(key) || !ValidBootText(value) ||
        !message.fields.emplace(std::move(key), std::move(value)).second ||
        message.fields.size() > 256)
      return std::nullopt;
    pos = end + 1;
  }
  if ((message.kind == BootKind::Settings) == message.fields.empty())
    return std::nullopt;
  return message;
}

// Name missing and changed fields in either direction; callers report only names
// and digests so accidental path-bearing overrides cannot leak into logs.
inline std::vector<std::string> BootDifferences(const Manifest& local, const Manifest& remote)
{
  std::vector<std::string> result;
  for (const auto& [key, value] : local)
  {
    const auto it = remote.find(key);
    if (it == remote.end() || it->second != value)
      result.push_back(key);
  }
  for (const auto& [key, value] : remote)
    if (!local.contains(key))
      result.push_back(key);
  return result;
}
}  // namespace NSRBootSync
