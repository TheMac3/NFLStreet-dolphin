// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Read-only GN7E69 revision-0 play-call privacy policy. The result is host
// presentation metadata only: never serialize it into input or rollback state.
#pragma once

#include <array>
#include <cstdint>

#include "Core/NSR/NSRGameStatsRead.h"

namespace NSRPlayCover
{
struct State
{
  std::uint32_t hidden = 0;
  std::uint32_t locked = 0;
  bool operator==(const State&) const = default;
};

struct Region
{
  float left, top, right, bottom;
};

// Native two-team play-call panels are tied to team indices, not possession
// or controller order: native team 1 is left, team 0 is right.
// Coordinates are relative to the presented game image.
inline Region SideRegion(std::uint32_t side)
{
  return side == 1 ? Region{0.0f, 0.0f, 0.5f, 1.0f} : Region{0.5f, 0.0f, 1.0f, 1.0f};
}

// Resolve *every* local pad through the same native registry as stats capture.
// Co-op partners see their shared side; a PC hosting both sides needs no cover.
// Invalid/ambiguous mappings never grant visibility to a remote side.
inline std::uint32_t LocalSides(const NSRGameStats::Memory& memory, std::uint32_t local_pads)
{
  const auto registry = memory.UInt(0x803eab60);
  if (!registry || !memory.Bytes(*registry, 8))
    return 0;
  const auto base = *memory.UInt(*registry);
  const auto count = *memory.UInt(*registry + 4, 1);
  if (count > 10 || !memory.Bytes(base, count * 12) || !memory.Bytes(0x802eacac, 40))
    return 0;
  std::array<std::uint32_t, 4> teams{};
  for (std::uint32_t index = 0; index < count; ++index)
  {
    const auto pad = *memory.UInt(0x802eacac + index * 4);
    const auto side = *memory.UInt(base + index * 12, 1);
    if (pad < 4 && side < 2)
      teams[pad] |= 1U << side;
  }
  std::uint32_t sides = 0;
  for (std::uint32_t pad = 0; pad < 4; ++pad)
    if ((local_pads & (1U << pad)) && (teams[pad] == 1 || teams[pad] == 2))
      sides |= teams[pad];
  return sides;
}

// Flow 1 is selection and 5 is its lock-in transition; 2 is lineup, 3 the snap.
// UI +42/+43 are set by fn_800577DC when each side commits its play (including
// flip at +4A/+4B). Read the complete allocation extent before using any fields.
inline State Read(const NSRGameStats::Memory& memory, std::uint32_t local_pads)
{
  const auto flow = memory.UInt(0x803eaad4);
  if (!flow || !memory.Bytes(*flow, 4))
    return {};
  const auto phase = *memory.UInt(*flow);
  if ((phase != 1 && phase != 2 && phase != 5) ||
      (phase == 5 && memory.UInt(*flow + 4) != 1U))
    return {};
  const auto opponents = 3U & ~LocalSides(memory, local_pads);
  // Native fn_8017DD68/8017DDDC toggles per-team audible menus at these bytes.
  // They can expose calls during lineup; ordinary lineup remains uncovered.
  if (phase == 2)
  {
    State audible;
    for (std::uint32_t side = 0; side < 2; ++side)
      if (memory.UInt(0x803eb48c + side, 1) == 1U)
        audible.hidden |= opponents & (1U << side);
    return audible;
  }
  const auto ui = memory.UInt(0x803ea58c);
  if (!ui || !memory.Bytes(*ui, 0x54))
    return {};
  State result;
  result.hidden = opponents;
  for (std::uint32_t side = 0; side < 2; ++side)
    if (memory.UInt(*ui + 0x42 + side, 1) == 1U)
      result.locked |= 1U << side;
  // Four-column selection is the native full-width human-vs-CPU layout.
  // In same-side co-op the shared human book occupies the whole screen;
  // masking the CPU half would hide teammates' own cards. A spectator/local
  // unmapped pad instead covers the entire visible opponent book.
  if (memory.UInt(*ui + 0x4e, 1) == 4U)
  {
    const auto humans = LocalSides(memory, 15);
    if (humans == 1 || humans == 2)
    {
      result.hidden = (humans & opponents) ? 3U : 0U;
      result.locked = (result.locked & humans) ? 3U : 0U;
    }
  }
  return result;
}
}  // namespace NSRPlayCover
