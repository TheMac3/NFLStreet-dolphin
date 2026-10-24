// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Frame-keyed rollback input exchange and match-handoff agreement state,
// independent of Dolphin and ENet.  The emulator adapter supplies local
// samples and decoded datagrams; this component owns prediction,
// acknowledgement, retransmit history, confirmed-state checksums, and
// transition proposals.  Keeping both state machines free of sockets lets
// peers be exercised deterministically in one process.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace NSRNetplay::RollbackNet
{
constexpr std::uint32_t MAGIC = 0x4E535242;  // 'NSRB'
constexpr std::uint32_t HANDOFF_MAGIC = 0x4E535248;  // 'NSRH'
// VERSION 5: the checksum field carries the canonical state digest (see
// NSRNetplay's StateDigest) instead of the nine-word semantic hash, and every
// datagram carries the sender's ConfigDigest.  HANDOFF_VERSION 2: proposals
// carry the full ConfigFingerprint.  HANDOFF_VERSION 3 (rc1regress): proposals
// also carry the sender's VI retrace counter (sync_clock). HANDOFF_VERSION 4
// adds the second retrace-derived clock needed to normalize a completed
// match's teardown state before the next entry digest. HANDOFF_VERSION 5
// (fix/audio2): entry proposals carry player 0's rollback input delay
// (automatic or fixed) and the link RTT it was chosen from. Mixed builds
// therefore fail to parse each other instead of comparing values that mean
// different things.  VERSION 6 keeps frame-driven time, DVD completion and
// audio modeling active during post-match lockstep; an older peer would run
// that transition with incompatible lifecycle semantics. HANDOFF_VERSION 5
// includes the full runtime-content SHA-256, content version and hook policy.
constexpr std::uint8_t VERSION = 6;
// Version 6 agrees menu delay before the first poll and at match leave.
constexpr std::uint8_t HANDOFF_VERSION = 6;
constexpr std::size_t MAX_PLAYERS = 4;
constexpr std::size_t PAD_SIZE = 12;
constexpr std::size_t MAX_HISTORY = 64;
constexpr std::uint32_t NO_FRAME = UINT32_MAX;
// Extra frames of remote input/checksum lookahead tolerated beyond what this
// peer's own delay and prediction settings permit.  Delay and prediction are
// per-process settings (match delay <= 8, menu delay <= 12, prediction <= 60), so a
// legitimate peer may be configured more permissively than we are.  Two
// datagram windows cover that whole clamp range while still bounding how much
// attacker-chosen future input a single sender can make us retain.
constexpr std::uint32_t FUTURE_INPUT_SLACK = static_cast<std::uint32_t>(2 * MAX_HISTORY);

using Pad = std::array<std::uint8_t, PAD_SIZE>;
using Pads = std::array<Pad, MAX_PLAYERS>;

// ---------------------------------------------------------------------------
// Build/config agreement (final review M2).  Peers that differ in anything
// that changes the simulation or the meaning of the state digest must refuse
// to enter a rollback epoch, naming what differs, instead of desyncing late
// and opaquely.  The adapter fills one 64-bit value per field (a parsed value
// where one exists, otherwise a hash of the option's canonical form); this
// component only transports and compares them, so the agreement logic is
// testable without Dolphin.
//
// Per-player input delay and prediction are deliberately absent: inputs carry
// their target frame, so they only change local stalling/correction policy
// (FutureInputLimit's slack bounds the asymmetry).
//
// Adding a field changes the handoff wire size: bump HANDOFF_VERSION.
// ---------------------------------------------------------------------------
enum class ConfigField : std::uint8_t
{
  Protocol,        // NSRB/NSRH wire versions
  StateDigest,     // canonical-range version + digest mode
  VtimeBase,       // NSR_RB_VTIME_BASE (virtual time base, rounded to a frame)
  VmemBytes,       // NSR_RB_VMEM_BYTES (snapshotted fake-VMEM prefix)
  ResimSpeed,      // NSR_RB_RESIM_{,GPU_,ARAM_,AUDIO_}SPEED, NSR_RB_RESIM_DRAW
  InstantDisc,     // NSR_RB_INSTANT_DISC
  IoIdle,          // NSR_RB_IO_IDLE (forced on with NSR_RB_NET)
  AudioRollback,   // NSR_RB_AUDIO_ROLLBACK (forced on with NSR_RB_NET)
  GameImage,       // game ID + revision + hash of the loaded executable text
  LauncherImage,   // NSR_FINGERPRINT (the launcher's image fingerprint)
  Seed,            // NSR_SEED
  RestoreRanges,   // restored segments (RB_KEEP/AUDIO_KEEP + NSR_RB_KEEP/RANGES)
  AudioClock,      // NSR_AC and NSR_AC_* model parameters
  ResimGates,      // audio/effect gates, skip lists, resim vsync, async present, PEEKZ
  SessionRules,    // fixed epoch, NSR_RB_SESSION_END, auto re-entry, auto handoff
  DolphinCore,     // cheats, MMU, MEM1 override, FPRF, accurate NaNs, custom RTC
  ContentSha0,     // full runtime bundle SHA-256, first 64 bits
  ContentSha1,
  ContentSha2,
  ContentSha3,
  ContentVersion,  // release identity
  ContentPolicy,   // injection hook/rules version
  Count
};
constexpr std::size_t CONFIG_FIELDS = static_cast<std::size_t>(ConfigField::Count);

struct ConfigFingerprint
{
  std::array<std::uint64_t, CONFIG_FIELDS> fields{};

  std::uint64_t& operator[](ConfigField field)
  {
    return fields[static_cast<std::size_t>(field)];
  }
  std::uint64_t operator[](ConfigField field) const
  {
    return fields[static_cast<std::size_t>(field)];
  }
  bool operator==(const ConfigFingerprint&) const = default;
};

// Stable lower-case name of a field for logs and telemetry.
const char* ConfigFieldName(ConfigField field);
// Fields whose values differ, in enum order.
std::vector<ConfigField> ConfigDifferences(const ConfigFingerprint& a, const ConfigFingerprint& b);
// One 64-bit summary (FNV-1a over the fields) carried by every NSRB datagram,
// so a fixed-epoch session without a handoff barrier still fails closed.
std::uint64_t ConfigDigest(const ConfigFingerprint& config);

// The decoded form of an NSRB datagram.  Encode/Decode use explicit little
// endian fields so recordings and loopback tests do not inherit host ABI
// padding or alignment.
struct Packet
{
  std::uint8_t sender = 0;
  std::uint8_t players = 0;
  std::uint8_t flags = 0;
  std::uint32_t session = 0;
  // ConfigDigest of the sender's configuration (VERSION 5).
  std::uint64_t config = 0;
  std::uint32_t sender_frame = 0;
  std::uint32_t send_us = 0;
  // Each peer reflects the newest timestamp seen from every other peer and
  // how long it held that timestamp.  The originator can therefore measure
  // an end-to-end RTT without synchronized host clocks, including a star
  // relay hop.  Unsigned subtraction deliberately handles the 32-bit clock
  // wrapping about every 71 minutes.
  std::array<std::uint32_t, MAX_PLAYERS> echo_us{};
  std::array<std::uint32_t, MAX_PLAYERS> echo_age_us{};
  std::array<std::uint32_t, MAX_PLAYERS> ack{NO_FRAME, NO_FRAME, NO_FRAME, NO_FRAME};
  // Canonical state digest of the sender's newest finalized frame start.
  std::uint32_t checksum_frame = NO_FRAME;
  std::uint64_t checksum = 0;
  std::uint32_t first_frame = 0;
  std::vector<Pad> pads;
};

// Match transitions use a separate fixed-size message because the entry
// barrier exists before an NSRB rollback epoch does.  The absolute frame is
// assigned by the confirmed lockstep timeline; the state hash (the canonical
// state digest since HANDOFF_VERSION 2) proves that every peer reached the
// same deterministic game state at that boundary, and the config fingerprint
// that they will simulate the epoch the same way.
enum class HandoffKind : std::uint8_t
{
  EnterRollback = 1,
  LeaveRollback = 2,
  SyncClocks = 3,
  StartMenus = 4,
};

struct HandoffPacket
{
  std::uint8_t sender = 0;
  std::uint8_t players = 0;
  HandoffKind kind = HandoffKind::EnterRollback;
  std::uint32_t transition = 0;
  std::uint32_t frame = 0;
  std::uint64_t semantic_hash = 0;  // canonical state digest (name kept for history)
  ConfigFingerprint config{};
  // Player 0's proposal supplies the VI counter baseline adopted at entry.
  // This field is deliberately not part of agreement equality: peers reach
  // the same lockstep frame with host-scheduling-dependent counter values.
  std::uint32_t sync_clock = 0;
  std::uint32_t sync_aux_clock = 0;
  // Rollback input delay for the epoch this entry starts (fix/audio2). Only
  // player 0's value is used: every peer adopts it (HostInputDelay), so all
  // players run the same delay whether it was set by hand or chosen
  // automatically from the measured links (AutoInputDelay). 0 = no proposal
  // (leave/clock barriers). Like the clocks, not part of agreement equality.
  std::uint8_t input_delay = 0;
  // The worst link the delay was chosen from (diagnostics, logged by every
  // peer): one-way latency estimate and jitter, in tenths of a millisecond.
  std::uint16_t delay_one_way_dms = 0;
  std::uint16_t delay_jitter_dms = 0;
  // Immutable host policy for the menu stream created after this barrier.
  // Zero means this barrier creates no menu stream. Bounds are wire-validated.
  std::uint8_t menu_delay = 0;
  std::uint16_t menu_one_way_dms = 0, menu_jitter_dms = 0;
  std::uint8_t menu_source = 0;  // 0 fallback, 1 ENet median, 2 timing probes, 3 fixed diagnostic
};

std::vector<std::uint8_t> EncodeHandoff(const HandoffPacket& packet);
std::optional<HandoffPacket> DecodeHandoff(std::span<const std::uint8_t> bytes);

// Automatic rollback input delay (fix/audio2), chosen by player 0 before
// each rollback epoch from the links it measured during the lockstep menus.
// `one_way_ms`: worst player-to-player input path from p75 dedicated-probe
// RTT (half RTT direct; half the sum of two host legs in a star). `jitter_ms`:
// robust inter-arrival variation, summed for a relayed path. The original
// fix/audio2 rule/curve remains; the 8 ms bias accounts for next-frame host
// servicing at both ends. Revisit only if probe sweeps shift the tuning table
// in NFLStreet docs/research-log.md (2026-10-23).
//
//   delay = clamp(ceil((one_way - bias + jitter_weight x jitter) / frame)
//                 - predict_frames, min_delay, max_delay)
//
// One frame is left to cheap shallow prediction, keeping 1-2 frames of input
// delay up to ~70 ms direct RTT. Probes never enter the simulation state.
struct AutoDelayRule
{
  double one_way_bias_ms = 8.0;  // ENet scheduling overestimate, one way
  double jitter_weight = 1.0;    // margin = weight x measured RTT jitter
  double predict_frames = 1.0;   // latency rollback is allowed to absorb
  std::uint8_t min_delay = 1, max_delay = 4;
};
std::uint8_t AutoInputDelay(double one_way_ms, double jitter_ms, const AutoDelayRule& rule = {});

// Menus cannot predict: budget the full one-way path plus measured variation
// and one frame for service/relay scheduling. 2..12 frames bounds both local
// responsiveness and history use (legacy redundancy is 32 frames). Revisit
// the upper bound if supported links need more than about 180 ms one way.
std::uint8_t MenuInputDelay(double one_way_ms, double jitter_ms);
// Median over a bounded window rejects isolated ENet retransmission spikes.
// No estimate until six fresh ACKs exist; callers retain the previous delay.
std::optional<std::uint32_t> MenuMedian(std::span<const std::uint32_t> samples);

// Host-only timing probes never enter the input exchange or reliable queues.
// The origin's monotonic timestamp is echoed verbatim (no clock sync needed).
// Reports carry mesh links to player 0; Quality carries the host's worst path
// (target = chosen delay, rtt_us = one-way us). All fields are little endian.
constexpr std::uint32_t PROBE_MAGIC = 0x5052534e;  // "NSRP"
enum class ProbeKind : std::uint8_t { Ping = 1, Pong, Report, Quality };
struct ProbePacket
{
  ProbeKind kind = ProbeKind::Ping;
  std::uint8_t sender = 0, target = 0;
  std::uint32_t sequence = 0, sent_us = 0, rtt_us = 0, jitter_us = 0;
};
std::vector<std::uint8_t> EncodeProbe(const ProbePacket& packet);
std::optional<ProbePacket> DecodeProbe(std::span<const std::uint8_t> bytes);

struct LinkQuality
{
  double rtt_ms = 0, jitter_ms = 0;
  std::size_t samples = 0;
};
// Player 0 aggregates every physical link; mesh reports must be fresh before
// calling. A missing link yields no estimate, preserving the previous choice.
using LinkMatrix = std::array<std::array<std::optional<LinkQuality>, MAX_PLAYERS>, MAX_PLAYERS>;
struct InputPath { double one_way_ms = 0, jitter_ms = 0; };
std::optional<InputPath> WorstInputPath(const LinkMatrix& links, std::uint8_t players, bool star);

// A 12-second / 48-sample sliding window: p75 RTT resists occasional stalls;
// p75 absolute inter-arrival variation / sqrt(2) estimates RTT jitter without
// allowing one barrier/burst to poison an EWMA for the following match.
// The adapter invalidates outstanding probes whenever either end is busy.
class LinkProbeWindow
{
public:
  // Start at most four probes/second; all arithmetic tolerates u32 clock wrap.
  std::optional<ProbePacket> Start(std::uint32_t now_us);
  // Only an outstanding exact (sequence,timestamp) echo can add one sample.
  bool Accept(const ProbePacket& pong, std::uint32_t now_us);
  std::optional<LinkQuality> Quality(std::uint32_t now_us) const;
  void DiscardPending();
  // A leave starts a fresh window; sequence numbers intentionally never reset.
  void Reset();
private:
  struct Sample { std::uint32_t sent_us, received_us, rtt_us; };
  std::vector<ProbePacket> m_pending;
  std::vector<Sample> m_samples;
  std::uint32_t m_sequence = 0, m_last_send = 0;
  bool m_started = false;
};

// Great/Good/OK/Poor reflect the chosen delay plus measured RTT jitter.
const char* InputQualityLabel(std::uint8_t delay, double jitter_ms);

// HandoffAgreement is deliberately transport-free.  Each peer proposes the
// same kind/frame/hash tuple and accepts duplicate, reordered announcements;
// the first conflicting tuple is sticky so the adapter can abort loudly.
// A differing configuration is reported separately (ConfigMismatch) and takes
// precedence: a peer configured differently cannot be expected to reach the
// same state, and naming the option is the actionable message.
// Clocks are retained per sender rather than compared, allowing the adapter
// to adopt player 0's baseline (HostClock).
class HandoffAgreement
{
public:
  HandoffAgreement(std::uint8_t players, std::uint8_t me, HandoffKind kind,
                   std::uint32_t transition, std::uint32_t frame,
                   std::uint64_t semantic_hash, const ConfigFingerprint& config = {},
                   std::uint32_t sync_clock = 0, std::uint32_t sync_aux_clock = 0,
                   std::uint8_t input_delay = 0, std::uint16_t delay_one_way_dms = 0,
                   std::uint16_t delay_jitter_dms = 0, std::uint8_t menu_delay = 0,
                   std::uint16_t menu_one_way_dms = 0, std::uint16_t menu_jitter_dms = 0,
                   std::uint8_t menu_source = 0);

  bool Accept(const HandoffPacket& packet);
  bool Complete() const;
  // Diagnostics: bit p set once player p's matching proposal was accepted.
  std::uint8_t SeenMask() const;
  bool Mismatched() const { return m_mismatch.has_value() || m_config_mismatch.has_value(); }
  const HandoffPacket& Local() const { return m_local; }
  const std::optional<HandoffPacket>& Mismatch() const { return m_mismatch; }
  const std::optional<HandoffPacket>& ConfigMismatch() const { return m_config_mismatch; }
  std::optional<std::uint32_t> HostClock() const;
  std::optional<std::uint32_t> HostAuxClock() const;
  // Reconstruct only a validated participant's immutable proposal. A relay
  // may retain a completed agreement to repair delivery to a lagging leaf
  // while the relay itself waits in the following barrier.
  std::optional<HandoffPacket> Proposal(std::uint8_t player) const;
  // Player 0's proposal (input delay, link estimate) once seen.
  std::optional<HandoffPacket> HostProposal() const;

private:
  std::uint8_t m_players;
  std::uint8_t m_me;
  HandoffPacket m_local;
  std::array<bool, MAX_PLAYERS> m_seen{};
  std::array<std::uint32_t, MAX_PLAYERS> m_sync_clocks{};
  std::array<std::uint32_t, MAX_PLAYERS> m_sync_aux_clocks{};
  std::array<HandoffPacket, MAX_PLAYERS> m_proposals{};
  std::optional<HandoffPacket> m_mismatch;
  std::optional<HandoffPacket> m_config_mismatch;
};

std::vector<std::uint8_t> Encode(const Packet& packet);
std::optional<Packet> Decode(std::span<const std::uint8_t> bytes);

// A well-formed datagram for this epoch whose contents no honest peer can
// produce.  These are distinct from stale or foreign packets (wrong session,
// player count, or self-addressed), which unsequenced delivery makes normal
// and which Accept therefore rejects without recording a violation.
enum class ViolationKind : std::uint8_t
{
  // The sender acknowledged local input we have not produced yet (or a frame
  // before the epoch start).  Honoring it would stop retransmission.
  ImpossibleAck = 1,
  // The sender restated an already-confirmed frame with different input.
  // Local samples are immutable once submitted, so this is a cheat or a
  // corrupted peer, and silently keeping either value would hide a desync.
  ConflictingInput = 2,
  // Input lies beyond FutureInputLimit(); see FUTURE_INPUT_SLACK.
  InputOutOfWindow = 3,
  // A finalized checksum frame lies beyond FutureInputLimit().
  ChecksumOutOfWindow = 4,
};

struct ProtocolViolation
{
  std::uint8_t player = 0;
  ViolationKind kind = ViolationKind::ImpossibleAck;
  std::uint32_t frame = NO_FRAME;
};

// An honest but differently configured peer (M2): not a protocol violation,
// but nothing it sends can be used.
struct ConfigMismatchInfo
{
  std::uint8_t player = 0;
  std::uint64_t remote = 0;
};

// The first confirmed state mismatch, with both digests for the log.
struct DesyncInfo
{
  std::uint8_t player = 0;
  std::uint32_t frame = NO_FRAME;
  std::uint64_t local = 0;
  std::uint64_t remote = 0;
};

// Exchange is CPU-thread-owned.  A transport may deliver packets in any
// order and may duplicate them; insertion and acknowledgements are
// idempotent.  Assemble records exactly what a frame ran with so a later
// confirmed input can identify the earliest wrong prediction.
//
// Accept validates a whole datagram before applying any of it: a packet that
// carries an impossible acknowledgement, a conflicting restatement of a
// confirmed input, or input/checksums outside the plausible window returns
// false, leaves acknowledgement, input, timing, and checksum state untouched,
// and is recorded in ViolationCount()/FirstViolation() for the adapter.
class Exchange
{
public:
  Exchange(std::uint32_t start, std::uint8_t players, std::uint8_t me,
           std::uint32_t delay, std::uint32_t max_prediction, std::uint32_t session,
           std::uint64_t config = 0);

  void SubmitLocal(std::uint32_t live_frame, Pad pad);
  Packet BuildPacket(std::uint32_t live_frame, std::uint32_t send_us = 0) const;
  bool Accept(const Packet& packet, std::uint32_t receive_us = 0);
  Pads Assemble(std::uint32_t frame);

  bool ShouldStall(std::uint32_t frame) const;
  std::uint32_t ConfirmedThrough() const;
  std::optional<std::uint32_t> PendingRollback() const;
  // Diagnostics: bit p set when player p's confirmed input for `frame`
  // differs from the input that frame last ran with (0 if unknown).
  std::uint8_t MispredictedPlayers(std::uint32_t frame) const;
  void BeginRollback();

  // Record the digest of the state at the start of `frame`.  Re-simulation
  // calls it again for the same frame; the newest value wins.
  void SetStateChecksum(std::uint32_t frame, std::uint64_t checksum);
  // The newest local state checksum at or before `frame` (frame, value).  The
  // leave barrier compares this in-match digest instead of re-hashing RAM
  // after the match teardown, which frees audio/heap memory per machine.
  std::optional<std::pair<std::uint32_t, std::uint64_t>>
  LocalStateChecksumAtOrBefore(std::uint32_t frame) const;
  // A rollback restored the state at the start of `frame`: every recorded
  // digest of a later frame start describes the abandoned timeline.  Without
  // this, finalization during re-simulation could advertise (and compare) a
  // stale mispredicted digest before re-simulation recomputes it.
  void DiscardChecksumsAfter(std::uint32_t frame);
  void UpdateFinalized();
  void TrimBefore(std::uint32_t frame);
  // Digests are compared only once the local digest is final too (frame <=
  // our finalized frame): before that it may describe a mispredicted state,
  // and the first mismatch is sticky.
  std::optional<std::uint32_t> FirstDesync() const;
  const std::optional<DesyncInfo>& FirstDesyncInfo() const { return m_first_desync_info; }
  // Every mismatching final frame in compare order (diagnostics; capped).
  const std::vector<DesyncInfo>& Desyncs() const { return m_desyncs; }
  std::uint32_t Finalized() const { return m_finalized; }
  std::uint64_t Config() const { return m_config; }
  const std::optional<ConfigMismatchInfo>& ConfigMismatch() const { return m_config_mismatch; }
  std::optional<std::uint32_t> OldestUnackedLocal() const;
  // Highest remote input or checksum frame Accept will store.  A peer's
  // ShouldStall keeps its live frame within max_prediction of its contiguous
  // view of our input (plus one, because the adapter submits before it
  // checks the stall), that view can never pass our own contiguous local
  // prefix, and its samples land delay frames later.  Our own delay and
  // prediction plus FUTURE_INPUT_SLACK therefore bound every honest sender,
  // including senders whose datagrams the star host relays unchanged.
  std::uint32_t FutureInputLimit() const;
  std::uint32_t ViolationCount() const { return m_violations; }
  const std::optional<ProtocolViolation>& FirstViolation() const { return m_first_violation; }
  // Retained confirmed input for one player, for diagnostics and tests.
  std::size_t HeldInputs(std::uint8_t player) const;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> HeldInputRange(std::uint8_t player) const;
  // Positive frame advantage means this peer is genuinely ahead after
  // projecting each reported remote frame forward by half of the measured
  // RTT.  TimeSyncWaitUs applies a wide deadband and a 30-frame cadence so
  // only the faster peer occasionally stretches a frame.
  double FrameAdvantage() const;
  double FrameAdvantage(std::uint32_t) const { return FrameAdvantage(); }
  std::uint32_t TimeSyncWaitUs(std::uint32_t local_frame);
  std::optional<std::uint32_t> SmoothedRttUs(std::uint8_t player) const;

  std::uint32_t Start() const { return m_start; }
  std::uint32_t Session() const { return m_session; }
  std::uint32_t Delay() const { return m_delay; }
  std::uint32_t MaxPrediction() const { return m_max_prediction; }
  const std::array<std::uint32_t, MAX_PLAYERS>& Contiguous() const { return m_contiguous; }

private:
  static Pad Canonical(Pad pad);
  Pad Predict(std::uint8_t player, std::uint32_t frame) const;
  void AdvanceContiguous(std::uint8_t player);
  void CompareChecksum(std::uint8_t player, std::uint32_t frame);
  void ObserveTiming(const Packet& packet, std::uint32_t receive_us);
  std::optional<ProtocolViolation> Validate(const Packet& packet) const;

  struct PeerTiming
  {
    bool have_remote_timestamp = false;
    bool have_rtt = false;
    std::uint32_t remote_send_us = 0;
    std::uint32_t remote_received_us = 0;
    std::uint32_t rtt_us = 0;
    std::uint32_t sampled_frame = NO_FRAME;
    std::array<double, 30> advantage{};
    std::size_t advantage_count = 0;
    std::size_t advantage_next = 0;
  };

  std::uint32_t m_start;
  std::uint8_t m_players;
  std::uint8_t m_me;
  std::uint32_t m_delay;
  std::uint32_t m_max_prediction;
  std::uint32_t m_session;
  std::uint64_t m_config;
  std::array<std::map<std::uint32_t, Pad>, MAX_PLAYERS> m_confirmed;
  std::array<std::uint32_t, MAX_PLAYERS> m_contiguous{NO_FRAME, NO_FRAME, NO_FRAME,
                                                      NO_FRAME};
  std::array<std::uint32_t, MAX_PLAYERS> m_peer_ack{NO_FRAME, NO_FRAME, NO_FRAME,
                                                    NO_FRAME};
  std::array<PeerTiming, MAX_PLAYERS> m_timing;
  std::uint32_t m_local_frame = NO_FRAME;
  std::uint32_t m_last_sync_frame = NO_FRAME;
  std::map<std::uint32_t, Pads> m_ran;
  std::map<std::uint32_t, std::uint8_t> m_predicted;
  std::optional<std::uint32_t> m_pending;
  std::map<std::uint32_t, std::uint64_t> m_local_checksums;
  std::array<std::map<std::uint32_t, std::uint64_t>, MAX_PLAYERS> m_remote_checksums;
  std::uint32_t m_finalized = NO_FRAME;
  std::optional<std::uint32_t> m_first_desync;
  std::optional<DesyncInfo> m_first_desync_info;
  std::vector<DesyncInfo> m_desyncs;
  std::optional<ConfigMismatchInfo> m_config_mismatch;
  std::uint32_t m_violations = 0;
  std::optional<ProtocolViolation> m_first_violation;
};
}  // namespace NSRNetplay::RollbackNet
