// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Passive NFL Street 2 (GN7E69 rev 0) results exporter. The native frontend owns
// calculation of stats and MVP selection. We inspect bounded direct RAM/VMEM
// views, rebuild play facts during correction, then publish one confirmed
// nsr.game.v1 file per match.
#include "Core/NSR/NSRGameStats.h"

#include "Core/NSR/NSRNetplay.h"

#include <cstdarg>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/NSR/NSRDraftLog.h"
#include "Core/NSR/NSRPlayCalls.h"
#include "Core/NSR/NSRGameStatsIdentity.h"
#include "Core/NSR/NSRGameStatsRead.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace NSRGameStats
{
namespace
{
// Game-stats notices go to stderr (headless tests) and the NSR log, so a
// player's nsr-p*.log shows whether a capture was saved, skipped or failed.
// Per-frame trace output (s.trace) stays stderr-only.
[[maybe_unused]] void StatsNote(const char* format, ...)
{
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  std::fputs(buffer, stderr);
  static unsigned s_notes = 0;  // bound the log if a reason alternates per frame
  if (++s_notes > 200)
    return;
  std::string line(buffer);
  while (!line.empty() && line.back() == '\n')
    line.pop_back();
  NSRNetplay::LogLine(line);
}

using Object = picojson::object;
using Array = picojson::array;
using Json = picojson::value;

struct ReadyResult
{
  Object object;
  std::uint32_t observed_frame, ordinal;
};

struct State
{
  bool configured = false, netplay = false, was_in_game = false, saved = false, trace = false;
  std::uint32_t me = 0, players = 1, match_frame = 0, ordinal = 0;
  std::array<std::vector<std::uint8_t>, 4> identities;
  std::array<std::string, 4> names;
  std::uint64_t started_at = 0;
  std::map<std::uint32_t, std::uint32_t> capture_anchors;
  bool anchor_pending = false;
  std::array<std::array<std::uint32_t, 7>, 2> lineup{};
  std::array<std::uint32_t, 2> team_ids{};
  std::uint32_t totals_team_ids = 0;
  std::array<std::uint32_t, 2> totals_arrays{};
  std::optional<std::uint32_t> totals_frame;
  std::array<int, 4> pad_teams{-1, -1, -1, -1};
  bool have_lineup = false, stats_seen = false;
  std::uint32_t mvp_ready_mask = 0;
  std::optional<Object> capture;
  std::optional<ReadyResult> ready;
  bool mvp_screen_visible = false;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> last_screen;
  // Pickup draft history (NSRDraftLog.h). Kept across "play again" rematches,
  // which reuse the drafted rosters; a new draft restarts it at zero picks.
  DraftLog draft;
  PlayCalls play_calls;
  std::string last_capture_failure;
};
State s;

// Environment values are metadata only, and never feed emulation or state hashes.
std::string Environment(const char* key)
{
  const auto* value = std::getenv(key);
  return value ? value : "";
}

// Direct views avoid MMIO and MMU helpers entirely, including their caches.
Memory View(const Core::CPUThreadGuard& guard)
{
  auto& memory = guard.GetSystem().GetMemory();
  return {{memory.GetRAM(), memory.GetRamSizeReal()},
          {memory.GetFakeVMEM(), memory.GetFakeVMEM() ? memory.GetFakeVMemSize() : 0}};
}

// The native fn_80219650 returns the current frontend group/screen from this
// bounded descriptor array. A transient absent screen is simply not a trigger.
std::optional<std::pair<std::uint32_t, std::uint32_t>> Screen(const Memory& memory)
{
  const auto root = memory.UInt(0x803eb688);
  if (!root || !memory.Bytes(*root, 0x4c))
    return std::nullopt;
  const auto index = *memory.UInt(*root + 0x3c);
  const auto array = *memory.UInt(*root + 0x48);
  if (index >= 128 || !memory.Bytes(array, (index + 1) * 0x14))
    return std::nullopt;
  return std::pair{*memory.UInt(array + index * 0x14 + 4, 2),
                   *memory.UInt(array + index * 0x14 + 6, 2)};
}

// Cache the field-slot/DIGP and controller registry while InGame still owns
// them. Teardown can release those allocations before the results screens.
// fn_80039F5C proves the 0x14D8 player stride; DIGP is +0xB5C. fn_800B6644
// proves the 12-byte registry stride and team byte at +0 (0xFF = unassigned).
// Its ten registry slots are not physical pad numbers: fn_80188044 reverses
// the ten-word pad map at 0x802EACAC to find the registry slot for a GC port.
void ObserveLineup(const Memory& memory)
{
  const auto descriptor = memory.UInt(0x803ea3f8);
  if (!descriptor || !memory.Bytes(*descriptor, 0xc))
    return;
  const auto base = *memory.UInt(*descriptor);
  const auto width = *memory.UInt(*descriptor + 8, 2);
  const auto count = *memory.UInt(*descriptor + 10, 2);
  if (width != 7 || count != 14 || !memory.Bytes(base, 14 * 0x14d8))
    return;
  for (std::uint32_t side = 0; side < 2; ++side)
  {
    s.team_ids[side] = memory.UInt(0x803ec07c + side * 4).value_or(0xffffffff);
    for (std::uint32_t slot = 0; slot < 7; ++slot)
      s.lineup[side][slot] = *memory.UInt(base + (side * 7 + slot) * 0x14d8 + 0xb5c, 2);
  }
  const auto registry = memory.UInt(0x803eab60);
  if (registry && memory.Bytes(*registry, 8))
  {
    const auto entries = *memory.UInt(*registry);
    const auto registry_count = *memory.UInt(*registry + 4, 1);
    if (registry_count <= 10 && memory.Bytes(entries, registry_count * 12) &&
        memory.Bytes(0x802eacac, 40))
    {
      s.pad_teams.fill(-1);
      for (std::uint32_t index = 0; index < registry_count; ++index)
      {
        const auto pad = *memory.UInt(0x802eacac + index * 4);
        const auto team = *memory.UInt(entries + index * 12, 1);
        if (pad < 4 && team <= 1)
          s.pad_teams[pad] = static_cast<int>(team);
      }
    }
  }
  s.have_lineup = s.team_ids[0] != s.team_ids[1];
}

// Require a unique match key; duplicate rows are evidence of a stale/bad table,
// not permission to attribute another player's statistics to the first match.
const Row* Find(const std::vector<Row>& rows, std::string_view key, std::uint32_t id)
{
  const Row* result = nullptr;
  for (const auto& row : rows)
  {
    if (Integer(row, key) == id)
    {
      if (result)
        return nullptr;
      result = &row;
    }
  }
  return result;
}

// Fixed tag mappings come from the game's own stat display/feat descriptors,
// with method/addresses in docs/nsr-game-stats.md, not Reborn Dolphin code.
Object PlayerStats(const Row* offense, const Row* defense)
{
  Object result;
  const auto copy = [&result](const Row* row, const char* key, const char* column) {
    const auto value = row ? Integer(*row, column) : std::optional<std::int64_t>{0};
    if (value)
      result.emplace(key, Json(static_cast<double>(*value)));
  };
  for (const auto& [key, column] :
       {std::pair{"pass_td", "dtag"}, {"rush_td", "dtug"}, {"rec_td", "dtcg"},
        {"thrown_int", "niag"}, {"fumbles", "ufug"}})
    copy(offense, key, column);
  for (const auto& [key, column] :
       {std::pair{"pick_six", "tisg"}, {"fum_td", "tflg"}, {"interceptions", "nisg"},
        {"forced_fumbles", "fflg"}, {"fumble_rec", "rflg"}, {"tackles", "atdg"},
        {"sacks", "kslg"}})
    copy(defense, key, column);
  return result;
}

// Called-play identity includes the native resource name and its visible label.
Json CallJson(const std::optional<PlayCall>& call, std::uint32_t side, int pad)
{
  Object result{{"side", Json(double(side))}, {"pad", pad >= 0 ? Json(double(pad)) : Json()},
                {"book_id", Json()}, {"slot_id", Json()}, {"raw_play_name", Json()},
                {"raw_formation_name", Json()}, {"category_id", Json()}, {"category_name", Json()},
                {"play_id", Json()}, {"play_name", Json()}, {"formation_id", Json()},
                {"formation_name", Json()}, {"audible", Json()}, {"flipped", Json()}};
  if (call)
  {
    result["play_id"] = Json(double(call->play_id));
    result["slot_id"] = Json(double(call->slot_id));
    result["book_id"] = Json(call->book);
    result["play_name"] = Json(CP1252ToUTF8(PlayScreenName(call->play_name)));
    result["raw_play_name"] = Json(CP1252ToUTF8(call->play_name));
    if (call->formation_id)
      result["formation_id"] = Json(double(*call->formation_id));
    if (call->formation_name)
    {
      result["formation_name"] = Json(CP1252ToUTF8(*call->formation_name));
      result["raw_formation_name"] = Json(CP1252ToUTF8(*call->formation_name));
    }
    if (call->category_id)
      result["category_id"] = Json(double(*call->category_id));
    if (call->category_name)
      result["category_name"] = Json(CP1252ToUTF8(*call->category_name));
    if (call->flipped)
      result["flipped"] = Json(*call->flipped);
  }
  return Json(std::move(result));
}

// Resolve native actor handles only through this play's observed roster.
Json RosterIndex(const Snap& snap, std::uint32_t id)
{
  for (std::size_t i = 0; i < snap.roster.size(); ++i)
    if (snap.roster[i].id == id)
      return Json(double(i));
  return Json(-1.0);
}

// The native result classifier returns pass=1/run=2. An interception event
// takes precedence. A pass-classified play with no throw and a tackle is a
// native sack (the event evaluator's explicit sack path also returns 1).
// Missing result/spot observations stay unknown, never become zero-yard plays.
Json SummaryJson(const Snap& snap)
{
  if (!snap.result || snap.events_dropped_from)
    return Json();
  const auto& result = *snap.result;
  bool thrown = false, caught = false, incomplete = false, intercepted = false;
  unsigned changes = 0;
  Json passer(-1.0), receiver(-1.0), rusher(-1.0), interceptor(-1.0);
  bool tackled = false;
  for (const auto& event : snap.events)
  {
    switch (event.code)
    {
    case 4: rusher = RosterIndex(snap, event.actor); break;
    case 5: thrown = true; passer = RosterIndex(snap, event.actor); break;
    case 6: caught = true; receiver = RosterIndex(snap, event.actor); break;
    case 8: rusher = RosterIndex(snap, event.actor); break;
    case 20: intercepted = true; interceptor = RosterIndex(snap, event.actor); break;
    case 21: incomplete = true; break;
    case 28: tackled = true; if (!thrown) rusher = RosterIndex(snap, event.actor); break;
    case 30: ++changes; break;
    default: break;
    }
  }
  const bool turnover = (changes & 1) != 0;
  const bool touchdown = result.result == 6 && snap.down != 0;
  const bool first_down = (result.kind == 1 || result.kind == 2) && result.result == 1 &&
                          !(result.flags & 4) && !(result.state_flags & 1) && snap.down != 0;
  const bool known = snap.z && result.z && !incomplete && !intercepted && changes == 0 &&
                     (result.kind == 2 || caught || tackled);
  Json kind;
  if (snap.down == 0)
    kind = Json(std::string("conversion"));
  else if (intercepted)
    kind = Json(std::string("interception"));
  else if (thrown)
    kind = Json(std::string("pass"));
  else if (result.kind == 1 && tackled)
    kind = Json(std::string("sack"));
  else if (result.kind == 2)
    kind = Json(std::string("run"));
  auto end = result.z;
  if (touchdown && snap.goal)
    end = snap.goal;
  return Json(Object{{"kind", kind}, {"yards_known", Json(known)},
                     {"yards", known && end ? Json(double(*end - *snap.z)) : Json()},
                     {"touchdown", Json(touchdown)}, {"first_down", Json(first_down)},
                     {"turnover", Json(turnover)}, {"safety", Json(result.scoring == -2)},
                     {"complete", Json(caught)},
                     {"conversion_good", snap.down == 0 ? Json(result.scoring == 2) : Json()},
                     {"passer", passer}, {"receiver", receiver},
                     {"rusher", rusher}, {"interceptor", interceptor},
                     {"native_kind", Json(double(result.kind))},
                     {"native_result", Json(double(result.result))}});
}

// Capture runs only on a confirmed result frame. The journal has already had
// abandoned events/results removed and corrected observations replayed.
Array PlaysJson(std::uint32_t through, const Array& players)
{
  Array result;
  const auto number = [](const std::optional<float>& value) {
    return value ? Json(double(*value)) : Json();
  };
  for (const auto& snap : s.play_calls.Snaps())
  {
    if (snap.frame > through || (snap.result && snap.result->frame > through))
      continue;
    const auto off = snap.offense;
    Object calls{{"schema", Json(std::string("nsr.play_calls.v1"))},
                 {"offense", CallJson(snap.calls[off], off, snap.pads[off])},
                 {"defense", CallJson(snap.calls[1 - off], 1 - off, snap.pads[1 - off])}};
    Array roster, events;
    for (const auto& entry : snap.roster)
    {
      Json name;
      for (const auto& player : players)
        if (player.get("digp").get<double>() == entry.digp &&
            player.get("side").get<double>() == entry.side)
          name = player.get("name");
      roster.emplace_back(Object{{"name", name}, {"side", Json(double(entry.side))},
                                  {"slot", Json(double(entry.slot))},
                                  {"digp", Json(double(entry.digp))}});
    }
    unsigned changes = 0;
    for (const auto& event : snap.events)
    {
      if (event.frame > through)
        continue;
      changes += event.code == 30;
      events.emplace_back(Object{{"frame", Json(double(event.frame))},
                                 {"code", Json(double(event.code))}, {"sub", Json(double(event.sub))},
                                 {"actor", RosterIndex(snap, event.actor)},
                                 {"other", RosterIndex(snap, event.other)},
                                 {"x", number(event.x)}, {"z", number(event.z)}});
    }
    result.emplace_back(Object{
        {"schema", Json(std::string("nsr.play.v1"))}, {"seq", Json(double(result.size() + 1))},
        {"snap_frame", Json(double(snap.frame))}, {"offense_side", Json(double(off))},
        {"end_side", Json(double(off ^ (changes & 1)))}, {"down", Json(double(snap.down))},
        {"conversion", Json(snap.down == 0)},
        {"score", Json(Array{Json(double(snap.score[0])), Json(double(snap.score[1]))})},
        {"snap", Json(Object{{"x", number(snap.x)}, {"z", number(snap.z)}})},
        {"line_z", number(snap.line)}, {"goal_z", number(snap.goal)},
        {"events_complete", Json(!snap.events_dropped_from)},
        {"summary", SummaryJson(snap)}, {"roster", Json(std::move(roster))},
        {"events", Json(std::move(events))}, {"play_calls", Json(std::move(calls))}});
  }
  return result;
}

// --- Pickup draft export (docs/nsr-game-stats.md "Pickup draft") ---------------
// Attach the observed draft only when it provably produced this match's rosters:
// every pick must resolve through the roster PLAY table to exactly one exported
// player on the drafting side. A stale draft from an earlier Pickup session, a
// different mode or an unresolvable table therefore omits the field instead of
// mislabelling players. The pad is exported only when the final controller
// registry confirms that pad belongs to the drafting team.
std::optional<Object> DraftJson(const Memory& memory, const Array& players)
{
  const auto& picks = s.draft.Picks();
  if (picks.empty())
    return std::nullopt;
  // Runtime tags keep the on-disc reversed spelling: YALP = PLAY, DIGP = PGID,
  // ANFP/ANLP = PFNA/PLNA (first/last name), as in the match AGLP table.
  static constexpr std::array<std::string_view, 1> pgid_field{"DIGP"};
  std::optional<std::vector<Row>> roster;
  for (const auto& database : DatabasesWithTable(memory, "YALP"))
  {
    auto rows = ReadTable(memory, database, "YALP", pgid_field);
    if (!rows)
      continue;
    bool resolved = true;
    for (const auto& pick : picks)
      resolved = resolved && Find(*rows, "DIGP", pick.pgid) != nullptr;
    if (resolved)
    {
      roster = std::move(rows);
      break;
    }
  }
  if (!roster)
  {
    StatsNote("[NSR stats] draft omitted: drafted PGIDs not in a roster table\n");
    return std::nullopt;
  }
  Array result;
  std::set<std::size_t> used;
  for (const auto& pick : picks)
  {
    const auto* row = Find(*roster, "DIGP", pick.pgid);
    const auto first = String(*row, "ANFP");
    const auto last = String(*row, "ANLP");
    if (!first || !last)
      return std::nullopt;
    const auto name = CP1252ToUTF8(*first + " " + *last);
    std::optional<std::size_t> match;
    for (std::size_t index = 0; index < players.size(); ++index)
    {
      if (players[index].get("name").get<std::string>() == name &&
          players[index].get("side").get<double>() == pick.team)
      {
        if (match)
          return std::nullopt;
        match = index;
      }
    }
    if (!match || !used.insert(*match).second)
    {
      StatsNote("[NSR stats] draft omitted: pick %u is not in this match\n", pick.pick);
      return std::nullopt;
    }
    const bool pad_known = pick.pad >= 0 && static_cast<std::uint32_t>(pick.pad) < s.players &&
                           s.pad_teams[static_cast<std::size_t>(pick.pad)] == pick.team;
    result.emplace_back(Object{
        {"pick", pick.pick ? Json(double(pick.pick)) : Json()},
        {"round", Json(double(pick.round))},
        {"side", Json(double(pick.team))},
        {"team_index", Json(double(pick.team))},
        {"drafter", Json(double(pick.drafter))},
        {"controller", Json(double(pick.controller))},
        {"pad", pad_known ? Json(double(pick.pad)) : Json()},
        {"pgid", Json(double(pick.pgid))},
        {"name", Json(name)},
        {"digp", players[*match].get("digp")}});
  }
  return Object{{"schema", Json(std::string("nsr.draft.v1"))},
                {"mode", Json(std::string("pickup"))},
                {"complete", Json(s.draft.Complete())},
                {"picks", Json(std::move(result))}};
}

// Snapshot stable match data only after a native results screen. Empty offense
// or defense tables legitimately mean no events; unavailable tables fail closed.
std::optional<Object> Capture(const Memory& memory, std::uint32_t frame)
{
  // Trace a changed validation failure once; do not flood a friend's result
  // screen or silently substitute guessed totals when native data is missing.
  const auto unavailable = [frame](const char* reason) -> std::optional<Object> {
    if (s.last_capture_failure != reason)  // once per distinct reason
    {
      s.last_capture_failure = reason;
      StatsNote("[NSR stats] capture unavailable frame %u: %s (totals %08x %08x/%08x)\n",
                   frame, reason, s.totals_team_ids, s.totals_arrays[0], s.totals_arrays[1]);
    }
    return std::nullopt;
  };
  if (s.netplay)
  {
    for (std::uint32_t pad = 0; pad < s.players; ++pad)
    {
      if (s.identities[pad].empty())
        return unavailable("peer identity");
    }
  }
  const auto teams = ReadTable(memory, "EMAG", "MAET");
  const auto players = ReadTable(memory, "EMAG", "AGLP");
  const auto totals = ReadTable(memory, "EMAG", "AGST");
  static constexpr std::array<std::string_view, 6> offense_fields{
      "DIGP", "dtag", "dtug", "dtcg", "niag", "ufug"};
  static constexpr std::array<std::string_view, 8> defense_fields{
      "DIGP", "tisg", "tflg", "nisg", "fflg", "rflg", "atdg", "kslg"};
  const auto offense = ReadTable(memory, "EMAG", "FOGP", offense_fields);
  const auto defense = ReadTable(memory, "EMAG", "EDGP", defense_fields);
  if (!s.have_lineup || !teams || !players || !totals || !offense || !defense)
    return unavailable("native tables");
  if (!UniqueIntegerKeys(*offense, "DIGP") || !UniqueIntegerKeys(*defense, "DIGP"))
    return unavailable("duplicate stats IDs");
  Array team_json, style_json, total_json, player_json, pads;
  std::set<std::uint32_t> lineup_ids;
  std::array<std::string, 2> names;
  for (std::uint32_t side = 0; side < 2; ++side)
  {
    const auto* team = Find(*teams, "DIGT", s.team_ids[side]);
    const auto* total = Find(*totals, "DIGT", s.team_ids[side]);
    if (!team || !total)
      return unavailable("team row");
    const auto name = String(*team, "ANDT");
    if (!name || name->empty())
      return unavailable("team name");
    names[side] = CP1252ToUTF8(*name);
    // fn_80055F4C writes the exact fifteen displayed totals, including
    // GameBreaker/2-point counts and credits whose calculation belongs to the
    // game. Side 0 uses r5's array, side 1 r4's; the ID pair proves attribution.
    if (!memory.Bytes(s.totals_team_ids, 8) ||
        memory.UInt(s.totals_team_ids + side * 4) != s.team_ids[side] ||
        !memory.Bytes(s.totals_arrays[side], 15 * 4))
      return unavailable("native total buffers");
    const auto score = *memory.UInt(s.totals_arrays[side]);
    const auto points = *memory.UInt(s.totals_arrays[side] + 4);
    // A negative formatter score is a hidden/non-numeric score (e.g. a
    // time-to-win challenge), not an enormous unsigned football score.
    if (score > 0x7fffffff)
      return unavailable("non-numeric score");
    team_json.emplace_back(Object{{"name", Json(names[side])}, {"score", Json(double(score))}});
    style_json.emplace_back(double(points));
    static constexpr std::array<const char*, 15> keys{
        "score", "style", "total_td", "rush_td", "pass_td", "def_td", "sacks", "safeties",
        "int", "ff", "first_downs", "gamebreakers", "gb_2pt", "hot_spots", "credits"};
    Object stats;
    for (std::uint32_t index = 0; index < keys.size(); ++index)
      stats.emplace(keys[index], Json(double(*memory.UInt(s.totals_arrays[side] + index * 4))));
    // Additional counters are tagged match rows rather than display cells.
    for (const auto& [key, column] :
         {std::pair{"fumble_rec", "rfst"}, {"thrown_int", "ipst"}})
    {
      const auto value = Integer(*total, column);
      if (!value)
        return unavailable("native total counters");
      stats.emplace(key, Json(double(*value)));
    }
    total_json.emplace_back(std::move(stats));
    for (std::uint32_t slot = 0; slot < 7; ++slot)
    {
      const auto id = s.lineup[side][slot];
      if (!lineup_ids.insert(id).second)
        return unavailable("duplicate lineup");
      const auto* player = Find(*players, "DIGP", id);
      if (!player || Integer(*player, "DIGT") != s.team_ids[side])
        return unavailable("lineup membership");
      const auto first = String(*player, "ANFP");
      const auto last = String(*player, "ANLP");
      if (!first || !last)
        return unavailable("player names");
      player_json.emplace_back(Object{
          {"name", Json(CP1252ToUTF8(*first + " " + *last))}, {"team", Json(names[side])},
          {"side", Json(double(side))}, {"slot", Json(double(slot))}, {"digp", Json(double(id))},
          {"stats", Json(PlayerStats(Find(*offense, "DIGP", id), Find(*defense, "DIGP", id)))}});
    }
  }
  for (std::uint32_t pad = 0; pad < s.players; ++pad)
  {
    if (s.pad_teams[pad] >= 0)
      pads.emplace_back(Object{{"pad", Json(double(pad))},
                               {"registry_team", Json(double(s.pad_teams[pad]))},
                               {"name", Json(s.names[pad])}});
  }
  Object reborn{{"schema", Json(std::string("nsr.reborn_match.v1"))},
                {"netplay_running", Json(s.netplay)}, {"game_id", Json(std::string("GN7E69"))},
                {"runtime_version", Json(Common::GetScmDescStr())}, {"local_pads", Json(Array{})},
                {"local_team_index", Json()}};
  Object metadata{{"room", Json(Environment("NSR_ROOM"))},
                  {"session_id", Json(Environment("NSR_TELEMETRY_SESSION"))},
                  {"seed", Json(Environment("NSR_SEED"))},
                  {"match_frame", Json(double(s.match_frame))}, {"capture_frame", Json(double(frame))},
                  {"match_ordinal", Json(double(s.ordinal))},
                  {"play_calls_complete", Json(s.play_calls.Complete())},
                  {"pads", Json(std::move(pads))}};
  auto draft = DraftJson(memory, player_json);
  auto plays = PlaysJson(frame, player_json);
  Object result{{"schema", Json(std::string("nsr.game.v1"))},
                {"source", Json(std::string("nsr-netplay"))}, {"teams", Json(std::move(team_json))},
                {"style", Json(std::move(style_json))}, {"team_totals", Json(std::move(total_json))},
                {"sides_verified", Json(true)}, {"per_player_matched", Json(true)},
                {"reborn_match", Json(std::move(reborn))}, {"players", Json(std::move(player_json))},
                {"nsr_netplay", Json(std::move(metadata))}};
  result.emplace("plays", Json(std::move(plays)));
  if (draft)
    result.emplace("draft", Json(std::move(*draft)));
  return result;
}

// UTC comes from the host's shared session timestamp plus the agreed frame
// offset, so peer files do not differ solely due to host clocks/write timing.
std::string Timestamp(std::uint32_t frame)
{
  const std::time_t time = static_cast<std::time_t>(s.started_at + frame / 60);
  std::tm broken{};
#ifdef _WIN32
  gmtime_s(&broken, &time);
#else
  gmtime_r(&time, &broken);
#endif
  std::array<char, 32> buffer{};
  std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &broken);
  return buffer.data();
}

// Restrict filenames to a small portable alphabet; player/team names remain
// intact inside JSON. Never allow a guest name to select directories/devices.
std::string FilenamePart(const std::string& name)
{
  std::string result;
  for (const auto c : name)
  {
    if (result.size() >= 40)
      break;
    result += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ?
                  c : '_';
  }
  return result.empty() ? "Team" : result;
}

// One bounded write attempt. Exclusive creation preserves previous captures;
// a temporary extension keeps the uploader from observing a partial JSON file.
void Save(Object object, std::uint32_t frame)
{
  // Publication can wait different numbers of frames for confirmation on each
  // peer. Use the shared stats snapshot frame, not the later local write frame.
  object.at("nsr_netplay").get<Object>()["capture_frame"] = Json(double(frame));
  object.emplace("captured_at", Json(Timestamp(frame)));
  const auto& teams = object.at("teams").get<Array>();
  const auto prefix = FilenamePart(teams[0].get("name").get<std::string>()) + "_vs_" +
                      FilenamePart(teams[1].get("name").get<std::string>());
  const std::string folder = File::GetUserPath(D_USER_IDX) + "GameStats/";
  if (!File::CreateFullPath(folder))
  {
    StatsNote("[NSR stats] FAILED to create %s\n", folder.c_str());
    return;
  }
  for (unsigned index = 1; index <= 999999; ++index)
  {
    const auto path = folder + fmt::format("{}_{:03d}.json", prefix, index);
    if (File::Exists(path))
      continue;
    const auto temporary = path + ".tmp";
    File::IOFile output(temporary, "wbx");
    if (!output)
    {
      if (File::Exists(temporary))
        continue;
      StatsNote("[NSR stats] FAILED to create %s\n", temporary.c_str());
      return;
    }
    // A second emulator can acquire this temporary name just after the first
    // renamed it. Recheck under that exclusive reservation before publishing.
    if (File::Exists(path))
    {
      output.Close();
      File::Delete(temporary);
      continue;
    }
    const auto text = Json(std::move(object)).serialize(true) + "\n";
    const bool ok = output.WriteBytes(text.data(), text.size()) && output.Close();
    if (ok && File::Rename(temporary, path))
      StatsNote("[NSR stats] saved %s\n", path.c_str());
    else
    {
      output.Close();
      File::Delete(temporary);
      StatsNote("[NSR stats] FAILED to write %s\n", path.c_str());
    }
    return;
  }
  StatsNote("[NSR stats] filename sequence exhausted in %s\n", folder.c_str());
}

// Publish a frozen live observation only after that specific frame survives
// correction. No memory reads are needed here, so leaving the MVP screen does
// not lose the result while the network confirms the last few UI frames.
void PublishConfirmed(std::optional<std::uint32_t> confirmed_through)
{
  if (!s.ready || !confirmed_through || *confirmed_through < s.ready->observed_frame)
    return;
  const auto anchor = s.capture_anchors.find(s.ready->ordinal);
  if (anchor == s.capture_anchors.end())
    return;
  Save(std::move(s.ready->object), anchor->second);
  s.ready.reset();
}
}  // namespace

void Configure(std::uint32_t local_pad, std::uint32_t players, bool netplay)
{
  if (s.configured || local_pad >= players || players > 4)
    return;
  s.configured = true;
  s.me = local_pad;
  s.players = players;
  s.netplay = netplay;
  s.trace = !Environment("NSR_STATS_TRACE").empty();
  std::string name = Environment("NSR_PLAYER_NAME");
  if (!ValidIdentityName(name))
    name.clear();
  s.names[local_pad] = name;
  if (local_pad == 0)
    s.started_at = static_cast<std::uint64_t>(std::time(nullptr));
  s.identities[local_pad] = EncodeIdentity(
      {static_cast<std::uint8_t>(local_pad), static_cast<std::uint8_t>(players),
       static_cast<std::uint32_t>(s.started_at), name});
}

std::vector<std::vector<std::uint8_t>> Identities()
{
  std::vector<std::vector<std::uint8_t>> result;
  for (const auto& identity : s.identities)
  {
    if (!identity.empty())
      result.push_back(identity);
  }
  return result;
}

std::optional<std::uint32_t> IdentitySender(std::span<const std::uint8_t> packet)
{
  const auto decoded = DecodeIdentity(packet);
  if (!decoded || decoded->players != s.players)
    return std::nullopt;
  return decoded->pad;
}

bool AcceptIdentity(std::span<const std::uint8_t> packet)
{
  const auto sender = IdentitySender(packet);
  if (!sender || *sender >= s.players || *sender == s.me || !s.identities[*sender].empty())
    return false;
  const auto decoded = *DecodeIdentity(packet);
  s.identities[*sender] = {packet.begin(), packet.end()};
  s.names[*sender] = decoded.name;
  if (*sender == 0)
    s.started_at = decoded.started_at;
  return true;
}

std::vector<std::uint8_t> TakeCaptureAnchor()
{
  if (!s.anchor_pending || s.me != 0)
    return {};
  s.anchor_pending = false;
  return EncodeCaptureAnchor(
      {static_cast<std::uint8_t>(s.players), s.ordinal, s.capture_anchors.at(s.ordinal)});
}

void AcceptCaptureAnchor(std::span<const std::uint8_t> packet)
{
  const auto anchor = DecodeCaptureAnchor(packet);
  if (!s.netplay || s.me == 0 || !anchor || anchor->players != s.players)
    return;
  const bool waiting_result = s.ready && anchor->ordinal == s.ready->ordinal;
  if ((!waiting_result &&
       (anchor->ordinal < s.ordinal || anchor->ordinal - s.ordinal > 1)) ||
      (anchor->ordinal == s.ordinal && anchor->frame < s.match_frame))
    return;
  // A one-match lookahead tolerates the host reaching an agreed transition
  // before this peer. Also accept a delayed anchor for the retained result
  // after a quick rematch. First value wins; the map is pruned on entry.
  s.capture_anchors.emplace(anchor->ordinal, anchor->frame);
}

void ObserveTotals(const Core::CPUThreadGuard& guard, std::uint32_t frame)
{
  const auto& ppc = guard.GetSystem().GetPPCState();
  s.totals_team_ids = ppc.gpr[3];
  s.totals_arrays = {ppc.gpr[5], ppc.gpr[4]};
  s.totals_frame = frame;
}

void ObserveMvp(const Core::CPUThreadGuard& guard)
{
  const auto side = guard.GetSystem().GetPPCState().gpr[31];
  if (s.stats_seen && side <= 1 && !(s.mvp_ready_mask & (1u << side)))
  {
    s.mvp_ready_mask |= 1u << side;
    if (s.trace)
      std::fprintf(stderr, "[NSR stats] native MVP side %u\n", side);
  }
}

void ObserveFrame(const Core::CPUThreadGuard& guard, std::uint32_t frame, bool in_game,
                  std::optional<std::uint32_t> confirmed_through)
{
  if (!s.configured)
    Configure(0, 1, false);
  PublishConfirmed(confirmed_through);
  const bool confirmed = confirmed_through && *confirmed_through >= frame;
  const auto memory = View(guard);
  const auto screen = Screen(memory);
  const bool mvp_was_visible = s.mvp_screen_visible;
  s.mvp_screen_visible = screen == std::pair{3u, 7u};
  if (s.trace && screen != s.last_screen)
  {
    std::fprintf(stderr, "[NSR stats] frame %u screen %u/%u ingame %d confirmed %d\n", frame,
                 screen ? screen->first : 0xffff, screen ? screen->second : 0xffff,
                 in_game, confirmed);
    s.last_screen = screen;
  }
  // Pickup draft picks happen in pre-match menus: no rollback epoch exists
  // there, so a live frame end is final. Reads only; see NSRDraftLog.h.
  if (!in_game)
  {
    if (const auto view = ReadDraft(memory))
    {
      const auto before = s.draft.Picks().size();
      s.draft.Observe(*view);
      if (s.trace && s.draft.Picks().size() > before)
      {
        const auto& pick = s.draft.Picks().back();
        std::fprintf(stderr, "[NSR stats] draft pick %u round %u team %u drafter %u controller %u pgid %u\n",
                     pick.pick, pick.round, pick.team, pick.drafter, pick.controller, pick.pgid);
      }
    }
  }
  if (in_game)
  {
    // Native "play again" can reuse the InGame group; a return to its play
    // selector after a completed capture is a new game even without leave.
    if (!s.was_in_game || (s.stats_seen && screen == std::pair{3u, 2u}))
    {
      s.play_calls = PlayCalls{};
      s.last_capture_failure.clear();
      s.match_frame = frame;
      ++s.ordinal;
      s.saved = false;
      s.have_lineup = false;
      s.totals_team_ids = 0;
      s.totals_arrays = {};
      s.totals_frame.reset();
      s.stats_seen = false;
      s.mvp_ready_mask = 0;
      s.capture.reset();
      s.anchor_pending = false;
      std::erase_if(s.capture_anchors, [](const auto& entry) {
        return entry.first < s.ordinal && (!s.ready || entry.first != s.ready->ordinal);
      });
    }
    if (!s.stats_seen && (!s.have_lineup || frame % 60 == 0))
      ObserveLineup(memory);
  }
  s.was_in_game = in_game;
  if (s.saved || !s.have_lineup)
    return;
  if (confirmed && screen == std::pair{3u, 6u})
  {
    if (!s.stats_seen)
      ObserveLineup(memory);
    s.stats_seen = true;
  }
  if (confirmed && s.stats_seen && !s.capture)
  {
    s.capture = Capture(memory, frame);
    if (s.capture)
      StatsNote("[NSR stats] confirmed stats snapshot match %u frame %u\n", s.ordinal,
                   frame);
    if (s.capture && (s.me == 0 || !s.netplay))
    {
      s.capture_anchors.emplace(s.ordinal, frame);
      s.anchor_pending = s.netplay;
    }
  }
  // The selection callback may have run exclusively during re-simulation,
  // where observers are suppressed. Two consecutive live MVP frames prove
  // the native screen's initialization has finished in that case as well.
  if (!s.capture || s.ready || !confirmed_through ||
      (s.mvp_ready_mask != 3 && !mvp_was_visible) || screen != std::pair{3u, 7u})
    return;
  const auto& players = s.capture->at("players").get<Array>();
  Array mvps;
  for (std::uint32_t side = 0; side < 2; ++side)
  {
    const auto mvp = memory.UInt(0x80309fc8 + side * 0x90);
    if (!mvp)
      return;
    for (const auto& player : players)
    {
      if (player.get("side").get<double>() == side && player.get("digp").get<double>() == *mvp)
      {
        Object selected = player.get<Object>();
        selected.emplace("selection", Json(std::string("native_mvp_screen")));
        selected.emplace("scope", Json(std::string("team")));
        mvps.emplace_back(std::move(selected));
        break;
      }
    }
    if (mvps.size() != side + 1)
      return;
  }
  // The native screen names two team MVPs. It does not rank one overall MVP.
  // Keep both; the convenience mvp field explicitly selects the winning team's
  // native choice, and remains null for a tie instead of inventing a tiebreak.
  const auto& teams = s.capture->at("teams").get<Array>();
  const auto first = teams[0].get("score").get<double>();
  const auto second = teams[1].get("score").get<double>();
  Json winner;
  if (first != second)
  {
    Object selected = mvps[first > second ? 0 : 1].get<Object>();
    selected["scope"] = Json(std::string("winning_team"));
    winner = Json(std::move(selected));
  }
  s.capture->emplace("mvp", std::move(winner));
  s.capture->emplace("mvps", Json(std::move(mvps)));
  s.saved = true;
  s.ready = ReadyResult{std::move(*s.capture), frame, s.ordinal};
  StatsNote("[NSR stats] staged native MVPs match %u frame %u\n", s.ordinal, frame);
  s.capture.reset();
  PublishConfirmed(confirmed_through);
}

// Both HLE Start observations preserve registers/instructions and replay after
// rollback. Events are copied at the native commit, not at a guessed UI frame.
void ObserveEvent(const Core::CPUThreadGuard& guard, std::uint32_t frame)
{
  if (!s.was_in_game || s.stats_seen || s.saved)
    return;
  const auto memory = View(guard);
  const auto& ppc = guard.GetSystem().GetPPCState();
  const auto event = ReadPlayEvent(memory, ppc.gpr[3], frame);
  if (!event)
    return;
  if (event->code == 3)
  {
    auto snap = ReadSnap(memory, frame);
    if (!snap)
      return;
    // The event itself is the authoritative snap spot.
    snap->x = event->x;
    snap->z = event->z;
    s.play_calls.Observe(std::move(*snap));
  }
  s.play_calls.ObserveEvent(*event);
  if (s.trace && (event->code == 3 || event->code == 2))
    std::fprintf(stderr, "[NSR stats] play event %u frame %u\n", event->code, frame);
}

// Final native result already contains the game's outcome classification.
void ObservePlayResult(const Core::CPUThreadGuard& guard, std::uint32_t frame)
{
  if (!s.was_in_game || s.stats_seen || s.saved)
    return;
  const auto& ppc = guard.GetSystem().GetPPCState();
  if (auto result = ReadPlayResult(View(guard), ppc.gpr[3], ppc.gpr[4], frame))
  {
    if (s.trace)
      std::fprintf(stderr, "[NSR stats] play result frame %u kind %u result %u flags %x score %d\n",
                   frame, result->kind, result->result, result->flags, result->scoring);
    s.play_calls.ObserveResult(std::move(*result));
  }
}

void RollbackFrom(std::uint32_t frame)
{
  s.play_calls.RollbackFrom(frame);
  // Native result-screen initialization can occur solely in re-simulation.
  // Discard pointers allocated on an abandoned path, then let that same
  // initialization rebuild them on the corrected path, just like play facts.
  if (s.totals_frame && *s.totals_frame >= frame)
  {
    s.totals_team_ids = 0;
    s.totals_arrays = {};
    s.totals_frame.reset();
  }
  if (!s.ready || s.ready->observed_frame < frame)
    return;
  if (s.ready->ordinal == s.ordinal)
  {
    s.capture = std::move(s.ready->object);
    s.capture->erase("mvp");
    s.capture->erase("mvps");
    s.saved = false;
  }
  s.ready.reset();
}

void Reset()
{
  s = State{};
}
}  // namespace NSRGameStats
