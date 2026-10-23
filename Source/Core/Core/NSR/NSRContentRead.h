// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Socket-free, atomic edit planning over native EA DB structures. Reuse the
// audited results reader's bounds/schema checks; never embed roster records.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Core/NSR/NSRGameStatsRead.h"

namespace NSRContent
{
struct Edit
{
  std::string first, last, field;
  std::uint32_t value;
  std::vector<std::uint32_t> expected;
};

// Normalize ASCII case/space without locale. Plan rejects non-ASCII edit
// identities until their semantics are qualified against Python casefold.
inline std::string Name(std::string value)
{
  for (char& c : value)
  {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c + ('a' - 'A'));
  }
  const auto begin = value.find_first_not_of(' ');
  if (begin == std::string::npos)
    return {};
  return value.substr(begin, value.find_last_not_of(' ') - begin + 1);
}

// The same allowlist/ranges as nsr_content.py. Storage bounds are checked too.
inline std::pair<std::uint32_t, std::uint32_t> Limits(const std::string& field)
{
  if (field == "PHGT")
    return {48, 96};
  if (field == "PWGT")
    return {0, 255};
  static const std::vector<std::string> ratings{
      "POVR", "PSPD", "PCEL", "PAGI", "PJUM", "PCTH", "PPSS", "PCOV", "PTAK", "PBLK", "PBTK"};
  if (std::find(ratings.begin(), ratings.end(), field) == ratings.end())
    return {1, 0};
  return {0, 127};
}

// Validate every requested copy and every field before returning any writes.
// The returned map merges adjacent packed fields while preserving all other bits.
inline std::optional<std::map<std::uint32_t, std::uint8_t>>
Plan(const NSRGameStats::Memory& memory, const std::vector<Edit>& edits, std::string* error = nullptr)
{
  const auto reject = [&](const std::string& message) -> std::nullopt_t {
    if (error)
      *error = message;
    return std::nullopt;
  };
  using namespace NSRGameStats;
  const auto rows = ReadTable(memory, "TATS", "YALP");
  const auto table = FindTable(memory, "TATS", "YALP");
  if (!rows || !table)
    return reject("missing or malformed STAT.PLAY");
  const auto stride = *memory.UInt(*table + 0x20);
  const auto count = *memory.UInt(*table + 0x44, 2);
  const auto deleted = *memory.UInt(*table + 0x24);
  const auto buffer = *memory.UInt(*table + 0x14);
  if (buffer < 0x7e000000 || buffer - 0x7e000000 > memory.vmem.size() ||
      count * stride > memory.vmem.size() - (buffer - 0x7e000000))
    return reject("roster record buffer is outside fake VMEM");
  std::vector<std::uint32_t> physical;
  for (std::uint32_t i = 0; i < count; ++i)
  {
    const auto bytes = *memory.Bytes(buffer + i * stride, stride);
    if (!((bytes[deleted / 8] >> (7 - deleted % 8)) & 1))
      physical.push_back(i);
  }
  std::map<std::uint32_t, std::uint8_t> writes;
  std::set<std::tuple<std::string, std::string, std::string>> seen;
  for (const auto& edit : edits)
  {
    const auto first = Name(edit.first), last = Name(edit.last);
    const auto [low, high] = Limits(edit.field);
    const auto ascii = [](const std::string& text) {
      return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c < 127; });
    };
    if ((!ascii(edit.first) || !ascii(edit.last) || (first.empty() && last.empty())) ||
        low > high || edit.value < low || edit.value > high || edit.expected.empty() ||
        !std::is_sorted(edit.expected.begin(), edit.expected.end()) ||
        std::adjacent_find(edit.expected.begin(), edit.expected.end()) != edit.expected.end() ||
        !seen.emplace(first, last, edit.field).second)
      return reject("invalid or duplicate content edit");
    std::uint32_t offset = 0, width = 0;
    auto tag = edit.field;
    std::reverse(tag.begin(), tag.end());
    for (std::uint32_t i = 0; i < *memory.UInt(*table + 0x4a, 1); ++i)
    {
      const auto field = *table + 0x4c + i * 16;
      if (memory.Tag(field + 8) == tag && memory.UInt(field) == 3)
      {
        offset = *memory.UInt(field + 4);
        width = *memory.UInt(field + 12);
      }
    }
    if (!width || width > 16 || edit.value >= (std::uint32_t{1} << width))
      return reject("missing or unsupported rating descriptor");
    std::set<std::uint32_t> originals;
    for (std::size_t i = 0; i < rows->size(); ++i)
    {
      const auto& row = (*rows)[i];
      const auto row_first = String(row, "ANFP"), row_last = String(row, "ANLP");
      if (!row_first || !row_last)
        return reject("missing roster identity fields");
      // Blank unused identities cannot match a requested player.
      if (Name(*row_first) != first || Name(*row_last) != last)
        continue;
      const auto value = Integer(row, tag);
      if (!value || *value < 0)
        return reject("invalid original rating");
      originals.insert(static_cast<std::uint32_t>(*value));
      for (std::uint32_t bit = 0; bit < width; ++bit)
      {
        const auto address = buffer + physical[i] * stride + (offset + bit) / 8;
        auto [it, inserted] = writes.emplace(address, (*memory.Bytes(address, 1))[0]);
        (void)inserted;
        const auto mask = static_cast<std::uint8_t>(1u << (7 - (offset + bit) % 8));
        it->second = static_cast<std::uint8_t>((it->second & ~mask) |
            (((edit.value >> (width - 1 - bit)) & 1) ? mask : 0));
      }
    }
    if (originals != std::set<std::uint32_t>(edit.expected.begin(), edit.expected.end()))
      return reject("player absent or original ratings differ: " + edit.first + " " + edit.last);
  }
  return writes;
}
}
