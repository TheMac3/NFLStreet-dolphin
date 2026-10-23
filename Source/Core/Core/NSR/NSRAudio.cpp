// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// NSR host audio under rollback (see NSRAudio.h).

#include "Core/NSR/NSRAudio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "Common/Swap.h"
#include "Core/HW/DSP.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

namespace NSRAudio
{
namespace
{
std::string DiagPrefix()
{
  const char* v = std::getenv("NSR_AUDIO_DIAG");
  return v ? v : "";
}
}  // namespace

bool g_diag = !DiagPrefix().empty();
bool g_fade_pending = false;

namespace
{
// ---------------------------------------------------------------------------
// Diagnostics state (NSR_AUDIO_DIAG). CPU thread only.
// ---------------------------------------------------------------------------
struct Voice
{
  bool running = false, seen = false;
  u16 format = 0, stream = 0;
  u32 loop = 0, end = 0, cur = 0;
  u32 start_tick = 0;
  bool start_resim = false;
  u32 live_frames = 0, resim_frames = 0;
};

struct Diag
{
  FILE* pcm = nullptr;
  FILE* events = nullptr;
  std::map<u32, Voice> voices;  // by parameter block address
  u64 sample = 0;               // stereo samples sent to the speakers so far
  u32 tick = 0;
  bool in_unit = false, resim = false;
  u32 unit_ax = 0, unit_ai_sent = 0, unit_ai_dropped = 0;  // inside the current unit
  u32 gap_ax = 0, gap_ai_sent = 0, gap_ai_dropped = 0;     // since the last unit ended
  u32 units = 0;
  bool opened = false;
};
Diag s_d;

bool Open()
{
  if (s_d.opened)
    return s_d.events != nullptr;
  s_d.opened = true;
  const std::string prefix = DiagPrefix();
  s_d.pcm = std::fopen((prefix + ".pcm").c_str(), "wb");
  s_d.events = std::fopen((prefix + ".events").c_str(), "w");
  if (!s_d.pcm || !s_d.events)
  {
    g_diag = false;
    return false;
  }
  std::fprintf(s_d.events, "# NSR audio diag v1: sample rate 32000 stereo s16le\n");
  return true;
}

bool FrameResim()
{
  return s_d.in_unit && s_d.resim;
}

void Stop(u32 pb, Voice& v, const char* why)
{
  std::fprintf(s_d.events, "V stop t=%u r=%d s=%llu pb=%x end=%x st=%u t0=%u r0=%d live=%u resim=%u why=%s\n",
               s_d.tick, FrameResim() ? 1 : 0, static_cast<unsigned long long>(s_d.sample), pb,
               v.end, v.stream, v.start_tick, v.start_resim ? 1 : 0, v.live_frames,
               v.resim_frames, why);
  v.running = false;
}
bool EnvOff(const char* name)
{
  const char* v = std::getenv(name);
  return v && std::string(v) == "0";
}

// ---------------------------------------------------------------------------
// Re-simulation audio pacing (see NSRAudio.h, item 1). CPU thread only.
//
// A frame's AX frame count is measured from its unit's start to the next
// unit's start (the live frame's tail, present and retrace wait, belongs to
// it; re-simulated units run back to back). Live counts are remembered per
// frame; a re-simulated unit of frame T aims at T's live count plus the
// shortfall so far in this re-simulation. The factor that should achieve it
// comes from a running estimate of AX frames per re-simulated unit per unit
// of speed factor; the shortfall term corrects that estimate's error within
// the same rollback. What is left after the last re-simulated frame (at most
// about one AX frame) is what the crossfade smooths.
//
// Determinism: only the emulated speed of the audio DMA/DSP slice during
// re-simulated units changes. Audio-engine state is per machine already
// (audio rolls back as a unit and is excluded from every digest); what the
// game reads from it is answered per frame by the audio clock model, never
// by audio timing. Verified with the RNG/whole-RAM gates (fix/audio2).
// ---------------------------------------------------------------------------
constexpr size_t PACE_FRAMES = 1024;  // > any rollback window (max prediction + ring)
struct Pace
{
  bool on = !EnvOff("NSR_AUDIO_PACE");
  u64 ax_total = 0;
  bool have_prev = false, prev_resim = false;
  u32 prev_tick = 0;
  u64 prev_begin_ax = 0;
  float prev_factor = 0.0f;
  double prev_target = 0.0;
  double debt = 0.0;               // AX frames behind the live run in this re-simulation
  double per_factor = 1.6 / 16.0;  // AX frames per re-simulated unit per unit of factor
  double live_mean = 3.34;         // AX frames per live frame (5 ms AX frames at 60 Hz)
  std::array<u8, PACE_FRAMES> live_ax{};
  std::array<u32, PACE_FRAMES> live_tag{};  // tick + 1 the entry belongs to (0 = none)
  u32 tick = 0;
  // The running re-simulated unit: AX frames to reach, so far, throttled.
  bool resim_unit = false, throttled = false;
  u32 unit_goal = 0, unit_ax = 0;
};
Pace s_p;
// Audio speed factor for the rest of a re-simulated unit that reached its AX
// frame goal: real time, so no further AX frame (5 ms of emulated time)
// starts before the unit (well under 1 ms of emulated time at 16x) ends.
// Not slower: the already scheduled next DMA event would then land units
// later and starve the following re-simulated units. Not exactly 1.0: that
// value means "live, send to speakers" (DSPManager::UpdateAudioDMA).
constexpr float THROTTLE_FACTOR = 1.01f;
Stats s_stats;

// ---------------------------------------------------------------------------
// Rollback crossfade (see NSRAudio.h, item 2).
// ---------------------------------------------------------------------------
constexpr u32 FADE_SAMPLES = 96;  // 3 ms at 32 kHz
struct Fade
{
  bool on = !EnvOff("NSR_AUDIO_XFADE");
  std::array<s16, 2 * FADE_SAMPLES> tail{};  // native-endian, memory channel order
  u32 len = 0, pos = 0;
};
Fade s_f;
}  // namespace

void AXFrameBegin()
{
  ++s_p.ax_total;
  if (s_p.resim_unit && !s_p.throttled && ++s_p.unit_ax >= s_p.unit_goal)
  {
    // Goal reached: hold the audio DMA back for the rest of this unit.
    s_p.throttled = true;
    Core::System::GetInstance().GetSystemTimers().SetNSRSpeedFactor(THROTTLE_FACTOR);
  }
  if (!g_diag || !Open())
    return;
  for (auto& [pb, v] : s_d.voices)
    v.seen = false;
  ++(s_d.in_unit ? s_d.unit_ax : s_d.gap_ax);
}

void FadeBlock(const s16* in, s16* out)
{
  for (int i = 0; i < 8; ++i)
  {
    for (int ch = 0; ch < 2; ++ch)
    {
      const s16 now = static_cast<s16>(Common::swap16(static_cast<u16>(in[2 * i + ch])));
      s16 v = now;
      if (s_f.pos < s_f.len)
      {
        const float w = static_cast<float>(s_f.pos + 1) / static_cast<float>(s_f.len + 1);
        const float old = s_f.tail[2 * s_f.pos + static_cast<u32>(ch)];
        v = static_cast<s16>(std::lround(old + (static_cast<float>(now) - old) * w));
      }
      out[2 * i + ch] = static_cast<s16>(Common::swap16(static_cast<u16>(v)));
    }
    if (s_f.pos < s_f.len)
      ++s_f.pos;
  }
  if (s_f.pos >= s_f.len)
    g_fade_pending = false;
}

float ResimAudioFactor(float base)
{
  if (!s_p.on)
    return base;
  const size_t i = s_p.tick % PACE_FRAMES;
  const double target = s_p.live_tag[i] == s_p.tick + 1 ? s_p.live_ax[i] : s_p.live_mean;
  const double want = target + s_p.debt;
  ++s_stats.resim_units;
  s_stats.target_ax += static_cast<unsigned long long>(target + 0.5);
  s_p.prev_target = target;
  s_p.resim_unit = true;
  s_p.unit_ax = 0;
  s_p.unit_goal = static_cast<u32>(std::max(0.0, std::round(want)));
  if (s_p.unit_goal == 0)
  {
    // Already ahead of the live run: no AX frame in this unit if possible.
    s_p.throttled = true;
    s_p.prev_factor = 0.0f;  // no estimate update from a throttled unit
    return THROTTLE_FACTOR;
  }
  s_p.throttled = false;
  // Aim a little past the goal (the estimate varies with each frame's CPU
  // work); AXFrameBegin throttles the rest of the unit once it is reached.
  const float factor = static_cast<float>(std::clamp(
      1.5 * want / std::max(s_p.per_factor, 0.005), 1.01, static_cast<double>(base) * 8.0));
  s_p.prev_factor = factor;
  return factor;
}

void SessionReset()
{
  s_p.live_tag.fill(0);
  s_p.have_prev = false;
  s_p.debt = 0.0;
  s_p.resim_unit = false;
  g_fade_pending = false;
}

Stats GetStats()
{
  return s_stats;
}

void AXVoice(u32 pb_addr, u16 running, u16 is_stream, u16 sample_format, u32 loop_addr,
             u32 end_addr, u32 cur_addr)
{
  if (!s_d.events)
    return;
  Voice& v = s_d.voices[pb_addr];
  v.seen = true;
  const bool on = running == 1;
  if (on)
  {
    const bool fresh = !v.running || v.end != end_addr || v.loop != loop_addr ||
                       v.format != sample_format;
    if (fresh)
    {
      if (v.running)
        Stop(pb_addr, v, "replaced");
      v.start_tick = s_d.tick;
      v.start_resim = FrameResim();
      v.live_frames = v.resim_frames = 0;
      std::fprintf(s_d.events,
                   "V start t=%u r=%d s=%llu pb=%x end=%x loop=%x fmt=%x st=%u cur=%x\n",
                   s_d.tick, FrameResim() ? 1 : 0, static_cast<unsigned long long>(s_d.sample),
                   pb_addr, end_addr, loop_addr, sample_format, is_stream, cur_addr);
    }
    else if (cur_addr < v.cur && cur_addr != loop_addr)
    {
      std::fprintf(s_d.events, "V back t=%u r=%d s=%llu pb=%x end=%x loop=%x from=%x to=%x\n",
                   s_d.tick, FrameResim() ? 1 : 0, static_cast<unsigned long long>(s_d.sample),
                   pb_addr, end_addr, loop_addr, v.cur, cur_addr);
    }
    ++(FrameResim() ? v.resim_frames : v.live_frames);
  }
  else if (v.running)
  {
    Stop(pb_addr, v, "stopped");
  }
  v.running = on;
  v.format = sample_format;
  v.stream = is_stream;
  v.loop = loop_addr;
  v.end = end_addr;
  v.cur = cur_addr;
}

void AXFrameEnd()
{
  if (!s_d.events)
    return;
  for (auto& [pb, v] : s_d.voices)
  {
    if (v.running && !v.seen)
      Stop(pb, v, "vanished");
  }
}

void AIBlock(const s16* samples, bool sent)
{
  if (!Open())
    return;
  if (!sent)
  {
    ++(s_d.in_unit ? s_d.unit_ai_dropped : s_d.gap_ai_dropped);
    return;
  }
  ++(s_d.in_unit ? s_d.unit_ai_sent : s_d.gap_ai_sent);
  s16 out[16];
  for (int i = 0; i < 8; ++i)
  {
    // Memory order is big-endian R, L (see Mixer::MixerFifo::PushSamples).
    out[2 * i] = static_cast<s16>(Common::swap16(static_cast<u16>(samples[2 * i + 1])));
    out[2 * i + 1] = static_cast<s16>(Common::swap16(static_cast<u16>(samples[2 * i])));
  }
  std::fwrite(out, sizeof(out), 1, s_d.pcm);
  s_d.sample += 8;
}

void UnitBegin(u32 tick, bool resim)
{
  // Pacing bookkeeping (always).
  if (s_p.have_prev)
  {
    const u64 n = s_p.ax_total - s_p.prev_begin_ax;
    if (!s_p.prev_resim)
    {
      const size_t i = s_p.prev_tick % PACE_FRAMES;
      s_p.live_ax[i] = static_cast<u8>(std::min<u64>(n, 255));
      s_p.live_tag[i] = s_p.prev_tick + 1;
      s_p.live_mean += (static_cast<double>(n) - s_p.live_mean) / 64.0;
    }
    else
    {
      s_stats.achieved_ax += n;
      s_p.debt += s_p.prev_target - static_cast<double>(n);
      // Learn AX frames per unit of factor from units that ran unthrottled
      // to their end (a throttled unit only says "at least this fast").
      // Too high an estimate undershoots (corrected here); too low a one
      // overshoots the factor, which the throttle cuts at the goal.
      if (s_p.prev_factor > 0.0f && !s_p.throttled)
        s_p.per_factor += (static_cast<double>(n) / s_p.prev_factor - s_p.per_factor) / 8.0;
    }
  }
  s_p.resim_unit = false;
  s_p.throttled = false;
  s_p.have_prev = true;
  s_p.prev_resim = resim;
  s_p.prev_tick = tick;
  s_p.prev_begin_ax = s_p.ax_total;
  s_p.prev_factor = 0.0f;
  s_p.tick = tick;
  if (!g_diag || !Open())
    return;
  std::fprintf(s_d.events, "B t=%u r=%d s=%llu gapax=%u gapai=%u gapdrop=%u\n", tick,
               resim ? 1 : 0, static_cast<unsigned long long>(s_d.sample), s_d.gap_ax,
               s_d.gap_ai_sent, s_d.gap_ai_dropped);
  s_d.tick = tick;
  s_d.resim = resim;
  s_d.in_unit = true;
  s_d.unit_ax = s_d.unit_ai_sent = s_d.unit_ai_dropped = 0;
}

void UnitEnd()
{
  if (!g_diag || !s_d.events || !s_d.in_unit)
    return;
  std::fprintf(s_d.events, "U t=%u r=%d s=%llu ax=%u ai=%u drop=%u\n", s_d.tick,
               s_d.resim ? 1 : 0, static_cast<unsigned long long>(s_d.sample), s_d.unit_ax,
               s_d.unit_ai_sent, s_d.unit_ai_dropped);
  s_d.in_unit = false;
  s_d.gap_ax = s_d.gap_ai_sent = s_d.gap_ai_dropped = 0;
  if (++s_d.units % 120 == 0)
  {
    std::fflush(s_d.events);
    std::fflush(s_d.pcm);
  }
}

void Restore(Core::System& system)
{
  s_p.debt = 0.0;
  if (s_f.on && !g_fade_pending)
  {
    // The abandoned timeline's next samples (not sent yet). A second restore
    // before any live sample keeps the first capture: that is still what
    // the speakers would have played next.
    u32 blocks = 0;
    const u8* p = system.GetDSP().GetNSRPendingAI(&blocks);
    const u32 n = std::min(blocks * 8, FADE_SAMPLES);
    if (p && n >= 8)
    {
      for (u32 k = 0; k < 2 * n; ++k)
        s_f.tail[k] = static_cast<s16>(Common::swap16(p + 2 * k));
      s_f.len = n;
      s_f.pos = 0;
      g_fade_pending = true;
      ++s_stats.fades;
    }
  }
  if (!g_diag || !Open())
    return;
  std::fprintf(s_d.events, "R from=%u to=%u s=%llu\n", s_p.tick, 0u,
               static_cast<unsigned long long>(s_d.sample));
}

}  // namespace NSRAudio
