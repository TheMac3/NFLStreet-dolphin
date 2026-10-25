// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// NSR passive performance telemetry: CPU-thread aggregation, a bounded hand-off
// queue, and a background sender (HTTP POST of gzipped JSON batches with retry,
// plus a local JSONL fallback).  See NSRTelemetry.h for the contract and
// NFLStreet docs/telemetry.md for the wire format and the ingest service.
//
// Threading model:
//   CPU thread:   Count/SetGauge/Observe/LiveFrame/SessionEnter/SessionLeave.
//                 Owns s_cpu (the open window); never waits on a lock it can't
//                 get: a closed window is handed over with try_lock and simply
//                 retried on the next live frame when the sender holds the lock.
//   any thread:   Event (rare; short mutex hold to append to the queue).
//   sender:       owns serialization, batching, compression, HTTP, files.
// Design rationale: windows are serialized on the sender, not on the CPU
// thread, so the per-frame cost is a handful of integer ops and one clock read.

#include "Core/NSR/NSRTelemetry.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <zlib.h>

#ifdef _WIN32
#include "Common/WindowsRegistry.h"
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/utsname.h>
#endif

#include "Common/CPUDetect.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/Version.h"
#include "Core/ConfigManager.h"
#include "Core/NSR/NSRTelemetryLogic.h"

namespace NSRTelemetry
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr int SLOTS = MAX_PEERS + 1;  // slot 0: not per-player; 1..4: player 0..3
constexpr std::uint32_t SUB_BITS = 5;  // 32 sub-buckets per octave: <= 3.1% bucket width
constexpr std::uint32_t SUB = 1u << SUB_BITS;
constexpr std::size_t QUEUE_MAX = 256;                // records waiting for the sender
constexpr std::size_t PENDING_BATCHES_MAX = 64;       // unsent batches kept for retry
constexpr std::size_t BATCH_BYTES_MAX = 192 * 1024;   // uncompressed JSON per POST
constexpr std::uint64_t LOCAL_FILE_MAX = 64ull << 20; // stop writing the fallback past this
// Shutdown: total time for the final send of every pending batch (see Run).
constexpr std::chrono::milliseconds FINAL_SEND_BUDGET{2000};

// Dolphin config keys ([NSR] in Dolphin.ini, or -C Dolphin.NSR.TelemetryURL=...).
const Config::Info<std::string> CFG_URL{{Config::System::Main, "NSR", "TelemetryURL"}, ""};
const Config::Info<std::string> CFG_TOKEN{{Config::System::Main, "NSR", "TelemetryToken"}, ""};

constexpr std::array<const char*, static_cast<size_t>(Counter::Count)> COUNTER_NAMES{
    "live_frames",         "rollbacks",        "resim_frames",        "mispredictions",
    "uncorrectable",       "quiet_skips",      "frames_over_budget",  "host_stalls",
    "dirty_pages_copied",  "dirty_pages_written", "audio_clock_catchups", "net_stalls",
    "sync_waits",          "packets_sent",     "packets_received",    "packets_rejected",
    "desyncs",
};
constexpr std::array<const char*, static_cast<size_t>(Gauge::Count)> GAUGE_NAMES{
    "rtt_ms", "rtt_var_ms", "loss_pct", "frame_advantage", "input_delay", "max_prediction",
    "ring_slots", "audio_gap_fills", "audio_skips", "menu_input_delay",
};
constexpr std::array<const char*, static_cast<size_t>(Hist::Count)> HIST_NAMES{
    "rollback_depth",  "frame_interval_us", "step_us",         "live_unit_us",
    "save_us",         "restore_us",        "live_ranges_us",  "jit_us",
    "check_us",        "net_rtt_us",        "remote_input_late_frames",
    "prediction_frames", "time_sync_wait_us", "net_stall_us", "lockstep_wait_us",
};
constexpr std::array<const char*, static_cast<size_t>(EventKind::Count)> EVENT_NAMES{
    "session_enter", "session_leave", "desync",            "peer_desync", "hang",
    "uncorrectable", "input_timeout", "transport_failure", "note",
    "phase",         "connection",    "handoff_abort",     "config_mismatch", "shutdown",
    "desync_summary",
    "stop_requested",
};
constexpr std::array<const char*, static_cast<size_t>(Phase::Count)> PHASE_NAMES{
    "none", "lockstep-menu", "rollback-match", "post-match",
};

// Events that explain why a run went wrong; the shutdown event names the
// last one, so "why did it stop" is one record.
constexpr bool IsProblem(EventKind kind)
{
  switch (kind)
  {
  case EventKind::Desync:
  case EventKind::PeerDesync:
  case EventKind::Hang:
  case EventKind::Uncorrectable:
  case EventKind::InputTimeout:
  case EventKind::TransportFailure:
  case EventKind::HandoffAbort:
  case EventKind::ConfigMismatch:
    return true;
  default:
    return false;
  }
}

// Every enum entry needs a name (a short initializer list would leave nullptr).
template <size_t N>
constexpr bool AllNamed(const std::array<const char*, N>& names)
{
  for (const char* n : names)
    if (n == nullptr)
      return false;
  return true;
}
static_assert(AllNamed(COUNTER_NAMES) && AllNamed(GAUGE_NAMES) && AllNamed(HIST_NAMES) &&
              AllNamed(EVENT_NAMES) && AllNamed(PHASE_NAMES));

// Bucket count for 64-bit values: values < 2*SUB are exact, then SUB buckets
// per power of two up to 2^64 (see BucketIndex).
constexpr std::uint32_t BUCKET_COUNT = (64 - SUB_BITS + 1) * SUB;

// ---------------------------------------------------------------------------
// Per-window aggregates.
struct GaugeAcc
{
  double last = 0, min = 0, max = 0, sum = 0;
  std::uint32_t n = 0;
};

struct HistAcc
{
  std::uint64_t count = 0, sum = 0, min = UINT64_MAX, max = 0;
  std::array<std::uint32_t, BUCKET_COUNT> buckets{};
};

struct Window
{
  std::uint64_t seq = 0;
  std::int64_t start_unix_ms = 0, end_unix_ms = 0;
  double start_mono_ms = 0, end_mono_ms = 0;
  std::uint32_t frame_first = 0, frame_last = 0;
  bool have_frame = false;
  std::uint64_t dropped = 0;  // records the queue/retry buffer dropped before this window
  std::uint32_t generation = 0;
  Phase phase = Phase::None;  // SetPhase closes the window, so one phase per window
  std::array<std::array<std::uint64_t, SLOTS>, static_cast<size_t>(Counter::Count)> counters{};
  std::array<std::array<GaugeAcc, SLOTS>, static_cast<size_t>(Gauge::Count)> gauges{};
  std::array<std::array<std::unique_ptr<HistAcc>, SLOTS>, static_cast<size_t>(Hist::Count)> hists;
};

struct Record
{
  std::unique_ptr<Window> window;  // serialized by the sender
  std::string json;                // already serialized (header, events)
  bool urgent = false;
};

struct Settings
{
  bool enabled = false;
  std::string url, token, local_path;
  bool local_always = false;
  std::chrono::milliseconds interval{5000};
  std::string session, run, room_code, player_name, fingerprint;
  int player = -1;
  std::uint64_t budget_us = 17535;  // 16.7 ms + 5%
  std::uint64_t stall_us = 50000;
};

// --- shared state ------------------------------------------------------------
std::mutex s_init_mutex;
std::atomic<bool> s_configured{false};
std::atomic<bool> s_enabled{false};
// Set by Shutdown: late callers during core teardown (packet handlers, the
// end of a session) must not start a new sender thread that nobody joins.
// Rearm() (the next session) clears it.
std::atomic<bool> s_shut_down{false};
Settings s_settings;  // written once under s_init_mutex before s_configured
// Start() facts for the header (under s_init_mutex; read by HeaderJson).
StartInfo s_start;
bool s_have_start = false;
// Current phase (CPU thread writes; events from any thread read it), the
// last live frame seen, and the last problem event (for the shutdown event).
std::atomic<std::uint8_t> s_phase{static_cast<std::uint8_t>(Phase::None)};
std::atomic<std::uint32_t> s_last_frame{0};
std::atomic<int> s_last_problem{-1};
std::atomic<std::uint32_t> s_last_problem_frame{0};
std::atomic<int> s_first_problem{-1};
const char* PhaseName(Phase phase)
{
  const size_t i = static_cast<size_t>(phase);
  return i < PHASE_NAMES.size() ? PHASE_NAMES[i] : "none";
}
Phase LoadPhase()
{
  return static_cast<Phase>(s_phase.load(std::memory_order_relaxed));
}

std::mutex s_queue_mutex;
std::condition_variable s_queue_cv;
std::deque<Record> s_queue;
bool s_stop = false;
std::atomic<std::uint64_t> s_dropped{0};
std::atomic<std::uint64_t> s_record_seq{0};
std::thread s_sender;
// Shutdown deadline (steady-clock ticks since epoch; 0 = not shutting down),
// set by Shutdown before it wakes the sender. Every HTTP request, including
// one already in flight when the emulator closes, aborts at this deadline,
// so the whole teardown shares one FINAL_SEND_BUDGET (final review L1).
std::atomic<Clock::rep> s_stop_deadline{0};
bool BeforeStopDeadline()
{
  const Clock::rep d = s_stop_deadline.load(std::memory_order_relaxed);
  return d == 0 || Clock::now().time_since_epoch().count() < d;
}
Clock::time_point s_epoch = Clock::now();

// --- CPU-thread state ----------------------------------------------------------
struct CpuState
{
  std::unique_ptr<Window> open;
  std::unique_ptr<Window> closed;  // waiting for the queue lock
  Clock::time_point window_start{};
  std::uint64_t window_seq = 0;
  std::uint32_t generation = 0;
};
CpuState s_cpu;

std::int64_t UnixMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

double MonoMs(Clock::time_point t)
{
  return std::chrono::duration<double, std::milli>(t - s_epoch).count();
}

std::string Env(const char* name)
{
  const char* v = std::getenv(name);
  return v ? v : "";
}

// Printable, bounded text for identifiers and display names.
std::string Clean(std::string_view text, size_t limit)
{
  std::string out;
  for (char c : text)
  {
    if (out.size() >= limit)
      break;
    if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F)
      out.push_back(c);
  }
  return out;
}

void JsonString(std::string& out, std::string_view s)
{
  out.push_back('"');
  for (char c : s)
  {
    switch (c)
    {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20)
        out += fmt::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
      else
        out.push_back(c);
    }
  }
  out.push_back('"');
}

void JsonNumber(std::string& out, double v)
{
  if (!(v == v) || v > 1e300 || v < -1e300)  // NaN / inf are not JSON
    out += "null";
  else
    out += fmt::format("{:.6g}", v);
}

// Common envelope for every record (also makes local JSONL lines self-describing).
void Envelope(std::string& out, const char* type, std::uint64_t seq)
{
  const Settings& s = s_settings;
  out += "{\"type\":";
  JsonString(out, type);
  out += ",\"session\":";
  JsonString(out, s.session);
  out += fmt::format(",\"player\":{},\"run\":", s.player);
  JsonString(out, s.run);
  out += fmt::format(",\"seq\":{}", seq);
}

std::string MetricKey(const char* name, int slot)
{
  return slot == 0 ? std::string(name) : fmt::format("{}@p{}", name, slot - 1);
}

std::uint64_t Percentile(const HistAcc& h, double q)
{
  if (h.count == 0)
    return 0;
  const std::uint64_t rank = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(q * static_cast<double>(h.count) + 0.999999));
  std::uint64_t seen = 0;
  for (std::uint32_t i = 0; i < BUCKET_COUNT; ++i)
  {
    if (h.buckets[i] == 0)
      continue;
    seen += h.buckets[i];
    if (seen >= rank)
    {
      // Middle of the bucket, clamped to the exact observed range.
      const std::uint64_t lo = BucketLow(i);
      const std::uint64_t hi = i + 1 < BUCKET_COUNT ? BucketLow(i + 1) : UINT64_MAX;
      const std::uint64_t mid = lo + (hi - lo) / 2;
      return std::clamp(mid, h.min, h.max);
    }
  }
  return h.max;
}

std::string SerializeWindow(const Window& w, std::uint64_t seq)
{
  std::string out;
  out.reserve(2048);
  Envelope(out, "window", seq);
  out += fmt::format(",\"window\":{},\"generation\":{},\"phase\":\"{}\",\"t\":{},\"t_end\":{},"
                     "\"mono_ms\":",
                     w.seq, w.generation, PhaseName(w.phase), w.start_unix_ms, w.end_unix_ms);
  JsonNumber(out, w.start_mono_ms);
  out += ",\"mono_end_ms\":";
  JsonNumber(out, w.end_mono_ms);
  if (w.have_frame)
    out += fmt::format(",\"frame_first\":{},\"frame_last\":{}", w.frame_first, w.frame_last);
  out += fmt::format(",\"dropped\":{}", w.dropped);

  out += ",\"counters\":{";
  bool first = true;
  for (size_t c = 0; c < w.counters.size(); ++c)
  {
    for (int slot = 0; slot < SLOTS; ++slot)
    {
      if (w.counters[c][slot] == 0)
        continue;
      if (!first)
        out.push_back(',');
      first = false;
      JsonString(out, MetricKey(COUNTER_NAMES[c], slot));
      out += fmt::format(":{}", w.counters[c][slot]);
    }
  }
  out += "},\"gauges\":{";
  first = true;
  for (size_t g = 0; g < w.gauges.size(); ++g)
  {
    for (int slot = 0; slot < SLOTS; ++slot)
    {
      const GaugeAcc& a = w.gauges[g][slot];
      if (a.n == 0)
        continue;
      if (!first)
        out.push_back(',');
      first = false;
      JsonString(out, MetricKey(GAUGE_NAMES[g], slot));
      out += ":{\"last\":";
      JsonNumber(out, a.last);
      out += ",\"min\":";
      JsonNumber(out, a.min);
      out += ",\"max\":";
      JsonNumber(out, a.max);
      out += ",\"mean\":";
      JsonNumber(out, a.sum / a.n);
      out += fmt::format(",\"n\":{}}}", a.n);
    }
  }
  out += "},\"hists\":{";
  first = true;
  for (size_t h = 0; h < w.hists.size(); ++h)
  {
    for (int slot = 0; slot < SLOTS; ++slot)
    {
      const HistAcc* a = w.hists[h][slot].get();
      if (!a || a->count == 0)
        continue;
      if (!first)
        out.push_back(',');
      first = false;
      JsonString(out, MetricKey(HIST_NAMES[h], slot));
      out += fmt::format(":{{\"n\":{},\"sum\":{},\"min\":{},\"max\":{},\"p50\":{},\"p95\":{},"
                         "\"p99\":{},\"buckets\":[",
                         a->count, a->sum, a->min, a->max, Percentile(*a, 0.50),
                         Percentile(*a, 0.95), Percentile(*a, 0.99));
      // Sparse [lower bound, count] pairs: the server can merge windows exactly.
      bool bfirst = true;
      for (std::uint32_t i = 0; i < BUCKET_COUNT; ++i)
      {
        if (a->buckets[i] == 0)
          continue;
        if (!bfirst)
          out.push_back(',');
        bfirst = false;
        out += fmt::format("[{},{}]", BucketLow(i), a->buckets[i]);
      }
      out += "]}";
    }
  }
  out += "}}";
  return out;
}

std::string OsDescription()
{
#ifdef _WIN32
  const OSVERSIONINFOW v = WindowsRegistry::GetOSVersion();
  return fmt::format("Windows {}.{}.{}", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
#elif defined(__unix__) || defined(__APPLE__)
  utsname u{};
  if (uname(&u) == 0)
    return fmt::format("{} {} {}", u.sysname, u.release, u.machine);  // no nodename (host name)
  return "unix";
#else
  return "unknown";
#endif
}

// NSR_RB_* rollback options that have numeric/flag values.  Anything else
// (file paths for replay/record/logs may contain user names) is reported only
// as "set".
std::string RollbackEnvJson()
{
  static constexpr const char* KEYS[] = {
      "NSR_RB_FORCE",        "NSR_RB_FORCE_FROM",  "NSR_RB_MISPREDICT",   "NSR_RB_VTIME_FROM",
      "NSR_RB_IO_IDLE",      "NSR_RB_AUDIO_ROLLBACK", "NSR_RB_INSTANT_DISC", "NSR_RB_RING",
      "NSR_RB_DIRTY",        "NSR_RB_RESIM_SPEED", "NSR_RB_RESIM_DRAW",   "NSR_RB_VMEM_BYTES",
      "NSR_RB_SESSION_END",  "NSR_RB_AUTO_REENTER", "NSR_RB_NET",         "NSR_RB_DELAY",
      "NSR_RB_MAX_PRED",     "NSR_RB_TIME_SYNC",   "NSR_RB_REPLAY",       "NSR_RB_RECORD",
      "NSR_DELAY",           "NSR_PLAYERS",        "NSR_TOPOLOGY",        "NSR_NETPLAY",
  };
  std::string out = "{";
  bool first = true;
  for (const char* key : KEYS)
  {
    const std::string v = Env(key);
    if (v.empty())
      continue;
    const bool simple =
        v.size() <= 16 && std::all_of(v.begin(), v.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 c == '.' || c == 'x' || c == '-';
        });
    if (!first)
      out.push_back(',');
    first = false;
    JsonString(out, key);
    out.push_back(':');
    JsonString(out, simple ? v : std::string("set"));
  }
  out += "}";
  return out;
}

std::string HeaderJson()
{
  const Settings& s = s_settings;
  std::string out;
  Envelope(out, "header", s_record_seq.fetch_add(1));
  // schema 2 (2026-10-18): phase, build_id, netplay, config (docs/telemetry.md).
  out += fmt::format(",\"t\":{},\"mono_ms\":0,\"schema\":2,\"phase\":\"{}\"", UnixMs(),
                     PhaseName(LoadPhase()));
  out += ",\"build_id\":";
  JsonString(out, Clean(Common::GetScmRevGitStr(), 12));
  out += ",\"room_code\":";
  JsonString(out, s.room_code);
  out += ",\"player_name\":";
  JsonString(out, s.player_name);
  out += ",\"build\":{\"rev\":";
  JsonString(out, Common::GetScmRevGitStr());
  out += ",\"desc\":";
  JsonString(out, Common::GetScmRevStr());
  out += ",\"branch\":";
  JsonString(out, Clean(Common::GetScmBranchStr(), 64));
  out += "},\"game\":{\"id\":";
  JsonString(out, Clean(SConfig::GetInstance().GetGameID(), 16));
  out += fmt::format(",\"revision\":{},\"fingerprint\":", SConfig::GetInstance().GetRevision());
  JsonString(out, s.fingerprint);
  out += "},\"host\":{\"os\":";
  JsonString(out, OsDescription());
  out += ",\"cpu\":";
  JsonString(out, Clean(cpu_info.model_name, 96));
  out += fmt::format(",\"cores\":{},\"threads\":{}}}",
                     cpu_info.num_cores, std::thread::hardware_concurrency());
  out += fmt::format(",\"window_ms\":{},\"budget_us\":{},\"rollback_env\":",
                     s.interval.count(), s.budget_us);
  out += RollbackEnvJson();
  if (s_have_start)
  {
    const StartInfo& st = s_start;
    out += fmt::format(",\"netplay\":{{\"players\":{},\"delay\":{},\"star\":{},\"punch\":{},"
                       "\"rollback_image\":{}}}",
                       st.players, st.delay, st.star, st.punch, st.rollback_image);
    out += ",\"config\":{\"scope\":";
    JsonString(out, st.config_scope);
    out += fmt::format(",\"digest\":\"{:016x}\",\"fields\":{{", st.config_digest);
    bool first = true;
    for (const auto& [name, value] : st.config_fields)
    {
      if (!first)
        out.push_back(',');
      first = false;
      JsonString(out, name);
      out += fmt::format(":\"{:016x}\"", value);
    }
    out += "}}";
  }
  out += "}";
  return out;
}

void PushRecord(Record&& r)
{
  {
    std::lock_guard lk(s_queue_mutex);
    if (s_stop)
      return;
    if (s_queue.size() >= QUEUE_MAX)
    {
      s_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    s_queue.push_back(std::move(r));
  }
  s_queue_cv.notify_one();
}

// ---------------------------------------------------------------------------
// Sender thread.
class Sender
{
public:
  void Run()
  {
    const Settings& s = s_settings;
    if (!s.url.empty())
      m_http = std::make_unique<Common::HttpRequest>(
          std::chrono::milliseconds{5000}, [](s64, s64, s64, s64) { return BeforeStopDeadline(); });
    auto next_send = Clock::now();
    for (;;)
    {
      std::deque<Record> batch;
      bool stopping = false;
      {
        std::unique_lock lk(s_queue_mutex);
        // Coalesce: send at most every ~2 s unless something urgent arrived;
        // wake for retries while batches are pending.
        // Short poll bound so a coalesced record never waits on the next notify.
        const auto poll = Clock::now() + std::chrono::milliseconds(500);
        const auto deadline = m_pending.empty() ? poll : std::min(poll, m_retry_at);
        s_queue_cv.wait_until(lk, std::max(deadline, Clock::now()), [&] {
          if (s_stop)
            return true;
          if (s_queue.empty())
            return false;
          const bool urgent = std::any_of(s_queue.begin(), s_queue.end(),
                                          [](const Record& r) { return r.urgent; });
          return urgent || Clock::now() >= next_send;
        });
        stopping = s_stop;
        batch.swap(s_queue);
      }
      Absorb(batch);
      if (stopping)
        break;
      if (!m_pending.empty() && Clock::now() >= m_retry_at)
        Flush();
      next_send = Clock::now() + std::chrono::seconds(2);
    }
    // Shutdown: one bounded final attempt, then spill whatever is left.
    // The bound is ONE total budget, from Shutdown(), for the request that
    // may still be in flight plus every pending batch (final review L1: a
    // per-request 2 s timeout let a slow server that still answers keep
    // teardown busy for up to PENDING_BATCHES_MAX x 2 s). Requests are
    // aborted through curl's progress callback once the budget is spent;
    // curl calls it at least about once a second while waiting, so teardown
    // waits at most ~FINAL_SEND_BUDGET + 1 s here.
    if (!m_pending.empty() && !m_auth_failed && !s.url.empty())
    {
      const Clock::rep set = s_stop_deadline.load(std::memory_order_relaxed);
      const auto deadline =
          set ? Clock::time_point(Clock::duration(set)) : Clock::now() + FINAL_SEND_BUDGET;
      m_http = std::make_unique<Common::HttpRequest>(
          FINAL_SEND_BUDGET, [deadline](s64, s64, s64, s64) { return Clock::now() < deadline; });
      Logic::FinalFlush(
          m_pending, deadline, [] { return Clock::now(); },
          [this](const Batch& b) { return Post(b); }, [this](Batch& b) { Spill(b); });
    }
    for (Batch& b : m_pending)
      Spill(b);
    m_pending.clear();
    if (m_file)
      m_file.reset();
  }

private:
  struct Batch
  {
    std::vector<std::string> lines;
    size_t bytes = 0;
    std::uint64_t id = 0;
    int attempts = 0;
    bool spilled = false;  // already copied to the local file
  };

  void Absorb(std::deque<Record>& records)
  {
    const Settings& s = s_settings;
    for (Record& r : records)
    {
      std::string line =
          r.window ? SerializeWindow(*r.window, s_record_seq.fetch_add(1)) : std::move(r.json);
      if (s.local_always || s.url.empty() || m_auth_failed)
        WriteLocal(line);
      if (s.url.empty() || m_auth_failed)
        continue;
      if (m_pending.empty() || m_pending.back().bytes + line.size() > BATCH_BYTES_MAX ||
          m_pending.back().attempts > 0)
      {
        if (m_pending.size() >= PENDING_BATCHES_MAX)
        {
          // Retry buffer full (server unreachable for minutes): keep the newest
          // data in memory, move the oldest batch to the local file.
          s_dropped.fetch_add(m_pending.front().lines.size(), std::memory_order_relaxed);
          Spill(m_pending.front());
          m_pending.pop_front();
        }
        // While the server is failing, new batches are mirrored to the local
        // file as they fill (see Spill).
        m_pending.push_back(Batch{{}, 0, m_next_batch++, 0, m_failing});
      }
      Batch& back = m_pending.back();
      if (back.spilled && !s.local_always)
        WriteLocal(line);
      back.bytes += line.size();
      back.lines.push_back(std::move(line));
    }
  }

  // Normal operation (the shutdown flush is Logic::FinalFlush in Run).
  void Flush()
  {
    while (!m_pending.empty() && !m_auth_failed)
    {
      Batch& b = m_pending.front();
      const int code = Post(b);
      if (code >= 200 && code < 300)
      {
        m_failing = false;
        m_pending.pop_front();
        m_backoff = std::chrono::milliseconds{1000};
        continue;
      }
      ++b.attempts;
      if (code == 401 || code == 403)
      {
        // Wrong or expired token: stop talking to the server for this run and
        // keep everything locally, so it can be uploaded later.
        m_auth_failed = true;
        for (Batch& pb : m_pending)
          Spill(pb);
        m_pending.clear();
        return;
      }
      if (code == 400 || code == 413)
      {
        // The server will never accept this batch: keep it locally, move on.
        Spill(b);
        m_pending.pop_front();
        continue;
      }
      // Unreachable or overloaded: copy everything pending to the local file
      // now (and later batches as they fill), so a crash or kill before a
      // retry succeeds loses nothing.  A later successful retry duplicates
      // these records there; `import` dedupes by (run, seq).
      m_failing = true;
      for (Batch& pb : m_pending)
        Spill(pb);
      // Exponential backoff with jitter, 1 s .. 60 s (429 and 5xx included).
      std::uniform_int_distribution<int> jitter(0, static_cast<int>(m_backoff.count() / 4));
      m_retry_at = Clock::now() + m_backoff + std::chrono::milliseconds(jitter(m_rng));
      m_backoff = std::min(m_backoff * 2, std::chrono::milliseconds{60000});
      return;
    }
  }

  // Returns the HTTP status, or 0 when no response arrived.
  int Post(const Batch& b)
  {
    if (!m_http || !m_http->IsValid())
      return 0;
    const Settings& s = s_settings;
    std::string body;
    body.reserve(b.bytes + 256);
    body += "{\"v\":1,\"session\":";
    JsonString(body, s.session);
    body += fmt::format(",\"player\":{},\"run\":", s.player);
    JsonString(body, s.run);
    body += fmt::format(",\"batch\":{},\"records\":[", b.id);
    for (size_t i = 0; i < b.lines.size(); ++i)
    {
      if (i)
        body.push_back(',');
      body += b.lines[i];
    }
    body += "]}";

    Common::HttpRequest::Headers headers;
    headers["Content-Type"] = "application/json";
    if (!s.token.empty())
      headers["Authorization"] = "Bearer " + s.token;
    std::vector<u8> payload;
    if (Gzip(body, &payload))
      headers["Content-Encoding"] = "gzip";
    else
      payload.assign(body.begin(), body.end());
    const auto response =
        m_http->Post(s.url, payload, headers, Common::HttpRequest::AllowedReturnCodes::All);
    if (!response)
      return 0;
    return m_http->GetLastResponseCode();
  }

  void Spill(Batch& b)
  {
    if (b.spilled || s_settings.local_always)  // local_always wrote it on arrival
      return;
    b.spilled = true;
    for (const auto& line : b.lines)
      WriteLocal(line);
  }

  static bool Gzip(const std::string& in, std::vector<u8>* out)
  {
    z_stream z{};
    if (deflateInit2(&z, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
      return false;
    out->resize(deflateBound(&z, static_cast<uLong>(in.size())) + 32);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = out->data();
    z.avail_out = static_cast<uInt>(out->size());
    const int rc = deflate(&z, Z_FINISH);
    const size_t produced = out->size() - z.avail_out;
    deflateEnd(&z);
    if (rc != Z_STREAM_END)
      return false;
    out->resize(produced);
    return true;
  }

  void WriteLocal(const std::string& line)
  {
    const Settings& s = s_settings;
    if (s.local_path.empty() || m_local_bytes > LOCAL_FILE_MAX)
      return;
    if (!m_file)
    {
      File::CreateFullPath(s.local_path);
      m_file = std::make_unique<File::IOFile>(s.local_path, "ab");
    }
    if (!m_file->IsOpen())
      return;
    m_file->WriteString(line);
    m_file->WriteString("\n");
    m_file->Flush();
    m_local_bytes += line.size() + 1;
  }

  std::unique_ptr<Common::HttpRequest> m_http;
  std::deque<Batch> m_pending;
  std::uint64_t m_next_batch = 0;
  Clock::time_point m_retry_at = Clock::now();
  std::chrono::milliseconds m_backoff{1000};
  bool m_auth_failed = false;
  bool m_failing = false;  // the last attempt failed (server unreachable/overloaded)
  std::unique_ptr<File::IOFile> m_file;
  std::uint64_t m_local_bytes = 0;
  std::mt19937 m_rng{std::random_device{}()};
};

// ---------------------------------------------------------------------------
// Configuration (once per process lifetime / after Shutdown).
void Configure()
{
  std::lock_guard lk(s_init_mutex);
  if (s_configured.load())
    return;
  Settings s;
  if (s_shut_down.load())
  {
    s_settings = Settings{};
    s_enabled = false;
    s_configured = true;
    return;
  }
  const std::string flag = Env("NSR_TELEMETRY");
  s.url = Env("NSR_TELEMETRY_URL");
  if (s.url.empty())
    s.url = Config::Get(CFG_URL);
  s.token = Env("NSR_TELEMETRY_TOKEN");
  if (s.token.empty())
    s.token = Config::Get(CFG_TOKEN);
  s.local_always = !Env("NSR_TELEMETRY_LOCAL").empty() && Env("NSR_TELEMETRY_LOCAL") != "0";
  s.enabled = flag != "0" && (!s.url.empty() || s.local_always || flag == "1");
  if (s.enabled)
  {
    if (const std::string v = Env("NSR_TELEMETRY_INTERVAL"); !v.empty())
      s.interval = std::chrono::milliseconds(
          std::clamp<long>(std::strtol(v.c_str(), nullptr, 10), 1, 60) * 1000);
    std::random_device rd;
    s.run = fmt::format("{:08x}{:08x}", rd(), rd());
    s.room_code = Clean(Env("NSR_ROOM_CODE"), 16);
    s.player_name = Clean(Env("NSR_PLAYER_NAME"), 24);
    s.fingerprint = Clean(Env("NSR_FINGERPRINT"), 64);
    s.session = Clean(Env("NSR_TELEMETRY_SESSION"), 64);
    if (s.session.empty())
      s.session = Clean(Env("NSR_ROOM"), 64);
    if (s.session.empty() && !Env("NSR_SEED").empty())
      s.session = fmt::format("seed-{:08x}", std::strtoul(Env("NSR_SEED").c_str(), nullptr, 0));
    if (s.session.empty())
      s.session = "run-" + s.run;
    // Session ids become directory names on the server: keep them tame.
    for (char& c : s.session)
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '-' || c == '_'))
        c = '_';
    s.player = Env("NSR_PLAYER").empty() ? 0 : std::atoi(Env("NSR_PLAYER").c_str());
    s.local_path = Env("NSR_TELEMETRY_FILE");
    if (s.local_path.empty())
      s.local_path = File::GetUserPath(D_USER_IDX) + "NSR" DIR_SEP "Telemetry" DIR_SEP +
                     s.session + "-p" + std::to_string(s.player) + "-" + s.run + ".jsonl";
  }
  s_settings = std::move(s);
  s_enabled = s_settings.enabled;
  s_epoch = Clock::now();
  if (s_settings.enabled)
  {
    {
      std::lock_guard qlk(s_queue_mutex);
      s_stop = false;
      s_queue.clear();
    }
    s_sender = std::thread([] {
      Sender sender;
      sender.Run();
    });
    PushRecord(Record{nullptr, HeaderJson(), true});
  }
  s_configured = true;
}

Window& Open()
{
  if (!s_cpu.open)
  {
    s_cpu.open = std::make_unique<Window>();
    s_cpu.open->seq = s_cpu.window_seq++;
    s_cpu.open->generation = s_cpu.generation;
    s_cpu.open->phase = LoadPhase();
    s_cpu.window_start = Clock::now();
    s_cpu.open->start_unix_ms = UnixMs();
    s_cpu.open->start_mono_ms = MonoMs(s_cpu.window_start);
  }
  return *s_cpu.open;
}

int Slot(int peer)
{
  return peer >= 0 && peer < MAX_PEERS ? peer + 1 : 0;
}

// Hand the closed window to the sender without ever waiting for the lock.
void TryHandOff()
{
  if (!s_cpu.closed)
    return;
  std::unique_lock lk(s_queue_mutex, std::try_to_lock);
  if (!lk.owns_lock())
    return;  // the sender is swapping the queue; retry next live frame
  if (s_stop)
  {
    s_cpu.closed.reset();
    return;
  }
  if (s_queue.size() >= QUEUE_MAX)
  {
    s_dropped.fetch_add(1, std::memory_order_relaxed);
    s_cpu.closed.reset();
    return;
  }
  s_queue.push_back(Record{std::move(s_cpu.closed), {}, false});
  lk.unlock();
  s_queue_cv.notify_one();
}

void CloseWindow(Clock::time_point now)
{
  if (!s_cpu.open)
    return;
  if (s_cpu.closed)
  {
    // The previous window never got the lock (extremely unlikely): merge is
    // not worth the code; count it as dropped.
    s_dropped.fetch_add(1, std::memory_order_relaxed);
    s_cpu.closed.reset();
  }
  Window& w = *s_cpu.open;
  w.end_unix_ms = UnixMs();
  w.end_mono_ms = MonoMs(now);
  w.dropped = s_dropped.exchange(0, std::memory_order_relaxed);
  s_cpu.closed = std::move(s_cpu.open);
  TryHandOff();
}
}  // namespace

// ---------------------------------------------------------------------------
// Public API.
std::uint32_t BucketIndex(std::uint64_t v)
{
  if (v < 2 * SUB)
    return static_cast<std::uint32_t>(v);
  const std::uint32_t e = static_cast<std::uint32_t>(std::bit_width(v)) - 1;  // >= SUB_BITS + 1
  const std::uint32_t shift = e - SUB_BITS;
  return (shift + 1) * SUB + static_cast<std::uint32_t>((v >> shift) - SUB);
}

std::uint64_t BucketLow(std::uint32_t i)
{
  if (i < 2 * SUB)
    return i;
  const std::uint32_t shift = i / SUB - 1;
  return static_cast<std::uint64_t>(SUB + i % SUB) << shift;
}

bool Enabled()
{
  if (!s_configured.load(std::memory_order_acquire))
    Configure();
  return s_enabled.load(std::memory_order_relaxed);
}

void Count(Counter c, std::uint64_t n, int peer)
{
  if (!Enabled())
    return;
  Open().counters[static_cast<size_t>(c)][Slot(peer)] += n;
}

void SetGauge(Gauge g, double value, int peer)
{
  if (!Enabled())
    return;
  GaugeAcc& a = Open().gauges[static_cast<size_t>(g)][Slot(peer)];
  if (a.n == 0)
  {
    a.min = a.max = value;
  }
  else
  {
    a.min = std::min(a.min, value);
    a.max = std::max(a.max, value);
  }
  a.last = value;
  a.sum += value;
  ++a.n;
}

void Observe(Hist h, std::uint64_t value, int peer)
{
  if (!Enabled())
    return;
  auto& slot = Open().hists[static_cast<size_t>(h)][Slot(peer)];
  if (!slot)
    slot = std::make_unique<HistAcc>();  // first sample of this metric in the window
  HistAcc& a = *slot;
  ++a.count;
  a.sum += value;
  a.min = std::min(a.min, value);
  a.max = std::max(a.max, value);
  ++a.buckets[std::min(BucketIndex(value), BUCKET_COUNT - 1)];
}

void LiveFrame(std::uint32_t frame, Clock::time_point now)
{
  if (!Enabled())
    return;
  Window& w = Open();
  if (!w.have_frame)
  {
    w.frame_first = frame;
    w.have_frame = true;
  }
  w.frame_last = frame;
  s_last_frame.store(frame, std::memory_order_relaxed);
  ++w.counters[static_cast<size_t>(Counter::LiveFrames)][0];
  TryHandOff();
  if (now - s_cpu.window_start >= s_settings.interval)
    CloseWindow(now);
}

// Windows remain visible while ENet is connecting or waiting for input.
void Poll()
{
  if (!Enabled())
    return;
  Open();
  TryHandOff();
  const auto now = Clock::now();
  if (now - s_cpu.window_start >= s_settings.interval)
    CloseWindow(now);
}

void SessionEnter(const RollbackInfo& info)
{
  if (!Enabled())
    return;
  // A new rollback epoch starts a new window so windows never straddle sessions.
  CloseWindow(Clock::now());
  s_cpu.generation = info.generation;
  SetGauge(Gauge::RingSlots, info.ring_slots);
  Event(EventKind::SessionEnter, info.enter_frame,
        {Int("generation", info.generation), Int("force", info.force),
         Int("mispredict", info.mispredict), Int("ring_slots", info.ring_slots),
         Int("io_idle", info.io_idle), Int("audio_rollback", info.audio_rollback),
         Int("dirty_tracking", info.dirty_tracking), Int("dirty_mode", info.dirty_mode),
         Int("frame_mode", info.frame_mode),
         Int("vmem_bytes", info.vmem_bytes), Hex("config_digest", info.config_digest)});
}

void SessionLeave(std::uint32_t frame, std::string_view reason, std::initializer_list<Field> totals)
{
  if (!Enabled())
    return;
  CloseWindow(Clock::now());
  Event(EventKind::SessionLeave, frame, totals, reason);
}

namespace
{
// Envelope + event name, frame, times and phase: the start of every event.
std::string EventPrefix(EventKind kind, std::uint32_t frame, size_t reserve)
{
  std::string out;
  out.reserve(reserve);
  Envelope(out, "event", s_record_seq.fetch_add(1));
  out += ",\"event\":";
  JsonString(out, EVENT_NAMES[static_cast<size_t>(kind)]);
  out += fmt::format(",\"frame\":{},\"phase\":\"{}\",\"t\":{},\"mono_ms\":", frame,
                     PhaseName(LoadPhase()), UnixMs());
  JsonNumber(out, MonoMs(Clock::now()));
  if (IsProblem(kind))
  {
    int unset = -1;
    s_first_problem.compare_exchange_strong(unset, static_cast<int>(kind));
    s_last_problem.store(static_cast<int>(kind), std::memory_order_relaxed);
    s_last_problem_frame.store(frame, std::memory_order_relaxed);
  }
  return out;
}
}  // namespace

void Event(EventKind kind, std::uint32_t frame, std::initializer_list<Field> fields,
           std::string_view text)
{
  if (!Enabled())
    return;
  std::string out = EventPrefix(kind, frame, 256);
  out += ",\"fields\":{";
  bool first = true;
  for (const Field& f : fields)
  {
    if (!first)
      out.push_back(',');
    first = false;
    JsonString(out, f.key);
    out.push_back(':');
    switch (f.type)
    {
    case Field::Type::Int:
      out += fmt::format("{}", f.i);
      break;
    case Field::Type::Hex:
      out += fmt::format("\"{:016x}\"", f.u);
      break;
    case Field::Type::Real:
      JsonNumber(out, f.d);
      break;
    }
  }
  out += "}";
  if (!text.empty())
  {
    out += ",\"text\":";
    JsonString(out, Clean(text, 320));  // handoff/config messages name both sides
  }
  out += "}";
  const bool urgent =
      kind != EventKind::SessionEnter && kind != EventKind::Note && kind != EventKind::Phase;
  PushRecord(Record{nullptr, std::move(out), urgent});
}

void DesyncSummary(const DesyncSummaryInfo& info)
{
  if (!Enabled())
    return;
  // At most 4096 units (~45 KB of JSON): the server rejects records over
  // 64 KB.  Real summaries have ~400 (24 MB MEM1 / 64 KB regions).
  constexpr size_t MAX_UNITS = 4096;
  const size_t n = std::min(info.units.size(), MAX_UNITS);
  std::string out = EventPrefix(EventKind::DesyncSummary, info.frame, 512 + n * 11);
  out += fmt::format(",\"generation\":{},\"evidence_frame\":{},\"fields\":{{\"player\":{},\"local\":\"{:016x}\",\"remote\":\"{:016x}\","
                     "\"semantic\":\"{:016x}\",\"semantic_valid\":{},\"aram\":\"{:016x}\",\"aram_valid\":{},"
                     "\"excluded_ranges\":{},\"excluded_hash\":\"{:016x}\","
                     "\"unit_bytes\":{},\"units\":{},\"units_truncated\":{}}}",
                     info.generation, info.evidence_frame == UINT32_MAX ? info.frame : info.evidence_frame,
                     info.player, info.local, info.remote, info.semantic, info.semantic_valid,
                     info.aram, info.aram_valid, info.excluded_ranges,
                     info.excluded_hash, info.unit_bytes, info.units.size(),
                     info.units.size() > n ? 1 : 0);
  out += ",\"source\":";
  JsonString(out, Clean(info.source ? info.source : "", 32));
  // File name only: a directory would carry the OS user name.
  std::string_view bundle = info.bundle;
  if (const size_t slash = bundle.find_last_of("/\\"); slash != std::string_view::npos)
    bundle.remove_prefix(slash + 1);
  out += ",\"bundle\":";
  JsonString(out, Clean(bundle, 128));
  out += ",\"digests\":[";
  for (size_t i = 0; i < n; ++i)
  {
    if (i)
      out.push_back(',');
    out += fmt::format("\"{:08x}\"", info.units[i]);
  }
  out += "]}";
  PushRecord(Record{nullptr, std::move(out), true});
}

void Start(const StartInfo& info)
{
  Rearm();  // a new boot after a previous core shutdown (GUI process)
  bool resend = false;
  {
    std::lock_guard lk(s_init_mutex);
    s_start = info;
    s_have_start = true;
    s_phase = static_cast<std::uint8_t>(info.phase);
    resend = s_configured.load();
  }
  // Not configured yet: Configure (via Enabled) sends the header with these
  // facts.  Already running (something reported first): send it again.
  if (Enabled() && resend)
    PushRecord(Record{nullptr, HeaderJson(), true});
}

void SetPhase(Phase phase, std::uint32_t frame)
{
  const Phase old = LoadPhase();
  if (old == phase)
    return;
  s_phase.store(static_cast<std::uint8_t>(phase), std::memory_order_relaxed);
  if (!Enabled())
    return;
  CloseWindow(Clock::now());
  Event(EventKind::Phase, frame, {}, fmt::format("{} -> {}", PhaseName(old), PhaseName(phase)));
}

Phase CurrentPhase()
{
  return LoadPhase();
}

std::string BundlePrefix()
{
  if (!Enabled())
    return {};
  std::string prefix = s_settings.local_path;
  if (prefix.size() > 6 && prefix.ends_with(".jsonl"))
    prefix.resize(prefix.size() - 6);
  return prefix;
}

void Shutdown(std::string_view reason, std::initializer_list<Field> fields)
{
  if (s_configured.load() && s_enabled.load())
  {
    std::string text(reason);
    if (const int first = s_first_problem.load(); first >= 0 && first < static_cast<int>(EventKind::Count))
      text += fmt::format("; first problem: {}", EVENT_NAMES[static_cast<size_t>(first)]);
    if (const int last = s_last_problem.load(); last >= 0 && last < static_cast<int>(EventKind::Count))
    {
      text += fmt::format("; last problem: {} at frame {}", EVENT_NAMES[static_cast<size_t>(last)],
                          s_last_problem_frame.load());
    }
    Event(EventKind::Shutdown, s_last_frame.load(), fields, text);
  }
  Shutdown();
}

void Desync(std::uint32_t frame, std::uint64_t local_checksum, std::uint64_t remote_checksum,
            int player)
{
  if (!Enabled())
    return;
  Count(Counter::Desyncs, 1, player);
  Event(EventKind::Desync, frame,
        {Hex("local", local_checksum), Hex("remote", remote_checksum), Int("player", player)});
}

void Shutdown()
{
  if (!s_configured.load())
    return;
  if (s_enabled.load())
  {
    CloseWindow(Clock::now());
    if (s_cpu.closed)
    {
      // Last window: wait for the lock here (the emulation is over).
      std::lock_guard lk(s_queue_mutex);
      if (s_queue.size() < QUEUE_MAX)
        s_queue.push_back(Record{std::move(s_cpu.closed), {}, true});
      s_cpu.closed.reset();
    }
    // Core::Stop may have already started this budget. Never extend it.
    Clock::rep unset = 0;
    s_stop_deadline.compare_exchange_strong(
        unset, (Clock::now() + FINAL_SEND_BUDGET).time_since_epoch().count());
    {
      std::lock_guard lk(s_queue_mutex);
      s_stop = true;
    }
    s_queue_cv.notify_all();
    // The sender's final attempt is bounded (one FINAL_SEND_BUDGET for all
    // pending batches together); then it spills to the local file and exits.
    if (s_sender.joinable())
      s_sender.join();
    s_stop_deadline = 0;  // the next session (Rearm) sends normally again
  }
  std::lock_guard lk(s_init_mutex);
  s_cpu = CpuState{};
  s_start = StartInfo{};
  s_have_start = false;
  s_phase = static_cast<std::uint8_t>(Phase::None);
  s_last_frame = 0;
  s_last_problem = -1;
  s_first_problem = -1;
  s_last_problem_frame = 0;
  s_enabled = false;
  s_shut_down = true;
  s_configured = true;  // stays off until Rearm()
}

void StopRequested()
{
  // No lazy Configure on a host thread during teardown. The CPU thread owns
  // the remaining records and Shutdown joins the sender after CPU exit.
  if (!s_enabled.load(std::memory_order_acquire))
    return;
  Clock::rep unset = 0;
  s_stop_deadline.compare_exchange_strong(
      unset, (Clock::now() + FINAL_SEND_BUDGET).time_since_epoch().count());
  Event(EventKind::StopRequested, s_last_frame.load(), {}, "Core::Stop requested");
}

void Rearm()
{
  if (!s_shut_down.load())
    return;
  std::lock_guard lk(s_init_mutex);
  s_shut_down = false;
  s_configured = false;  // the next call re-reads the configuration
}
}  // namespace NSRTelemetry
