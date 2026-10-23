// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Boot-only canonical settings and disposable cards for NSR sessions. No guest
// code may run until NSRNetplay::PrepareBoot has agreed this manifest.
#pragma once

#include <map>
#include <optional>
#include <string>

namespace NSRBootSync
{
// Network sessions and explicit offline quickstart need canonical fresh cards.
// Manual offline practice retains persistent saves.
bool Enabled();
// Install the Netplay layer and return effective named settings and card digests.
// Returns nullopt on temporary-card creation failure; the caller must abort boot.
std::optional<std::map<std::string, std::string>> Prepare();
// Remove only this session's disposable backing directory after card shutdown.
void Cleanup();
// Fixed GC card format time; together with canonical SRAM makes empty cards equal.
constexpr unsigned long long CARD_FORMAT_TIME = 0x30000000;
}  // namespace NSRBootSync
