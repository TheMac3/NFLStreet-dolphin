// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// TEST-ONLY: emulated "physical" SDL controllers for the NSR controller
// permutation test (NFLStreet docs/controller-testing.md), for machines with
// no real pads and no root (so no /dev/uinput). Completely inert unless
// NSR_TEST_VIRTUAL_PADS is set in the environment.
//
// Each listed pad is an SDL virtual joystick with a real controller's USB
// vendor/product ID and SDL name (ps4 = DualShock 4 054c:09cc "PS4 Controller",
// ps5 = DualSense 054c:0ce6, xbox360 = 045e:028e, xboxone = 045e:02ea), so it
// goes through SDL's gamepad mapping/type detection, Dolphin's SDL device naming
// ("SDL/0/PS4 Controller") and the [GCPad1] mapping the launcher writes, then
// Pad::GetStatus(0) exactly like a plugged-in pad. What it does not cover: the
// OS HID/XInput/DirectInput drivers in front of SDL (see the test report).
//
// Environment:
//   NSR_TEST_VIRTUAL_PADS   comma list of KIND[@MS]; @MS attaches that pad MS
//                           milliseconds of wall-clock time after SDL starts
//                           (late plug-in / Bluetooth wake-up), else at start.
//   NSR_TEST_PAD_SCRIPT     timed input for one pad. Lines: `UNTIL BUTTONS [LX LY [RX RY]]`
//                           (state held until emulated field UNTIL; BUTTONS is
//                           `none` or s/e/w/n|cross/circle/square/triangle,
//                           start, back, guide, l1, r1, l2, r2, l3, r3,
//                           up/down/left/right, touch joined by '+'; sticks are
//                           -127..127 with +Y up), plus `loop START LEN`.
//                           The clock is the emulated field count since boot
//                           (CoreTiming ticks / ticks per field): a host-side
//                           physical controller, but repeatable per run.
//   NSR_TEST_PAD_SCRIPT_PAD index into NSR_TEST_VIRTUAL_PADS driven by the script (default 0).
//   NSR_TEST_PAD_LOG        file: attach events and every applied state change.
//
// Revisit/remove: once the permutation test is replaced by real-hardware CI or
// it stops being run; nothing else depends on it.

#pragma once

namespace ciface::SDL::NSRTestVirtualPads
{
// SDL thread, right after SDL_Init and before the initial event drain.
void AttachFromEnv();
// InputBackend::UpdateInput (before SDL_UpdateGamepads): apply the scripted state.
void Drive();
// Before SDL_Quit.
void Shutdown();
}  // namespace ciface::SDL::NSRTestVirtualPads
