// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Platform-neutral telemetry sender decisions shared by NSRTelemetry.cpp and
// its standalone regression test (Tools/NSR/NSRRollbackNetplayTest.cpp).
// Like NSRRollbackLogic.h, nothing here knows about Dolphin, curl or threads:
// the clock and the HTTP poster are parameters, so the shutdown bound can be
// checked with a fake clock and a fake slow server.

#pragma once

#include <cstddef>

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
}  // namespace NSRTelemetry::Logic
