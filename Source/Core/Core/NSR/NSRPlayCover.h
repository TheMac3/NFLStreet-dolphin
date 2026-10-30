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
// Layout describes only host geometry; audible choices occupy a different HUD area.
enum class Layout : std::uint8_t
{
  Split,
  Shared,
  Audible
};

struct State
{
  std::uint32_t hidden = 0;
  std::uint32_t locked = 0;
  Layout layout = Layout::Split;
  bool operator==(const State&) const = default;
};

struct Region
{
  float left, top, right, bottom;
};

// Native team 1 is left, team 0 right. The split-card bounds reproduce observed
// Reborn presentation (not its code/assets): 15..49% / 51..85% X, 27.5..87.5% Y.
// Audible/prediction choices sit higher on the HUD; protect that list separately.
// Shared full-width books are relevant only to explicitly covered spectators.
// All coordinates are relative to Dolphin's presented game image, not the window.
inline Region SideRegion(std::uint32_t side, Layout layout = Layout::Split)
{
  if (layout == Layout::Shared)
    return {0.05f, 0.275f, 0.95f, 0.875f};
  if (layout == Layout::Audible)
    return side == 1 ? Region{0.075f, 0.17f, 0.49f, 0.52f} :
                       Region{0.51f, 0.17f, 0.925f, 0.52f};
  return side == 1 ? Region{0.15f, 0.275f, 0.49f, 0.875f} :
                     Region{0.51f, 0.275f, 0.85f, 0.875f};
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

// Flow 5 is card selection on every down; flow 1 only prepares the initial play.
// In particular, later selections enter 5 from 4, not 1. Never gate on history.
// Reborn also suppresses its card cover while the native byte at 803EB4A0 is set.
// Flow 2 is lineup, where separate audible/prediction flags reveal call choices.
// UI +42/+43 are set by fn_800577DC when each side commits its play (including
// flip at +4A/+4B). Read the complete allocation extent before using any fields.
inline State Read(const NSRGameStats::Memory& memory, std::uint32_t local_pads)
{
  const auto flow = memory.UInt(0x803eaad4);
  if (!flow || !memory.Bytes(*flow, 4))
    return {};
  const auto phase = *memory.UInt(*flow);
  if (phase != 2 && phase != 5)
    return {};
  const auto opponents = 3U & ~LocalSides(memory, local_pads);
  // Native fn_8017DD68/8017DDDC toggles per-team audible menus at these bytes.
  // Prediction submenu fn_8017DFA8/8017E01C uses the next per-team array.
  // Both can expose choices during lineup; ordinary lineup remains uncovered.
  if (phase == 2)
  {
    State audible;
    audible.layout = Layout::Audible;
    for (std::uint32_t side = 0; side < 2; ++side)
      if (memory.UInt(0x803eb48c + side, 1) == 1U ||
          memory.UInt(0x803eb490 + side, 1) == 1U)
        audible.hidden |= opponents & (1U << side);
    return audible;
  }
  if (memory.UInt(0x803eb4a0, 1) != 0U)
    return {};
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
      result.layout = Layout::Shared;
      result.hidden = (humans & opponents) ? 3U : 0U;
      result.locked = (result.locked & humans) ? 3U : 0U;
    }
  }
  return result;
}
}  // namespace NSRPlayCover
