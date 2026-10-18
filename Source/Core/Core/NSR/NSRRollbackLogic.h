// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Small, platform-neutral rollback decisions shared by NSRNetplay and its
// standalone regression test.  These helpers intentionally know nothing
// about Dolphin memory, ENet, or host threads, so boundary behavior can be
// exercised on Linux and Windows without starting an emulator.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
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

// Stock InGame feeds its fixed-step tick accumulator only while the pause
// nesting depth is zero, so a frame that starts paused runs no sim tick
// (fix/pausetime). The rollback tick hook must reproduce that decision.
constexpr bool PausedFrame(std::uint32_t pause_depth)
{
  return pause_depth != 0;
}

// Lifecycle transitions (fix/lifecycle). The session leaves at the end of
// the first frame InGame no longer runs; that teardown frame frees audio,
// heap and disc resources and must never run on a predicted input
// (re-simulating it is unsafe, so a later correction cannot be applied). The
// match end is always requested one frame earlier: fn_800282DC clears
// InGame's keep-running byte, the update returns it, and the module manager
// tears InGame down in the next frame. Requests seen in testing: the pause
// menu's Quit confirmation (pause depth != 0); a tutorial drill's modal
// results dialog ("|^Retry|Quit", then "|^No|Yes"; 4-player regress seed
// 3918529284: dialog open from frame 6890, A at 6960 requests the end, 6961
// tears down); and a real match's post-game flow (fn_800AB3B0 at 0x800AB550,
// no modal UI; CPU match: request in 101554, teardown 101555). The input
// driven ones come from modal UI, so a live frame waits for every player's
// confirmed input when it starts
//  - with the pause menu open,
//  - with a modal dialog on top of the in-game UI stack,
//  - after the end was requested (the teardown frame itself), or
//  - within `grace` frames after one of the above, which covers screens
//    that close a few frames before the request takes effect.
// The wait is host-side only (no guest state depends on it), so peers may
// disagree on it without affecting determinism. The menus are static, so
// the added input latency is invisible; normal play never waits (but a
// confirmed frame is lockstep: ~one network hop per frame while it lasts).
// Deferred: if a non-modal request turns out to be input-driven (the
// post-game flow above has not been traced for that), a correction could
// move the request into a re-simulated frame; RbStep logs "re-simulated
// frame ... starts after a match-end request" then. Revisit on that log line.
struct LifecycleSignals
{
  std::uint32_t pause_depth = 0;
  bool in_game_running = true;  // InGame's keep-running byte != 0
  bool modal_dialog = false;    // in-game UI stack top is the modal dialog
};

constexpr bool LifecycleTransition(const LifecycleSignals& s)
{
  return PausedFrame(s.pause_depth) || !s.in_game_running || s.modal_dialog;
}

// `hold_until` (exclusive end of the grace window, 0 = none) is updated.
constexpr bool LifecycleConfirmFrame(std::uint32_t frame, const LifecycleSignals& s,
                                     std::uint32_t grace, std::uint32_t& hold_until)
{
  if (LifecycleTransition(s))
  {
    hold_until = frame + 1 + grace;
    return true;
  }
  return hold_until != 0 && Before(frame, hold_until);
}

// Whether the rollback pad poll (VI post-retrace interrupt) may copy this
// machine's local sample into the game's PADStatus buffer (fix/modal).
// Outside a session, in menus and loading, it does, as the unpatched poll.
// In a frame-mode session the buffer belongs to RbStep, which installs the
// agreed pads at the start of every frame, paused or not. A frame that spans
// three or more retraces (the Restart reset, the Quit teardown/load, some
// play transitions) ends poll "tick mode", and the poll used to overwrite
// the agreed pads with local ones for the rest of that frame, at a
// host-timing-dependent point that differs per peer. Never in a session.
// Legacy tick-unit sessions (frame_mode off) keep the old behavior: there a
// paused or loading frame runs no RbStep, so the poll is the pads' only path.
constexpr bool PollFeedsPadBuffer(bool tick_mode, bool at_live, bool menu_frame_lockstep,
                                  bool frame_session)
{
  return !tick_mode && at_live && !menu_frame_lockstep && !frame_session;
}

// Undo log for rollback-owned bytes that live outside the MEM1/VMEM snapshots
// (final-review M1: the game's ARAM replay ring). Instead of copying the
// window into every ring slot, each guest write into the window first records
// the bytes it is about to overwrite, tagged with a monotonically increasing
// sequence number. A snapshot remembers Mark() when it is saved; restoring it
// undoes every record newer than that mark, newest first, which leaves the
// window exactly as it was when the snapshot was taken (the write order within
// and across frames is irrelevant: reverse-order undo of the complete write
// history since the mark is exact).
//
// Why an undo log and not a per-slot copy of the window: the game writes
// ~2.6 KB every second frame into a ~560 KB window, so the log holds a few
// tens of KB for a 24-slot ring while slot copies would cost 24 x 560 KB of
// memory plus a copy (or a dirty-page scheme) per save.
//
// Invariants (the caller's job):
//   - Marks are only restored backward along the executed timeline: a slot
//     saved in a timeline that a later restore abandoned is re-saved before
//     it can be restored again (the rollback ring re-saves every re-simulated
//     frame), so its stale mark is never used. Sequence numbers are never
//     reused after an undo, so a stale mark cannot alias a newer record.
//   - TrimThrough(oldest) only with the oldest mark any still-restorable
//     snapshot holds; records at or before it can never be undone again.
class WindowUndoLog
{
public:
  // Called before the guest overwrites mem[address, address + length). Only
  // the part inside the window [lo, hi) is recorded; `mem` is the whole
  // backing store (addresses index it directly). Returns bytes recorded.
  std::size_t Record(std::span<const std::uint8_t> mem, std::uint32_t lo, std::uint32_t hi,
                     std::uint32_t address, std::uint32_t length)
  {
    const std::uint64_t end = std::uint64_t{address} + length;
    const std::uint64_t a = std::max<std::uint64_t>(address, lo);
    const std::uint64_t b = std::min<std::uint64_t>({end, hi, mem.size()});
    if (a >= b)
      return 0;
    Entry& e = m_entries.emplace_back();
    e.seq = ++m_seq;
    e.address = static_cast<std::uint32_t>(a);
    e.old.assign(mem.begin() + static_cast<std::ptrdiff_t>(a),
                 mem.begin() + static_cast<std::ptrdiff_t>(b));
    m_bytes += e.old.size();
    return e.old.size();
  }

  std::uint64_t Mark() const { return m_seq; }

  // Undo every record newer than `mark` into `mem`, newest first. Returns the
  // number of records undone.
  std::size_t UndoTo(std::uint64_t mark, std::span<std::uint8_t> mem)
  {
    std::size_t undone = 0;
    while (!m_entries.empty() && m_entries.back().seq > mark)
    {
      const Entry& e = m_entries.back();
      if (std::uint64_t{e.address} + e.old.size() <= mem.size())
        std::copy(e.old.begin(), e.old.end(), mem.begin() + e.address);
      m_bytes -= e.old.size();
      m_entries.pop_back();
      ++undone;
    }
    return undone;
  }

  // Drop records no restorable snapshot can undo any more.
  void TrimThrough(std::uint64_t oldest_mark)
  {
    while (!m_entries.empty() && m_entries.front().seq <= oldest_mark)
    {
      m_bytes -= m_entries.front().old.size();
      m_entries.pop_front();
    }
  }

  // Session boundary: forget everything (the sequence restarts too, because
  // every snapshot mark of the previous session is discarded with it).
  void Clear()
  {
    m_entries.clear();
    m_bytes = 0;
    m_seq = 0;
  }

  std::size_t Records() const { return m_entries.size(); }
  std::size_t Bytes() const { return m_bytes; }

private:
  struct Entry
  {
    std::uint64_t seq = 0;
    std::uint32_t address = 0;
    std::vector<std::uint8_t> old;
  };
  std::deque<Entry> m_entries;
  std::size_t m_bytes = 0;
  std::uint64_t m_seq = 0;
};

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

// While the game is paused the music model's pending deadlines move one
// frame later per paused frame (fix/pausetime), as the real stream is
// stopped. Zero means "no event" and stays zero.
constexpr SongTimeline HoldSongOneFrame(SongTimeline song)
{
  if (song.start)
    ++song.start;
  if (song.end)
    ++song.end;
  return song;
}
}  // namespace NSRNetplay::RollbackLogic
