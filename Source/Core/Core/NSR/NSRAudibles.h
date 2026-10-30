// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// GN7E69 rev 0 defender rush picker. All mutable control state lives in the
// trace-reserved guest cave and follows MEM1 snapshots/digests. The caller
// supplies exchanged pads once per simulation frame, including re-simulation.
// No hardware input, host time, rendering state, allocation or network is used.
#pragma once

#include <array>
#include <cstdint>
#include "Core/NSR/NSRGameStatsRead.h"

namespace NSRAudibles
{
using U32 = std::uint32_t;
using Pads = std::array<std::array<std::uint8_t, 12>, 4>;
constexpr U32 STATE = 0x80413a00, SIZE = 0x100, MAGIC = 0x41554432;
constexpr U32 RUSH = 0x82000000;
constexpr U32 CHORD = 0x18;  // processed Z + D-pad Up; unchanged by Reborn remaps
constexpr const char* VERSION = "rush-picker-v3-normal-input-captain-mirrors-rejected";
// Per-port records: mode, target, previous buttons, context, reason, count,
// applied player/sequence/old command. Reserve the last words for diagnostics.
constexpr U32 Port(U32 port) { return STATE + 16 + port * 48; }
enum Reason : U32 { Ready, Mirror, Special, Partner, Applied, Cancelled, Invalid };

struct Context
{
  U32 side = 2, captain = 4, player_base = 0, play = 0, formation = 0, key = 0;
  std::array<U32, 4> selected{};
  std::array<U32, 4> sides{2, 2, 2, 2};
};

// Resolve the native registry through its pad map, never assuming registry
// indices equal physical ports. Ambiguous mappings grant no editor ownership.
inline Context ReadContext(const NSRGameStats::Memory& m)
{
  Context c;
  const auto flow = m.UInt(0x803eaad4), match = m.UInt(0x803eb444);
  if (m.UInt(0x803eb2ac) != 15U || !flow || m.UInt(*flow) != 2U ||
      m.UInt(0x803ec5fc) != 0U || m.UInt(0x803eaa4d, 1) != 0U ||
      m.UInt(0x803eaa84, 1) != 0U ||
      !match || !m.Bytes(*match, 0x39))
    return c;
  const auto offense = *m.UInt(*match + 0x38, 1);
  if (offense > 1)
    return c;
  // Match the normal branch of native pre-snap dispatch (800BE4C8):
  // pause and its two global suppressors above, then side input mode 0.
  // Whole-play audible/prediction/other native modes retain their inputs.
  const auto input = m.UInt(0x803eaba8);
  if (!input || m.UInt(*input + (offense ^ 1U) * 0x20) != 0U)
    return c;
  const auto descriptor = m.UInt(0x803ea3f8), registry = m.UInt(0x803eab60);
  const auto book = m.UInt(0x803ecaf4);
  if (!descriptor || !m.Bytes(*descriptor, 12) || !registry ||
      !m.Bytes(*registry, 8) || !book)
    return c;
  const auto base = *m.UInt(*descriptor), width = *m.UInt(*descriptor + 8, 2);
  if (width != 7 || m.UInt(*descriptor + 10, 2) != 14U || !m.Bytes(base, 14 * 0x14d8))
    return c;
  const auto rows = *m.UInt(*registry), count = *m.UInt(*registry + 4, 1);
  if (count > 10 || !m.Bytes(rows, count * 12) || !m.Bytes(0x802eacac, 40))
    return c;
  std::array<U32, 4> seen{};
  for (U32 index = 0; index < count; ++index)
  {
    const auto pad = *m.UInt(0x802eacac + index * 4);
    if (pad >= 4)
      continue;
    ++seen[pad];
    c.sides[pad] = *m.UInt(rows + index * 12, 1);
    c.selected[pad] = *m.UInt(rows + index * 12 + 1, 1);
  }
  c.side = offense ^ 1U;
  for (U32 pad = 0; pad < 4; ++pad)
  {
    if (seen[pad] != 1)
      c.sides[pad] = 2;
    if (c.sides[pad] == c.side && c.captain == 4)
      c.captain = pad;
  }
  c.player_base = base + c.side * 7 * 0x14d8;
  const U32 selected = *book + 4 + c.side * 0x6974 + 0x20;
  c.formation = selected + 0x14;
  c.play = selected + 0xcf8;
  if (!m.Bytes(selected, 0xcf8 + 0x20c))
    return {};
  // The selected-play header plus formation capture audible/reload changes;
  // assignment edits themselves must not invalidate their own picker.
  c.key = 2166136261U;
  for (U32 i = 0; i < 0x38; ++i)
    c.key = (c.key ^ *m.UInt(c.play + i, 1)) * 16777619U;
  for (U32 i = 0; i < 0x28; ++i)
    c.key = (c.key ^ *m.UInt(c.formation + i, 1)) * 16777619U;
  return c;
}

// Conservative mirror boundary: the snap loader combines formation/play flags,
// a per-player category inversion and a formation-record slot permutation.
// Until runtime consumption is qualified for those combinations, show a reason
// and write nothing. Native special override queues also take precedence.
inline Reason Availability(const NSRGameStats::Memory& m, const Context& c, U32 slot)
{
  if (c.side > 1 || slot >= 7)
    return Invalid;
  if (m.UInt(c.formation + 0x17, 1) != 0U)
    return Mirror;
  const U32 player = c.player_base + slot * 0x14d8;
  if (m.UInt(player + 1, 1) != slot || m.UInt(player + 2, 1) != c.side)
    return Invalid;
  const auto special = m.UInt(0x803eb098);
  const auto category = m.UInt(player + 0xb62, 1);
  if (!special || !m.Bytes(*special, 15) || !category)
    return Invalid;
  if (m.UInt(*special + 14, 1) != 0U && (*category == 17 || *category == 18))
    return Special;
  const auto overrides = m.UInt(0x803eaaf4);
  if (!overrides || !m.Bytes(*overrides + 0x230 + c.side * 28, 28))
    return Invalid;
  for (U32 i = 0; i < 7; ++i)
    if (m.UInt(*overrides + 0x230 + c.side * 28 + i * 4) == player)
      return Special;
  return Ready;
}

// Consume every gameplay control until release; Start still pauses. Preserve
// connection/error bytes, center both sticks and zero analog/digital triggers.
inline void Consume(std::array<std::uint8_t, 12>& pad)
{
  pad[0] &= 0x10;  // Start = 0x1000
  pad[1] = 0;
  for (U32 i = 2; i < 6; ++i)
    pad[i] = 0;  // PADStatus stick axes are signed, with zero at center
  for (U32 i = 6; i < 10; ++i)
    pad[i] = 0;
}

// The legacy inner tick re-installs this frame's assembled pads. Replay only
// the guest-owned consumption mask there; never advance selection or latches
// twice in one frame, and never let that shim undo the frame's input filter.
inline bool FilterTick(const NSRGameStats::Memory& m, Pads& pads)
{
  if (m.UInt(STATE) != MAGIC)
    return false;
  const auto mask = m.UInt(STATE + 4);
  if (!mask || (*mask & ~15U))
    return false;
  for (U32 port = 0; port < 4; ++port)
    if (*mask & (1U << port))
      Consume(pads[port]);
  return true;
}

// Apply after the frame's snapshot and input assembly, identically on live and
// replayed frames. Writes go through the emulator's normal guest-write path so
// dirty trackers see the state. False means the reserved cave/image conflicts.
template <typename Write>
bool Step(const NSRGameStats::Memory& m, Pads& pads, Write write)
{
  if (!m.Bytes(STATE, SIZE) || m.UInt(0x800be678) != 0x4bfff49dU)
    return false;  // old image-only prototype and other hooks are incompatible
  if (m.UInt(STATE) != MAGIC)
  {
    // The prototype shares this reservation with optional isopatch trace.
    // Refuse all earlier cave code as well as occupied control-state bytes.
    for (U32 i = 0x804132c0; i < STATE; ++i)
      if (m.UInt(i, 1) != 0U)
        return false;
    for (U32 i = 0; i < SIZE; ++i)
      if (m.UInt(STATE + i, 1) != 0U)
        return false;
    write(STATE, MAGIC);
  }
  const auto c = ReadContext(m);
  U32 consume_mask = 0;
  for (U32 port = 0; port < 4; ++port)
  {
    const U32 record = Port(port);
    U32 mode = *m.UInt(record), target = *m.UInt(record + 4);
    const U32 buttons = (U32{pads[port][0]} << 8) | pads[port][1];
    const U32 previous = *m.UInt(record + 8);
    const bool neutral = (buttons & ~0x1000U) == 0 && pads[port][6] < 32 && pads[port][7] < 32;
    const bool eligible = c.side < 2 && c.sides[port] == c.side;
    if (!eligible || m.UInt(record + 12) != c.key)
    {
      mode = mode == 0 ? 0U : 3U;
      write(record + 12, eligible ? c.key : 0);
      write(record + 16, Ready);
    }
    bool consume = mode != 0;
    if (mode == 0 && eligible && previous == 0 && buttons == CHORD)
    {
      mode = 2;  // entry chord must be fully released before navigation
      target = c.selected[port] < 7 ? c.selected[port] : 0;
      write(record + 16, port == c.captain ? Ready : Partner);
      consume = true;
    }
    else if ((mode == 2 || mode == 3) && neutral)
      mode = mode == 2 && eligible && port == c.captain ? 1U : 0U;
    else if (mode == 1 && previous == 0)
    {
      if (buttons == 1 || buttons == 2)  // D-pad left/right, no face remapping
      {
        target = buttons == 1 ? (target + 6) % 7 : (target + 1) % 7;
        write(record + 16, Ready);
      }
      else if (buttons == 4 || buttons == 0x10)  // down or Z cancels
      {
        mode = 3;
        write(record + 16, Cancelled);
      }
      else if (buttons == 8 && eligible && port == c.captain)
      {
        const auto reason = Availability(m, c, target);
        write(record + 16, reason);
        if (reason == Ready)
        {
          const U32 sequence = c.play + 0x38 + target * 40;
          const U32 old = *m.UInt(sequence);
          write(sequence, RUSH);
          for (U32 offset = 4; offset < 40; offset += 4)
            write(sequence + offset, 0);
          write(record + 20, *m.UInt(record + 20) + 1);
          write(record + 24, c.player_base + target * 0x14d8);
          write(record + 28, sequence);
          write(record + 32, old);
          write(record + 16, Applied);
          mode = 3;
        }
      }
    }
    write(record, mode);
    write(record + 4, target);
    write(record + 8, buttons & ~0x1000U);
    if (consume)
    {
      Consume(pads[port]);
      consume_mask |= 1U << port;
    }
  }
  write(STATE + 4, consume_mask);
  return true;
}
}  // namespace NSRAudibles
