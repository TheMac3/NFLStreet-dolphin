# Native game results capture

`NSRGameStats` independently reads the results of NFL Street 2 **GN7E69,
revision 0**. It writes one `nsr.game.v1` JSON file under the running Dolphin
user directory's `GameStats/` after both **Game Stats** and **MVPs** appear.
Abandoning a match before those screens produces no file. Reborn Dolphin's
format strings established the interchange keys; none of its implementation
was copied. Real captures, memory, disassembly and emulator states stay outside
Git in NFLStreet's ignored `local/` tree.

## Observation and ownership

The exporter only reads bounded direct MEM1/fake-VMEM spans on the CPU thread.
It does not invoke guest routines, write guest memory, use MMU helpers, enter
rollback snapshots or influence the input exchange. Two HLE **Start** hooks
preserve the original native instruction and registers: one remembers the
native stats formatter's output pointers; one observes native MVP selection.
Both are suppressed during re-simulation. Instruction fingerprints and the
existing GN7E69 revision-0 rollback-image guard constrain installation.

At a live frame end, the Game Stats snapshot requires confirmed input and no
pending rollback (or the existing post-match frame-lockstep path).
Native paused frames already require confirmed input before running; the
exporter adds no wait. The final lineup/controller registry is frozen at Game
Stats. The native formatter output is copied once. A native MVP observation is
staged on the live timeline, then published when that specific frame is
confirmed, even if the user has since left MVPs. A rollback that rewinds the
observation discards it before restoring guest state. If native initialization
ran entirely during re-simulation, two consecutive live MVP frames and valid
selected DIGPs establish that initialization has completed. A new InGame epoch
or a return to the play selector after results clears the previous match. File IO occurs only
on publication. A temporary exclusive file is closed before atomic rename to
`<safe-team>_vs_<safe-team>_NNN.json`; uploaders should enumerate `*.json` only.

Optional reliable `NSRI` packets distribute names and a shared timestamp.
The existing ENet connection-to-player binding validates every sender; a star
host relays accepted identities and replays known identities to new leaves.
The payload is `NSRI`, version byte 1, pad byte, player-count byte, name-length
byte, four big-endian Unix-second bytes, then at most 64 bytes of validated
UTF-8. Only pad 0 supplies the timestamp. Identities are immutable per session.
A reliable host-only `NSRG` packet shares the host's corrected snapshot frame:
magic, version byte 1, player-count byte, two zero reserved bytes, big-endian
u32 match ordinal, big-endian u32 snapshot frame (16 bytes total). The existing
sender binding accepts it only from player 0. A bounded one-match lookahead
tolerates peers reaching transitions at different host times. It influences
only capture metadata and UTC; no input or clock agreement changes.
Missing metadata delays publication, never simulation; old peers can play but
cannot produce a complete named capture. No rollback protocol/fingerprint field
or guest digest includes this metadata.

## Interchange format

The compatible core uses `schema: "nsr.game.v1"`, `source: "nsr-netplay"`,
`captured_at` (UTC ISO timestamp), `teams: [{name, score}, {name, score}]`,
`style: [number, number]`, `team_totals: [object, object]`, `sides_verified: true`,
`per_player_matched: true`, and `players: [{name, team, side, slot, digp, stats}]`.
**`team` is a team-name string.** Side and slot are zero based, in the game's
native order, which need not be screen left-to-right. Names are converted from
the game's Windows-1252 bytes to UTF-8. Player entries cover the seven final
field slots per team; DIGP is native roster identity, not a user identifier.

The fifteen exact native displayed totals are `score`, `style`, `total_td`,
`rush_td`, `pass_td`, `def_td`, `sacks`, `safeties`, `int`, `ff`,
`first_downs`, `gamebreakers`, `gb_2pt`, `hot_spots`, `credits`. Team rows also
provide `fumble_rec` and `thrown_int`. Player counters are `pass_td`, `rush_td`,
`rec_td`, `thrown_int`, `fumbles`, `pick_six`, `fum_td`, `interceptions`,
`forced_fumbles`, `fumble_rec`, `tackles`, `sacks`. Reborn's short team key is
`ff`; only player lines use `forced_fumbles` (`strings -n 2` reveals the short
key). Absent event rows mean zero;
missing schemas, invalid fields, duplicate event IDs or unmatched identities
suppress the export. Unsupported hidden/non-numeric scores suppress export
rather than turning a negative native formatter sentinel into a huge score.
Time-to-win text is not a numeric `score` and is not exported as one.

`reborn_match` retains `schema: "nsr.reborn_match.v1"`, `netplay_running`,
`game_id: "GN7E69"`, and `runtime_version`. Its `local_pads` is empty and
`local_team_index` is null: a byte-identical shared capture cannot describe a
different local owner on every peer. Consumers should use the global ownership
map below for nsr-netplay files.

Additions:

- `nsr_netplay`: `room` from NSR_ROOM, `session_id` from
  NSR_TELEMETRY_SESSION, `seed` from NSR_SEED (string), `match_frame`,
  `capture_frame`, `match_ordinal`, and `pads: [{pad, name, registry_team}]`.
  `pad` is the global GameCube controller index; `registry_team` is native side
  0 or 1. CPU/unassigned pads are omitted. Each peer's NSR_PLAYER_NAME is
  distributed through NSRI. Empty names remain empty; the exporter invents no
  user identity. Consumers can deduplicate with session/seed/match identity.
- `mvps`: the two players actually selected by the native **MVPs** screen,
  including the player fields above, `selection: "native_mvp_screen"`, and
  `scope: "team"`. The game selects one per team, not one overall winner.
- `mvp`: the winning team's native selection with `scope: "winning_team"`;
  null for a tied score. This is a labeled convenience view of `mvps`.

`captured_at` uses the host's shared session-start Unix second plus the agreed
host's confirmed Game Stats snapshot frame (`capture_frame`) divided by 60. It represents
emulated elapsed time; peer clocks,
pauses and network waits do not change file bytes. Filenames use each user's
available sequence number and can differ when directories have different
history. Identical builds/settings/session environment produce identical JSON
contents. NSR_ROOM/NSR_TELEMETRY_SESSION must describe the same session on all
peers, as the launcher already arranges.

## Reverse-engineering evidence

All addresses below are **GN7E69 revision 0**, obtained from the game DOL using
`tools/dol.py`/Capstone, then checked with private RAM/VMEM and native rendered
results. These are observations about the game, not Reborn Dolphin internals.

| Structure | Address / native routine | Evidence and meaning |
| --- | --- | --- |
| Small-data base | r13 = `0x803F22A0` | Startup code; older `0x803F02A0` is incorrect |
| Database root | `*(0x803EBE88)`, `fn_801F88F0` | Database next/+4 table list/+8 tag; table next +4/tag +0x1C |
| Packed table | `fn_8020C034` | Buffer +0x14, stride +0x20, deletion bit +0x24, capacity/allocated/high-water u16 +0x40/+0x42/+0x44, field count byte +0x4A; fields at +0x4C, 16 bytes each: type/bit offset/tag/width |
| Match tables | `EMAG` | Reversed GAME tag; `MAET` team names ANDT; `AGLP` player ANFP/ANLP/DIGP/DIGT; `FOGP` offense; `EDGP` defense; `AGST` team totals |
| Active players | `*(0x803EA3F8)`, `fn_80039F5C` | Buffer +0, width/total u16 +8/+10 = 7/14; stride 0x14D8, DIGP u16 +0xB5C |
| Native team IDs | `0x803EC07C`, `0x803EC080` | Team getters and lineup attribution |
| Controller registry | `*(0x803EAB60)`, `fn_800B6644` | Buffer +0/count byte +4; 12-byte records, team byte +0 (FF unassigned); ten registry slots are not ten physical pads |
| Physical pad map | `0x802EACAC`, `fn_80188044` | Ten u32 entries reverse registry slot to physical port; FFFFFFFF unused |
| Frontend | `*(0x803EB688)`, `fn_80219650` | Index +0x3C; array +0x48, stride 0x14, group/screen u16 +4/+6; 3/6 Game Stats, 3/7 MVPs, 3/2 play selector; 1/20 is a feat popup |
| Displayed stats | `0x80055F4C`, instruction `0x9421FF80` | r3 team ID pair, r5 side-0 output, r4 side-1 output, each fifteen u32 cells in key order above; read after native call completes |
| Ordinary score | `*(0x803EB444)+0x3C/+0x3E`, `fn_801787DC` | u16 scores independently corroborate displayed cells |
| Style points | `*(0x803EACD4)+0x1A68/+0x1A6C`, `fn_800D41F8` | Independently corroborates displayed style cells |
| MVP selection | `fn_8005ADCC`; observer `0x8005AE44`, instruction `0x4BFFFE85` | r31 side after calculation; result `0x80309FC4 + side*0x90`, float score +0 / DIGP u32 +4; both sides are calculated on entry |

Per-player native tags: FOGP dtag/dtug/dtcg/niag/ufug map to
pass_td/rush_td/rec_td/thrown_int/fumbles. EDGP
tisg/tflg/nisg/fflg/rflg/atdg/kslg map to
pick_six/fum_td/interceptions/forced_fumbles/fumble_rec/tackles/sacks.
AGST rfst/ipst map to fumble_rec/thrown_int. These tagged columns were checked
against native stats/feat descriptors and live rows. The formatter remains the
authority for displayed team counters, including credits/GameBreakers.

## Validation and deferred coverage

Socket-free tests in `Tools/NSR/NSRRollbackNetplayTest.cpp` exercise synthetic
MSB-packed fields, signed values, deletion, cycles, bounds, overlapping columns,
required schemas, duplicate event IDs, NSRI bounds/version/UTF-8/timestamps,
and NSRG framing/endianness/version/ordinal validation.
No game fixtures are tracked. Run the normal rollback/reference RAM gates,
two-human proxy gate and `tools/regress.py --quick` in the owning NFLStreet repo.
Qualification details and private screenshots/JSON are in the gamestats report.

Revisit when supporting a different disc revision, non-numeric challenge scores,
bench/roster substitutions, old peers without NSRI, or a launcher with different
session IDs on each peer. A Windows release build should run the same capture
qualification; Linux builds and MSVC project entries do not substitute for that.
