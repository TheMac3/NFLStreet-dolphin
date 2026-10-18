// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Passive NFL Street 2 (GN7E69 rev 0) results exporter. The native frontend owns
// calculation of stats and MVP selection. We inspect bounded direct RAM/VMEM
// views only on the live timeline, then write one nsr.game.v1 file per match.
#include "Core/NSR/NSRGameStats.h"

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
#include "Core/NSR/NSRGameStatsIdentity.h"
#include "Core/NSR/NSRGameStatsRead.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace NSRGameStats
{
namespace
{
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
  std::array<int, 4> pad_teams{-1, -1, -1, -1};
  bool have_lineup = false, stats_seen = false;
  std::uint32_t mvp_ready_mask = 0;
  std::optional<Object> capture;
  std::optional<ReadyResult> ready;
  bool mvp_screen_visible = false;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> last_screen;
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

// Snapshot stable match data only after a native results screen. Empty offense
// or defense tables legitimately mean no events; unavailable tables fail closed.
std::optional<Object> Capture(const Memory& memory, std::uint32_t frame)
{
  if (s.netplay)
  {
    for (std::uint32_t pad = 0; pad < s.players; ++pad)
    {
      if (s.identities[pad].empty())
        return std::nullopt;
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
    return std::nullopt;
  if (!UniqueIntegerKeys(*offense, "DIGP") || !UniqueIntegerKeys(*defense, "DIGP"))
    return std::nullopt;
  Array team_json, style_json, total_json, player_json, pads;
  std::set<std::uint32_t> lineup_ids;
  std::array<std::string, 2> names;
  for (std::uint32_t side = 0; side < 2; ++side)
  {
    const auto* team = Find(*teams, "DIGT", s.team_ids[side]);
    const auto* total = Find(*totals, "DIGT", s.team_ids[side]);
    if (!team || !total)
      return std::nullopt;
    const auto name = String(*team, "ANDT");
    if (!name || name->empty())
      return std::nullopt;
    names[side] = CP1252ToUTF8(*name);
    // fn_80055F4C writes the exact fifteen displayed totals, including
    // GameBreaker/2-point counts and credits whose calculation belongs to the
    // game. Side 0 uses r5's array, side 1 r4's; the ID pair proves attribution.
    if (!memory.Bytes(s.totals_team_ids, 8) ||
        memory.UInt(s.totals_team_ids + side * 4) != s.team_ids[side] ||
        !memory.Bytes(s.totals_arrays[side], 15 * 4))
      return std::nullopt;
    const auto score = *memory.UInt(s.totals_arrays[side]);
    const auto points = *memory.UInt(s.totals_arrays[side] + 4);
    // A negative formatter score is a hidden/non-numeric score (e.g. a
    // time-to-win challenge), not an enormous unsigned football score.
    if (score > 0x7fffffff)
      return std::nullopt;
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
        return std::nullopt;
      stats.emplace(key, Json(double(*value)));
    }
    total_json.emplace_back(std::move(stats));
    for (std::uint32_t slot = 0; slot < 7; ++slot)
    {
      const auto id = s.lineup[side][slot];
      if (!lineup_ids.insert(id).second)
        return std::nullopt;
      const auto* player = Find(*players, "DIGP", id);
      if (!player || Integer(*player, "DIGT") != s.team_ids[side])
        return std::nullopt;
      const auto first = String(*player, "ANFP");
      const auto last = String(*player, "ANLP");
      if (!first || !last)
        return std::nullopt;
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
                  {"match_ordinal", Json(double(s.ordinal))}, {"pads", Json(std::move(pads))}};
  return Object{{"schema", Json(std::string("nsr.game.v1"))},
                {"source", Json(std::string("nsr-netplay"))}, {"teams", Json(std::move(team_json))},
                {"style", Json(std::move(style_json))}, {"team_totals", Json(std::move(total_json))},
                {"sides_verified", Json(true)}, {"per_player_matched", Json(true)},
                {"reborn_match", Json(std::move(reborn))}, {"players", Json(std::move(player_json))},
                {"nsr_netplay", Json(std::move(metadata))}};
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
    std::fprintf(stderr, "[NSR stats] FAILED to create %s\n", folder.c_str());
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
      std::fprintf(stderr, "[NSR stats] FAILED to create %s\n", temporary.c_str());
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
      std::fprintf(stderr, "[NSR stats] saved %s\n", path.c_str());
    else
    {
      output.Close();
      File::Delete(temporary);
      std::fprintf(stderr, "[NSR stats] FAILED to write %s\n", path.c_str());
    }
    return;
  }
  std::fprintf(stderr, "[NSR stats] filename sequence exhausted in %s\n", folder.c_str());
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

void ObserveTotals(const Core::CPUThreadGuard& guard)
{
  const auto& ppc = guard.GetSystem().GetPPCState();
  s.totals_team_ids = ppc.gpr[3];
  s.totals_arrays = {ppc.gpr[5], ppc.gpr[4]};
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
  if (in_game)
  {
    // Native "play again" can reuse the InGame group; a return to its play
    // selector after a completed capture is a new game even without leave.
    if (!s.was_in_game || (s.stats_seen && screen == std::pair{3u, 2u}))
    {
      s.match_frame = frame;
      ++s.ordinal;
      s.saved = false;
      s.have_lineup = false;
      s.totals_team_ids = 0;
      s.totals_arrays = {};
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
    if (s.capture && s.trace)
      std::fprintf(stderr, "[NSR stats] confirmed stats snapshot match %u frame %u\n", s.ordinal,
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
  if (s.trace)
    std::fprintf(stderr, "[NSR stats] staged native MVPs match %u frame %u\n", s.ordinal, frame);
  s.capture.reset();
  PublishConfirmed(confirmed_through);
}

void RollbackFrom(std::uint32_t frame)
{
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
