// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Allowlisted authored Pickup order patches, GN7E69 disc 0 rev 0.
// Generated from NFLStreet tools/draftmodes.py by tools/export_draft_recipe.py.
// Only fixed recipes are accepted; no executable bytes come from a bundle.
#pragma once
#include <array>
#include <map>
#include <optional>
#include <string>
#include "Core/NSR/NSRGameStatsRead.h"

namespace NSRContent
{
inline constexpr std::array<std::uint32_t, 25> DRAFT_SNAKE{{
  0x816d9768, 0x812d900c, 0x2c09000f, 0x40820050, 0x892b0014, 0x2c090002,
  0x40820044, 0x892b0016, 0x2c090007, 0x40820038, 0x812b0004, 0x89490000,
  0x89290001, 0x7d4a4a14, 0x394a0001, 0x554afffe, 0x892b0019, 0x880b0015,
  0x7d290278, 0x7d295278, 0x994b0015, 0x992b0019, 0x4bc095dc, 0x816d9768,
  0x4bc09550,
}};
inline constexpr std::array<std::uint32_t, 27> DRAFT_COCKY{{
  0x816d9768, 0x812d900c, 0x2c09000f, 0x40820058, 0x892b0014, 0x2c090002,
  0x4082004c, 0x892b0016, 0x2c090007, 0x40820040, 0x812b0004, 0x89490000,
  0x89290001, 0x7d4a4a14, 0x2c0a0007, 0x39400000, 0x41800008, 0x39400001,
  0x892b0019, 0x880b0015, 0x7d290278, 0x7d295278, 0x994b0015, 0x992b0019,
  0x4bc095d4, 0x816d9768, 0x4bc09548,
}};
inline constexpr std::array<std::uint32_t, 6> DRAFT_ONE{{
  0x812d900c, 0x2c09000f, 0x4082000c, 0x38600001, 0x4e800020, 0x4bc6b458,
}};
inline constexpr std::array<std::uint32_t, 6> DRAFT_TWO{{
  0x812d900c, 0x2c09000f, 0x4082000c, 0x38600000, 0x4e800020, 0x4bc6b458,
}};
inline constexpr std::array<std::uint32_t, 7> DRAFT_SELECTREADONE{{
  0x812d900c, 0x2c09000f, 0x4082000c, 0x38000001, 0x980d96ca, 0x880d96ca,
  0x4bbffda0,
}};
inline constexpr std::array<std::uint32_t, 7> DRAFT_SELECTREADTWO{{
  0x812d900c, 0x2c09000f, 0x4082000c, 0x38000000, 0x980d96ca, 0x880d96ca,
  0x4bbffda0,
}};
inline constexpr std::array<std::uint32_t, 7> DRAFT_SELECTCOMMITONE{{
  0x812d900c, 0x2c09000f, 0x4082000c, 0x38800001, 0x988d96ca, 0x888d96ca,
  0x4bbff3a0,
}};
inline constexpr std::array<std::uint32_t, 7> DRAFT_SELECTCOMMITTWO{{
  0x812d900c, 0x2c09000f, 0x4082000c, 0x38800000, 0x988d96ca, 0x888d96ca,
  0x4bbff3a0,
}};

// Plan every word before any write. Exact reapplication after a native reload
// is safe; a different or stale patch/revision fails closed. The caller commits
// ratings and executable plans only after both have validated.
inline std::optional<std::map<std::uint32_t, std::uint32_t>> PlanDraft(
    const NSRGameStats::Memory& view, std::uint32_t order, std::uint32_t first,
    std::string* error)
{
  std::map<std::uint32_t, std::uint32_t> words;
  if (order > 2 || first > 2)
  {
    *error = "invalid draft policy";
    return std::nullopt;
  }
  if (view.Tag(0x80000000) != "GN7E" || view.UInt(0x80000004) != 0x36390000)
  {
    *error = "draft requires GN7E69 disc 0 revision 0";
    return std::nullopt;
  }
  // Validate the complete cave before publishing any branch into it.
  const auto append = [&](std::uint32_t address, std::uint32_t value, std::uint32_t original) {
    const auto current = view.UInt(address);
    if (!current || (*current != original && *current != value))
      return false;
    words.emplace(address, value);
    return true;
  };
  const auto cave = [&](std::uint32_t address, const auto& recipe) {
    for (const auto value : recipe)
    {
      if (!append(address, value, 0))
        return false;
      address += 4;
    }
    return true;
  };
  bool valid = true;
  if (order)
    valid = (order == 1 ? cave(0x80411600, DRAFT_SNAKE) : cave(0x80411600, DRAFT_COCKY)) &&
            append(0x8001abac, 0x483f6a54, 0x816d9768);
  if (first)
    valid = valid && (first == 1 ? cave(0x80411700, DRAFT_ONE) : cave(0x80411700, DRAFT_TWO)) &&
            append(0x80018fc0, 0x483f8741, 0x48063bad);
  if (first)
    valid = valid && (first == 1 ? cave(0x80411740, DRAFT_SELECTREADONE) : cave(0x80411740, DRAFT_SELECTREADTWO)) &&
            (first == 1 ? cave(0x80411780, DRAFT_SELECTCOMMITONE) : cave(0x80411780, DRAFT_SELECTCOMMITTWO)) &&
            append(0x800114f4, 0x4840024c, 0x880d96ca) &&
            append(0x80010b34, 0x48400c4c, 0x888d96ca);
  if (!valid)
  {
    *error = "draft original instruction/cave mismatch";
    return std::nullopt;
  }
  return words;
}
}
