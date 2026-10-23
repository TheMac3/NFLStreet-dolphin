// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Platform-neutral telemetry sender decisions shared by NSRTelemetry.cpp and
// its standalone regression test (Tools/NSR/NSRRollbackNetplayTest.cpp).
// Like NSRRollbackLogic.h, nothing here knows about Dolphin, curl or threads:
// the clock and the HTTP poster are parameters, so the shutdown bound can be
// checked with a fake clock and a fake slow server.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace NSRTelemetry::Logic
{
struct FinalFlushResult
{
  std::size_t sent = 0;      // batches the server acknowledged (2xx)
  std::size_t rejected = 0;  // batches the server will never accept (400/413)
  int stop_code = 0;         // status that ended the flush early (0: none or no response)
  bool deadline_hit = false; // stopped because the shared budget ran out
};

// Shutdown flush: post queued batches front to back under ONE total deadline
// shared by every request (the final-review finding L1: a per-request timeout
// let a slow-but-answering server hold teardown for 64 x 2 s).
//   post(batch)   -> HTTP status, 0 when no response arrived; it must itself
//                    give up at `deadline` (the sender aborts curl through its
//                    progress callback), so one request cannot overrun it by
//                    more than the transport's abort granularity.
//   reject(batch) -> the batch can never be accepted (400/413): keep it
//                    locally; it is then popped like a delivered one.
// A delivered batch is popped. Anything else (no response, 401/403, 429,
// 5xx) stops at once, and so does reaching the deadline: whatever is still
// queued is left for the caller to spill to the local JSONL file.
// Design rationale: a shutdown is the one place a slow server can make the
// player wait (the emulator window is already closing); losing the remote
// copy costs nothing because the local file is importable later
// (services/telemetry `import` dedupes by (run, seq)).
template <typename Queue, typename TimePoint, typename Now, typename Post, typename Reject>
FinalFlushResult FinalFlush(Queue& queue, TimePoint deadline, Now&& now, Post&& post,
                            Reject&& reject)
{
  FinalFlushResult result;
  while (!queue.empty())
  {
    if (!(now() < deadline))
    {
      result.deadline_hit = true;
      break;
    }
    const int code = post(queue.front());
    if (code >= 200 && code < 300)
    {
      ++result.sent;
      queue.pop_front();
      continue;
    }
    if (code == 400 || code == 413)
    {
      reject(queue.front());
      ++result.rejected;
      queue.pop_front();
      continue;
    }
    result.stop_code = code;
    result.deadline_hit = !(now() < deadline);
    break;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Desync summaries (whole-session telemetry, 2026-10-18).  Peers upload
// digests of their own state at a desync frame; the server diffs two
// players' lists to name the differing regions.  Only hashes leave the
// machine, never RAM bytes.

constexpr std::uint64_t FNV_OFFSET = 1469598103934665603ULL;
constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

// The milestone-1 lockstep MEM1 hash: FNV-1a over 64-bit words, skipping
// words whose offset `skip(off)` excludes.  With `regions`, the same pass
// also hashes each `region_bytes` region on its own (same skips, same
// words), so the wire hash is unchanged whether or not regions are wanted.
template <typename Skip>
std::uint64_t LockstepMem1Hash(const unsigned char* base, std::uint32_t size, Skip&& skip,
                               std::vector<std::uint64_t>* regions = nullptr,
                               std::uint32_t region_bytes = 0x10000)
{
  std::uint64_t h = FNV_OFFSET;
  if (regions)
    regions->assign((size + region_bytes - 1) / region_bytes, FNV_OFFSET);
  for (std::uint32_t off = 0; off + 8 <= size; off += 8)
  {
    if (skip(off))
      continue;
    std::uint64_t w;
    std::memcpy(&w, base + off, 8);
    h = (h ^ w) * FNV_PRIME;
    if (regions)
    {
      std::uint64_t& r = (*regions)[off / region_bytes];
      r = (r ^ w) * FNV_PRIME;
    }
  }
  return h;
}

// Fold 64-bit digests into 32-bit unit digests of `per_unit` consecutive
// values each (a short last unit is folded as is).  Used for the rollback
// state digest's per-page values (16 pages = 64 KB per unit) and for the
// lockstep region hashes (per_unit 1).  32 bits per unit keeps a summary
// near 4 KB; a false "equal" needs a 2^-32 collision in that unit.
inline std::vector<std::uint32_t> FoldUnits(const std::vector<std::uint64_t>& values,
                                            std::size_t per_unit)
{
  std::vector<std::uint32_t> out;
  if (per_unit == 0)
    return out;
  out.reserve((values.size() + per_unit - 1) / per_unit);
  for (std::size_t i = 0; i < values.size(); i += per_unit)
  {
    std::uint64_t h = FNV_OFFSET;
    for (std::size_t j = i; j < values.size() && j < i + per_unit; ++j)
      h = (h ^ values[j]) * FNV_PRIME;
    out.push_back(static_cast<std::uint32_t>(h ^ (h >> 32)));
  }
  return out;
}

}  // namespace NSRTelemetry::Logic
