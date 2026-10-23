// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"

#include "InputCommon/GCPadStatus.h"

namespace GCAdapter
{
void Init();
void ResetRumble();
void Shutdown();
void SetAdapterCallback(std::function<void(void)> func);
void StartScanThread();
void StopScanThread();

// Buttons have PAD_GET_ORIGIN set on new connection
// Netplay and CSIDevice_GCAdapter make use of this.
GCPadStatus Input(int chan);

void Output(int chan, u8 rumble_command);
bool IsDetected(const char** error_message);
bool DeviceConnected(int chan);
void ResetDeviceType(int chan);
bool UseAdapter();

// --- NSR: adapter as the input of a standard controller (Core/NSR/NSRPadSource.h) ---
// The adapter port named by NSR_PAD_ADAPTER (read once), or nullopt when the
// player doesn't use an adapter. Also turns adapter scanning on (UseAdapter).
std::optional<int> NSRSourceChannel();
// The adapter pad as a sanitized standard-controller status, a neutral
// connected pad when the adapter has no pad plugged in, or nullopt when no
// adapter is detected (Pad::GetStatus then falls back to the GCPad mapping).
std::optional<GCPadStatus> NSRStandardControllerInput();
// Rumble the adapter pad feeding the standard controller; false when unused.
bool NSRRumble(bool on);
// One-line state for launcher/netplay logs (no paths, no device serials).
std::string NSRSourceSummary();

}  // namespace GCAdapter
