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

#include <vector>

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
// Called after each tick's sim part 2 (no arguments).
void RollbackTickEndHook(const Core::CPUThreadGuard& guard);
// Time sources. Inside a tick, for game code, from NSR_RB_VTIME_FROM on,
// they return virtual time: base + (tick - start) * one 60 Hz frame +
// 64 * (call index within the tick), so a re-simulated tick sees exactly the
// time it saw the first time and every machine sees the same time.
// Everything else (SDK callers, outside ticks) gets the real time base.
void OSGetTimeHook(const Core::CPUThreadGuard& guard);   // u64 in r3:r4
void OSGetTickHook(const Core::CPUThreadGuard& guard);   // u32 in r3
void RollbackTimeHook(const Core::CPUThreadGuard& guard);  // TBL in r3 (mftb site)
// Diagnostics: NSR_RB_PROBES=addr,... installs ProbeHook (HookType::Start)
// at those function entries; inside the NSR_RB_WATCH window it logs each
// change of the watched word together with the probes before and after it.
std::vector<unsigned int> ProbeAddresses();
void ProbeHook(const Core::CPUThreadGuard& guard);
void SetHookPc(unsigned int pc);  // set by HLE::Execute before each hook

// Tear down the network session (called on emulation shutdown).
void Shutdown();
}  // namespace NSRNetplay
