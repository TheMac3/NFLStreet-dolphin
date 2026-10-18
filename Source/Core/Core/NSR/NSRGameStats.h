// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Host-only results capture and optional peer identity metadata. CPU-thread owned;
// none of these operations call guest code, change guest memory or participate in
// rollback state/digests. The adapter must exclude re-simulation before calling.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Core
{
class CPUThreadGuard;
}

namespace NSRGameStats
{
// Called once after the netplay adapter knows the local slot and player count.
void Configure(std::uint32_t local_pad, std::uint32_t players, bool netplay);

// Cached reliable NSRI metadata records, replayed to newly connected peers.
std::vector<std::vector<std::uint8_t>> Identities();

// Validate the bounded payload before the adapter checks ENet sender binding.
std::optional<std::uint32_t> IdentitySender(std::span<const std::uint8_t> packet);

// Identities are immutable for one emulator run. A conflict is ignored, never
// allowed to rename a peer or change the host's shared timestamp later.
bool AcceptIdentity(std::span<const std::uint8_t> packet);

// One reliable host announcement after its corrected stats snapshot. The
// adapter must bind received NSRG packets to player 0's established connection.
std::vector<std::uint8_t> TakeCaptureAnchor();
void AcceptCaptureAnchor(std::span<const std::uint8_t> packet);

// Observe native MVP rendering after fn_8005ADCC has selected/calculated its
// player. This is an HLE Start hook: PPC registers and the original BL survive.
void ObserveMvp(const Core::CPUThreadGuard& guard);

// Native Game Stats formatter supplies its two output arrays at function entry;
// remember their addresses, then read them only after the live frame completes.
void ObserveTotals(const Core::CPUThreadGuard& guard);

// Only called at live frame ends. A missing frontier means correction is still
// pending. Stage an MVP on the live timeline, then publish after its frame is
// confirmed; requiring the newest frame to be confirmed can miss an entire UI
// screen under latency. Post-match lockstep supplies the current frame.
void ObserveFrame(const Core::CPUThreadGuard& guard, std::uint32_t frame, bool in_game,
                  std::optional<std::uint32_t> confirmed_through);

// Discard any staged observation on an abandoned timeline before restoring a
// rollback snapshot. This updates host bookkeeping only, never guest state.
void RollbackFrom(std::uint32_t frame);

// Clear pointers, deduplication and identities across in-process title boots.
void Reset();
}  // namespace NSRGameStats
