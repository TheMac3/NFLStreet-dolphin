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
rollback snapshots or influence the input exchange. HLE **Start** hooks preserve the original native instruction and registers.
The totals formatter observer remembers its output pointers during both live
execution and re-simulation; rollback invalidates abandoned pointers, because
initialization may occur only on the corrected path. The MVP callback remains
live-only, with the confirmed-screen fallback described below. Snap/event and
final-result observations also replay, as described in the play-call section. Instruction fingerprints and the
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
Identity packets are accepted during the preboot agreement as well as play:
connection setup sends them immediately, and dropping them behind the boot
filter would permanently lose the peer name. This metadata adds no boot wait.
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

## Pickup draft

`players[].slot` and `players[].digp` do not describe the draft. DIGP is the
match roster key: the native lineup orders each team's seven players by the
role the game assigned after the draft (the quarterback, or whoever plays it,
is first), not by pick. The slot is the player's index in the active-player
array at Game Stats, i.e. their spot in the **final** play's formation. The
draft pool that knows the pick order is freed before the match starts.

`NSRDraftLog.h` therefore watches the native Pickup draft passively. At every
live frame end outside a match it reads the draft table and pool (no rollback
epoch exists in those menus, so each observation is final). A frame that adds
exactly one picked record is one ordered pick. CPU picks are separate frames
too. Gaps (several picks between observations), a shrinking pool or a new draft
are handled by restarting or marking the draft incomplete; nothing is guessed.

At Game Stats the observed draft is attached only if every pick resolves
through the roster `PLAY` table (runtime tags keep the reversed on-disc
spelling: `YALP`, `DIGP` = PGID -> `ANFP`/`ANLP`) to exactly one
exported player on the drafting side, so a stale draft from an earlier
Pickup session or another mode never mislabels players. Rematches ("play
again") reuse the drafted rosters and keep the draft.

```json
"draft": {"schema": "nsr.draft.v1", "mode": "pickup", "complete": true,
          "picks": [{"pick": 1, "round": 1, "side": 0, "team_index": 0,
                     "drafter": 0, "controller": 1, "pad": 1, "pgid": 116,
                     "name": "...", "digp": 1}]}
```

- `pick`: overall pick 1-14 in observed order (null only in an incomplete
  draft). `round`: the drafting team's n-th pick (1-7), for every draft order.
- `side`/`team_index`: native side, which is also the index into `teams`.
- `drafter`: native drafter slot; `controller`: its raw controller handle
  (`0xFF` = CPU drafter); `pad`: GameCube port index (0 = port 1) of the human
  who picked, exported only when the final controller registry places that
  pad on the drafting side, otherwise null.
- `pgid`: native roster id; `name`/`digp`: the exported player line it maps to.

Evidence (GN7E69 revision 0, r13 = `0x803F22A0`): `fn_8001AA8C(id, slot_out)`
commits a pick. It finds the pool record through `fn_800172C4` (base
`*0x803EBA0C`, count `*0x803EBA10`, stride 0x18, +0 id) and stores +4 = the
active team's pick count before the increment, +0x14 = active team (table
+0x19), +0x15 = active drafter (table +0x15); unpicked records keep -1 at +4.
Draft table `*0x803EBA08`: +4 drafter counts, +8 controller handles
(`fn_80018F9C` fills them from `fn_8007B090(team)`), +0x0C/+0x0D team counts,
+0x14 drafter count (2), +0x16 capacity (7). `0x803EB2AC == 15` is Pickup.
A scripted two-human Pickup draft on both peers recorded all fourteen picks in
the same order and with the same ids as an independent RAM observer
(NFLStreet `local/work/draftlog/`, private). Handles are treated as controller
registry slots mapped through `0x802EACAC`; in every observed run the mapping
was the identity, so the registry cross-check is what keeps `pad` honest.

Revisit: drafts with more than one drafter per team (none observed), CPU
auto-pick bursts within one frame (would export `complete: false`), and any
other disc revision.

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

## Passive snap and play-call observations

The playcalls branch adds `plays[]` (`nsr.play.v1`), one journal entry per
native snap, with `play_calls.schema = "nsr.play_calls.v1"`. It observes both
offense and defense selected records, without calling guest functions or
changing guest RAM, CPU registers, input, RNG, snapshots or state digests.
Native argument registers locate the RAM record and name its result kind.
The HLE **Start** hooks execute the original instructions normally.

The CPU-thread-owned journal runs during live simulation **and re-simulation**.
`RollbackFrom(frame)` removes snaps, events and finalized results at/after
that frame, including a result of a snap that happened before the rollback.
Corrected execution repopulates it. Serialization uses the existing confirmed
Game Stats snapshot/host capture anchor; predicted snapshots are not published.
The journal clears with the existing new-match stats bookkeeping and is bounded
to 4096 snaps and 128 events per snap. An unfinished play retains null summary.
Reaching the snap bound sets `nsr_netplay.play_calls_complete` false; reaching
the event bound sets that play's `events_complete` false and leaves its summary
null. Later results never overwrite the last retained snap after truncation.

Each role (`offense`, `defense`) has:

- `side`: native in-game side, matching this export's team order.
- `pad`: zero-based global pad when the registry proves exactly one human
  belongs to the side. CPU and ambiguous co-op ownership are null. This is
  unique-side ownership, not a record of which co-op button press won.
- `book_id`, `play_id` (PLYL), `slot_id` (PBPL occurrence), `play_name`
  (visible label), and `raw_play_name` (resource label with its order prefix).
- `formation_id`/`formation_name` (SETL), with `raw_formation_name` retained;
  optional `category_id`/`category_name` (PBST) are a separate menu grouping.
  A category such as All is never substituted for a formation.
- `flipped`: observed native mirror byte when it is boolean, otherwise null.
  `audible` remains null: final selection is observed at the snap, but the
  provenance of an audible versus reselecting the same play is unqualified.
  Revisit with a controlled native audible-switch probe.

No play catalog is bundled. All identities and labels come from the owner's
loaded game, and formation/category joins use that exact runtime book.
Unavailable records/fields remain null. Screen labels hide the two-digit
`NN_` profile-order prefix; the raw string allows audits and future revisions.

`plays[]` also records snap frame/spot, offense, down (native 6 becomes 0 for
a conversion), score at snap, line/goal coordinates, the snap's roster and
native events. `summary` uses the finalized native result plus those events:
run/pass/sack/interception, offensive yards when known, native first down,
touchdown, turnover, safety and conversion outcome. Yardage is the observed
normalized final spot minus the snap spot, using the goal line for a TD.
Turnovers, incompletes and unobserved spots leave yardage unknown. The site
may count a known incomplete pass as zero. A TD following a turnover remains
a touchdown event, but must not count as an offensive TD or success.
Unknown actor indexes use the `nsr.play.v1` sentinel -1.

### GN7E69 revision-0 evidence and guards

Addresses below were checked against the owned Reborn DOL with Capstone;
runtime readers use bounded MEM1/fake-VMEM spans. Navigation labels from the
local NFL Street 2 decomp were cross-checked against the instructions, rather
than copied into this implementation. Re-verify on any other image revision.

| Address/layout | Evidence/meaning |
| --- | --- |
| `0x800B1540`, expected instruction `0x7C7E1B78` | Native event commit; r3 points to the completed 0x1C-byte event. Event code 3 is the snap. |
| `0x8017BD70`, expected instruction `0x9421FFB0` | Native result/stat processor entry, after event evaluation; r3 points to the finalized result, r4 is its native kind. |
| `*(0x803ECAF4) + 4 + side*0x6974` | Loaded side playbook block, from the earlier playbooks loader research. Current selection starts at +0x20. |
| Current +0xCF8 / +0xCFC / +0xEE8 | PLYL ID / PBPL occurrence / 28-byte resource name, populated by the native play loader. |
| Current +0x10, **one byte** | Native mirror flag: `0x80168D84` reads it with lbz; `0x80168D90` writes it with stb. Position/art consumers use it to mirror. |
| Runtime PLYL → SETL, PBPL → PBST | Native IDs resolve formation and category independently within the active book. |
| `*(0x803EB444)` | Scrimmage: down+0x30, offense byte+0x38, score halfwords+0x3C/+0x3E, snap x/z+0x0C/+0x10, line+0x08, goal+0x90. |
| Native result +0x04/+0x18/+0x1D | Result, flags, signed scoring delta; finalized spot is +0x08/+0x0C. |

The native processor increments the first-down counter for kind 1/2,
result 1, no result flag 4, no scrimmage flag 1, on non-conversion downs.
Result 6 is the touchdown/goal-line case. Native scoring delta 2 identifies a
good conversion, and -2 a safety. Event 20 identifies interception, event 30
changes possession, and 5/6/21 distinguish throw/catch/incomplete. The native
evaluator's sack path returns pass kind 1 without a throw; a tackled native
run uses kind 2. All decoding remains observational; native code applies the
actual game outcome after the hook returns.

A scripted two-peer short match with both humans defending selected the same
A-card call on five snaps: PLYL20 / PBPL239 / SETL2. Both peers exported
byte-identical JSON; a separate read-only RAM observer and private extracted
book catalog corroborated the selected IDs, occurrence, labels and formation.
The five offensive calls also resolved against that catalog. One first down
and one touchdown agreed with native totals, and importing the capture into
the stats service counted each role exactly once per snap. This case exercises
ambiguous co-op attribution (null pad), not identification of its winning button
press. Private scripts, captures and exact hashes stay in the task report.

Synthetic socket-free tests cover bounded reads, malformed pointers/names,
byte-width flip decoding, unknown ownership, conversions and rollback of a
final result while retaining its earlier snap. Private qualification artifacts
and exact runtime gate results are in the owning NFLStreet task report.
Windows/two-PC, explicit audible provenance and a natural multi-match capture
soak remain release qualifications; the hooks do not alter session transitions.

## Validation and deferred coverage

Socket-free tests in `Tools/NSR/NSRRollbackNetplayTest.cpp` exercise synthetic
Pickup draft tables (ordered picks, rejected torn/stale views, restarts and gaps),
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
