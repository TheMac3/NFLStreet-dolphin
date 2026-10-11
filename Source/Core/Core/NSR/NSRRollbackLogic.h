// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Small, platform-neutral rollback decisions shared by NSRNetplay and its
// standalone regression test.  These helpers intentionally know nothing
// about Dolphin memory, ENet, or host threads, so boundary behavior can be
// exercised on Linux and Windows without starting an emulator.

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace NSRNetplay::RollbackLogic
{
// A page needs copying when a slot has never been synchronized or the page
// was stamped after that slot's synchronization epoch.
constexpr bool PageNeedsCopy(std::uint32_t stamp, std::optional<std::uint32_t> synced)
{
  return !synced || stamp > *synced;
}

// Session boundaries invalidate both kinds of slot metadata. Keeping either
// map would let a zeroed ring slot masquerade as synchronized state on
// re-entry, or replay allocation events from the preceding epoch.
template <typename SyncedSlots, typename SequenceSlots>
void ClearSessionSlotMetadata(SyncedSlots* synced, SequenceSlots* sequences)
{
  synced->clear();
  sequences->clear();
}

// SplitMix64 gives a reproducible, well-distributed diagnostic depth for a
// (seed, frame) pair. A zero maximum is valid and always means no rollback.
constexpr std::uint32_t ForcedDepth(std::uint32_t maximum, std::optional<std::uint32_t> seed,
                                    std::uint32_t live)
{
  if (!seed)
    return maximum;
  std::uint64_t z = (std::uint64_t{*seed} << 32 | live) + 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;
  return static_cast<std::uint32_t>(z % (std::uint64_t{maximum} + 1));
}

// The oldest reachable state is constrained by ring retention, session
// entry, and the newest load barrier. Slots includes the current live slot.
constexpr std::uint32_t RollbackLowerBound(std::uint32_t live, std::uint32_t slots,
                                           std::uint32_t session,
                                           std::uint32_t barrier)
{
  const std::uint32_t retained = slots > 0 && live >= slots - 1 ? live - (slots - 1) : 0;
  return std::max({retained, session, barrier});
}

// Search backward for an I/O-idle snapshot without crossing the lower bound.
// The caller supplies its own map lookup so missing entries stay distinct
// from an explicitly busy snapshot.
template <typename IsIdle>
std::optional<std::uint32_t> FindIdleTarget(std::uint32_t target, std::uint32_t lower,
                                            bool current_idle, IsIdle&& is_idle)
{
  if (!current_idle || target < lower)
    return std::nullopt;
  while (target > lower && !is_idle(target))
    --target;
  return is_idle(target) ? std::optional<std::uint32_t>{target} : std::nullopt;
}

// Return one rotating audit page without allocating in the per-frame dirty
// tracker. The cursor advances exactly once per inspected page.
inline std::optional<std::uint32_t> NextRotatingAuditPage(
    std::span<const std::uint32_t> pages, std::uint32_t* cursor)
{
  if (pages.empty())
    return std::nullopt;
  return pages[(*cursor)++ % pages.size()];
}

struct Range
{
  std::uint32_t lo = 0;
  std::uint32_t hi = 0;

  constexpr bool operator==(const Range&) const = default;
};

// Normalize half-open sidecar ranges. Invalid/empty entries are ignored and
// touching ranges merge because they describe one exclusion interval.
inline std::vector<Range> MergeRanges(std::vector<Range> ranges)
{
  std::erase_if(ranges, [](const Range& range) { return range.lo >= range.hi; });
  std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
    return a.lo < b.lo || (a.lo == b.lo && a.hi < b.hi);
  });
  std::vector<Range> merged;
  for (const Range& range : ranges)
  {
    if (!merged.empty() && range.lo <= merged.back().hi)
      merged.back().hi = std::max(merged.back().hi, range.hi);
    else
      merged.push_back(range);
  }
  return merged;
}

// Pure audio-model arithmetic. Wrapping subtraction matches the guest's u32
// frame clock and therefore remains correct across frame-number rollover.
constexpr std::uint32_t AudioNow(std::uint32_t frame, std::uint32_t in_frame_waits,
                                 std::uint32_t busy_poll_catchups)
{
  return frame + in_frame_waits + busy_poll_catchups;
}

constexpr std::uint32_t FramesToMs(std::uint32_t frames)
{
  return static_cast<std::uint32_t>(std::uint64_t{frames} * 1000 / 60);
}

constexpr std::uint32_t BusyPollCatchups(std::uint32_t queries, std::uint32_t queries_per_tick)
{
  return queries_per_tick == 0 ? 0 : queries / queries_per_tick;
}

constexpr bool Before(std::uint32_t frame, std::uint32_t end)
{
  return static_cast<std::int32_t>(frame - end) < 0;
}

struct SongTimeline
{
  std::uint32_t start = 0;
  std::uint32_t end = 0;
};

constexpr SongTimeline ScheduleSong(std::uint32_t now, std::uint32_t gap,
                                    std::uint32_t duration)
{
  return {now + gap, now + gap + duration};
}
}  // namespace NSRNetplay::RollbackLogic
