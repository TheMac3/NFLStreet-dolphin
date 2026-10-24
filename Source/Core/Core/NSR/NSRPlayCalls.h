// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Passive GN7E69 revision-0 snap reader and rollback-aware host journal. All
// inputs are bounded guest RAM views; no guest calls, writes or emulator state.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Core/NSR/NSRGameStatsRead.h"

namespace NSRGameStats
{
struct PlayCall
{
  std::uint32_t side = 0, play_id = 0, slot_id = 0;
  std::optional<std::uint32_t> formation_id, category_id;
  std::string book, play_name;
  std::optional<std::string> formation_name, category_name;
  std::optional<bool> flipped;
};

struct PlayEvent
{
  std::uint32_t frame = 0, code = 0, sub = 0, actor = 0, other = 0;
  std::optional<float> x, z;
};

struct PlayResult
{
  std::uint32_t frame = 0, kind = 0, result = 0, flags = 0, state_flags = 0;
  int scoring = 0;
  std::optional<float> x, z;
};

struct PlayRoster
{
  std::uint32_t id = 0, digp = 0, side = 0, slot = 0;
};

struct Snap
{
  std::uint32_t frame = 0, offense = 0, down = 0;
  std::array<std::uint32_t, 2> score{};
  std::optional<float> x, z, line, goal;
  std::array<std::optional<PlayCall>, 2> calls;
  std::array<int, 2> pads{-1, -1};
  std::vector<PlayRoster> roster;
  std::vector<PlayEvent> events;
  std::optional<PlayResult> result;
  std::optional<std::uint32_t> events_dropped_from;
};

// Reject nonfinite or implausible field coordinates rather than creating bad JSON.
inline std::optional<float> FieldFloat(const Memory& memory, std::uint32_t address)
{
  const auto bits = memory.UInt(address);
  if (!bits)
    return std::nullopt;
  const float value = std::bit_cast<float>(*bits);
  return std::isfinite(value) && std::abs(value) <= 1000 ? std::optional{value} : std::nullopt;
}

// Native fn_80066704 fills the PLYL id/name at current+CF8/+EE8. PBPL is
// a menu occurrence, not a play identity. SETL is resolved from that exact
// book's runtime tables; duplicate keys or missing tables leave it unknown.
inline std::optional<PlayCall> ReadPlayCall(const Memory& memory, std::uint32_t side)
{
  const auto root = memory.UInt(0x803ecaf4);
  if (side > 1 || !root || !memory.Bytes(*root, 4 + 2 * 0x6974))
    return std::nullopt;
  const auto team = *root + 4 + side * 0x6974;
  const auto current = team + 0x20;
  PlayCall call;
  call.side = side;
  call.book = *memory.Tag(team + 8);
  call.play_id = *memory.UInt(current + 0xcf8);
  call.slot_id = *memory.UInt(current + 0xcfc);
  const auto name = *memory.Bytes(current + 0xee8, 28);
  const auto end = std::find(name.begin(), name.end(), 0);
  if (end == name.end() || end == name.begin())
    return std::nullopt;
  call.play_name.assign(name.begin(), end);
  const auto flipped = *memory.UInt(current + 0x10, 1);
  if (flipped <= 1)
    call.flipped = flipped != 0;
  // PBPL identifies a menu occurrence. Its PBST category is separate from
  // the play's SETL formation ("All" is a category, never a formation).
  static constexpr std::array<std::string_view, 3> slot_fields{"LPBP", "TSBP", "LYLP"};
  static constexpr std::array<std::string_view, 1> category_fields{"TSBP"};
  const auto slots = ReadTable(memory, call.book, "LPBP", slot_fields);
  if (slots && UniqueIntegerKeys(*slots, "LPBP"))
    for (const auto& row : *slots)
      if (Integer(row, "LPBP") == call.slot_id && Integer(row, "LYLP") == call.play_id)
      {
        const auto category = Integer(row, "TSBP");
        if (category && *category >= 0 && *category <= 65535)
          call.category_id = static_cast<std::uint32_t>(*category);
      }
  if (call.category_id)
  {
    const auto categories = ReadTable(memory, call.book, "TSBP", category_fields);
    if (categories && UniqueIntegerKeys(*categories, "TSBP"))
      for (const auto& row : *categories)
        if (Integer(row, "TSBP") == call.category_id)
          call.category_name = String(row, "eman");
  }
  static constexpr std::array<std::string_view, 2> play_fields{"LYLP", "LTES"};
  static constexpr std::array<std::string_view, 1> formation_fields{"LTES"};
  const auto plays = ReadTable(memory, call.book, "LYLP", play_fields);
  if (!plays || !UniqueIntegerKeys(*plays, "LYLP"))
    return call;
  for (const auto& row : *plays)
  {
    if (Integer(row, "LYLP") != call.play_id)
      continue;
    const auto formation = Integer(row, "LTES");
    if (formation && *formation >= 0 && *formation <= 65535)
      call.formation_id = static_cast<std::uint32_t>(*formation);
  }
  if (!call.formation_id)
    return call;
  const auto formations = ReadTable(memory, call.book, "LTES", formation_fields);
  if (!formations || !UniqueIntegerKeys(*formations, "LTES"))
    return call;
  for (const auto& row : *formations)
  {
    if (Integer(row, "LTES") == call.formation_id)
      call.formation_name = String(row, "eman");
  }
  return call;
}

// Called only at the instruction that commits native event 3 (the snap).
// Scrimmage coordinates already face +z: native FlipSides flips them on possession
// changes. Down 6 is the native conversion mode; exported down 0 preserves the
// nsr.play.v1 convention. Unknown/nonstandard modes remain unobserved.
inline std::optional<Snap> ReadSnap(const Memory& memory, std::uint32_t frame)
{
  const auto scrimmage = memory.UInt(0x803eb444);
  if (!scrimmage || !memory.Bytes(*scrimmage, 0xb4))
    return std::nullopt;
  Snap snap;
  snap.frame = frame;
  snap.offense = *memory.UInt(*scrimmage + 0x38, 1);
  const auto down = *memory.UInt(*scrimmage + 0x30);
  if (snap.offense > 1 || (down != 6 && (down < 1 || down > 4)))
    return std::nullopt;
  snap.down = down == 6 ? 0 : down;
  snap.score = {*memory.UInt(*scrimmage + 0x3c, 2), *memory.UInt(*scrimmage + 0x3e, 2)};
  snap.x = FieldFloat(memory, *scrimmage + 0xc);
  snap.z = FieldFloat(memory, *scrimmage + 0x10);
  snap.line = FieldFloat(memory, *scrimmage + 8);
  snap.goal = FieldFloat(memory, *scrimmage + 0x90);
  for (std::uint32_t side = 0; side < 2; ++side)
    snap.calls[side] = ReadPlayCall(memory, side);
  // A single human on the side has unambiguous ownership. Multiple registry
  // entries or co-op pads never imply that the first human called the play.
  const auto registry = memory.UInt(0x803eab60);
  if (registry && memory.Bytes(*registry, 8))
  {
    const auto base = *memory.UInt(*registry);
    const auto count = *memory.UInt(*registry + 4, 1);
    if (count <= 10 && memory.Bytes(base, count * 12) && memory.Bytes(0x802eacac, 40))
    {
      std::array<unsigned, 2> humans{};
      for (std::uint32_t i = 0; i < count; ++i)
      {
        const auto side = *memory.UInt(base + i * 12, 1);
        const auto pad = *memory.UInt(0x802eacac + i * 4);
        if (side < 2 && pad < 4)
        {
          ++humans[side];
          snap.pads[side] = static_cast<int>(pad);
        }
      }
      for (std::uint32_t side = 0; side < 2; ++side)
        if (humans[side] != 1)
          snap.pads[side] = -1;
    }
  }
  // Native entity id is the first word of each active player; resolve event
  // actors against the exact snap lineup, not the reordered results lineup.
  const auto players = memory.UInt(0x803ea3f8);
  if (players && memory.Bytes(*players, 12))
  {
    const auto base = *memory.UInt(*players);
    if (memory.UInt(*players + 8, 2) == 7 && memory.UInt(*players + 10, 2) == 14 &&
        memory.Bytes(base, 14 * 0x14d8))
    {
      for (std::uint32_t i = 0; i < 14; ++i)
      {
        const auto address = base + i * 0x14d8;
        const auto id = *memory.UInt(address);
        const auto side = (id >> 8) & 255;
        if (id && side < 2)
          snap.roster.push_back({id, *memory.UInt(address + 0xb5c, 2), side, i % 7});
      }
    }
  }
  return snap;
}

// Observe the committed native record at fn_800B1540 (r3). Coordinates and
// ids are copied before the native ring can reuse them. No guest calls occur.
inline std::optional<PlayEvent> ReadPlayEvent(const Memory& memory, std::uint32_t address,
                                             std::uint32_t frame)
{
  if (!memory.Bytes(address, 0x1c))
    return std::nullopt;
  return PlayEvent{frame, *memory.UInt(address + 0x14, 2),
                   *memory.UInt(address + 0x16, 2), *memory.UInt(address),
                   *memory.UInt(address + 4), FieldFloat(memory, address + 0xc),
                   FieldFloat(memory, address + 0x10)};
}

// fn_8017BD70 receives the finalized native play result after its event pass.
// Preserve the raw classification as well as its validated coordinates.
inline std::optional<PlayResult> ReadPlayResult(const Memory& memory, std::uint32_t address,
                                               std::uint32_t kind, std::uint32_t frame)
{
  const auto state = memory.UInt(0x803eb444);
  if (!memory.Bytes(address, 0x20) || !state || !memory.Bytes(*state, 4))
    return std::nullopt;
  return PlayResult{frame, kind, *memory.UInt(address + 4), *memory.UInt(address + 0x18),
                    *memory.UInt(*state), static_cast<std::int8_t>(*memory.UInt(address + 0x1d, 1)),
                    FieldFloat(memory, address + 8), FieldFloat(memory, address + 0xc)};
}

// The native UI hides the two-digit profile-order prefix; retain the complete
// resource string separately so the name can be audited without losing ids.
inline std::string PlayScreenName(const std::string& raw)
{
  return raw.size() > 3 && raw[0] >= '0' && raw[0] <= '9' && raw[1] >= '0' &&
                 raw[1] <= '9' && raw[2] == '_' ? raw.substr(3) : raw;
}

// Snap observations are replayed during re-simulation. Truncation happens
// before the guest restore so the corrected path replaces abandoned calls.
class PlayCalls
{
public:
  // Start one journal entry per native snap on the current simulation path.
  void Observe(Snap snap)
  {
    if (m_snaps.size() < 4096)
      m_snaps.emplace_back(std::move(snap));
    else if (!m_dropped_from)
      m_dropped_from = snap.frame;
  }
  // Events after play-over belong to the native post-play evaluator; retain
  // them until the next snap, bounded to the native per-play record capacity.
  void ObserveEvent(PlayEvent event)
  {
    if (!m_snaps.empty() && !m_dropped_from)
    {
      auto& snap = m_snaps.back();
      if (snap.events.size() < 128)
        snap.events.emplace_back(std::move(event));
      else if (!snap.events_dropped_from)
        snap.events_dropped_from = event.frame;
    }
  }
  // Finalization is separately timestamped: rewinding inside a play must
  // remove its old result even when its original snap remains in history.
  void ObserveResult(PlayResult result)
  {
    if (!m_snaps.empty() && !m_dropped_from)
      m_snaps.back().result = std::move(result);
  }
  // Remove all dependent observations on the abandoned timeline.
  void RollbackFrom(std::uint32_t frame)
  {
    std::erase_if(m_snaps, [frame](const Snap& snap) { return snap.frame >= frame; });
    if (m_dropped_from && *m_dropped_from >= frame)
      m_dropped_from.reset();
    for (auto& snap : m_snaps)
    {
      std::erase_if(snap.events, [frame](const PlayEvent& event) { return event.frame >= frame; });
      if (snap.result && snap.result->frame >= frame)
        snap.result.reset();
      if (snap.events_dropped_from && *snap.events_dropped_from >= frame)
        snap.events_dropped_from.reset();
    }
  }
  // The serializer only reads this journal after the result frame is confirmed.
  const std::vector<Snap>& Snaps() const { return m_snaps; }
  // Explicit coverage prevents a bounded journal looking like a complete game.
  bool Complete() const { return !m_dropped_from; }
private:
  std::vector<Snap> m_snaps;
  std::optional<std::uint32_t> m_dropped_from;
};
}  // namespace NSRGameStats
