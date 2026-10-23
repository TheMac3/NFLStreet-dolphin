// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// NSR local pad source: a GameCube controller on a Wii U / Mayflash (Wii U
// mode) USB adapter as the *input* of the standard controller in port 1.
//
// Why not SIDEVICE_WIIU_ADAPTER (12) on the local port? Stock Dolphin only
// reads the adapter when the SI port's device type is 12, but that device is
// not guest-identical to a standard controller (6) outside Dolphin's own
// netplay/movie determinism mode, which NSR never enables:
//   * SI_DeviceGCAdapter::RunBuffer answers SI_NONE to the SDK's type probe
//     while the adapter or that adapter port is empty, and GCAdapter.cpp puts
//     PAD_ERR_STATUS in the pad word, so the guest PAD library marks the port
//     unplugged and keeps re-probing it with extra SI transfers;
//   * a newly connected pad reports PAD_GET_ORIGIN with its physical resting
//     stick/trigger values; the device latches them (SetOrigin) and the SDK
//     re-reads the origin into its own RAM (PAD Origin[]) on that one peer.
// Both are per-peer guest RAM / SI-traffic differences that a mixed session
// (one peer 12, the other 6) would have, so the boot manifest keeps every
// port a standard controller and the adapter is instead read host-side,
// exactly like a mapped XInput/SDL pad: Pad::GetStatus(0) returns the
// adapter's pad, recentred on its own origin, with the origin/error bits
// stripped and always connected. The guest sees a plain standard controller
// on every peer; only the input values differ, and those are exchanged.
//
// These helpers are pure (no libusb, no config) so Tools/NSR tests cover them.
// Deferred: only guest port 1 (the NSR local port) can use the adapter; offline
// couch play with several adapter pads would need a per-port channel list.
// Revisit if friends ask for local multiplayer on one adapter.

#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

#include "InputCommon/GCPadStatus.h"

namespace NSRPadSource
{
constexpr int CHANNELS = 4;

// NSR_PAD_ADAPTER: the adapter port (0-3, adapter port 1 = 0) the launcher
// found in the player's setup. Anything else (unset, empty, "off") disables
// the source, so a stray value can never change which SI device boots.
constexpr std::optional<int> ParseChannel(std::string_view text)
{
  if (text.size() == 1 && text[0] >= '0' && text[0] < '0' + CHANNELS)
    return text[0] - '0';
  return std::nullopt;
}

// Prefer the configured adapter port; if nothing is plugged in there, take the
// lowest port that has a pad (friends plug into whichever socket is free).
constexpr std::optional<int> PickChannel(int preferred, const std::array<bool, CHANNELS>& plugged)
{
  if (preferred >= 0 && preferred < CHANNELS && plugged[preferred])
    return preferred;
  for (int chan = 0; chan < CHANNELS; ++chan)
  {
    if (plugged[chan])
      return chan;
  }
  return std::nullopt;
}

// A connected standard controller at rest: what the guest gets while the
// adapter is present but its pad is unplugged, so the port never flaps.
constexpr GCPadStatus NeutralPad()
{
  GCPadStatus pad{};
  pad.stickX = GCPadStatus::MAIN_STICK_CENTER_X;
  pad.stickY = GCPadStatus::MAIN_STICK_CENTER_Y;
  pad.substickX = GCPadStatus::C_STICK_CENTER_X;
  pad.substickY = GCPadStatus::C_STICK_CENTER_Y;
  pad.isConnected = true;
  return pad;
}

// Netplay: every standard-controller port answers the guest as a connected
// pad, a neutral one while no local device feeds it. Found by the emulated
// controller permutation test (NFLStreet docs/controller-testing.md):
//   * only port 1 maps this player's own controller; ports 2-4 carry the other
//     players' exchanged pads and have no local device, so their SI ports
//     answered "no controller" on every machine. The netplay exchange then
//     writes those players' pads into PADStatus, but the game's controller
//     registry never took the port: the joiner's pad moved nothing on Select
//     Controller ("my friend's controller is never recognized"). The test
//     harness bound an idle pipe device to every port, which hid this;
//   * a local pad that appears after the game probed port 1 (a Bluetooth pad
//     woken late, slow device enumeration) stayed unplugged to the guest for
//     the whole session: the exchange always hands the game a connected pad,
//     so it never sees the error that makes it re-probe the port. Meanwhile
//     the other peers saw a connected port 1 there: a guest-visible difference
//     (lockstep desyncs in the late-plug test);
//   * a player's extra local mappings (Reborn's GCPad2-4, kept for offline
//     couch play) no longer make one machine's SI ports differ from another's.
// The exchanged pads are unchanged (an unplugged port was already sent as a
// neutral, connected pad). Offline practice keeps Dolphin's normal behaviour,
// where the game sees a missing controller and re-probes when it appears.
constexpr GCPadStatus NetplayPad(const GCPadStatus& pad, bool netplay)
{
  return netplay && !pad.isConnected ? NeutralPad() : pad;
}

// Shift an axis so the pad's resting value becomes `center` (the guest's fixed
// standard-controller origin). Same result as the SDK's origin clamp would give
// for a real pad, but computed host-side so the guest never stores an origin.
constexpr u8 Recenter(u8 value, u8 origin, u8 center)
{
  return static_cast<u8>(std::clamp(int{value} - int{origin} + int{center}, 0, 255));
}

constexpr u8 TriggerAboveRest(u8 value, u8 origin)
{
  return static_cast<u8>(std::max(int{value} - int{origin}, 0));
}

// The adapter's raw pad as a standard controller's GCPadStatus. `origin` is
// the pad's state captured when it was plugged in (GCAdapter port origin).
constexpr GCPadStatus StandardControllerPad(const GCPadStatus& raw, const GCPadStatus& origin)
{
  GCPadStatus pad = NeutralPad();
  pad.button = static_cast<u16>(raw.button & ~(PAD_GET_ORIGIN | PAD_ERR_STATUS | PAD_USE_ORIGIN));
  pad.stickX = Recenter(raw.stickX, origin.stickX, GCPadStatus::MAIN_STICK_CENTER_X);
  pad.stickY = Recenter(raw.stickY, origin.stickY, GCPadStatus::MAIN_STICK_CENTER_Y);
  pad.substickX = Recenter(raw.substickX, origin.substickX, GCPadStatus::C_STICK_CENTER_X);
  pad.substickY = Recenter(raw.substickY, origin.substickY, GCPadStatus::C_STICK_CENTER_Y);
  pad.triggerLeft = TriggerAboveRest(raw.triggerLeft, origin.triggerLeft);
  pad.triggerRight = TriggerAboveRest(raw.triggerRight, origin.triggerRight);
  // Match GCPad::GetInput, which reports full analog A/B for pressed buttons.
  pad.analogA = (pad.button & PAD_BUTTON_A) ? 0xFF : 0x00;
  pad.analogB = (pad.button & PAD_BUTTON_B) ? 0xFF : 0x00;
  return pad;
}
}  // namespace NSRPadSource
