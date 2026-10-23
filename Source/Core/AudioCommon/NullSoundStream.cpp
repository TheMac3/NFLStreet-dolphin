// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/NullSoundStream.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "AudioCommon/Mixer.h"

namespace
{
constexpr unsigned int TAP_RATE = 48000;
}

NullSound::~NullSound()
{
  SetRunning(false);
}

bool NullSound::Init()
{
  if (const char* tap = std::getenv("NSR_AUDIO_TAP"); tap && *tap)
  {
    m_tap_path = tap;
    GetMixer()->SetSampleRate(TAP_RATE);
    return true;
  }
  // Make Mixer aware that audio output is disabled.
  GetMixer()->SetSampleRate(0);

  return true;
}

bool NullSound::SetRunning(bool running)
{
  if (m_tap_path.empty())
    return true;
  if (running && !m_tap_run.exchange(true))
    m_tap = std::thread([this] { TapLoop(); });
  else if (!running && m_tap_run.exchange(false) && m_tap.joinable())
    m_tap.join();
  return true;
}

void NullSound::SetVolume(int volume)
{
}

void NullSound::TapLoop()
{
  FILE* out = std::fopen(m_tap_path.c_str(), "ab");
  if (!out)
    return;
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  unsigned long long produced = 0, reported = 0;
  std::vector<short> buf;
  // Stats are rewritten every second: test harnesses usually kill Dolphin.
  const auto report = [&] {
    std::fflush(out);
    if (FILE* txt = std::fopen((m_tap_path + ".txt").c_str(), "w"))
    {
      const auto s = GetMixer()->GetNSRDMAStats();
      std::fprintf(txt, "seconds %.1f gap_fills %llu silent %llu skips %llu\n",
                   static_cast<double>(produced) / TAP_RATE, static_cast<unsigned long long>(s[0]),
                   static_cast<unsigned long long>(s[1]), static_cast<unsigned long long>(s[2]));
      std::fclose(txt);
    }
  };
  while (m_tap_run.load())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const double secs = std::chrono::duration<double>(clock::now() - start).count();
    const auto due = static_cast<unsigned long long>(secs * TAP_RATE);
    if (due <= produced)
      continue;
    const std::size_t n = static_cast<std::size_t>(due - produced);
    buf.resize(n * 2);
    GetMixer()->Mix(buf.data(), n);
    std::fwrite(buf.data(), sizeof(short), n * 2, out);
    produced = due;
    if (produced - reported >= TAP_RATE)
    {
      report();
      reported = produced;
    }
  }
  report();
  std::fclose(out);
}
