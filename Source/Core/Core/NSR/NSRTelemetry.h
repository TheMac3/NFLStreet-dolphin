// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// NSR passive performance telemetry (NFLStreet docs/telemetry.md).
//
// Purpose: when friends play online and something feels wrong (rollbacks,
// stalls, desyncs), the server already holds the numbers needed to debug it.
// Nothing here may change what the game sees or slow the emulated machine:
//
//   - Metrics (Count / SetGauge / Observe) are CPU-thread-only, allocation-free
//     in the steady state and O(1): an array add, a min/max, a histogram
//     bucket increment.  No locks, no I/O, no formatting.
//   - Every NSR_TELEMETRY_INTERVAL seconds (default 5) the CPU thread moves
//     the current window (counters, gauges, histograms) into a bounded queue
//     and starts a fresh one.  Events go to the same queue immediately.
//   - A background sender thread turns queued records into JSON, batches and
//     gzips them, and POSTs them to the ingest service with Common::HttpRequest.
//     Failures retry with exponential backoff; when the retry buffer or the
//     queue is full, records are dropped and counted (`dropped` in the next
//     window), and everything that could not be delivered is appended to a
//     local JSONL file under the Dolphin user folder (NSR/Telemetry/).
//
// Coverage (2026-10-18): the whole netplay session, not only rollback matches.
// The glue calls Start() when netplay is set up (before the peers connect),
// so the header, connection problems, lockstep-menu windows and desyncs, the
// handoff barriers and the shutdown reason all reach the server even when a
// run never gets to a match.  Each window and event carries its Phase.
//
// Metrics: Count / SetGauge / Observe.
// Configuration (environment wins over Dolphin config [NSR] in Dolphin.ini):
//   NSR_TELEMETRY=0             disabled, whatever else is set
//   NSR_TELEMETRY_URL=<url>     ingest endpoint, e.g. https://nsr.example/v1/telemetry
//                               (config: NSR.TelemetryURL)
//   NSR_TELEMETRY_TOKEN=<tok>   bearer token: per-session token from the
//                               rendezvous, or the private POC shared key
//                               (config: NSR.TelemetryToken)
//   NSR_TELEMETRY_LOCAL=1       also write every record to the local JSONL file
//   NSR_TELEMETRY_FILE=<path>   local JSONL path override
//   NSR_TELEMETRY_INTERVAL=<s>  window length in seconds (1..60, default 5)
//   NSR_TELEMETRY_SESSION=<id>  shared match id (default NSR_ROOM, else derived from NSR_SEED)
//   NSR_ROOM_CODE, NSR_PLAYER_NAME, NSR_PLAYER, NSR_FINGERPRINT: session header
// Telemetry is on when a URL or NSR_TELEMETRY_LOCAL is set (or NSR_TELEMETRY=1,
// which writes locally), so ordinary determinism runs are unaffected.
//
// Privacy: the header carries the room code, session id, player slot and the
// display name the player typed, plus build/OS/CPU facts.  No IP addresses,
// keys, tokens, file paths or user names are recorded.
//
// Stage-2 netcode integration (names chosen to match NSRRollbackNetplay):
//   per frame, for each remote player p (peer = p):
//     SetGauge(Gauge::NetRttMs, peer->roundTripTime, p)
//     SetGauge(Gauge::NetRttVarMs, peer->roundTripTimeVariance, p)            // jitter
//     SetGauge(Gauge::NetLossPct, 100.0 * peer->packetLoss / ENET_PEER_PACKET_LOSS_SCALE, p)
//     SetGauge(Gauge::FrameAdvantage, exchange.FrameAdvantage(live), p)
//     Observe(Hist::PredictionFrames, live - exchange.ConfirmedThrough())
//   on receiving a confirmed remote input for frame f at live frame L:
//     Observe(Hist::RemoteInputLateFrames, L > f ? L - f : 0, sender)
//   SendRollbackLatest: Count(Counter::PacketsSent); HandlePacket: Count(Counter::PacketsReceived, 1, sender);
//     Decode failure / session mismatch: Count(Counter::PacketsRejected, 1, sender)
//   ++r.net_stalls: Count(Counter::NetStalls), Observe(Hist::NetStallUs, waited_us)
//   ++r.sync_waits: Count(Counter::SyncWaits), Observe(Hist::TimeSyncWaitUs, waited_us)
//   DESYNC log: Desync(frame, local_checksum, remote_checksum, player)
//   peer reports a desync (flag 0x02): Event(EventKind::PeerDesync, frame, {Int("player", p)})
//   input timeout / transport failure: Event(EventKind::InputTimeout / TransportFailure, frame, ...)
//   UNCORRECTABLE network input: Event(EventKind::Uncorrectable, pending, {Int("live", live), ...})
//     (the stage-1 glue also derives this event from the uncorrectable counter)

#pragma once

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NSRTelemetry
{
// ---------------------------------------------------------------------------
// Metric identifiers.  Enums (not strings) keep the CPU-thread path to an
// array index; NSRTelemetry.cpp maps each to its stable JSON name, which the
// ingest service and docs/telemetry.md use.  Append new entries before Count
// (the wire format uses names, so order is free to change).
enum class Counter : std::uint8_t
{
  LiveFrames,          // live frames in the window
  Rollbacks,           // restores (any source: forced, misprediction, network)
  ResimFrames,         // re-simulated frames
  Mispredictions,      // predictions found wrong
  Uncorrectable,       // mispredictions older than the ring/session/barrier
  QuietSkips,          // rollbacks deferred: disc busy / no quiet snapshot
  FramesOverBudget,    // live frame interval above the frame budget (16.7 ms + 5%)
  HostStalls,          // live frame interval above 50 ms (a visible hitch)
  DirtyPagesCopied,    // snapshot pages copied by the dirty tracker
  DirtyPagesWritten,   // guest pages seen written (tracker samples)
  AudioClockCatchups,  // extra audio-model ticks (in-frame retrace waits + busy polls)
  // Stage-2 network metrics (declared now, fed by the netcode later).
  NetStalls,           // host stalls at the prediction window
  SyncWaits,           // time-sync yields (peer ahead)
  PacketsSent,
  PacketsReceived,
  PacketsRejected,     // malformed / wrong session / wrong player
  Desyncs,             // confirmed checksum mismatches (first one is an event)
  Count
};

enum class Gauge : std::uint8_t
{
  NetRttMs,            // ENet round-trip time per peer
  NetRttVarMs,         // ENet round-trip variance per peer (jitter)
  NetLossPct,          // ENet packet loss per peer, percent
  FrameAdvantage,      // our frame minus the peer's reported frame
  InputDelay,          // configured local input delay (frames)
  MaxPrediction,       // configured prediction window (frames)
  RingSlots,           // snapshot ring size
  // fix/audio2: the mixer's game-audio fifo since start (cumulative; a
  // window's growth = last - min): granules replayed because emulation fell
  // behind real time (Fill Audio Gaps; heard as stutter/"crunch") and queue
  // overflows where audio was dropped.
  AudioGapFills,
  AudioSkips,
  Count
};

enum class Hist : std::uint8_t
{
  RollbackDepth,           // frames re-simulated per rollback
  FrameIntervalUs,         // host time between live frame ends (what the player sees)
  StepUs,                  // rollback step before the live unit: restore + re-sims + saves
  LiveUnitUs,              // the live frame's own unit (includes the retrace wait when throttled)
  SaveUs,                  // per live frame: snapshot saves (NSR_RB_COST phase PH_SAVE)
  RestoreUs,               // ... page compare + copy on restore (PH_RESTORE)
  LiveRangesUs,            // ... OS-list walk + keep/put back (PH_LIVE)
  JitUs,                   // ... JIT/icache invalidation (PH_JIT)
  CheckUs,                 // ... forced-mode self-check (PH_CHECK)
  // Stage-2 network metrics.
  NetRttUs,                // per-sample RTT where the netcode measures it itself
  RemoteInputLateFrames,   // live frame minus input frame when a remote input arrives
  PredictionFrames,        // live frame minus confirmed-through frame
  TimeSyncWaitUs,          // host wait per time-sync yield
  NetStallUs,              // host wait per prediction-window stall
  LockstepWaitUs,          // lockstep menus: host wait per frame for the peers' pads
  Count
};

enum class EventKind : std::uint8_t
{
  SessionEnter,
  SessionLeave,
  Desync,
  PeerDesync,
  Hang,
  Uncorrectable,
  InputTimeout,
  TransportFailure,
  Note,
  // Whole-session coverage (2026-10-18; docs/telemetry.md "Session phases").
  Phase,           // the netplay phase changed (fields: from, to as names in text)
  Connection,      // connection progress: connected, retry (text names it)
  HandoffAbort,    // a lockstep<->rollback transition barrier failed (text: why)
  ConfigMismatch,  // a peer's config fingerprint differs (text names the fields)
  Shutdown,        // the emulation stopped (text: reason, last problem)
  DesyncSummary,   // digests of the local state at a desync frame (no RAM bytes)
  StopRequested,   // Core::Stop on the host thread, before CPU/HW teardown
  Count
};

// Where the netplay session is.  Every window and event carries it, so a
// desync in the lockstep menus is told apart from one in a match.  Frame
// numbers are per phase: lockstep-menu uses the retrace counter the
// milestone-1 exchange agrees on, rollback-match and post-match the rollback
// unit counter.  Offline/test runs stay `none` until a rollback session.
enum class Phase : std::uint8_t
{
  None,
  LockstepMenu,
  RollbackMatch,
  PostMatch,
  Count
};

// One event field: a static key with an integer, hex (u64 hashes; JSON
// numbers lose precision past 2^53, so these go out as strings) or real value.
struct Field
{
  enum class Type : std::uint8_t
  {
    Int,
    Hex,
    Real
  };
  const char* key = "";  // must be a string literal / static storage
  Type type = Type::Int;
  std::int64_t i = 0;
  std::uint64_t u = 0;
  double d = 0;
};
inline Field Int(const char* key, std::int64_t v)
{
  Field f;
  f.key = key;
  f.type = Field::Type::Int;
  f.i = v;
  return f;
}
inline Field Hex(const char* key, std::uint64_t v)
{
  Field f;
  f.key = key;
  f.type = Field::Type::Hex;
  f.u = v;
  return f;
}
inline Field Real(const char* key, double v)
{
  Field f;
  f.key = key;
  f.type = Field::Type::Real;
  f.d = v;
  return f;
}

// peer: -1 = not per-player; 0..3 = that player index.
constexpr int NO_PEER = -1;
constexpr int MAX_PEERS = 4;

// Rollback configuration for the session header and session-enter event.
struct RollbackInfo
{
  std::uint32_t enter_frame = 0;
  std::uint32_t generation = 0;
  std::uint32_t force = 0;
  std::uint32_t mispredict = 0;
  std::uint32_t ring_slots = 0;
  bool io_idle = false;
  bool audio_rollback = false;
  bool dirty_tracking = false;
  std::uint32_t dirty_mode = 0;  // NSRNetplay DirtyTracker::Mode wire value
  bool frame_mode = false;
  std::uint32_t vmem_bytes = 0;
  std::uint64_t config_digest = 0;  // the epoch's agreed config digest (0: offline)
};

// Netplay start (header) facts the glue knows and this module does not:
// the rollback config fingerprint (so mixed builds/configs are visible from
// the headers alone, before any handoff could refuse them) and the topology.
struct StartInfo
{
  Phase phase = Phase::LockstepMenu;  // the phase the header reports
  std::uint64_t config_digest = 0;
  // Boot settings before guest boot; rollback initialization after RbInit.
  std::string config_scope = "rollback";
  std::vector<std::pair<std::string, std::uint64_t>> config_fields;
  std::uint32_t players = 0;
  std::uint32_t delay = 0;
  bool star = true;
  bool punch = false;           // via the rendezvous (hole punching)
  bool rollback_image = false;  // the rollback hooks are active (RbInit ran)
};

// A desync's local evidence, uploaded so two players' summaries can be
// diffed server-side without anyone sending files.  Only hashes: `units`
// are 32-bit folds of the state digest per `unit_bytes` of state (MEM1
// regions in lockstep; groups of state pages in rollback), never RAM bytes.
struct DesyncSummaryInfo
{
  std::uint32_t frame = 0;
  std::uint32_t generation = 0;
  std::uint32_t evidence_frame = UINT32_MAX;  // Defaults to frame; leave may prove an older slot.
  int player = NO_PEER;              // the peer whose checksum differed
  std::uint64_t local = 0, remote = 0;
  const char* source = "";           // "lockstep_mem1", "rollback_state", "handoff_state"
  std::uint32_t unit_bytes = 0;      // state bytes per unit digest
  std::vector<std::uint32_t> units;  // per-unit digests, in address order
  std::uint64_t semantic = 0;        // the semantic-words checksum, when known
  bool semantic_valid = false;
  std::uint64_t aram = 0;            // already-computed gameplay-ring digest
  bool aram_valid = false;           // false for lockstep or an expired ring slot
  std::uint32_t excluded_ranges = 0; // digest exclusions in effect (rollback)
  std::uint64_t excluded_hash = 0;   // ... and a hash of their list
  std::string bundle;                // local bundle file name (no directory), if written
};

// --- lifecycle (CPU thread) --------------------------------------------------
// Reads configuration once per process (or after Shutdown); starts the sender
// thread on first use when enabled.  Cheap and idempotent.
bool Enabled();
// Full stop: flush the open window, give the sender a short grace period to
// deliver, spill the rest to the local file.  Called from NSRNetplay::Shutdown.
void Shutdown();
// Host-thread Core::Stop notification: start the shared network deadline and
// queue an urgent reason without touching CPU-thread windows or joining it.
void StopRequested();
// After Shutdown, telemetry stays off (late callers during teardown are
// ignored) until Rearm(): the rollback glue calls it when a session starts,
// i.e. on the next boot in a long-lived (GUI) process.
void Rearm();
// Netplay connected/connecting (lockstep menus): rearm, record the start facts
// for the header and start the sender.  Call before any other telemetry call
// of the run so the header carries them; a later call re-sends the header.
void Start(const StartInfo& info);
// Phase changes close the open window (windows never straddle phases) and
// raise a `phase` event.  CPU thread.
void SetPhase(Phase phase, std::uint32_t frame);
Phase CurrentPhase();
// Prefix for this run's local desync bundle files, next to the local JSONL
// (`<user>/NSR/Telemetry/<session>-p<k>-<run>`); empty when disabled.
std::string BundlePrefix();
// Shutdown with a reason: one `shutdown` event (reason, the last problem
// event seen, `fields`) and then Shutdown().
void Shutdown(std::string_view reason, std::initializer_list<Field> fields);

void SessionEnter(const RollbackInfo& info);
// `totals`: session totals for the leave event (rollbacks, re-sims, ...).
void SessionLeave(std::uint32_t frame, std::string_view reason,
                  std::initializer_list<Field> totals = {});

// --- metrics (CPU thread only; no-ops when disabled) -------------------------
void Count(Counter c, std::uint64_t n = 1, int peer = NO_PEER);
void SetGauge(Gauge g, double value, int peer = NO_PEER);
void Observe(Hist h, std::uint64_t value, int peer = NO_PEER);

// Per live frame (CPU thread): advances the window clock and closes the
// window when its interval elapsed.  `frame` is the live frame just finished.
void LiveFrame(std::uint32_t frame, std::chrono::steady_clock::time_point now);
// Connection/input waits still close periodic windows, without inventing
// live frames. Called by the thread currently owning the ENet session.
void Poll();

// --- events (any thread) -----------------------------------------------------
void Event(EventKind kind, std::uint32_t frame, std::initializer_list<Field> fields = {},
           std::string_view text = {});
// Convenience for the stage-2 checksum oracle: counts and reports the mismatch.
void Desync(std::uint32_t frame, std::uint64_t local_checksum, std::uint64_t remote_checksum,
            int player);
// The `desync_summary` event (see DesyncSummaryInfo).  Any thread.
void DesyncSummary(const DesyncSummaryInfo& info);

// --- test hooks ----------------------------------------------------------------
// Percentile from the module's histogram bucketing (exposed for the unit test).
std::uint32_t BucketIndex(std::uint64_t value);
std::uint64_t BucketLow(std::uint32_t index);
}  // namespace NSRTelemetry
