// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Host-only NSR performance clocks. Never scheduled in CoreTiming, serialized,
// hashed or used to choose guest inputs. Relaxed atomics allow the texture
// workers and GPU thread to report safely; NSR netplay itself is single core.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>

#include "Common/CommonTypes.h"

namespace NSRHostMetrics
{
enum Kind : size_t { Present, TextureRequest, TextureUpload, TextureDecode, NetworkWait, Pacing, Count };
inline std::atomic<bool> enabled{false};
inline std::array<std::atomic<u64>, Count> ns{};
inline std::atomic<u64> texture_count{0};

// Capture monotonically increasing totals; consumers own their baselines.
inline std::array<u64, Count> Read()
{
  std::array<u64, Count> result{};
  for (size_t i = 0; i < Count; ++i)
    result[i] = ns[i].load(std::memory_order_relaxed);
  return result;
}

// Time an existing host operation without adding any guest events or waits.
class Scope
{
public:
  explicit Scope(Kind kind) : m_kind(kind), m_on(enabled.load(std::memory_order_relaxed))
  {
    if (m_on)
      m_begin = std::chrono::steady_clock::now();
  }
  ~Scope()
  {
    if (m_on)
      ns[m_kind].fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - m_begin).count()), std::memory_order_relaxed);
  }
private:
  Kind m_kind;
  bool m_on;
  std::chrono::steady_clock::time_point m_begin;
};
}  // namespace NSRHostMetrics
