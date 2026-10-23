// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "AudioCommon/SoundStream.h"

class NullSound final : public SoundStream
{
public:
  ~NullSound() override;
  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;

  static bool IsValid() { return true; }

private:
  // NFLStreet netplay audio tests (NSR_AUDIO_TAP=<file>): pull the mixer in
  // real time like a 48 kHz device would and write what it returns
  // (s16le stereo) to <file>; mixer underrun/overflow counts go to
  // <file>.txt. Lets headless runs measure what a player would hear,
  // including Fill Audio Gaps repeats when emulation stalls.
  void TapLoop();
  std::string m_tap_path;
  std::thread m_tap;
  std::atomic<bool> m_tap_run{false};
};
