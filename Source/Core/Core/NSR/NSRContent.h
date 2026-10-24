// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Boot-time ratings injection for GN7E69 rev 0. The launcher verifies the signed
// release; Dolphin verifies the exact supplied bundle and binds it into M2.
#pragma once

#include <array>
#include <cstdint>

namespace Core
{
class CPUThreadGuard;
}

namespace NSRContent
{
// Full SHA-256, version and application policy, all compared before rollback.
std::array<std::uint64_t, 6> Fingerprint();
// Called immediately after native database registration, before its consumers.
void DatabaseLoaded(const Core::CPUThreadGuard& guard, bool resim, std::uint32_t frame);
// Existing-play order boundary; HookType::Replace preserves the stock instruction.
void PlaybookMenu(const Core::CPUThreadGuard& guard, bool resim, std::uint32_t frame);
// Replaces only an audited lha r0 at an enabled split consumer.
void RatingHook(const Core::CPUThreadGuard& guard);
// A supplied bundle must have been applied before any rollback epoch starts.
bool Ready();
// Forget host state on emulation shutdown, including a failed boot.
void Reset();
}
