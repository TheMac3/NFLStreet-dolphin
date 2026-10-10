// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Reads a private append-only confirmed-input spool without advancing emulated time.
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace NSRNetplay
{
class ReplayFollow
{
public:
  enum class Result
  {
    Wait,
    Pads,
    Complete,
    Error
  };
  using Clock = std::chrono::steady_clock;

  // Status files belong to a fresh private spool; the writer publishes .end atomically.
  ReplayFollow(std::string path, unsigned delay, unsigned timeout_ms)
      : m_path(std::move(path)), m_delay(delay), m_timeout(timeout_ms)
  {
    for (const char* suffix : {".ready", ".complete", ".error"})
    {
      std::error_code ec;
      const bool exists = std::filesystem::exists(m_path + suffix, ec);
      if (ec || exists)
      {
        Fail("stale or inaccessible follow status files");
        return;
      }
    }
    m_file = std::fopen(m_path.c_str(), "rb");
    if (!m_file)
      Fail("cannot open follow input");
  }

  ~ReplayFollow()
  {
    if (m_file)
      std::fclose(m_file);
  }
  ReplayFollow(const ReplayFollow&) = delete;
  ReplayFollow& operator=(const ReplayFollow&) = delete;

  // Publish a small status file by rename so the renderer cannot observe partial contents.
  bool Status(const std::string& suffix, const std::string& value) const
  {
    const std::string name = m_path + suffix;
    std::ofstream out(name + ".tmp", std::ios::binary | std::ios::trunc);
    out << value;
    out.close();
    if (!out)
      return false;
    std::error_code ec;
    std::filesystem::rename(name + ".tmp", name, ec);
    return !ec;
  }

  // Consume exactly frame. File size supplies lookahead without preloading future pads.
  // A valid terminal marker releases the configured delay tail and is acknowledged once.
  Result Poll(unsigned frame, std::array<unsigned char, 48>& pads)
  {
    if (!m_error.empty())
      return Result::Error;
    const auto now = Clock::now();
    std::error_code ec;
    auto size = std::filesystem::file_size(m_path, ec);
    if (ec)
      return Fail("cannot stat follow input");
    if (size < m_size)
      return Fail("follow file was truncated");
    if (size != m_size)
    {
      m_size = size;
      m_progress = now;
    }
    m_head = size / RECORD_SIZE;
    if (m_head > MAX_RECORDS)
      return Fail("follow archive limit exceeded");

    const bool ended = std::filesystem::exists(m_path + ".end", ec);
    if (ec)
      return Fail("cannot stat follow end marker");
    if (ended)
    {
      // The producer may append the final record before atomically publishing .end.
      size = std::filesystem::file_size(m_path, ec);
      if (ec)
        return Fail("cannot stat terminal follow input");
      m_head = size / RECORD_SIZE;
      if (m_head > MAX_RECORDS)
        return Fail("follow archive limit exceeded");
      std::ifstream end(m_path + ".end", std::ios::binary);
      char text[32]{};
      end.read(text, sizeof(text));
      const auto length = end.gcount();
      unsigned last = 0;
      const auto parsed = std::from_chars(text, text + (length ? length - 1 : 0), last);
      if (length < 2 || length >= 32 || text[length - 1] != '\n' ||
          parsed.ec != std::errc{} || parsed.ptr != text + length - 1 ||
          size % RECORD_SIZE || m_head == 0 || last != m_head - 1)
      {
        return Fail("invalid follow end marker");
      }
      m_end.assign(text, static_cast<size_t>(length));
    }

    if (frame != m_next)
      return Fail("follow consumer skipped a frame");
    if (!m_end.empty() && frame == m_head)
    {
      if (!Status(".complete", m_end))
        return Fail("cannot write follow completion");
      return Result::Complete;
    }
    if (m_end.empty() && m_head <= static_cast<uint64_t>(frame) + m_delay)
    {
      // Keep bytes from a short record across clearerr/EOF retries.
      if (m_head == frame && size % RECORD_SIZE && !ReadPartial())
        return Result::Error;
      if (now - m_progress >= m_timeout)
        return Fail("follow EOF timeout (no new bytes)");
      return Result::Wait;
    }

    if (!ReadPartial())
      return Result::Error;
    if (m_used != m_record.size())
      return Fail("follow file changed while reading");
    const unsigned index = unsigned(m_record[0]) | (unsigned(m_record[1]) << 8) |
                           (unsigned(m_record[2]) << 16) | (unsigned(m_record[3]) << 24);
    if (index != frame)
      return Fail("follow input gap or duplicate");
    std::copy(m_record.begin() + 4, m_record.end(), pads.begin());
    m_used = 0;
    ++m_next;
    // A buffered history may take arbitrarily long to render; only blocked time may time out.
    m_progress = now;
    return Result::Pads;
  }

  // Return complete records available beyond the requested consumer index.
  uint64_t Backlog(unsigned frame) const
  {
    return m_head > frame ? m_head - frame : 0;
  }

  // Errors are sticky so corrupt input can never resume or fall through to local pads.
  const std::string& Error() const { return m_error; }

private:
  static constexpr uint64_t RECORD_SIZE = 52;
  static constexpr uint64_t MAX_RECORDS = 216000;

  // A short fread is temporary on a growing regular file, not successful EOF.
  bool ReadPartial()
  {
    std::clearerr(m_file);
    m_used += std::fread(m_record.data() + m_used, 1, m_record.size() - m_used, m_file);
    if (std::ferror(m_file))
    {
      Fail("follow read failed");
      return false;
    }
    return true;
  }

  // Retain the first failure for the core log and renderer's .error status.
  Result Fail(const std::string& error)
  {
    if (m_error.empty())
      m_error = error;
    return Result::Error;
  }

  std::string m_path;
  std::string m_end;
  std::string m_error;
  FILE* m_file = nullptr;
  unsigned m_delay;
  unsigned m_next = 0;
  std::chrono::milliseconds m_timeout;
  Clock::time_point m_progress = Clock::now();
  uint64_t m_size = 0;
  uint64_t m_head = 0;
  std::array<unsigned char, RECORD_SIZE> m_record{};
  size_t m_used = 0;
};
}  // namespace NSRNetplay
