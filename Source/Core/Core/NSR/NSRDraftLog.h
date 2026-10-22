// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Passive Pickup Game draft recorder for NFL Street 2 GN7E69 revision 0.
//
// Purpose: the post-game capture (nsr.game.v1) names each player's native DIGP
// and field slot, but neither records the draft: DIGP is the team's native
// lineup order (the quarterback is first) and the slot is the player's spot in
// the final formation. Draft order exists only while the Pickup draft screen
// owns its pool, which is freed before the match starts. This header reads that
// pool at live frame ends and turns each accepted pick into an ordered record.
//
// Design: header-only, no emulator/filesystem/game-call dependency, reads only
// through the bounded NSRGameStats::Memory view, so synthetic RAM exercises
// the exact live code in Tools/NSR/NSRRollbackNetplayTest.cpp. It never writes
// guest memory, never runs during re-simulation (the adapter guarantees this)
// and contributes nothing to inputs, snapshots or digests.
//
// GN7E69 rev 0 evidence (tools/dol.py + Capstone on the owned DOL, then live
// RAM observation of a scripted two-human Pickup draft; r13 = 0x803F22A0):
// - fn_8001AA8C(id, slot_out) commits one pick. It locates the pool record via
//   fn_800172C4 (base *(r13-0x6894) = *0x803EBA0C, count *(r13-0x6890) =
//   *0x803EBA10, stride 0x18, +0 player id) and stores: +4 = the active team's
//   count before the increment (that team's 0-based pick number), +0x14 = the
//   active team (table +0x19), +0x15 = the active drafter slot (table +0x15).
//   Unpicked records hold -1 at +4. The id equals the roster PLAY.PGID.
// - Draft table *(r13-0x6898) = *0x803EBA08: +4 drafter count-array pointer,
//   +8 controller handle bytes per drafter (fn_80018F9C stores each from
//   fn_8007B090(team), an EMAG controller-table lookup), +0x0C/+0x0D picks per
//   team, +0x14 drafter count (2 in Pickup), +0x16 list capacity (7).
// - 0x803EB2AC == 15 identifies Pickup (getter 0x801486A0).
// - The ten-word map at 0x802EACAC turns a controller registry slot into a
//   physical pad (fn_80188044). A handle is treated as a registry slot; the
//   caller only trusts the result when that pad is registered on the drafting
//   team, so a different handle meaning degrades to "pad unknown", not a lie.
//
// Deferred: picks that complete between two observed frames (impossible for
// human input at 60 Hz, but a CPU auto-pick burst could) are kept with
// complete=false rather than guessed. Revisit if CPU drafters are qualified.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "Core/NSR/NSRGameStatsRead.h"

namespace NSRGameStats
{
inline constexpr std::uint32_t DRAFT_MODE_ADDRESS = 0x803eb2ac;
inline constexpr std::uint32_t DRAFT_MODE_PICKUP = 15;
inline constexpr std::uint32_t DRAFT_TABLE_ADDRESS = 0x803eba08;
inline constexpr std::uint32_t DRAFT_POOL_ADDRESS = 0x803eba0c;
inline constexpr std::uint32_t DRAFT_POOL_COUNT_ADDRESS = 0x803eba10;
inline constexpr std::uint32_t PAD_MAP_ADDRESS = 0x802eacac;
inline constexpr std::uint32_t DRAFT_TEAM_SIZE = 7;

// One picked pool record as the native draft left it.
struct DraftRecord
{
  std::uint32_t pgid;
  std::uint8_t team, drafter, team_pick;
};

// One coherent frame-end view of a Pickup draft in progress.
struct DraftView
{
  std::uint32_t total = 0;
  std::array<std::uint8_t, 2> controllers{};
  std::array<std::int32_t, 2> controller_pads{-1, -1};
  std::vector<DraftRecord> picked;
};

// An accepted pick in overall order. `pad` is the handle's pad candidate; the
// capture confirms it against the final controller registry before export.
struct DraftPick
{
  std::uint32_t pick, pgid;
  std::uint8_t team, drafter, round, controller;
  std::int32_t pad;
};

// Read the Pickup draft, or nothing when it is absent, freed or mid-update.
// Every structural invariant the native commit maintains is re-checked, so a
// stale pool from an earlier mode or a torn allocation is never accepted.
inline std::optional<DraftView> ReadDraft(const Memory& memory)
{
  if (memory.UInt(DRAFT_MODE_ADDRESS) != DRAFT_MODE_PICKUP)
    return std::nullopt;
  const auto table = memory.UInt(DRAFT_TABLE_ADDRESS);
  if (!table || !memory.Bytes(*table, 0x1a))
    return std::nullopt;
  const auto drafters = *memory.UInt(*table + 0x14, 1);
  const auto capacity = *memory.UInt(*table + 0x16, 1);
  const auto counts = memory.UInt(*table + 4);
  const auto handles = memory.UInt(*table + 8);
  if (drafters != 2 || capacity != DRAFT_TEAM_SIZE || !counts || !handles ||
      !memory.Bytes(*counts, 2) || !memory.Bytes(*handles, 2))
    return std::nullopt;
  const std::array<std::uint32_t, 2> team_counts{*memory.UInt(*table + 0x0c, 1),
                                                 *memory.UInt(*table + 0x0d, 1)};
  const std::array<std::uint32_t, 2> drafter_counts{*memory.UInt(*counts, 1),
                                                    *memory.UInt(*counts + 1, 1)};
  const auto pool = memory.UInt(DRAFT_POOL_ADDRESS);
  const auto length = memory.UInt(DRAFT_POOL_COUNT_ADDRESS);
  if (!pool || !length || *length < 2 * DRAFT_TEAM_SIZE || *length > 1024 ||
      !memory.Bytes(*pool, *length * 0x18) || team_counts[0] > DRAFT_TEAM_SIZE ||
      team_counts[1] > DRAFT_TEAM_SIZE ||
      drafter_counts[0] + drafter_counts[1] != team_counts[0] + team_counts[1])
    return std::nullopt;

  DraftView view;
  view.total = team_counts[0] + team_counts[1];
  std::array<std::uint32_t, 2> seen_teams{};
  std::set<std::uint32_t> ids;
  for (std::uint32_t index = 0; index < *length; ++index)
  {
    const auto record = *pool + index * 0x18;
    const auto team_pick = *memory.UInt(record + 4);
    if (team_pick == 0xffffffff)
      continue;
    const DraftRecord picked{*memory.UInt(record), static_cast<std::uint8_t>(*memory.UInt(record + 0x14, 1)),
                             static_cast<std::uint8_t>(*memory.UInt(record + 0x15, 1)),
                             static_cast<std::uint8_t>(team_pick)};
    if (team_pick >= DRAFT_TEAM_SIZE || picked.team > 1 || picked.drafter > 1 ||
        !ids.insert(picked.pgid).second)
      return std::nullopt;
    ++seen_teams[picked.team];
    view.picked.push_back(picked);
  }
  if (seen_teams != team_counts)
    return std::nullopt;
  for (std::uint32_t drafter = 0; drafter < 2; ++drafter)
  {
    view.controllers[drafter] = static_cast<std::uint8_t>(*memory.UInt(*handles + drafter, 1));
    const auto pad = view.controllers[drafter] < 10 ?
                         memory.UInt(PAD_MAP_ADDRESS + view.controllers[drafter] * 4) :
                         std::nullopt;
    if (pad && *pad < 4)
      view.controller_pads[drafter] = static_cast<std::int32_t>(*pad);
  }
  return view;
}

// Ordered draft history across frame-end views. Only one-pick increments are
// ordered evidence; anything else (a gap, a shrinking pool, a restarted draft)
// either starts over or is kept but marked incomplete.
class DraftLog
{
public:
  void Observe(const DraftView& view)
  {
    if (view.total == m_total && view.picked.size() == m_picks.size())
      return;
    if (view.total < m_total || view.total == 0)
      Clear();
    if (view.total == m_total)
      return;
    std::set<std::uint32_t> known;
    for (const auto& pick : m_picks)
      known.insert(pick.pgid);
    std::vector<DraftRecord> added;
    for (const auto& record : view.picked)
    {
      if (!known.contains(record.pgid))
        added.push_back(record);
    }
    if (view.total != m_total + 1 || added.size() != 1)
    {
      // Missed or simultaneous picks: keep their team-relative facts only.
      m_gap = true;
      for (const auto& record : added)
        Append(record, view, 0);
    }
    else
    {
      Append(added.front(), view, view.total);
    }
    m_total = view.total;
  }

  // A complete draft is fourteen picks, each observed alone, seven per team.
  bool Complete() const
  {
    return !m_gap && m_picks.size() == 2 * DRAFT_TEAM_SIZE;
  }
  const std::vector<DraftPick>& Picks() const { return m_picks; }
  void Clear()
  {
    m_picks.clear();
    m_total = 0;
    m_gap = false;
  }

private:
  void Append(const DraftRecord& record, const DraftView& view, std::uint32_t pick)
  {
    m_picks.push_back(DraftPick{pick, record.pgid, record.team, record.drafter,
                                static_cast<std::uint8_t>(record.team_pick + 1),
                                view.controllers[record.drafter],
                                view.controller_pads[record.drafter]});
  }

  std::vector<DraftPick> m_picks;
  std::uint32_t m_total = 0;
  bool m_gap = false;
};

// Databases that own a table tag, in load order. The global roster PLAY table
// ("YALP") is not part of the per-match EMAG database, and older per-team
// databases may carry their own PLAY tables, so callers verify the candidate
// (every drafted PGID must resolve) instead of trusting the first hit.
inline std::vector<std::string> DatabasesWithTable(const Memory& memory, std::string_view table)
{
  std::vector<std::string> result;
  auto db = memory.UInt(0x803ebe88);
  for (unsigned i = 0; db && *db && i < 64; ++i)
  {
    if (!memory.Bytes(*db, 0x14))
      break;
    const auto tag = memory.Tag(*db + 8);
    if (tag && FindTable(memory, *tag, table))
      result.push_back(*tag);
    db = memory.UInt(*db);
  }
  return result;
}
}  // namespace NSRGameStats
