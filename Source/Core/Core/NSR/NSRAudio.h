// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// NSR host audio under rollback (fix/audio2). Everything here is host-only:
// it observes and paces the emulated audio hardware (HLE AX voices, the AI
// DMA stream that Dolphin sends to the mixer) and never writes emulated
// memory, so it cannot change what the game sees.
//
// Why: the audio engine (middleware, AX voices) rolls back with the game in
// netplay, re-simulated frames run on a 16x machine and their audio is not
// sent to the speakers (it was heard live). Two host-side effects followed:
//
//  1. Re-simulated frames ran ~1.6 AX frames each against ~3.3 in live
//     frames (the live frame's retrace wait is emulated idle time, during
//     which audio DMA keeps running; re-simulated frames do not wait). After
//     every rollback every voice was therefore 9..30 ms behind what had
//     just been heard: the speakers replayed that slice, a stutter at every
//     rollback (~13 per second in real play): "crunchy". Pacing: each
//     re-simulated frame gets the audio DMA speed that makes it run as many
//     AX frames as the same frame ran live (ResimAudioFactor), with the
//     shortfall of one frame carried into the next frame of the same
//     re-simulation.
//  2. Whatever the corrected timeline changes (a voice the prediction started
//     but the correction does not, the 0..1 AX frame pacing remainder) is a
//     step in the waveform at the first live sample: a click. Crossfade: the
//     abandoned timeline's next samples, still in the AI DMA buffer when the
//     restore happens, are blended into the first ~3 ms of the corrected
//     output (FadeBlock).
// (Missing event sounds were a different cause, in the sound-start gate; see
// AudioGateHook in NSRNetplay.cpp.)
//
// NSR_AUDIO_PACE=0 / NSR_AUDIO_XFADE=0 turn either fix off (A/B tests).
//
// Diagnostics (NSR_AUDIO_DIAG=<prefix>, off by default):
//   <prefix>.pcm     the samples sent to the speakers (s16 little-endian
//                    stereo, 32 kHz: exactly what AudioCommon::SendAIBuffer
//                    receives, live frames only).
//   <prefix>.events  one line per voice start/stop/rewind, per rollback and
//                    per unit (frame) with its AX frame count; every line
//                    carries the output sample position, so the analysis
//                    tool (NFLStreet tools/audiodiag.py) can line events up
//                    with the audio.
// All hooks run on the CPU thread (HLE DSP and the AI DMA event both run
// there in single-core mode), so no locking.
#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace NSRAudio
{
// True when NSR_AUDIO_DIAG is set (read once). Callers test it before
// calling the diagnostics hooks below.
extern bool g_diag;
// True while the first live samples after a rollback are being crossfaded.
extern bool g_fade_pending;

// One AX command list (one 5 ms AX frame) begins / ends. AXFrameBegin is
// always called (pacing counts AX frames); AXFrameEnd and AXVoice only with
// g_diag.
void AXFrameBegin();
void AXFrameEnd();
// A voice parameter block as read before the AX frame processes it.
void AXVoice(u32 pb_addr, u16 running, u16 is_stream, u16 sample_format, u32 loop_addr,
             u32 end_addr, u32 cur_addr);
// One 32-byte AI DMA block (8 stereo samples, big-endian) as sent to the
// speakers (`sent`) or suppressed in re-simulated time. Diagnostics only.
void AIBlock(const s16* samples, bool sent);
// While g_fade_pending: the crossfaded copy of one AI DMA block (big-endian
// in and out).
void FadeBlock(const s16* in, s16* out);

// From the rollback driver (CPU thread). UnitBegin/UnitEnd bracket every
// rollback unit (frame), live or re-simulated; Restore is called when a
// snapshot is restored, before the re-simulation starts.
void UnitBegin(u32 tick, bool resim);
void UnitEnd();
void Restore(Core::System& system);
// The audio speed factor (SystemTimers::SetNSRSpeedFactor) for the
// re-simulated unit that just began; `base` is the fixed factor
// (NSR_RB_RESIM_AUDIO_SPEED, default 16), returned when pacing is off or has
// no live measurement for this frame.
float ResimAudioFactor(float base);
// Session enter/leave: forget per-frame measurements.
void SessionReset();

struct Stats
{
  unsigned long long resim_units = 0, target_ax = 0, achieved_ax = 0, fades = 0;
};
Stats GetStats();
}  // namespace NSRAudio
