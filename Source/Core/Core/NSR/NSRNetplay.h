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
// Deferred (revisit at milestone 2, rollback): replace env vars with Dolphin
// config + a launcher UI, add time sync / frame skipping, matchmaking.

#pragma once

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

// True when NSR_NETPLAY or NSR_SEED is set (checked once). NSR_SEED alone
// (offline practice) only installs the per-session RNG seed.
bool IsEnabled();

// HLE hook body (HookType::Replace).
void Hook(const Core::CPUThreadGuard& guard);

// Tear down the network session (called on emulation shutdown).
void Shutdown();
}  // namespace NSRNetplay
