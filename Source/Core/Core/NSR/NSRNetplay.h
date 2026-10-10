// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// NSRNetplay: delay-based lockstep input exchange for NFL Street 2 (GN7E69),
// POC milestone 1 of the NFLStreet project.
//
// The game side (an in-game patch built by NFLStreet's tools/isopatch.py
// `netplay` patch) calls a stub address once per VI retrace from its pad
// poll, with r3 = PADStatus[4] buffer and r4 = retrace counter. Dolphin
// replaces that stub with NSRNetplay::Hook (an HLE "Replace" hook), which
// exchanges pads with the peer and writes the agreed pads into the buffer.
//
// Waiting for the peer happens here, on the host, inside the HLE call, so it
// costs no emulated time. That keeps both machines' emulated timelines
// identical regardless of network jitter, which is what makes lockstep
// deterministic. (Spinning inside the game would burn emulated cycles a
// different number of times on each machine.)
//
// Up to 4 players (the game's controller-port limit), one per machine; player
// k owns game port k+1. Topology (NSR_TOPOLOGY):
//   star (default) everyone connects to player 0, who relays packets; only
//                  player 0 needs a reachable UDP port.
//   mesh           everyone connects to every lower-numbered player.
//
// Configuration (POC; environment variables, read at boot):
//   NSR_NETPLAY = 1 | host | client  enables the hook (unset: stub stays a no-op)
//   NSR_PLAYER  = our index k (0-based)       NSR_PLAYERS = 2..4
//   NSR_PORT    = UDP port to bind (default 51515)
//   NSR_PEERS   = ip:port of player 0 (star), or of players 0..k-1 in order (mesh)
//   NSR_TOPOLOGY = star | mesh
//   (shortcut: host = player 0 of 2; client = player 1 of 2 with NSR_PEER=ip:port)
//   NSR_DELAY   = input delay in frames (default 2)
//   NSR_LOG     = path for a text log (default: stderr)
//   NSR_DUMP_PREFIX = write MEM1 at the first desync frame (diagnostics)
//   NSR_SEED    = per-session RNG seed (decimal or 0x hex), agreed in the lobby;
//                 written to SEED_ADDRESS on the first hook call, so every
//                 match differs while all machines stay identical
//
// Rollback (milestone 2, stage 1; NFLStreet docs/poc-milestone2.md): images
// built with isopatch's `rollback` patch send every pad poll through
// RB_POLL_STUB (PADRead + Reborn's remap go to a scratch buffer; Dolphin
// copies the result to the game's pad buffer except while re-simulating) and
// every sim tick through RB_TICK_STUB (a loop that re-runs past ticks while
// Dolphin asks for them). Stage 1 is a local
// correctness test, no network yet:
//   NSR_RB_FORCE = k   roll back k ticks before every live tick, re-simulate
//                      with the recorded pads, and check that the re-simulated
//                      state matches the original byte for byte (outside
//                      RB_KEEP); mismatches are summarised in the log
//   NSR_RB_FORCE_FROM = t     only from live tick t + k on (skip boot/menus)
//   NSR_RB_KEEP  = lo-hi,...  extra physical ranges a restore leaves alone
//   NSR_RB_DUMP_AT = t,...    write MEM1 before live tick t
//                             (NSR_DUMP_PREFIX-tick-t.bin)
//   NSR_RB_HASH_LOG = 1       log a MEM1 hash every 60 live ticks
//   NSR_RB_VTIME_FROM = t     virtual (tick-based) time for game code in
//                             ticks from live tick t on (see OSGetTimeHook)
//   NSR_RB_RECORD / NSR_RB_REPLAY = path
//                             write / read each live tick's pads, so two runs
//                             see identical input per tick whatever their
//                             timing (compare them with NSR_RB_DUMP_AT)
//   NSR_RB_REPLAY_FOLLOW = path  tail a private append-only 52-byte pad stream.
//     Requires CPUThread=False, no RECORD/REPLAY/FORCE/MISPREDICT. Host sleeps
//     at missing input and never predicts. .ready acknowledges startup; a
//     decimal last-unit .end drains the delay tail; .complete acknowledges
//     consumption; .error reports corruption or EOF timeout. Terminal states
//     remain in an interruptible host wait for renderer-driven shutdown.
//   NSR_RB_REPLAY_DELAY = N     minimum complete units behind the head (default
//                             0; use 0 when the relay already imposes delay).
//   NSR_RB_REPLAY_TIMEOUT_MS = N  no-progress wait limit (default 30000).
//     Historical backlog runs unthrottled with capture suppressed until near
//     the delayed head; gameplay drawing remains enabled. Normal replay is 1x,
//     and the launch speed is restored on shutdown.
//   NSR_RB_PROFILE = path     (no rollback) classify every 64-byte granule of
//                             MEM1 by who writes it: the sim (between tick
//                             start and tick end) or the rest of the frame
//                             (between tick end and the next tick start);
//                             rewritten every 600 ticks as "lo hi in out";
//                             counting starts at NSR_RB_FORCE_FROM;
//                             NSR_RB_GRANULE sets the resolution (default 64)
//   NSR_RB_RANGES = path      restore/compare only these ranges ("lo hi ..."
//                             per line, e.g. a filtered profile), minus keeps
//
// Deferred (revisit at milestone 2, rollback): replace env vars with Dolphin
// config + a launcher UI, add time sync / frame skipping, matchmaking.

#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace NSRNetplay
{
// Address of the in-game stub the hook replaces. Must match isopatch.py's
// NETPLAY_STUB.
constexpr unsigned int STUB_ADDRESS = 0x804119AC;
// The patched RNG seed source (fn_801CF7AC) returns this word and advances
// it; must match isopatch.py's SEED_WORD.
constexpr unsigned int SEED_ADDRESS = 0x804114F4;

// Rollback image stubs; must match isopatch.py's RB_POLL_STUB / RB_TICK_STUB.
constexpr unsigned int RB_POLL_STUB = 0x80416900;
constexpr unsigned int RB_TICK_STUB = 0x80416904;
constexpr unsigned int RB_TICK_END_STUB = 0x80416908;
constexpr unsigned int RB_TIME_STUB = 0x8041690C;
// Frame mode (isopatch rollback, frame routine around the main-loop
// iteration fn_80026AD0: message dispatch incl. the draw pass, then the group
// updates incl. InGame::fn_8002854C): Dolphin is asked before and after every
// main-loop frame; rollback re-runs whole frames (Slippi's model), skipping
// the vsync wait and heavy world draws on re-simulated ones. (Wrapping only
// InGame left the draw pass's state, e.g. camera smoothing, behind.)
constexpr unsigned int RB_FRAME_STUB = 0x80416914;
constexpr unsigned int RB_FRAME_END_STUB = 0x80416918;
constexpr unsigned int TICK_COUNT_FN = 0x802378C8;  // fn_802378C8 (gated)
constexpr unsigned int VI_WAIT_FOR_RETRACE = 0x80243B18;  // SDK (skipped in re-sim)
// The world draw inside InGame's render pass (fn_802285CC -> node draw
// 0x80228594 -> fn_801DD140): ~98% of the pass's CPU time in a call-tree
// profile. Skipped in re-simulated frames. The rest of the pass is a UI/scene
// traversal that also runs UI logic (play-call menu), so it must keep running.
constexpr unsigned int RENDER_FN = 0x801DD140;
// SDK time functions in GN7E69 (replaced so ticks can see tick-based time).
constexpr unsigned int OS_GET_TIME = 0x8023E794;
constexpr unsigned int OS_GET_TICK = 0x8023E7AC;

// True when NSR_NETPLAY or NSR_SEED is set (checked once). NSR_SEED alone
// (offline practice) only installs the per-session RNG seed.
bool IsEnabled();

// HLE hook body (HookType::Replace).
void Hook(const Core::CPUThreadGuard& guard);

// Rollback image hooks (HookType::Replace). Poll: r3 = scratch PADStatus[4],
// r4 = retrace counter, r5 = phase (0: raw from PADRead, before Reborn's
// remap; 1: processed). Tick: r3 = game pad buffer; returns r3 = 1 to run
// another tick after this one (re-simulation in progress), else 0.
void RollbackPollHook(const Core::CPUThreadGuard& guard);
void RollbackTickHook(const Core::CPUThreadGuard& guard);
void RollbackFrameHook(const Core::CPUThreadGuard& guard);     // r3 = 1: run another frame
void RollbackFrameEndHook(const Core::CPUThreadGuard& guard);
void TickCountHook(const Core::CPUThreadGuard& guard);  // HookType::Gate
std::vector<unsigned int> ResimSkipList();
std::vector<unsigned int> EffectGateList();  // NSR_RB_EFFECT_GATE
// Load barrier (GN7E69): the game's disc-load start (fn_801ED634, main
// thread) and completion (fn_801EC914, disc worker) are handshakes with the
// live disc system; no rollback may cross them. Rollback targets are clamped
// to after the last such event (netplay: confirm inputs before a load).
constexpr unsigned int LOAD_START_FN = 0x801ED634;
constexpr unsigned int LOAD_DONE_FN = 0x801EC914;
void LoadBarrierHook(const Core::CPUThreadGuard& guard);  // HookType::Start  // functions skipped in re-simulated frames
void PresentHook(const Core::CPUThreadGuard& guard);    // HookType::Gate
// fn_80198B24 reads the depth buffer at a screen point (GXPeekZ) for an
// object's visibility flag. EFB contents differ between GPUs/backends, so
// during the session it returns NSR_RB_PEEKZ (default 0, what the Null
// backend returns and what all determinism tests ran with).
constexpr unsigned int PEEK_Z_FN = 0x80198B24;
void PeekZHook(const Core::CPUThreadGuard& guard);  // HookType::Gate
// The game's VI pre-retrace callback fn_801F7DF4 advances a retrace counter
// (*(0x803EBE44)+0x20) and a retrace-driven clock that game code reads. The
// rollback frame routine also calls it once per frame. During the session
// only that call runs (counters follow frames: a frame may take several
// retraces, differently on each peer); before it, only the interrupt's.
// Present fn_801CE82C paces frames with a GX FIFO breakpoint (GXEnableBreakPt,
// called at 0x801CE818) that only the VI post-retrace callback releases; the
// next frame's GXDrawDone sleeps until then, so every frame (re-simulated
// too) cost a retrace. During the session that call becomes a plain retrace
// wait in live frames and is skipped in re-simulated ones. The VI post
// callback's game counter (fn_801F7B04: ++*(u32*)0x803EBE48) then advances
// once per frame instead (found by Codex, local/work/codex/resim-cost).
constexpr unsigned int GX_ENABLE_BREAKPT = 0x8024C5D0;
constexpr unsigned int PRESENT_BREAKPT_RETURN = 0x801CE81C;
constexpr unsigned int VI_POST_COUNTER_FN = 0x801F7B04;
constexpr unsigned int VI_POST_COUNTER = 0x803EBE48;
constexpr unsigned int VI_PRE_CB = 0x801F7DF4;
constexpr unsigned int RB_FRAME_CODE = 0x80416A80;  // isopatch frame routine (calls VI_PRE_CB)
void ViCallbackHook(const Core::CPUThreadGuard& guard);  // HookType::Gate
// Called after each tick's sim part 2 (no arguments).
void RollbackTickEndHook(const Core::CPUThreadGuard& guard);
// Rollback session with NSR_RB_INSTANT_DISC: DVD commands complete with
// zero latency, i.e. at the end of the JIT block that issued them, so game
// code observes disc I/O completing at the same point of its own execution
// in live and re-simulated frames. (With emulated latency, even with
// FastDiscSpeed, completions land at emulated times; re-simulated frames run
// faster (no vsync wait, fewer draws), so a read issued in frame F completed
// in F's vsync wait live but in F+1 when re-simulated: the game branched
// differently and diverged at play starts.) Returns the latency to use.
long long DiscLatency(long long ticks);
// Counts DVD commands (DVDInterface::ExecuteCommand), so rollback can tell
// which frames had disc I/O in flight.
void NoteDiscCommand();
// Diagnostics: memcheck hits (NSR_RB_MEMWATCH), logged with the rollback tick.
void LogMemHit(u32 pc, u32 lr, bool write, size_t size, u64 value, u32 addr);
// Time sources. Inside a tick, for game code, from NSR_RB_VTIME_FROM on,
// they return virtual time: base + (tick - start) * one 60 Hz frame +
// 64 * (call index within the tick), so a re-simulated tick sees exactly the
// time it saw the first time and every machine sees the same time.
// Everything else (SDK callers, outside ticks) gets the real time base.
void OSGetTimeHook(const Core::CPUThreadGuard& guard);   // u64 in r3:r4
void OSGetTickHook(const Core::CPUThreadGuard& guard);   // u32 in r3
void RollbackTimeHook(const Core::CPUThreadGuard& guard);  // TBL in r3 (mftb site)
// Audio gate (rollback). Game code calls into the audio middleware (its
// state is live, never rewound). During a live tick each such call runs and
// its result (r3, r4, f1) is recorded; during re-simulation the call is
// skipped and the recorded result returned, so sounds aren't started twice
// and the game gets the same handles. Entry points: functions in
// 0x80263000..0x80285A90 (audio middleware, between the SDK text chunks)
// called from code below 0x8025E000 (game, EA engine). Generated from
// main.dol call sites. Opt-in: NSR_RB_AUDIO_GATE=1 (sound-start path only)
// or =addr,... (gating every entry crashes; see AudioGateHook).
constexpr std::array<unsigned int, 75> AUDIO_GATE_ENTRIES{{
    0x80266CB8, 0x80266D00, 0x802692B8, 0x802697D8, 0x8026A2DC, 0x8026A4D4,
    0x8026A870, 0x8026A96C, 0x8026A980, 0x8026A9F4, 0x8026AAF0, 0x8026AB88,
    0x8026B124, 0x8026B250, 0x8026B4D8, 0x8026C18C, 0x8026C23C, 0x8026C280,
    0x8026C39C, 0x8026C3BC, 0x8026C9BC, 0x8026CA50, 0x8026CAA8, 0x8026D0AC,
    0x8026D0EC, 0x8026E280, 0x8026E2A8, 0x8026E364, 0x8026E388, 0x8026E4C0,
    0x8026E8FC, 0x8026EA80, 0x8026EAC0, 0x8026EBBC, 0x8026ED1C, 0x8026EDC4,
    0x8026EE24, 0x8026EE6C, 0x8026EEB0, 0x8026EF0C, 0x8026EF38, 0x8026EF88,
    0x8026F1E8, 0x8026F5F8, 0x8026F710, 0x8026F788, 0x8026F894, 0x8026F908,
    0x8026F97C, 0x8026FBCC, 0x8026FBFC, 0x8026FCAC, 0x802703A0, 0x80270458,
    0x802714A8, 0x8027555C, 0x8027D3AC, 0x8027ECD8, 0x8027ED6C, 0x8027EFAC,
    0x8027F000, 0x8027F030, 0x8027F0DC, 0x8027F21C, 0x8027F2C0, 0x8027F2EC,
    0x8027F3A4, 0x8027F77C, 0x8027F7B0, 0x8027F880, 0x8027F910, 0x8027FAB8,
    0x802856D8, 0x8028572C, 0x802857D8,
}};
constexpr unsigned int RB_GATE_RETURN = 0x80416910;  // gated calls return here (HLE)
void AudioGateHook(const Core::CPUThreadGuard& guard);
void AudioGateReturnHook(const Core::CPUThreadGuard& guard);

// Audio clock (NFLStreet docs: audioclock report). Diagnostics first:
// NSR_AC_LOG=path logs the audio clock model's decisions and the RNG words at
// every frame start; with NSR_AC_CALLS=1 it also logs every game->audio-middleware call made from a frame
// (diagnostics only: results are captured through a return stub, so a call
// the JIT inlined by branch following, both bl and blr, is missed)
// during the session (frame, re-simulated, entry, call site, wrapper call
// site, r3..r5 -> r3 and out-parameters), plus RNG words at every frame
// start, so a reference run and a rollback run can be compared call by call.
constexpr unsigned int AC_RETURN_STUB = 0x80416AFC;  // `blr` written by Dolphin (free patch data)
void AudioCallLogHook(const Core::CPUThreadGuard& guard);     // HookType::Start
void AudioCallReturnHook(const Core::CPUThreadGuard& guard);  // HookType::Replace
void AudioClockFrameLog(const Core::CPUThreadGuard& guard);
// NSR_AC_TRACE=addr,...: log calls of these functions too (same format as NSR_AC_CALLS).
std::vector<unsigned int> AudioTraceList();
// Audio clock model (during the rollback session, NSR_AC != 0): the sim's
// view of audio progress is a deterministic function of frames and game data,
// kept in MEM1 (restored by rollback); see NSRNetplay.cpp "Audio clock model".
//   Queries (HookType::Gate, answered and skipped): speech queue status,
//   speech event position, sound handle finished, stream volume, bank upload
//   ready. Effects (HookType::Start, real call still runs): sound start and
//   speech queue (their results become virtual ids), handle/id translation for
//   stop/fade/skip, stream stop/volume bookkeeping.
void AudioClockSessionStart(const Core::CPUThreadGuard& guard);
void AudioClockQueryHook(const Core::CPUThreadGuard& guard);   // HookType::Gate
void AudioClockEffectHook(const Core::CPUThreadGuard& guard);  // HookType::Start
std::vector<unsigned int> AudioClockQueryHooks();
std::vector<unsigned int> AudioClockEffectHooks();
// Audio clock, music (EA Trax over PathFinder): during the session the
// PathFinder -> game listener dispatchers run only from the frame routine,
// on frame-decided song starts/ends (see NSRNetplay.cpp "music").
void MusicSessionStart(const Core::CPUThreadGuard& guard, u32 frame);
void MusicDispatchHook(const Core::CPUThreadGuard& guard);  // HookType::Gate
void MusicPickHook(const Core::CPUThreadGuard& guard);      // HookType::Start (fn_80072424)
void MusicInjectEvent(const Core::CPUThreadGuard& guard);   // from ViCallbackHook
void MusicTrampHook(const Core::CPUThreadGuard& guard);     // HookType::Replace
// Retrace waits inside the sim during the session count as one virtual
// retrace each (see NSRNetplay.cpp, SimWaitBegin).
void AudioClockSimWait(const Core::CPUThreadGuard& guard);
void SimWaitTrampHook(const Core::CPUThreadGuard& guard);  // HookType::Replace
constexpr unsigned int SIM_WAIT_TRAMP_ADDR = 0x80416AF4;
constexpr unsigned int MUSIC_TRAMP_ADDR = 0x80416AF8;
constexpr unsigned int MUSIC_PICK_SONG_FN = 0x80072424;
std::vector<unsigned int> MusicDispatchers();

// Diagnostics: NSR_RB_PROBES=addr,... installs ProbeHook (HookType::Start)
// at those function entries; inside the NSR_RB_WATCH window it logs each
// change of the watched word together with the probes before and after it.
std::vector<unsigned int> ProbeAddresses();
void ProbeHook(const Core::CPUThreadGuard& guard);
void SetHookPc(unsigned int pc);  // set by HLE::Execute before each hook

// Tear down the network session (called on emulation shutdown).
bool IsReplayCatchingUp();
void Shutdown();
}  // namespace NSRNetplay
