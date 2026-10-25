// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Native existing-play menu indirection, GN7E69 disc 0 rev 0. No play data or
// executable code comes from packs. Socket-free helpers validate the whole
// selection before the adapter writes the game's already allocated byte array.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "Core/NSR/NSRGameStatsRead.h"

namespace NSRContent
{
struct PlaybookGroup
{
  std::uint32_t entry = 0, category = 0;
  std::vector<std::uint32_t> library, slots;
};
using Playbook = std::vector<PlaybookGroup>;

// Same native GC-port registry used by NSRGameStats. Lowest participating pad
// owns a shared side, including a captain who explicitly chose the stock book.
inline std::optional<std::array<int, 4>> PlaybookTeams(const NSRGameStats::Memory& memory,
                                                     std::uint32_t players)
{
  if (players < 1 || players > 4)
    return std::nullopt;
  const auto registry = memory.UInt(0x803eab60);
  if (!registry || !memory.Bytes(*registry, 8))
    return std::nullopt;
  const auto entries = memory.UInt(*registry);
  const auto count = memory.UInt(*registry + 4, 1);
  if (!entries || !count || *count > 10 || !memory.Bytes(*entries, *count * 12))
    return std::nullopt;
  std::array<int, 4> teams{{-1, -1, -1, -1}};
  for (std::uint32_t i = 0; i < *count; ++i)
  {
    const auto pad = memory.UInt(0x802eacac + i * 4);
    const auto team = memory.UInt(*entries + i * 12, 1);
    if (!pad || !team)
      return std::nullopt;
    if (*pad < players && *team < 2)
    {
      if (teams[*pad] >= 0)
        return std::nullopt;
      teams[*pad] = static_cast<int>(*team);
    }
  }
  return teams;
}

// A valid CPU-only side has no captain. Malformed registries are distinguished
// by PlaybookTeams so the adapter can fail closed instead of playing stock.
inline std::optional<std::uint32_t> PlaybookCaptain(const NSRGameStats::Memory& memory,
                                                  std::uint32_t side, std::uint32_t players)
{
  const auto teams = PlaybookTeams(memory, players);
  if (!teams || side > 1)
    return std::nullopt;
  for (std::uint32_t pad = 0; pad < players; ++pad)
    if ((*teams)[pad] == static_cast<int>(side))
      return pad;
  return std::nullopt;  // CPU side retains its ordinary native book.
}

// Resolve PBPL occurrences to the exact native ORDER BY ord_ indices. Never
// confuse PLYL play IDs, PBPL occurrence IDs and category indices. Source counts,
// IDs and ordinals must all agree before returning even one destination byte.
inline std::optional<std::vector<std::uint8_t>> PlanPlaybook(
    const NSRGameStats::Memory& memory, const PlaybookGroup& group,
    std::uint32_t native_count, std::string* error)
{
  const auto reject = [&](const char* message) -> std::optional<std::vector<std::uint8_t>> {
    *error = message;
    return std::nullopt;
  };
  if ((group.entry != 1 && group.entry != 2) || group.slots.empty() ||
      group.slots.size() > group.library.size() || native_count > 127 ||
      native_count != group.library.size())
    return reject("invalid playbook category/count");
  using namespace NSRGameStats;
  const std::string tag = group.entry == 2 ? "1TBP" : "1DBP";
  const auto rows = ReadTable(memory, tag, "LPBP");
  if (!rows)
    return reject("missing playbook PBPL table");
  std::vector<std::pair<std::int64_t, std::uint32_t>> ordered;
  for (const auto& row : *rows)
  {
    const auto category = Integer(row, "TSBP"), id = Integer(row, "LPBP"), ordinal = Integer(row, "_dro");
    if (!category || !id || !ordinal || *id < 0 || *id > 1023 || *ordinal < 1 || *ordinal > 127)
      return reject("invalid PBPL schema/ordinal");
    if (*category == group.category)
      ordered.emplace_back(*ordinal, static_cast<std::uint32_t>(*id));
  }
  std::sort(ordered.begin(), ordered.end());
  if (ordered.size() != group.library.size())
    return reject("stale native playbook library");
  for (std::size_t i = 0; i < ordered.size(); ++i)
    if (ordered[i].second != group.library[i] || (i && ordered[i - 1].first == ordered[i].first))
      return reject("native playbook IDs/order differ");
  std::vector<std::uint8_t> indices;
  std::set<std::uint32_t> seen;
  for (const auto id : group.slots)
  {
    const auto it = std::find(group.library.begin(), group.library.end(), id);
    if (it == group.library.end() || !seen.insert(id).second)
      return reject("invalid/duplicate playbook slot");
    indices.push_back(static_cast<std::uint8_t>(it - group.library.begin()));
  }
  return indices;
}
}  // namespace NSRContent
