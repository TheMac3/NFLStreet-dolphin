// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// See NSRNetplay.h for the design. This file holds:
//   - session setup over ENet for 2-4 players in a full mesh (each player
//     binds a port, connects to every lower-numbered player, and accepts the
//     higher-numbered ones),
//   - the per-retrace exchange (send our pad for frame f + delay to everyone
//     with redundancy; block until every other player's pad for frame f is in),
//   - periodic whole-MEM1 hashes, compared with every peer, to detect desyncs.

#include "Core/NSR/NSRNetplay.h"
#include "Core/NSR/NSRRollbackLogic.h"
#include "Core/NSR/NSRRollbackNetplay.h"
#include "Core/NSR/ReplayFollow.h"

#include <algorithm>
#if defined(__linux__)
#include <ucontext.h>
#include <dlfcn.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <ctime>
#include <csignal>
#include <cerrno>
// Snapshot dirty-page tracking needs Linux 6.7+ (userfaultfd async
// write-protect + PAGEMAP_SCAN); elsewhere snapshots use full copies.
#if defined(__linux__)
#define NSR_HAVE_UFFD 1
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/userfaultfd.h>
#include <linux/fs.h>
#include <unistd.h>
#else
#define NSR_HAVE_UFFD 0
#endif
#include <fcntl.h>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#include <enet/enet.h>
#include <fmt/format.h>
// Portable snapshot dirty tracking hashes pages with XXH3-128 (vendored in
// Externals/xxhash). Inlined: the page loop wants the fixed-size fast path, and
// the static copies can't clash with the xxhash library or zstd's private copy.
#define XXH_INLINE_ALL
#include <xxhash.h>
#if !defined(XXH3_SECRET_DEFAULT_SIZE)
#error "NSRNetplay needs Externals/xxhash (0.8+, XXH3); an older xxhash.h is first on the include path"
#endif

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/MemTools.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCCache.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/NSR/NSRTelemetry.h"  // NSR telemetry
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"

namespace NSRNetplay
{
// Defined with the state digest; refuses a differently configured peer (M2).
static void FailConfigMismatch(u32 player, const RollbackNet::ConfigFingerprint* local,
                               const RollbackNet::ConfigFingerprint* remote, u64 local_digest,
                               u64 remote_digest);

namespace
{
constexpr u32 PAD_SIZE = 12;          // PADStatus
constexpr u32 MAX_PLAYERS = 4;        // the game's controller-port limit
constexpr u32 REDUNDANCY = 32;        // past pads repeated in every packet
constexpr u32 CHECKSUM_INTERVAL = 60; // frames between MEM1 hashes
constexpr u32 PACKET_MAGIC = 0x4E535251;  // 'NSRQ' (v2: carries sender index)
constexpr int CONNECT_TIMEOUT_MS = 60000;
constexpr int PEER_TIMEOUT_MS = 10000;
constexpr u16 DEFAULT_PORT = 51515;

// RAM that legitimately differs between machines and isn't game state, so
// it's left out of hashes:
//   - SDK memory-card control block + work areas (CARDMount at fn_801D45DC
//     uses 0x80384DE0 + chan * 0xA000): per-mount card values.
//   - SDK raw SI controller input (before PADRead): each machine's own pads.
//   - NFLStreet patch area: test scripts, freeze/loop words.
constexpr std::array<std::pair<u32, u32>, 3> HASH_SKIP{{
    {0x00384C00, 0x00398E00},
    {0x003A69C0, 0x003A6A00},
    {0x00411A00, 0x00412000},
}};

// Test-script table written by NFLStreet's tools/detrun.py (same layout the
// isopatch `input` routine reads): 8-byte entries (u32 until_retrace,
// u16 buttons, s8 stick x, s8 stick y), terminated by until = 0; loop words
// (start, length) at SCRIPT_LOOP. Evaluated here, at zero emulated cost, so
// a test script doesn't change how many instructions the game runs.
constexpr u32 SCRIPT_TABLE = 0x80411A40;
constexpr u32 SCRIPT_LOOP = 0x80411F08;
constexpr u32 SCRIPT_MAX = 152;

using Pad = std::array<u8, PAD_SIZE>;

struct Packet
{
  u32 magic;
  u32 sender;             // player index of the sender
  u32 latest_frame;       // frame of pads[count - 1]
  u32 count;              // pads for frames latest-count+1 .. latest
  u32 checksum_frame;     // 0 = none
  u32 pad_;               // keep checksum 8-aligned
  u64 checksum;
  Pad pads[REDUNDANCY];
};

struct Session
{
  bool initialized = false;
  bool enabled = false;
  // Star (default): everyone connects only to player 0, who relays every
  // packet to the others, so only player 0 needs a reachable port. Mesh:
  // everyone connects to every lower-numbered player (lower latency between
  // non-host players, but players 0..n-2 all need reachable ports).
  bool star = true;
  bool exchanged_once = false;  // first frame may wait the full connect window

  // Hole punching via the rendezvous server (NSR_RENDEZVOUS set): every
  // player binds an ephemeral port, registers it with the server from that
  // same socket, learns the peers' public + LAN addresses, and both sides
  // send punch packets so their NATs open before ENet connects.
  struct Punch
  {
    bool enabled = false;
    ENetAddress server{};
    std::string room, key, lan_ip;
    bool have_peers = false;
    std::array<ENetAddress, MAX_PLAYERS> target{}, alt{};
    std::array<bool, MAX_PLAYERS> needed{};
    u32 connect_attempts = 0;
  } punch;
  u32 me = 0;          // our player index = our game port (0-based)
  u32 players = 2;
  u32 delay = 2;
  FILE* log = stderr;
  ENetHost* host = nullptr;
  std::array<ENetPeer*, MAX_PLAYERS> peers{};  // by player index (ours unused)
  bool failed = false;

  std::map<u32, Pad> local_pads;                          // by frame they apply to
  std::array<std::map<u32, Pad>, MAX_PLAYERS> remote_pads;  // by player, frame
  std::map<u32, std::array<Pad, MAX_PLAYERS>> applied;    // frame -> pads by player
  std::map<u32, u64> local_hashes;
  std::array<std::map<u32, u64>, MAX_PLAYERS> remote_hashes;
  u32 last_sent_hash_frame = 0;
  bool started = false;
  u32 first_frame = 0;   // frame of the first hook call (same on every machine)
  u32 next_target = 0;   // next frame index we haven't recorded a pad for
  u32 desyncs = 0;
  u32 last_desync_frame = 0;  // log each mismatched frame once
  // Diagnostics: MEM1 copies for recent checksum frames; on the first
  // mismatch, the copy for that frame is written to NSR_DUMP_PREFIX-<frame>.bin.
  std::map<u32, std::vector<u8>> snapshots;
  bool dumped = false;
};

Session s_session;
// The socket-free exchange is created at the agreed rollback epoch.  ENet
// remains a thin datagram adapter and the CPU thread is its sole owner.
std::unique_ptr<RollbackNet::Exchange> s_rb_exchange;
std::optional<RollbackNet::HandoffAgreement> s_rb_handoff;
std::optional<u32> s_rb_logged_desync;
bool s_rb_bad_session_logged = false;
bool s_rb_peer_desync_logged = false;
size_t s_rb_desyncs_dumped = 0;  // Exchange::Desyncs() entries already dumped
// One log line per epoch for the first rejected (protocol-violating) packet.
bool s_rb_violation_logged = false;

template <typename... Args>
void Log(fmt::format_string<Args...> format, Args&&... args)
{
  const std::string line = fmt::format(format, std::forward<Args>(args)...);
  std::fprintf(s_session.log, "[NSR] %s\n", line.c_str());
  std::fflush(s_session.log);
}

std::string Env(const char* name, const char* fallback = "")
{
  const char* v = std::getenv(name);
  return v ? v : fallback;
}

// Return a compact monotonic timestamp for the rollback wire protocol.  Only
// timestamp differences are used, so peers need neither a common epoch nor
// synchronized host clocks.
u32 HostNowUs()
{
  return static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

// Host-time phase accounting for NSR_RB_COST (cumulative nanoseconds; the cost
// log prints per-unit deltas). GPU and DSP time are summed by Fifo.cpp and
// SystemTimers.cpp into g_nsr_host_ns[] while g_nsr_host_timing is set.
enum HostPhase : int
{
  PH_SAVE,         // RbSave: snapshot copies
  PH_RESTORE,      // RbRestore: page compare + copy of changed pages
  PH_LIVE,         // RbLiveRanges walk + keep/put-back copies (restore and check)
  PH_JIT,          // JIT page invalidation + icache reset after a restore
  PH_CHECK,        // RbCheck: re-simulated state vs the saved original
  PH_COUNT
};
u64 s_phase_ns[PH_COUNT] = {};
struct PhaseTimer
{
  explicit PhaseTimer(HostPhase p) : phase(p) {}
  ~PhaseTimer()
  {
    s_phase_ns[phase] += std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
  }
  HostPhase phase;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

std::vector<std::string> Split(const std::string& s, char sep)
{
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size())
  {
    const size_t end = s.find(sep, start);
    out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return out;
}

Pad NeutralPad()
{
  return Pad{};  // no buttons, centred sticks, no triggers, err = 0 (connected)
}

u64 HashMem1(Core::System& system)
{
  // FNV-1a over 64-bit words of MEM1, skipping HASH_SKIP.
  auto& memory = system.GetMemory();
  const u8* base = memory.GetRAM();
  const u32 size = memory.GetRamSizeReal();
  u64 h = 1469598103934665603ULL;
  for (u32 off = 0; off + 8 <= size; off += 8)
  {
    bool skip = false;
    for (const auto& [lo, hi] : HASH_SKIP)
      skip |= off >= lo && off < hi;
    if (skip)
      continue;
    u64 w;
    std::memcpy(&w, base + off, 8);
    h = (h ^ w) * 1099511628211ULL;
  }
  return h;
}

bool ParseAddress(const std::string& text, ENetAddress* out)
{
  const auto colon = text.rfind(':');
  if (colon == std::string::npos)
    return false;
  if (enet_address_set_host_ip(out, text.substr(0, colon).c_str()) != 0 &&
      enet_address_set_host(out, text.substr(0, colon).c_str()) != 0)
  {
    return false;
  }
  out->port = static_cast<u16>(std::atoi(text.substr(colon + 1).c_str()));
  return out->port != 0;
}

std::string HostOf(const std::string& endpoint)
{
  return endpoint.substr(0, endpoint.rfind(':'));
}

// "NSRP <you-public> <k>=<public>/<lan> ..." from the rendezvous server.
void HandlePeerList(const std::string& text)
{
  Session& s = s_session;
  const auto parts = Split(text, ' ');
  if (parts.size() < 3 || s.punch.have_peers)
    return;
  const std::string my_public_ip = HostOf(parts[1]);
  for (size_t i = 2; i < parts.size(); ++i)
  {
    const auto eq = parts[i].find('=');
    const auto slash = parts[i].find('/');
    if (eq == std::string::npos || slash == std::string::npos)
      continue;
    const u32 k = static_cast<u32>(std::atoi(parts[i].substr(0, eq).c_str()));
    if (k >= s.players || k == s.me || !s.punch.needed[k])
      continue;
    const std::string pub = parts[i].substr(eq + 1, slash - eq - 1);
    const std::string lan = parts[i].substr(slash + 1);
    // Same public IP = same router: talk over the LAN (many routers don't
    // hairpin). Otherwise the public (NAT-mapped) address, LAN as fallback.
    const bool same_router = HostOf(pub) == my_public_ip;
    ParseAddress(same_router ? lan : pub, &s.punch.target[k]);
    ParseAddress(same_router ? pub : lan, &s.punch.alt[k]);
    Log("player {} at {} (alt {})", k, same_router ? lan : pub, same_router ? pub : lan);
  }
  s.punch.have_peers = true;
}

int ENET_CALLBACK Intercept(ENetHost* host, ENetEvent* /*event*/)
{
  const auto* data = reinterpret_cast<const char*>(host->receivedData);
  const size_t size = host->receivedDataLength;
  if (size >= 4 && std::memcmp(data, "NSRH", 4) == 0)
    return 1;  // a peer's punch packet: its job (opening our NAT) is done
  if (size >= 5 && std::memcmp(data, "NSRP ", 5) == 0)
  {
    HandlePeerList(std::string(data, size));
    return 1;
  }
  return 0;  // ENet traffic
}

void SendRaw(const ENetAddress& to, const std::string& payload)
{
  ENetBuffer buffer;
  buffer.data = const_cast<char*>(payload.data());
  buffer.dataLength = payload.size();
  enet_socket_send(s_session.host->socket, &to, &buffer, 1);
}

// Configuration. Either the 2-player shortcut
//   NSR_NETPLAY=host                (player 0 of 2, binds NSR_PORT)
//   NSR_NETPLAY=client NSR_PEER=ip:port   (player 1 of 2)
// or the general form for 2-4 players
//   NSR_NETPLAY=1 NSR_PLAYER=k NSR_PLAYERS=n NSR_PORT=p
//   NSR_PEERS=ip:port,ip:port,...   (addresses of players 0..k-1, in order)
bool ReadConfig(Session& s, std::vector<std::string>* lower_peers)
{
  const std::string mode = Env("NSR_NETPLAY");
  if (mode.empty())
    return false;
  if (mode == "host")
  {
    s.me = 0;
    s.players = 2;
  }
  else if (mode == "client")
  {
    s.me = 1;
    s.players = 2;
    lower_peers->push_back(Env("NSR_PEER", "127.0.0.1:51515"));
  }
  else
  {
    s.me = static_cast<u32>(std::atoi(Env("NSR_PLAYER", "0").c_str()));
    s.players = static_cast<u32>(std::atoi(Env("NSR_PLAYERS", "2").c_str()));
    if (!Env("NSR_PEERS").empty())
      *lower_peers = Split(Env("NSR_PEERS"), ',');
  }
  s.players = std::clamp<u32>(s.players, 2, MAX_PLAYERS);
  s.star = Env("NSR_TOPOLOGY", "star") != "mesh";
  if (const std::string rv = Env("NSR_RENDEZVOUS"); !rv.empty())
  {
    if (!s.star)
    {
      Log("hole punching needs the star topology");
      return false;
    }
    s.punch.enabled = ParseAddress(rv, &s.punch.server);
    s.punch.room = Env("NSR_ROOM");
    s.punch.key = Env("NSR_KEY");
    s.punch.lan_ip = Env("NSR_LAN_IP", "0.0.0.0");
    if (!s.punch.enabled || s.punch.room.empty() || s.punch.key.empty())
    {
      Log("bad rendezvous config ({})", rv);
      return false;
    }
    for (u32 i = 0; i < s.players; ++i)
      s.punch.needed[i] = i != s.me && (s.me == 0 || i == 0);
    return true;  // addresses come from the server, not NSR_PEERS
  }
  if (s.star && lower_peers->size() > 1)
    lower_peers->resize(1);  // star: only player 0's address is needed
  const size_t needed = s.star ? (s.me > 0 ? 1 : 0) : s.me;
  if (s.me >= s.players || lower_peers->size() != needed)
  {
    Log("bad config: player {} of {} with {} lower-player addresses", s.me, s.players,
        lower_peers->size());
    return false;
  }
  s.delay = static_cast<u32>(std::max(0, std::atoi(Env("NSR_DELAY", "2").c_str())));
  return true;
}

bool SetupPunch();
void SendPunches();
void ConnectToHost();

bool Setup()
{
  Session& s = s_session;
  s.initialized = true;
  if (const std::string path = Env("NSR_LOG"); !path.empty() && s.log == stderr)
  {
    if (FILE* f = std::fopen(path.c_str(), "w"))
      s.log = f;
  }
  std::vector<std::string> lower_peers;
  if (!ReadConfig(s, &lower_peers))
    return false;
  if (enet_initialize() != 0)
  {
    Log("enet_initialize failed");
    return false;
  }

  if (s.punch.enabled)
    return SetupPunch();

  // Everyone binds (higher-numbered players connect to us); if we're the
  // last player nobody connects in, but binding is harmless.
  ENetAddress addr{};
  addr.host = ENET_HOST_ANY;
  const std::string port = Env("NSR_PORT");
  addr.port = port.empty() ? DEFAULT_PORT : static_cast<u16>(std::atoi(port.c_str()));
  const bool listens = s.star ? s.me == 0 : s.me != s.players - 1;
  s.host = enet_host_create(listens ? &addr : nullptr, MAX_PLAYERS, 1, 0, 0);
  if (!s.host)
  {
    Log("enet_host_create failed (port {})", addr.port);
    return false;
  }
  // Connect to every lower-numbered player. The connect data carries our
  // index so they know who we are.
  for (u32 i = 0; i < lower_peers.size(); ++i)
  {
    const std::string& p = lower_peers[i];
    const auto colon = p.rfind(':');
    ENetAddress target{};
    enet_address_set_host(&target, p.substr(0, colon).c_str());
    target.port = static_cast<u16>(std::atoi(p.substr(colon + 1).c_str()));
    s.peers[i] = enet_host_connect(s.host, &target, 1, s.me);
    if (s.peers[i])
      s.peers[i]->data = reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1));
  }
  Log("player {} of {} delay={} port={} topology={} seed={}", s.me, s.players, s.delay,
      addr.port, s.star ? "star" : "mesh", Env("NSR_SEED", "(image default)"));
  return true;
}

u32 PeerIndex(const ENetPeer* peer)
{
  return static_cast<u32>(reinterpret_cast<uintptr_t>(peer->data)) - 1;  // 0 = unknown
}

bool Linked(u32 i)
{
  const Session& s = s_session;
  return s.peers[i] && s.peers[i]->state == ENET_PEER_STATE_CONNECTED;
}

// Connections this machine itself must hold: in a star, player 0 links to
// everyone and everyone else links only to player 0; in a mesh, everyone.
bool AllConnected()
{
  const Session& s = s_session;
  for (u32 i = 0; i < s.players; ++i)
  {
    if (i == s.me)
      continue;
    const bool needed = !s.star || s.me == 0 || i == 0;
    if (needed && !Linked(i))
      return false;
  }
  return true;
}

void DumpSnapshot(u32 frame)
{
  Session& s = s_session;
  const std::string prefix = Env("NSR_DUMP_PREFIX");
  if (prefix.empty() || s.dumped)
    return;
  auto it = s.snapshots.find(frame);
  if (it == s.snapshots.end())
    return;
  const std::string path = prefix + "-" + std::to_string(frame) + ".bin";
  if (FILE* f = std::fopen(path.c_str(), "wb"))
  {
    std::fwrite(it->second.data(), 1, it->second.size(), f);
    std::fclose(f);
    s.dumped = true;
    Log("wrote {}", path);
  }
}

void CheckHash(u32 player, u32 frame)
{
  Session& s = s_session;
  auto local = s.local_hashes.find(frame);
  auto remote = s.remote_hashes[player].find(frame);
  if (local == s.local_hashes.end() || remote == s.remote_hashes[player].end())
    return;
  if (local->second != remote->second && frame != s.last_desync_frame)
  {
    s.last_desync_frame = frame;
    ++s.desyncs;
    Log("DESYNC at frame {} vs player {}: local {:016x} remote {:016x}", frame, player,
        local->second, remote->second);
    DumpSnapshot(frame);
  }
}

u32 TelemetryLiveFrame();  // NSR telemetry: next live frame (defined with the glue)

// Bind the sender carried in an unsequenced datagram to the ENet connection
// that delivered it.  A star leaf receives both the host's own packets and
// the other leaves' relayed packets from player 0; everywhere else the two
// identities must be identical.
bool PacketSenderMatchesPeer(u32 sender, u32 source)
{
  const Session& s = s_session;
  if (source >= s.players)
    return false;
  if (s.star && s.me != 0)
    return source == 0;
  return source == sender;
}

void StopForRollbackTransportFailure()
{
  // Milestone-1 lockstep historically falls back offline.  Rollback cannot:
  // returning from a failed preflight would let the guest advance without a
  // prepared frame, so a configured rollback session always fails closed.
  if (!Env("NSR_RB_NET").empty())
    Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
}

void HandlePacket(const ENetPacket* packet, u32 source)
{
  Session& s = s_session;
  u32 magic = 0;
  if (packet->dataLength >= sizeof(magic))
    std::memcpy(&magic, packet->data, sizeof(magic));
  // A peer running another protocol version cannot be parsed at all; say so
  // once (M2) instead of leaving only a timeout.  Byte 4 is the version in
  // both rollback formats.
  if ((magic == RollbackNet::HANDOFF_MAGIC || magic == RollbackNet::MAGIC) &&
      packet->dataLength > 4)
  {
    const u8 version = static_cast<const u8*>(packet->data)[4];
    const u8 expected =
        magic == RollbackNet::MAGIC ? RollbackNet::VERSION : RollbackNet::HANDOFF_VERSION;
    static bool version_logged = false;
    if (version != expected && !version_logged)
    {
      version_logged = true;
      Log("bad config: peer {} sends rollback protocol {} version {}, this build speaks {}",
          source, magic == RollbackNet::MAGIC ? "NSRB" : "NSRH", version, expected);
    }
  }
  if (magic == RollbackNet::HANDOFF_MAGIC)
  {
    const auto decoded = RollbackNet::DecodeHandoff(
        std::span<const u8>(static_cast<const u8*>(packet->data), packet->dataLength));
    if (!decoded || decoded->players != s.players || decoded->sender == s.me ||
        decoded->sender >= s.players || !PacketSenderMatchesPeer(decoded->sender, source))
    {
      return;
    }
    if (!s_rb_handoff || !s_rb_handoff->Accept(*decoded))
      return;
    // The star host relays the original proposal unchanged.  Agreement is
    // still all-to-all: every receiver validates the sender and full tuple.
    // Reliability must survive this second hop; otherwise the sender can
    // leave its barrier before one leaf ever observes the proposal.
    if (s.star && s.me == 0)
    {
      for (u32 i = 1; i < s.players; ++i)
      {
        if (i != decoded->sender && Linked(i))
        {
          enet_peer_send(s.peers[i], 0,
                         enet_packet_create(packet->data, packet->dataLength,
                                            ENET_PACKET_FLAG_RELIABLE));
        }
      }
    }
    return;
  }
  if (magic == RollbackNet::MAGIC)
  {
    const auto decoded = RollbackNet::Decode(
        std::span<const u8>(static_cast<const u8*>(packet->data), packet->dataLength));
    if (!decoded || !s_rb_exchange || decoded->players != s.players || decoded->sender == s.me ||
        decoded->sender >= s.players || !PacketSenderMatchesPeer(decoded->sender, source))
    {
      NSRTelemetry::Count(NSRTelemetry::Counter::PacketsRejected);  // NSR telemetry
      return;
    }
    if (decoded->session != s_rb_exchange->Session())
    {
      NSRTelemetry::Count(NSRTelemetry::Counter::PacketsRejected, 1, decoded->sender);
      // One peer can complete the reliable entry barrier and emit its first
      // new-epoch NSRB packet while this peer is still receiving that
      // barrier's final proposal. Reject it as usual, but it is an expected
      // transition race rather than a protocol/session diagnostic.
      if (!s_rb_handoff && !s_rb_bad_session_logged)
      {
        s_rb_bad_session_logged = true;
        Log("rollback session mismatch from player {}: expected {:08x}, got {:08x}",
            decoded->sender, s_rb_exchange->Session(), decoded->session);
      }
      return;
    }
    // Accept validates the whole datagram first (impossible ack, conflicting
    // restatement, out-of-window input/checksum) and applies nothing when it
    // fails.  Do this before acting on flags or relaying the packet so one
    // peer cannot make the hub amplify an invalid datagram.
    if (!s_rb_exchange->Accept(*decoded, HostNowUs()))
    {
      NSRTelemetry::Count(NSRTelemetry::Counter::PacketsRejected, 1, decoded->sender);
      if (const auto& mismatch = s_rb_exchange->ConfigMismatch(); mismatch && !s.failed)
      {
        // Same epoch, different simulation config (fixed-epoch sessions have
        // no handoff barrier; otherwise the barrier already refused it).
        FailConfigMismatch(mismatch->player, nullptr, nullptr, s_rb_exchange->Config(),
                           mismatch->remote);
        return;
      }
      const auto& v = s_rb_exchange->FirstViolation();
      if (v && !s_rb_violation_logged)
      {
        s_rb_violation_logged = true;
        Log("rejected rollback packet from player {}: protocol violation kind {} at frame {}",
            v->player, static_cast<int>(v->kind), v->frame);
        NSRTelemetry::Event(NSRTelemetry::EventKind::Note, TelemetryLiveFrame(),  // NSR telemetry
                            {NSRTelemetry::Int("protocol_violation", static_cast<int>(v->kind)),
                             NSRTelemetry::Int("player", v->player),
                             NSRTelemetry::Int("frame", static_cast<s64>(v->frame))});
      }
      return;
    }
    if ((decoded->flags & 0x02) != 0 && !s_rb_peer_desync_logged)
    {
      s_rb_peer_desync_logged = true;
      Log("player {} reports a confirmed semantic checksum mismatch", decoded->sender);
      NSRTelemetry::Event(NSRTelemetry::EventKind::PeerDesync, TelemetryLiveFrame(),  // NSR telemetry
                          {NSRTelemetry::Int("player", decoded->sender)});
    }
    // NSR telemetry: arrivals, and how late the newest input is for our live frame.
    NSRTelemetry::Count(NSRTelemetry::Counter::PacketsReceived, 1, decoded->sender);
    if (!decoded->pads.empty())
    {
      const u32 newest = decoded->first_frame + static_cast<u32>(decoded->pads.size()) - 1;
      const u32 live = TelemetryLiveFrame();
      NSRTelemetry::Observe(NSRTelemetry::Hist::RemoteInputLateFrames,
                            live > newest ? live - newest : 0, decoded->sender);
    }
    // The host relays the original validated session datagram unchanged;
    // every receiver performs the same strict parser and session check.
    if (s.star && s.me == 0)
    {
      for (u32 i = 1; i < s.players; ++i)
      {
        if (i != decoded->sender && Linked(i))
        {
          enet_peer_send(s.peers[i], 0,
                         enet_packet_create(packet->data, packet->dataLength,
                                            ENET_PACKET_FLAG_UNSEQUENCED));
        }
      }
    }
    return;
  }
  if (packet->dataLength < offsetof(Packet, pads))
    return;
  Packet p{};
  std::memcpy(&p, packet->data, std::min(sizeof(Packet), packet->dataLength));
  // Service can receive a peer's first redundant send while we are still in
  // the connection wait.  Drop it until our own stream has a frame anchor;
  // it will be resent.  Thereafter both sides are in lockstep, so this slack
  // comfortably covers scheduling skew without retaining attacker-selected
  // far-future map keys forever.
  const u64 future_limit = s.local_pads.empty() ? 0 :
                               static_cast<u64>(s.local_pads.rbegin()->first) + REDUNDANCY + 128;
  if (p.magic != PACKET_MAGIC || p.count > REDUNDANCY || p.sender >= s.players ||
      p.sender == s.me || !PacketSenderMatchesPeer(p.sender, source) ||
      packet->dataLength != offsetof(Packet, pads) + p.count * sizeof(Pad) ||
      (p.count != 0 && p.latest_frame < p.count - 1) || s.local_pads.empty() ||
      p.latest_frame > future_limit ||
      (p.checksum_frame != 0 && p.checksum_frame > future_limit))
  {
    return;
  }
  // Star hub: pass the packet on, unchanged, to every other player.
  if (s.star && s.me == 0)
  {
    for (u32 i = 1; i < s.players; ++i)
    {
      if (i != p.sender && Linked(i))
      {
        enet_peer_send(s.peers[i], 0,
                       enet_packet_create(packet->data, packet->dataLength,
                                          ENET_PACKET_FLAG_UNSEQUENCED));
      }
    }
  }
  for (u32 i = 0; i < p.count; ++i)
  {
    const u32 frame = p.latest_frame - (p.count - 1 - i);
    s.remote_pads[p.sender].try_emplace(frame, p.pads[i]);
  }
  if (p.checksum_frame)
  {
    s.remote_hashes[p.sender][p.checksum_frame] = p.checksum;
    CheckHash(p.sender, p.checksum_frame);
  }
}

void Service(int timeout_ms)
{
  Session& s = s_session;
  ENetEvent event;
  while (enet_host_service(s.host, &event, static_cast<u32>(timeout_ms)) > 0)
  {
    timeout_ms = 0;  // drain what's queued without waiting again
    switch (event.type)
    {
    case ENET_EVENT_TYPE_CONNECT:
    {
      // Outgoing connections already know their index; incoming ones tell
      // us through the connect data.
      u32 index = PeerIndex(event.peer);
      if (event.peer->data == nullptr)
      {
        index = event.data;
        event.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(index + 1));
      }
      if (index >= s.players || index == s.me)
      {
        Log("rejected connection with invalid player index {}", index);
        enet_peer_disconnect_now(event.peer, 0);
      }
      else if (s.peers[index] && s.peers[index] != event.peer && Linked(index))
      {
        Log("duplicate connection claiming player {}", index);
        enet_peer_disconnect_now(event.peer, 0);
        s.failed = true;
        StopForRollbackTransportFailure();
      }
      else
      {
        s.peers[index] = event.peer;
        Log("connected to player {}", index);
      }
      break;
    }
    case ENET_EVENT_TYPE_RECEIVE:
    {
      const u32 source = event.peer->data ? PeerIndex(event.peer) : MAX_PLAYERS;
      HandlePacket(event.packet, source);
      enet_packet_destroy(event.packet);
      break;
    }
    case ENET_EVENT_TYPE_DISCONNECT:
      // A client's connect attempt timed out (NAT not open yet): retry,
      // alternating the address, a few times before giving up.
      if (s.punch.enabled && s.me != 0 && !s.exchanged_once && s.peers[0] == event.peer &&
          s.punch.connect_attempts < 4)
      {
        Log("connect attempt {} to the host failed; retrying", s.punch.connect_attempts);
        SendPunches();
        ConnectToHost();
        break;
      }
      if (event.peer->data)
        Log("player {} disconnected", PeerIndex(event.peer));
      s.failed = true;
      StopForRollbackTransportFailure();
      break;
    default:
      break;
    }
  }
}

void SendPunches()
{
  Session& s = s_session;
  for (u32 i = 0; i < s.players; ++i)
  {
    if (s.punch.needed[i] && !Linked(i))
    {
      SendRaw(s.punch.target[i], "NSRH");
      SendRaw(s.punch.alt[i], "NSRH");
    }
  }
}

void ConnectToHost()
{
  Session& s = s_session;
  // Alternate between the chosen address and the fallback on retries.
  const ENetAddress& to = (s.punch.connect_attempts % 2 == 0) ? s.punch.target[0] : s.punch.alt[0];
  ++s.punch.connect_attempts;
  s.peers[0] = enet_host_connect(s.host, &to, 1, s.me);
  if (s.peers[0])
    s.peers[0]->data = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
}

bool SetupPunch()
{
  Session& s = s_session;
  ENetAddress any{};
  any.host = ENET_HOST_ANY;
  any.port = 0;  // ephemeral: the server tells peers our NAT-mapped port
  s.host = enet_host_create(&any, MAX_PLAYERS, 1, 0, 0);
  if (!s.host)
  {
    Log("enet_host_create failed");
    return false;
  }
  s.host->intercept = Intercept;
  ENetAddress local{};
  enet_socket_get_address(s.host->socket, &local);
  const std::string reg = fmt::format("NSRR {} {} {} {}:{}", s.punch.room, s.me, s.punch.key,
                                      s.punch.lan_ip, local.port);
  Log("player {} of {} delay={} topology=star punch via rendezvous (local port {}) seed={}", s.me,
      s.players, s.delay, local.port, Env("NSR_SEED", "(image default)"));

  const auto start = std::chrono::steady_clock::now();
  auto last = start - std::chrono::seconds(1);
  while (!s.punch.have_peers)
  {
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::milliseconds(250))
    {
      SendRaw(s.punch.server, reg);
      last = now;
    }
    Service(20);
    if (now - start > std::chrono::milliseconds(CONNECT_TIMEOUT_MS))
    {
      Log("rendezvous: not every player registered within {} ms", CONNECT_TIMEOUT_MS);
      return false;
    }
  }
  SendPunches();
  if (s.me != 0)
    ConnectToHost();
  return true;
}

void SendLatest(u32 latest_frame)
{
  Session& s = s_session;
  Packet p{};
  p.magic = PACKET_MAGIC;
  p.sender = s.me;
  p.latest_frame = latest_frame;
  // The longest contiguous run of recorded pads ending at latest_frame.
  u32 n = 0;
  while (n < REDUNDANCY && n <= latest_frame && s.local_pads.contains(latest_frame - n))
    ++n;
  for (u32 i = 0; i < n; ++i)
    p.pads[i] = s.local_pads.at(latest_frame - (n - 1 - i));
  p.count = n;
  if (s.last_sent_hash_frame)
  {
    p.checksum_frame = s.last_sent_hash_frame;
    p.checksum = s.local_hashes[s.last_sent_hash_frame];
  }
  const size_t size = offsetof(Packet, pads) + n * sizeof(Pad);
  for (u32 i = 0; i < s.players; ++i)
  {
    if (i == s.me || !Linked(i))
      continue;
    enet_peer_send(s.peers[i], 0, enet_packet_create(&p, size, ENET_PACKET_FLAG_UNSEQUENCED));
  }
  enet_host_flush(s.host);
}

// Send one NSRB datagram containing the unacknowledged local prefix plus all
// receive acknowledgements.  Repeating this every frame (and while stalled)
// makes packet loss recovery independent of ENet reliability queues.
void SendRollbackLatest(u32 live_frame)
{
  Session& s = s_session;
  if (!s_rb_exchange || !s.host)
    return;
  const u32 send_us = HostNowUs();
  const std::vector<u8> bytes = RollbackNet::Encode(s_rb_exchange->BuildPacket(live_frame, send_us));
  for (u32 i = 0; i < s.players; ++i)
  {
    if (i != s.me && Linked(i))
    {
      enet_peer_send(s.peers[i], 0,
                     enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_UNSEQUENCED));
      NSRTelemetry::Count(NSRTelemetry::Counter::PacketsSent, 1, static_cast<int>(i));
    }
  }
  enet_host_flush(s.host);
}

// Broadcast the current transition proposal.  Unlike continuously repeated
// input history, a peer stops announcing this proposal as soon as its local
// barrier completes. ENet reliability therefore closes the window where one
// leaf misses a relayed proposal after the other peers have already left.
void SendHandoff()
{
  Session& s = s_session;
  if (!s_rb_handoff || !s.host)
    return;
  const std::vector<u8> bytes = RollbackNet::EncodeHandoff(s_rb_handoff->Local());
  for (u32 i = 0; i < s.players; ++i)
  {
    if (i != s.me && Linked(i))
      enet_peer_send(s.peers[i], 0,
                     enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE));
  }
  enet_host_flush(s.host);
}

bool WaitForConnection()
{
  Session& s = s_session;
  const auto start = std::chrono::steady_clock::now();
  auto last_punch = start;
  while (!AllConnected() && !s.failed)
  {
    Service(10);
    if (s.punch.enabled && std::chrono::steady_clock::now() - last_punch > std::chrono::milliseconds(100))
    {
      SendPunches();
      last_punch = std::chrono::steady_clock::now();
    }
    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(CONNECT_TIMEOUT_MS))
    {
      Log("not all {} players connected within {} ms; giving up", s.players, CONNECT_TIMEOUT_MS);
      s.failed = true;
      StopForRollbackTransportFailure();
    }
  }
  if (!s.failed)
    Log("all {} players connected", s.players);
  return !s.failed;
}

bool HaveAllPads(u32 frame)
{
  const Session& s = s_session;
  for (u32 i = 0; i < s.players; ++i)
  {
    if (i != s.me && !s.remote_pads[i].contains(frame))
      return false;
  }
  return true;
}

// Returns the scripted pad for this retrace, or nothing when no script is
// loaded / the script has ended (then the real local controller is used).
std::optional<Pad> ScriptPad(const Core::CPUThreadGuard& guard, u32 counter)
{
  using PowerPC::MMU;
  if (MMU::HostRead<u32>(guard, SCRIPT_TABLE) == 0)
    return std::nullopt;
  const u32 loop_start = MMU::HostRead<u32>(guard, SCRIPT_LOOP);
  const u32 loop_len = MMU::HostRead<u32>(guard, SCRIPT_LOOP + 4);
  if (loop_len != 0 && counter >= loop_start)
    counter = loop_start + (counter - loop_start) % loop_len;
  for (u32 i = 0; i < SCRIPT_MAX; ++i)
  {
    const u32 entry = SCRIPT_TABLE + i * 8;
    const u32 until = MMU::HostRead<u32>(guard, entry);
    if (until == 0)
      return std::nullopt;
    if (counter < until)
    {
      Pad pad{};
      const u16 buttons = MMU::HostRead<u16>(guard, entry + 4);
      pad[0] = static_cast<u8>(buttons >> 8);
      pad[1] = static_cast<u8>(buttons);
      pad[2] = MMU::HostRead<u8>(guard, entry + 6);
      pad[3] = MMU::HostRead<u8>(guard, entry + 7);
      return pad;
    }
  }
  return std::nullopt;
}

void WritePads(const Core::CPUThreadGuard& guard, u32 buf, const std::array<Pad, MAX_PLAYERS>& pads)
{
  for (u32 port = 0; port < s_session.players; ++port)
  {
    for (u32 i = 0; i < PAD_SIZE; ++i)
      PowerPC::MMU::HostWrite<u8>(guard, pads[port][i], buf + port * PAD_SIZE + i);
  }
}

// A rollback epoch can span many VI retraces and peers need not consume the
// same host/hardware timeline while correcting predictions.  Start a fresh
// lockstep sequence after every match instead of asking the 32-pad resend
// window to bridge that intentional gap.
void ResetLockstepStream()
{
  Session& s = s_session;
  s.local_pads.clear();
  for (auto& pads : s.remote_pads)
    pads.clear();
  s.applied.clear();
  s.local_hashes.clear();
  for (auto& hashes : s.remote_hashes)
    hashes.clear();
  s.snapshots.clear();
  s.last_sent_hash_frame = 0;
  s.started = false;
  s.first_frame = 0;
  s.next_target = 0;
}

// --- Rollback (milestone 2, stage 1) ----------------------------------------
// See NSRNetplay.h. The game-side tick routine calls RollbackTickHook before
// every sim tick (sim part 1 + part 2, which also read the controllers), so
// the state at that call is "the state before tick t". Snapshots are host-side
// copies of MEM1 taken there: zero emulated time, like the lockstep wait.

constexpr u32 RB_PAD_BUF = 0x803E92CC;  // the game's PADStatus[4]
constexpr u32 RB_GRANULE = 64;          // mismatch-report resolution

// RAM a restore leaves alone and a comparison ignores (physical offsets):
//   - main-thread stack (_stack_end.._stack_addr from the boot code: r1 =
//     0x803FE4C0, 64 KB). The tick hook runs inside it; restoring it would
//     overwrite live return addresses. Below r1 it is dead scratch.
//   - HASH_SKIP: card work areas, raw SI input, the NFLStreet patch area
//     (retrace counter, test scripts).
//   - the rollback patch area (stubs, PADRead scratch, routines).
//   - the trace ring (diagnostic images only).
//   - Dolphin SDK state (OS, alarms, threads, DVD, VI, SI, PAD, AI/AX/DSP
//     audio, AR, CARD, GX, EXI, ...), which follows the hardware and real
//     time, not game ticks. Ranges = .data/.bss/.sbss blocks referenced only
//     by SDK code (text >= 0x80230000), found by scanning lis/addi and r13
//     references in main.dol (256-byte resolution; see NFLStreet
//     docs/poc-milestone2.md). Includes the AX voice/parameter-block arrays
//     and two SDK-created threads' stacks.
//   - low memory: OS globals (time, arena, thread queue, current context).
// Live OS objects elsewhere (game-created threads and their stacks, queued
// alarms, the GX FIFO) are found at restore time; see RbLiveRanges.
constexpr std::array<std::pair<u32, u32>, 21> RB_KEEP{{
    {0x003EE4C0, 0x003FE4C0},  // main-thread stack
    {0x00384C00, 0x00398E00},  // memory-card work areas
    {0x00411A00, 0x00412000},  // NFLStreet patch area (counters, scripts)
    {0x00416900, 0x00416B00},  // rollback patch area (including wait/music trampolines)
    {0x004132C0, 0x00413E00},  // trace ring
    {0x00000000, 0x00003100},  // low memory
    // SDK .data, only the words SDK code references (lis/addi pairs, see
    // NFLStreet docs/research-log.md 2026-10-09). The old blocks
    // (0x802B6000.., 0x802FB400..0x802FFE00, 0x80300800.., 0x80302400..) also
    // held game/EA-engine data (restored) and audio-middleware data (moved to
    // AUDIO_KEEP); keeping those live leaked state set by later frames into
    // re-simulated ones (EA floats at 0x802FB45C changed at a play start).
    // 0x802C7000..0x802C7200 used to be kept too ("no direct references" in
    // the original main.dol), but Reborn's added code (0x80410xxx, the tail of
    // its text) dereferences the pointer at 0x802C7014: once a later frame
    // nulled it, a rollback across that frame resumed Reborn's code on the
    // null pointer and looped on invalid reads (RC1 regress, frame 14,382).
    // It is game state and is restored with the rest.
    {0x002B7240, 0x002B7260},
    {0x002FB468, 0x002FE350},
    {0x002FE440, 0x002FE444},  // referenced by game and SDK code
    {0x002FE4C0, 0x002FF4A0},
    {0x00302500, 0x00306100},
    {0x003A0100, 0x003A0300},  // SDK .bss
    {0x003A0700, 0x003A0800},
    // SDK + audio .bss, by owner (decomp evidence + which code references
    // each block): OS/DVD/VI/GX/CARD/EXI/SI from 0x803A3A70, then the audio
    // middleware (code 0x8025E000..0x80285A90, incl. its worker thread) and
    // AX/GBA, up to 0x803D4760. Audio is one real-time subsystem: rewinding
    // the middleware while AX stays live corrupts the AX voice list. The
    // block before it, 0x803A2800..0x803A3A70, belongs to the EA engine
    // (code 0x80236000..0x8023873C: rings, tick accumulator) and is restored.
    {0x003A3A70, 0x003A6A08},  // OS, DVD, VI, GX FIFO object, CARD, EXI, SI
    {0x003D4400, 0x003D4760},  // GBA controls
    // Audio hardware handshake, live even when audio rolls back: AX DSP
    // command list, output DMA buffers, DSP DRAM image, DSP task.
    {0x003BD980, 0x003C29D0},
    {0x003EC160, 0x003EC5C0},  // SDK .sdata
    {0x003ED020, 0x003ED4E0},  // SDK .sbss (incl. the OS alarm queue), up to DSP
    {0x003ED4E0, 0x003ED510},  // DSP task globals (hardware handshake)
    {0x003ED568, 0x003ED574},  // AX output frame / AI DMA frame / DSP ready
    {0x003ED584, 0x003ED58C},  // AX DSP init/done flags
}};

using Pads = std::array<Pad, MAX_PLAYERS>;

// The game's fixed-timestep tick clock (GN7E69). InGame's tick loop calls
// fn_802378C8, which drains a real-time accumulator and adds the ticks it
// returns to a game clock, *between* sim calls:
//   outer = *(TICK_CLOCK_PTR); outer+4 += fn_802379DC(*(outer+0xC))
// The sim reads outer+4. Re-simulation skips fn_802378C8, so Dolphin
// records outer+4 for every tick and puts the recorded value back before
// each re-simulated tick. The inner accumulator (outer+0xC -> 0x10 bytes)
// follows real time and is never restored.
constexpr u32 TICK_CLOCK_PTR = 0x803EC138;  // -0x6168(r13)
// The game's pause nesting depth (fix/pausetime). Pause entry fn_8002885C
// increments it, exit fn_800288B0 decrements it. InGame (0x80028604) feeds
// the tick accumulator (fn_80237830(1)) only while it is zero, so stock runs
// no sim tick at all while the pause menu is open: the play clock, game
// timers and the tick clock outer+4 stand still. It is game .sbss (restored
// by rollback, identical on every peer), so TickCountHook may key on it.
// Not the flag at -0x5CA0(r13): fn_80028918 also writes that from sim code.
constexpr u32 PAUSE_DEPTH = 0x803EC5FC;  // -0x5CA4(r13)
// A retrace-derived game clock not covered by VI_POST_COUNTER. The two are
// proposed independently at lifecycle barriers because teardown-frame audio
// waits can advance them by different amounts.
constexpr u32 TRANSITION_AUX_CLOCK = 0x8039B438;
// The game's disc worker (GN7E69): thread, its semaphores (request queue at
// 0x80381C10), descriptor and DVD staging buffer (Codex audit). In I/O-idle
// mode (NSR_RB_IO_IDLE) they are restored with everything else, because
// rollback only ever restores snapshots taken while the disc system was idle
// and only restores while it is idle now. Otherwise they are kept live.
constexpr u32 DISC_WORKER_THREAD = 0x8037F8C0;
// Its two OSSemaphores: B (0x80381C0C, queue 0x80381C10) is what the worker
// sleeps on, A (0x80381C00) counts free read slots: 21 when no read is
// anywhere in its pipeline, 20 while one is outstanding (observed in play).
constexpr u32 DISC_WORKER_QUEUE = 0x80381C10;
constexpr u32 DISC_SEM_A = 0x80381C00, DISC_SEM_B = 0x80381C0C, DISC_SLOTS_IDLE = 21;
constexpr std::array<std::pair<u32, u32>, 3> DISC_KEEP{{
    {0x00381C00, 0x00381C18},  // the worker's two OSSemaphores
    {0x003D55E0, 0x003D55FC},  // its descriptor
    {0x003D5600, 0x003D7600},  // its DVD staging buffer (reads are <= 0x2000 here)
}};
constexpr u32 PENDING_LOADS = 0x803EBD84;  // -0x651C(r13): game loads in flight

// Audio software state: the audio middleware and AX library .bss/.sbss
// (excluding the hardware handshake kept in RB_KEEP), the audio thread
// (OSThread and stack live in that .bss). Kept live by default. With
// NSR_RB_AUDIO_ROLLBACK it rolls back with the game instead: everything
// audio-related rewinds together (consistent pointers between the game's
// sound layer, the middleware, its heap pools and generated code), only when
// the audio thread is asleep at its usual wait point. Cost: an audible
// hiccup on rollback, never a logic difference.
constexpr std::array<std::pair<u32, u32>, 10> AUDIO_KEEP{{
    // Audio-middleware .data (referenced only from 0x8025E000..0x80285A90).
    {0x00289A00, 0x00289B00},
    {0x002B7260, 0x002B7B00},
    {0x002C1900, 0x002C2900},
    {0x002FF4A0, 0x002FFE00},
    {0x00300800, 0x00301900},
    {0x00302400, 0x00302500},
    {0x003A6A08, 0x003BD980},
    {0x003C29D0, 0x003D4400},
    {0x003ED510, 0x003ED568},
    {0x003ED58C, 0x003ED5F0},
}};
constexpr u32 AUDIO_THREAD = 0x803AA828;
constexpr u32 AUDIO_THREAD_QUEUE = 0x803AAB40;  // where it sleeps between audio frames

// Per-frame counters that live outside the sim but are read by it. They
// advance between sim calls (tick loop bookkeeping) or in the frame tail, so
// re-simulation would not advance them: Dolphin records each one at every
// live tick and writes the recorded value back before each re-simulated
// tick. {pointer cell, field offset} -> *(*cell + offset). Both were found
// by the perturbation probe (NSR_RB_PERTURB_AT).
struct TickCounter
{
  u32 cell, offset;
};
constexpr std::array<TickCounter, 4> TICK_COUNTERS{{
    {TICK_CLOCK_PTR, 0x0},  // tick clock object: all three counter words
    {TICK_CLOCK_PTR, 0x4},  // game clock: ticks consumed (fn_802378C8)
    {TICK_CLOCK_PTR, 0x8},  // second copy, read by the frame tail
                            // (copied into 0x80487A88 in a 2-tick rollback
                            // test, 2 behind before this was restored)
    {0x803EBE44, 0x20},     // frame counter read by the sound rate limiter
                            // (fn_801F7ABC; advanced by fn_801F7AE8)
}};

enum class AcMode
{
  Off,
  On,
  Shadow
};

// Environment-backed rollback options belong to one emulation lifetime.  Keeping them here,
// rather than in function-local statics, lets Shutdown fully reset a process before another game
// boot parses its own configuration.
struct RollbackConfig
{
  bool nsr_enabled = false;
  bool netplay_enabled = false;
  std::optional<u32> seed;
  bool instant_disc = false;
  bool sample = false;
  std::string stacks_path;
  std::vector<std::string> transplant;
  std::vector<std::string> dump_units;
  std::vector<std::string> adopt;
  std::optional<u32> force_random_seed;
  bool disc_trace = false;
  bool io_trace = false;
  bool quiet_trace = false;
  bool async_present = false;
  u32 peek_z = 0;
  bool keep_resim_vsync = false;
  std::vector<u32> audio_gate_only;
  std::vector<u32> effect_gates;
  std::vector<u32> audio_trace;
  std::vector<u32> probes;
  std::vector<u32> skip_resim;
  bool probe_all = false;
  u32 session_end = 0;
  bool auto_reenter = false;
  AcMode ac_mode = AcMode::On;
  std::string ac_log_path;
  std::vector<std::pair<u32, u32>> ac_hash_ranges;
  u32 ac_speech_frames = 75;
  u32 ac_speech_lead = 0;
  u32 ac_sfx_default = 30;
  u32 ac_sfx_tail = 2;
  u32 ac_bank_frames = 6;
  u32 ac_polls_per_tick = 1000;
  u32 ac_song_frames = 10800;
  u32 ac_song_gap = 380;
  // NSR_RB_DIGEST=semantic: exchange only the nine semantic words, as before
  // the canonical state digest (diagnostics: shows what the old check misses).
  bool digest_semantic_only = false;
  // NSR_RB_DIGEST_EVERY=n (1..60, default 1): digest and exchange only frame
  // starts that are multiples of n (peers must agree; it is in the config
  // fingerprint).  A slow host trades detection latency for host time.
  u32 digest_every = 1;
  // NSR_RB_TEST_CORRUPT=frame:addr (diagnostics, one peer only): flip one byte
  // of game state at the start of `frame` (live and re-simulated alike, so a
  // rollback cannot undo it) to prove the desync check detects divergence.
  std::optional<std::pair<u32, u32>> test_corrupt;
};

struct Rollback
{
  bool initialized = false;
  bool config_loaded = false;
  RollbackConfig config;
  bool seed_written = false;
  u32 force = 0;          // NSR_RB_FORCE: ticks to roll back before every live tick
  // NSR_RB_MISPREDICT=d (with NSR_RB_REPLAY, and NSR_RB_FORCE > 0 for the ring):
  // netplay with input delay d on one machine. Each live frame runs with the
  // last confirmed pads (frame t-d) as its prediction; when frame c = t-d's
  // true pads differ from what c ran with, roll back to c (or the newest
  // I/O-idle snapshot before it) and re-run with the corrected pads.
  u32 mispredict = 0;
  u32 mispredict_pending = UINT32_MAX;  // earliest frame still to correct
  u64 mispredictions = 0;
  u64 uncorrectable = 0;  // mispredictions older than the ring/session/barrier
  bool net_enabled = false;  // NSR_RB_NET: real peer input source
  // With NSR_RB_NET and no fixed NSR_RB_VTIME_FROM, the rollback image
  // locksteps menus and negotiates every InGame epoch automatically.
  bool auto_handoff = false;
  // Initial menus use cycle-identical poll lockstep.  Once rollback has run,
  // peers may have different VI-retrace histories, so subsequent menus use
  // the absolute-frame exchange with prediction disabled instead.
  bool menu_frame_lockstep = false;
  u32 net_delay = 1;         // local samples apply this many frames later
  u32 max_prediction = 7;    // host-stall beyond this remote-confirmed window
  bool time_sync = true;     // the frame-ahead peer yields up to one frame
  u64 net_stalls = 0;
  u64 sync_waits = 0;
  u32 force_from = 0;     // NSR_RB_FORCE_FROM: first live tick that is rolled back
  u32 force_to = 0xFFFFFFFF;  // NSR_RB_FORCE_TO: last one (diagnostics)
  u32 perturb_at = 0;     // NSR_RB_PERTURB_AT: diagnostics, see RollbackTickHook
  u32 vtime_from = 0;     // NSR_RB_VTIME_FROM (0 = off)
  bool in_unit = false;   // inside a rollback unit: a tick (tick mode) or a frame
  bool frame_mode = false;  // the image has the frame routine (rollback unit = frame)
  bool frame_tick_done = false;  // frame mode: this frame's one tick was handed out
  bool io_idle_mode = false;  // NSR_RB_IO_IDLE: roll back only to I/O-idle snapshots
  bool audio_rollback = false;  // NSR_RB_AUDIO_ROLLBACK (see AUDIO_KEEP)
  std::map<u32, bool> io_idle;  // unit -> disc system idle at its start (live)
  u64 skipped_busy = 0;       // forced rollbacks skipped: disc busy now / no idle target
  u32 barrier = 0;          // no rollback may restore a snapshot older than this unit
  u64 barrier_events = 0, barrier_resim = 0, clamped = 0;
  bool in_resim = false;  // the running tick is a re-simulation
  // Audio gate: per tick, the gated calls in order with their results.
  struct GateCall
  {
    u32 entry = 0, r3 = 0, r4 = 0;
    u64 f1 = 0;
    bool done = false;
  };
  std::map<u32, std::vector<GateCall>> gate_log;
  struct GatePending
  {
    u32 lr, tick;
    size_t index;
  };
  std::vector<GatePending> gate_pending;
  size_t gate_index = 0;  // gated calls so far in the running re-simulated tick
  u64 gate_replayed = 0, gate_mismatched = 0, gate_recorded = 0;
  u32 tick_running = 0;   // index of the tick currently running
  u64 vtime_base = 0;     // real time base at live tick vtime_from
  bool vtime_on = false;
  u32 vtime_calls = 0;    // time reads so far in this tick
  u64 disc_commands = 0;       // DVD commands executed (NoteDiscCommand)
  u64 disc_commands_seen = 0;  // ... as of the previous unit start
  u32 watch = 0, watch_from = 0, watch_to = 0;  // NSR_RB_WATCH=addr:from:to (diagnostics)
  std::vector<u8> prev;   // state before the previous live tick (perturb mode)
  bool hash_log = false;  // NSR_RB_HASH_LOG
  std::vector<u32> dump_at;
  std::vector<std::pair<u32, u32>> segments;  // RAM ranges restored/compared
  // Dolphin's fake VMEM (0x7E000000.., GameCube without MMU emulation): GN7E69
  // keeps its EA database heap there (base 0x7E000000, 0x104000 bytes, set up
  // at 0x80024778): tables, indexes, cursors, query bindings and the heap's own
  // block headers. The sim queries and modifies it every frame (team/playbook
  // selects, depth-chart delete+insert), and MEM1 holds pointers into it, so
  // it is game state: snapshots carry its first `vmem_bytes` after MEM1
  // (rb-div-4594: leaving it live made a re-simulated frame's DB select
  // return another row, fn_8016BAD8 skipped its RNG draw, and k=2/k=4
  // forced rollbacks diverged).
  u8* vmem = nullptr;
  u32 vmem_bytes = 0;  // NSR_RB_VMEM_BYTES (default 0x104000; 0 = off)
  u32 mem1_size = 0;   // snapshot offset of the VMEM copy

  Pads latest{};  // most recent processed local sample (from the poll)
  u32 ticks_since_poll = 0;  // tick-mode detection (see RollbackPollHook)
  u32 idle_polls = 0;
  bool tick_mode = false;
  FILE* record = nullptr;          // NSR_RB_RECORD: live tick pads out
  std::map<u32, Pads> replay;      // NSR_RB_REPLAY: live tick pads in

  u32 live = 0;    // index of the next live tick
  u32 cursor = 0;  // index of the tick about to run (< live while re-simulating)
  bool rolled = false;
  bool session_ended = false;
  bool session_end_consumed = false;
  bool enter_pending = false;
  bool reenter_ready = false;
  bool in_game_seen = false;
  bool session_saw_in_game = false;
  u32 session_generation = 0;
  std::map<u32, Pads> pads;              // tick -> pads it ran with
  std::map<u32, std::array<u32, 4>> counters;  // tick -> TICK_COUNTERS values before it
  std::vector<std::vector<u8>> ring;     // ring[t % (force + 1)] = state before tick t

  u64 checks = 0, mismatched_checks = 0, rollbacks = 0, resim_ticks = 0;
  std::map<u32, u32> granule_hits;  // granule index -> checks it differed in
  u32 logged_mismatches = 0;

  // NSR_RB_PROFILE: per-granule write counts inside / outside the sim.
  std::string profile_path;
  u32 pgran = RB_GRANULE;  // NSR_RB_GRANULE: profile resolution in bytes (4..4096)
  std::vector<u8> at_start, at_end;  // RAM at the last tick start / tick end
  bool have_end = false;
  std::vector<u32> inside, outside;

  bool memwatch_armed = false;
  u32 probe_last_value = 0, probe_last_pc = 0, probe_last_lr = 0;
  FILE* ac_log = nullptr;
  FILE* stacks = nullptr;
};

Rollback s_rb;

// A rollback epoch and its following frame-lockstep transition are one
// deterministic lifecycle.  The transition does not snapshot or predict,
// but it must keep the frame-derived clocks that asynchronous DVD/audio work
// observes until the next epoch takes ownership.  Initial boot menus remain
// on Dolphin's natural clocks because menu_frame_lockstep is armed only by a
// completed rollback session.
bool FrameDrivenLifecycleActive()
{
  return s_rb.vtime_on || s_rb.menu_frame_lockstep;
}

// Follow input is CPU-thread-owned; only the capture-suppression flag crosses threads.
std::unique_ptr<ReplayFollow> s_follow;
std::atomic<bool> s_follow_catchup{false};
bool s_follow_failed = false;
unsigned s_follow_delay = 0;
float s_follow_speed = 1.0f;

// Disc-streaming destinations (GN7E69): the game's disc worker (descriptor at
// 0x803D55E0: destination +0x10, bytes +0x18) streams audio (EA audio driver,
// commentary) into heap buffers during play. That data and its completion
// follow real time like the rest of audio, so every destination seen is kept
// live for the rest of the session. Sampled at every poll and tick, from the
// start of the rollback session (NSR_RB_VTIME_FROM) on.
std::map<u32, u32> s_io_ranges;  // physical start -> end

void RbSampleIo(const Core::CPUThreadGuard& guard)
{
  // Only loads during the rollback session (in-match streaming). Loading
  // screens fill gameplay memory (models, animations) through the same
  // worker; those destinations are game state and must be restored.
  if (!s_rb.vtime_on || s_rb.io_idle_mode)
    return;
  const u32 dest = PowerPC::MMU::HostRead<u32>(guard, 0x803D55F0);
  const u32 bytes = PowerPC::MMU::HostRead<u32>(guard, 0x803D55F8);
  if ((dest >> 24) != 0x80 || bytes == 0 || bytes > 0x100000)
    return;
  const u32 lo = dest & 0x01FFFFFF, hi = lo + ((bytes + 31) & ~31u);
  auto [it, inserted] = s_io_ranges.try_emplace(lo, hi);
  if (!inserted)
    it->second = std::max(it->second, hi);
}
std::atomic<s64> s_rb_last_tick_ms{0};  // host time of the last tick hook (watchdog)
std::atomic<u32> s_rb_watch_live{0};
std::atomic<u64> s_rb_watch_rollbacks{0};
std::jthread s_rb_watchdog;

void EnterRollbackSession(const Core::CPUThreadGuard& guard, u32 frame);
void LeaveRollbackSession(const Core::CPUThreadGuard& guard, const char* reason);
// NSR telemetry glue: defined in the block at the end of this file.
void TelemetryUnitStart(std::chrono::steady_clock::time_point step_start);
void TelemetryUnitEnd(const Core::CPUThreadGuard& guard);
void TelemetrySessionEnter();
void TelemetrySessionLeave(const char* reason);
void TelemetryShutdown();

std::optional<u64> ParseU64(const std::string& text)
{
  if (text.empty())
    return std::nullopt;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text.c_str(), &end, 0);
  if (end == text.c_str() || *end != '\0')
    return std::nullopt;
  return v;
}

std::optional<u32> ParseU32(const std::string& text)
{
  if (text.empty())
    return std::nullopt;
  char* end = nullptr;
  const unsigned long v = std::strtoul(text.c_str(), &end, 0);
  if (end == text.c_str() || *end != '\0')
    return std::nullopt;
  return static_cast<u32>(v);
}

std::vector<u32> ParseAddressList(const std::string& text)
{
  std::vector<u32> out;
  for (const std::string& item : Split(text, ','))
  {
    if (const auto value = ParseU32(item))
      out.push_back(*value);
  }
  return out;
}

RollbackConfig& ConfigForRollback()
{
  Rollback& r = s_rb;
  if (r.config_loaded)
    return r.config;

  RollbackConfig& c = r.config;
  c.nsr_enabled = std::getenv("NSR_NETPLAY") != nullptr || std::getenv("NSR_SEED") != nullptr;
  c.netplay_enabled = std::getenv("NSR_NETPLAY") != nullptr;
  c.seed = ParseU32(Env("NSR_SEED"));
  c.instant_disc = !Env("NSR_RB_INSTANT_DISC").empty();
  c.sample = !Env("NSR_RB_SAMPLE").empty();
  c.stacks_path = Env("NSR_RB_STACKS");
  c.transplant = Split(Env("NSR_RB_TRANSPLANT"), ':');
  c.dump_units = Split(Env("NSR_RB_DUMP_UNIT"), ',');
  c.adopt = Split(Env("NSR_RB_ADOPT"), ':');
  c.force_random_seed = ParseU32(Env("NSR_RB_FORCE_RANDOM"));
  c.disc_trace = !Env("NSR_RB_DISC_TRACE").empty();
  c.io_trace = !Env("NSR_RB_IO_TRACE").empty();
  c.quiet_trace = !Env("NSR_RB_QUIET_TRACE").empty();
  c.async_present = !Env("NSR_RB_ASYNC_PRESENT").empty();
  c.peek_z = ParseU32(Env("NSR_RB_PEEKZ")).value_or(0);
  c.keep_resim_vsync = !Env("NSR_RB_RESIM_VSYNC").empty();
  const std::string audio_gate = Env("NSR_RB_AUDIO_GATE", "1");
  if (audio_gate == "1")
    c.audio_gate_only = {0x8026AAF0};
  else if (audio_gate != "0")
    c.audio_gate_only = ParseAddressList(audio_gate);
  c.effect_gates = ParseAddressList(Env("NSR_RB_EFFECT_GATE"));
  c.audio_trace = ParseAddressList(Env("NSR_AC_TRACE"));
  c.probes = ParseAddressList(Env("NSR_RB_PROBES"));
  c.skip_resim = ParseAddressList(Env("NSR_RB_SKIP_RESIM"));
  c.probe_all = !Env("NSR_RB_PROBE_ALL").empty();
  c.session_end = ParseU32(Env("NSR_RB_SESSION_END")).value_or(0);
  c.auto_reenter = !Env("NSR_RB_AUTO_REENTER").empty();
  const std::string ac_mode = Env("NSR_AC", "1");
  c.ac_mode = ac_mode == "0" ? AcMode::Off : ac_mode == "shadow" ? AcMode::Shadow : AcMode::On;
  c.ac_log_path = Env("NSR_AC_LOG");
  for (const std::string& item : Split(Env("NSR_AC_HASH"), ','))
  {
    const auto parts = Split(item, '-');
    if (parts.size() == 2)
    {
      const auto lo = ParseU32(parts[0]);
      const auto hi = ParseU32(parts[1]);
      if (lo && hi && *hi > *lo)
        c.ac_hash_ranges.emplace_back(*lo, *hi);
    }
  }
  c.ac_speech_frames = std::max<u32>(1, ParseU32(Env("NSR_AC_SPEECH_FRAMES")).value_or(75));
  c.ac_speech_lead = ParseU32(Env("NSR_AC_SPEECH_LEAD")).value_or(0);
  c.ac_sfx_default = ParseU32(Env("NSR_AC_SFX_DEFAULT")).value_or(30);
  c.ac_sfx_tail = ParseU32(Env("NSR_AC_SFX_TAIL")).value_or(2);
  c.ac_bank_frames = ParseU32(Env("NSR_AC_BANK_FRAMES")).value_or(6);
  c.ac_polls_per_tick =
      std::max<u32>(1, ParseU32(Env("NSR_AC_POLLS_PER_TICK")).value_or(1000));
  c.ac_song_frames = std::max<u32>(1, ParseU32(Env("NSR_AC_SONG_FRAMES")).value_or(10800));
  c.ac_song_gap = ParseU32(Env("NSR_AC_SONG_GAP")).value_or(380);
  c.digest_semantic_only = Env("NSR_RB_DIGEST") == "semantic";
  c.digest_every = std::clamp<u32>(ParseU32(Env("NSR_RB_DIGEST_EVERY")).value_or(1), 1, 60);
  if (const auto parts = Split(Env("NSR_RB_TEST_CORRUPT"), ':'); parts.size() == 2)
  {
    const auto frame = ParseU32(parts[0]);
    const auto address = ParseU32(parts[1]);
    if (frame && address)
      c.test_corrupt = std::pair<u32, u32>{*frame, *address};
  }
  r.config_loaded = true;
  return c;
}

bool IsIoIdle(const u8* ram);  // defined with the restore helpers

// ---------------------------------------------------------------------------
// Snapshot page work on worker threads (NSR_RB_THREADS = threads used for it,
// counting the CPU thread; default: a quarter of the host's hardware threads,
// 1..4; 1 = everything on the CPU thread as before).
//
// Snapshot saves, restores and the portable dirty tracker's page hashing are
// loops over ~6,400 independent 4 KiB pages; one host core moves only ~6-10
// GB/s, so splitting the loop over a few cores makes them memory-bandwidth
// bound instead. Run() hands out fixed chunks of the index range from an
// atomic counter; the calling (CPU) thread works too and returns once every
// chunk is done, so the caller sees the loop as synchronous.
//
// Design notes:
//   - Workers sleep on a condition variable between loops; the caller starts
//     on the first chunk immediately, so a slow wake-up costs parallelism, not
//     latency.
//   - A worker joins a loop only while chunks are left, and a new loop starts
//     only when no worker is still inside the previous one (m_active == 0), so
//     a late worker can't run the previous loop's body over the next loop's
//     indices.
//   - Deferred: pinning workers to cores / sizing by physical cores. Revisit if
//     the per-loop wake-up shows in profiles on hosts with few cores.
class PageWorkers
{
public:
  ~PageWorkers() { Stop(); }

  void Start(u32 total_threads)
  {
    Stop();
    for (u32 i = 1; i < total_threads; ++i)
    {
      m_threads.emplace_back([this] {
        Common::SetCurrentThreadName("NSR snapshot worker");
        Loop();
      });
    }
  }

  void Stop()
  {
    {
      std::lock_guard lk(m_mutex);
      m_stop = true;
    }
    m_work_cv.notify_all();
    for (std::thread& t : m_threads)
      t.join();
    m_threads.clear();
    m_stop = false;
  }

  u32 Threads() const { return static_cast<u32>(m_threads.size()) + 1; }

  // fn(begin, end) over [0, n) in chunks of `chunk` indices; fn must be safe
  // to run concurrently on disjoint ranges.
  template <typename F>
  void Run(u32 n, u32 chunk, F&& fn)
  {
    using Fn = std::remove_reference_t<F>;
    const u32 chunks = (n + chunk - 1) / chunk;
    if (m_threads.empty() || chunks < 2)
    {
      if (n)
        fn(0u, n);
      return;
    }
    {
      std::unique_lock lk(m_mutex);
      m_idle_cv.wait(lk, [this] { return m_active == 0; });
      m_call = [](void* ctx, u32 a, u32 b) { (*static_cast<Fn*>(ctx))(a, b); };
      m_ctx = const_cast<void*>(static_cast<const void*>(&fn));
      m_n = n;
      m_chunk = chunk;
      m_chunks = chunks;
      m_next.store(0);
      m_done.store(0);
      ++m_gen;
    }
    m_work_cv.notify_all();
    Work();
    while (m_done.load(std::memory_order_acquire) < chunks)
      std::this_thread::yield();
  }

private:
  void Work()
  {
    for (;;)
    {
      const u32 c = m_next.fetch_add(1);
      if (c >= m_chunks)
        return;
      const u32 a = c * m_chunk;
      m_call(m_ctx, a, std::min(m_n, a + m_chunk));
      m_done.fetch_add(1, std::memory_order_release);
    }
  }

  void Loop()
  {
    u64 seen = 0;
    std::unique_lock lk(m_mutex);
    for (;;)
    {
      m_work_cv.wait(lk, [&] { return m_stop || m_gen != seen; });
      if (m_stop)
        return;
      seen = m_gen;
      if (m_next.load() >= m_chunks)
        continue;  // the caller (or others) already took every chunk
      ++m_active;
      lk.unlock();
      Work();
      lk.lock();
      if (--m_active == 0)
        m_idle_cv.notify_all();
    }
  }

  std::vector<std::thread> m_threads;
  std::mutex m_mutex;
  std::condition_variable m_work_cv, m_idle_cv;
  bool m_stop = false;
  u64 m_gen = 0;
  u32 m_active = 0;
  // The current loop; written under m_mutex while no worker is inside one.
  void (*m_call)(void*, u32, u32) = nullptr;
  void* m_ctx = nullptr;
  u32 m_n = 0, m_chunk = 1, m_chunks = 0;
  std::atomic<u32> m_next{0}, m_done{0};
};
PageWorkers s_workers;
constexpr u32 PAGE_CHUNK = 32;  // pages per work item (128 KiB)

// ---------------------------------------------------------------------------
// Dirty-page tracking for snapshots (NSR_RB_DIRTY):
//   1 / unset  kernel tracker on Linux; protect tracker on Windows/macOS;
//   kernel     kernel tracker only (else full copies);
//   protect    portable first-write protection, with hashing as a safe fallback;
//   hash       page hashing (portable; forced, e.g. to test it on Linux);
//   0          full copies as before.
//
// A full snapshot is ~26 MB, but a frame writes only ~0.5-2 MB of it.
//
// Kernel tracker (Linux 6.7+). The kernel tracks which pages of emulated memory were written: every host
// mapping of MEM1 and fake VMEM (emulated memory is one shared-memory object
// mapped at several host addresses: the RAM pointer, the physical and the
// BAT/logical fastmem views; each has its own page-table entries) is
// registered with a userfaultfd in asynchronous write-protect mode
// (UFFD_FEATURE_WP_ASYNC, Linux 6.7+; UFFD_USER_MODE_ONLY, so no privilege
// is needed). A write to a protected page is resolved inside the kernel
// (~1 us, once per page between samples) and marks the page written;
// PAGEMAP_SCAN with PM_SCAN_WP_MATCHING returns the written pages of a range
// and protects them again in one call (~50 us for all of game memory). This
// sees every writer in the process: the JIT's fastmem stores, Dolphin's
// HostWrite/DMA copies, kernel copies into the buffer, and our restores.
//
// Alternatives measured and rejected: soft-dirty bits (clear_refs) clear the
// whole process's page tables, ~7.5 ms per sample with the snapshot ring.
// Portable protection is safe only because MemTools gives this tracker first
// refusal on faults, before the JIT can mistake them for MMIO and backpatch a
// store site permanently.
//
// Bookkeeping: Sample() stamps every page written since the previous sample
// with a new epoch number. A snapshot slot remembers the epoch at which it
// equalled RAM (`synced`). Pages stamped later are the only ones that can
// differ between that slot and RAM now, so:
//   - a save copies only those pages (and marks the slot synced now);
//   - a restore compares/copies only those pages;
//   - the re-simulation check compares only those pages.
// Every save/restore/check samples first, so writes made anywhere (game,
// hardware DMA, HLE hooks) between two operations are always seen.
// Slots without a recorded epoch (new, or not ring slots) get full copies.
// NSR_RB_DIRTY_VERIFY=1 checks every incremental save and restore against a
// full comparison and logs any page the tracker missed.
//
// Portable protection (Windows/macOS and explicitly selectable on Linux).
// Every writable alias of MEM1/fake VMEM is made read-only after a sample.
// The first writer faults, marks the state page dirty, and restores write
// access to that alias. This covers JIT fastmem, MMU slowmem, DMA, HLE copies,
// fake VMEM, and NSR's own restore/model writes without per-writer hooks.
// Logical BAT remaps rebuild the view list and conservatively dirty all pages.
// A startup write/restore self-test must observe its own fault; any failure
// removes all protection and selects hashing, never an unchecked tracker.
// NSR_RB_DIRTY_AUDIT=1 hashes every tracked page at every sample and repairs
// any page whose content changed without a protection fault.
//
// Page hashing (portable fallback). Sample() hashes every page that holds
// rolled-back bytes (XXH3-128, on the PageWorkers) and stamps pages whose hash differs from the
// previous sample's. Only the sample changes; the epoch bookkeeping above is
// shared, so a save/restore still touches only the stamped pages. Hashing
// reads all game memory once per sample (no writes, no compare against a
// second copy), against a full copy's read + write of all of it: ~1.5-2.5 ms
// per sample vs ~5 ms per full save/restore on a Ryzen 3700X, where both are
// memory-bandwidth bound. It sees every writer in the process by
// construction (it looks at content, not at mappings, so BAT remaps don't
// matter). Deferred: skipping the sample of the save that directly follows a
// restore (only our own writes in between) would save one sample per
// rollback, but needs every writer in that gap accounted for; revisit if the
// fallback's rollback frames miss the frame budget on players' machines.
//
// Correctness under hash collisions: a page whose bytes changed but whose
// hash didn't would be left out of the copy, silently. The hash is 128-bit
// with a per-session random key (XXH3 secret from the seed), so for a changed
// page the chance is ~2^-128 per comparison: at most ~5e8 changed-page
// comparisons per hour of play (~250 changed pages per sample) gives ~1e-30
// per hour, far below DRAM soft-error rates. Two more guards, so a miss
// can't stay silent:
//   - each hashed save also compares NSR_RB_HASH_AUDIT pages (default 16,
//     rotating over all pages, ~10 us) of the slot with RAM byte for byte; a
//     difference is logged, repaired in the slot and stamped (so every other
//     slot treats the page as changed);
//   - netplay's periodic whole-MEM1 hash exchange catches any divergence
//     between peers, whatever caused it.
// Rejected: 64-bit hashes (same speed for 4 KiB inputs, collision chance
// ~1e-11 per hour; no reason to take it), comparing against a shadow copy
// (reads 2x the bytes), hashing only on save with per-slot hash arrays (same
// cost, 24x the memory).
struct DirtyTracker
{
  static constexpr u32 PAGE = 0x1000;
  // Opening one page per exception made the protection boundary itself cost
  // several milliseconds on write-heavy frames. Conservatively dirtying a
  // 32 KiB group amortizes signal/VEH and protection syscalls; false-positive
  // pages only cause an extra snapshot copy and cannot hide a write.
  static constexpr u32 PROTECT_GROUP_PAGES = 8;
  enum class Mode
  {
    Off,      // no tracking: full copies (possibly on worker threads)
    Kernel,   // userfaultfd + PAGEMAP_SCAN
    Protect,  // VirtualProtect/mprotect + MemTools first-fault callback
    Hash,     // page hashing
  };
  Mode mode = Mode::Off;
  bool on = false;     // stamps are valid (Kernel or Hash)
  bool paged = false;  // chunk tables built (per-page save/restore loops usable)
  bool verify = false;
  bool exhaustive_audit = false;
  bool protect_fallback = false;
  // Hash mode.
  u8* ram = nullptr;
  u8* vmem = nullptr;
  u32 vmem_bytes = 0;
  std::vector<XXH128_hash_t> hashes;  // per state page: hash at the last sample
  std::vector<u32> hash_pages;        // state pages that hold rolled-back bytes
  alignas(64) std::array<u8, XXH3_SECRET_DEFAULT_SIZE> secret{};
  std::optional<u64> shared_key;  // set by Rekey: the agreed seed of `secret`
  u32 audit_pages = 0, audit_next = 0;
  u64 audit_checked = 0, audit_mismatches = 0;
  int uffd = -1;
  int pagemap_fd = -1;
  u32 mem1_bytes = 0;
  u32 mem1_pages = 0;   // state-space pages [0, mem1_pages) = MEM1
  u32 vmem_pages = 0;   // then fake VMEM (the snapshotted prefix)
  struct View
  {
    uintptr_t host;   // host address of state page `first`
    u32 first, count; // state-space page range this mapping covers
  };
  std::vector<View> views;
  std::vector<u32> stamp;     // per state page: epoch of its last observed write
  u32 epoch = 1;
#if NSR_HAVE_UFFD
  std::vector<page_region> regions;
#endif
  std::map<const u8*, u32> synced;  // slot data -> epoch it equalled RAM
  // Segment pieces per MEM1 page: chunks[page_first[p] .. page_first[p+1]).
  std::vector<std::pair<u32, u32>> chunks;
  std::vector<u32> page_first;
  u64 samples = 0, sample_ns = 0, pages_seen = 0, failures = 0, remaps = 0;
  u64 pages_copied = 0;  // NSR telemetry: pages RbSave copied (dirty or never synced)
  u32 seen_remaps = 0;
  Memory::MemoryManager* memory = nullptr;
  std::unique_ptr<std::atomic<u8>[]> protect_dirty;
  std::vector<u8> protect_observed;
  std::atomic<bool> protect_fault_failed{false};
  std::atomic<u64> protect_faults{0};
  u64 protect_audit_checked = 0, protect_audit_misses = 0;
  inline static DirtyTracker* active_protect_tracker = nullptr;

  // All host mappings of the tracked shared memory, from /proc/self/maps.
  std::vector<View> ParseViews() const
  {
#if NSR_HAVE_UFFD
    std::vector<View> out;
    // Shared-memory layout (Memmap.cpp): MEM1 at 0 (power-of-two size),
    // then the L1 cache (256 KiB), then fake VMEM.
    const u64 ram_pow2 = u64{1} << (64 - __builtin_clzll(u64{mem1_bytes} - 1));
    const u64 vmem_shm = ram_pow2 + 0x40000;
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps)
      return out;
    const std::string tag = fmt::format("/dolphin-emu.{}", getpid());
    char line[512];
    while (std::fgets(line, sizeof(line), maps))
    {
      unsigned long lo, hi, off;
      char perms[8];
      if (std::sscanf(line, "%lx-%lx %7s %lx", &lo, &hi, perms, &off) != 4 ||
          !std::strstr(line, tag.c_str()))
        continue;
      auto add = [&](u64 base, u32 pages, u32 first_state) {
        // [off, off + hi - lo) of the shm file is mapped at lo; intersect with
        // [base, base + pages * PAGE) and record it in state-space pages.
        const u64 a = std::max<u64>(off, base);
        const u64 b = std::min<u64>(off + (hi - lo), base + u64{pages} * PAGE);
        if (a >= b)
          return;
        out.push_back({static_cast<uintptr_t>(lo + (a - off)),
                       static_cast<u32>(first_state + (a - base) / PAGE),
                       static_cast<u32>((b - a + PAGE - 1) / PAGE)});
      };
      add(0, mem1_pages, 0);
      if (vmem_pages)
        add(vmem_shm, vmem_pages, mem1_pages);
    }
    std::fclose(maps);
    return out;
#else
    return {};
#endif
  }

  bool Register(const View& v) const
  {
#if NSR_HAVE_UFFD
    uffdio_register reg{};
    reg.range.start = v.host;
    reg.range.len = u64{v.count} * PAGE;
    reg.mode = UFFDIO_REGISTER_MODE_WP;
    if (ioctl(uffd, UFFDIO_REGISTER, &reg) == 0)
      return true;
    Log("snapshot dirty tracking: can't register mapping {:x}+{:#x}: {}", v.host, reg.range.len,
        std::strerror(errno));
    return errno == EBUSY;
#else
    (void)v;
    return false;
#endif
  }

  // Dolphin remaps all logical views whenever the game writes a BAT (the new
  // mappings lose their registration); Memory::g_nsr_logical_remaps reports
  // that, and the views are also re-read every 600 frames as a safety net.
  // After a change, register the mappings again, and since writes through
  // them may have gone unseen, every page counts as written.
  void RefreshViews(bool remapped = false)
  {
    if (mode != Mode::Kernel)
      return;  // hashing reads content through the RAM pointer: no mappings
    auto now = ParseViews();
    const bool same = !remapped && now.size() == views.size() &&
                      std::equal(now.begin(), now.end(), views.begin(), [](const View& x, const View& y) {
                        return x.host == y.host && x.first == y.first && x.count == y.count;
                      });
    if (same)
      return;
    if (!remapped)
      Log("snapshot dirty tracking: game memory mappings changed ({} -> {})", views.size(), now.size());
    views = std::move(now);
    for (const View& v : views)
      Register(v);
    Sample();
    std::fill(stamp.begin(), stamp.end(), epoch);
  }

  // Page geometry and the per-page segment pieces; used by every mode (the
  // per-page save/restore loops also run full copies on worker threads).
  void Prepare(const std::vector<std::pair<u32, u32>>& segments, u32 mem1_size, u32 vmem_size)
  {
    mem1_bytes = mem1_size;
    mem1_pages = (mem1_size + PAGE - 1) / PAGE;
    vmem_bytes = vmem_size;
    vmem_pages = (vmem_size + PAGE - 1) / PAGE;
    stamp.assign(mem1_pages + vmem_pages, epoch);  // nothing synced yet
    chunks.clear();
    page_first.assign(mem1_pages + 1, 0);
    for (u32 p = 0; p < mem1_pages; ++p)
    {
      page_first[p] = static_cast<u32>(chunks.size());
      const u32 plo = p * PAGE, phi = plo + PAGE;
      for (const auto& [lo, hi] : segments)
      {
        const u32 x = std::max(lo, plo), y = std::min(hi, phi);
        if (x < y)
          chunks.emplace_back(x, y);
      }
    }
    page_first[mem1_pages] = static_cast<u32>(chunks.size());
    verify = !Env("NSR_RB_DIRTY_VERIFY").empty();
    exhaustive_audit = !Env("NSR_RB_DIRTY_AUDIT").empty();
    paged = true;
  }

  bool IsTrackedPage(u32 page) const
  {
    return page >= mem1_pages || page_first[page] != page_first[page + 1];
  }

  // Build state-space views from Dolphin's authoritative alias list. Each
  // resulting view maps page `first` to host and is clipped to the bytes this
  // rollback session snapshots.
  void BuildPortableViews()
  {
    views.clear();
    if (!memory)
      return;
    for (const Memory::NSRMemoryView& alias : memory->GetNSRMemoryViews())
    {
      auto add = [&](u32 physical_base, u32 bytes, u32 first_state) {
        const u64 alias_lo = alias.physical_address;
        const u64 alias_hi = alias_lo + alias.size;
        const u64 wanted_lo = physical_base;
        const u64 wanted_hi = wanted_lo + bytes;
        const u64 lo = std::max(alias_lo, wanted_lo);
        const u64 hi = std::min(alias_hi, wanted_hi);
        if (lo >= hi)
          return;
        views.push_back({reinterpret_cast<uintptr_t>(alias.host) +
                             static_cast<uintptr_t>(lo - alias_lo),
                         first_state + static_cast<u32>((lo - wanted_lo) / PAGE),
                         static_cast<u32>((hi - lo + PAGE - 1) / PAGE)});
      };
      add(0, mem1_bytes, 0);
      if (vmem_bytes)
        add(0x7E000000, vmem_bytes, mem1_pages);
    }
  }

  static bool SetPageProtection(uintptr_t host, u32 count, bool read_only)
  {
    const size_t bytes = static_cast<size_t>(count) * PAGE;
#if defined(_WIN32)
    DWORD old_protection = 0;
    return VirtualProtect(reinterpret_cast<void*>(host), bytes,
                          read_only ? PAGE_READONLY : PAGE_READWRITE, &old_protection) != 0;
#else
    return mprotect(reinterpret_cast<void*>(host), bytes,
                    read_only ? PROT_READ : PROT_READ | PROT_WRITE) == 0;
#endif
  }

  // Change protection in maximal runs so a sample costs syscalls per dirty
  // run, not per page. `dirty_only` is used after the first full protection.
  bool SetPortableProtection(bool read_only, bool dirty_only)
  {
    bool ok = true;
    for (const View& view : views)
    {
      u32 run = 0;
      for (u32 q = 0; q <= view.count; ++q)
      {
        const u32 page = view.first + q;
        const bool in_fifo = page < mem1_pages && page >= fifo_pages.first &&
                             page < fifo_pages.second;
        const bool selected = q < view.count && IsTrackedPage(page) && !(read_only && in_fifo) &&
                              (!dirty_only || protect_dirty[page].load(std::memory_order_relaxed));
        if (selected && run == 0)
          run = q + 1;
        if (!selected && run != 0)
        {
          const u32 first = run - 1;
          if (!SetPageProtection(view.host + static_cast<uintptr_t>(first) * PAGE, q - first,
                                 read_only))
            ok = false;
          run = 0;
        }
      }
    }
    return ok;
  }

  bool SetStateRangeProtection(u32 first, u32 end, bool read_only)
  {
    bool ok = true;
    for (const View& view : views)
    {
      const u32 lo = std::max(first, view.first);
      const u32 hi = std::min(end, view.first + view.count);
      if (lo < hi &&
          !SetPageProtection(view.host + static_cast<uintptr_t>(lo - view.first) * PAGE, hi - lo,
                             read_only))
        ok = false;
    }
    return ok;
  }

  static bool PortableFaultThunk(uintptr_t fault_address)
  {
    return active_protect_tracker && active_protect_tracker->HandlePortableFault(fault_address);
  }

  // Exception-context first-write handler. Only atomics and one platform page
  // protection call are allowed here; logging/fallback happens at Sample().
  bool HandlePortableFault(uintptr_t fault_address)
  {
    if (mode != Mode::Protect || !protect_dirty)
      return false;
    for (const View& view : views)
    {
      const u64 end = static_cast<u64>(view.host) + static_cast<u64>(view.count) * PAGE;
      if (fault_address < view.host || static_cast<u64>(fault_address) >= end)
        continue;
      const u32 q = static_cast<u32>((fault_address - view.host) / PAGE);
      const u32 page = view.first + q;
      if (!IsTrackedPage(page))
        return false;
      const u32 group_first = q / PROTECT_GROUP_PAGES * PROTECT_GROUP_PAGES;
      const u32 group_count = std::min(PROTECT_GROUP_PAGES, view.count - group_first);
      for (u32 i = 0; i < group_count; ++i)
      {
        const u32 group_page = view.first + group_first + i;
        if (IsTrackedPage(group_page))
          protect_dirty[group_page].store(1, std::memory_order_release);
      }
      if (!SetPageProtection(view.host + static_cast<uintptr_t>(group_first) * PAGE, group_count,
                             false))
      {
        protect_fault_failed.store(true, std::memory_order_release);
        return false;
      }
      protect_faults.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  void PrepareHashPages()
  {
    hash_pages.clear();
    for (u32 p = 0; p < mem1_pages; ++p)
    {
      if (IsTrackedPage(p))
        hash_pages.push_back(p);
    }
    for (u32 q = 0; q < vmem_pages; ++q)
      hash_pages.push_back(mem1_pages + q);
    std::random_device rd;
    const u64 seed = (u64{rd()} << 32) ^ rd();
    XXH3_generateSecret_fromSeed(secret.data(), seed);
    shared_key.reset();
    hashes.assign(mem1_pages + vmem_pages, XXH128_hash_t{});
  }

  void HashBaseline()
  {
    s_workers.Run(static_cast<u32>(hash_pages.size()), PAGE_CHUNK, [&](u32 a, u32 b) {
      for (u32 i = a; i < b; ++i)
      {
        const u32 page = hash_pages[i];
        if (page < mem1_pages && page >= fifo_pages.first && page < fifo_pages.second)
          continue;
        const auto [bytes, len] = PageBytes(page);
        hashes[page] = XXH3_128bits_withSecret(bytes, len, secret.data(), secret.size());
      }
    });
  }

  // Protection supplies the changed-page set, so maintain the digest's
  // keyed page hashes without scanning clean memory. This is the Windows
  // equivalent of the kernel tracker's incremental digest cache: the first
  // epoch hashes every page once, then samples hash only pages that faulted.
  void HashObservedPages(const std::vector<u8>& observed)
  {
    s_workers.Run(static_cast<u32>(hash_pages.size()), PAGE_CHUNK, [&](u32 a, u32 b) {
      for (u32 i = a; i < b; ++i)
      {
        const u32 page = hash_pages[i];
        if (!observed[page] ||
            (page < mem1_pages && page >= fifo_pages.first && page < fifo_pages.second))
          continue;
        const auto [bytes, len] = PageBytes(page);
        hashes[page] = XXH3_128bits_withSecret(bytes, len, secret.data(), secret.size());
      }
    });
  }

  // Audit protection against a complete content scan. A miss is repaired by
  // stamping it dirty, so audit mode remains deterministic while making the
  // tracker defect visible in logs and telemetry counters.
  u64 AuditPortable(const std::vector<u8>& observed, u32 now)
  {
    std::atomic<u64> checked{0};
    std::atomic<u64> missed{0};
    s_workers.Run(static_cast<u32>(hash_pages.size()), PAGE_CHUNK, [&](u32 a, u32 b) {
      u64 local_missed = 0;
      for (u32 i = a; i < b; ++i)
      {
        const u32 page = hash_pages[i];
        if (page < mem1_pages && page >= fifo_pages.first && page < fifo_pages.second)
          continue;
        const auto [bytes, len] = PageBytes(page);
        const XXH128_hash_t hash =
            XXH3_128bits_withSecret(bytes, len, secret.data(), secret.size());
        if (!XXH128_isEqual(hash, hashes[page]))
        {
          hashes[page] = hash;
          if (!observed[page])
          {
            stamp[page] = now;
            ++local_missed;
          }
        }
      }
      checked.fetch_add(b - a, std::memory_order_relaxed);
      missed.fetch_add(local_missed, std::memory_order_relaxed);
    });
    protect_audit_checked += checked.load();
    const u64 new_misses = missed.load();
    if (new_misses != 0 && protect_audit_misses < 20)
      Log("DIRTY PROTECT AUDIT: {} changed page(s) had no first-write fault", new_misses);
    protect_audit_misses += new_misses;
    return new_misses;
  }

  bool InitProtect(Memory::MemoryManager& memory_manager, u8* ram_base, u8* vmem_base)
  {
    memory = &memory_manager;
    ram = ram_base;
    vmem = vmem_base;
    if (!EMM::IsExceptionHandlerSupported() || !ram || (vmem_pages && !vmem))
      return false;
    BuildPortableViews();
    if (views.empty())
      return false;
    seen_remaps = Memory::g_nsr_logical_remaps;
    protect_dirty = std::make_unique<std::atomic<u8>[]>(mem1_pages + vmem_pages);
    protect_observed.assign(mem1_pages + vmem_pages, 0);
    for (u32 page = 0; page < mem1_pages + vmem_pages; ++page)
      protect_dirty[page].store(0, std::memory_order_relaxed);
    mode = Mode::Protect;
    active_protect_tracker = this;
    EMM::SetMemoryFaultHandler(&PortableFaultThunk);
    if (!SetPortableProtection(true, false))
      return ReleaseProtect();

    // Startup self-test: change and restore one tracked canonical byte. A
    // missing fault disables this mode before any incremental snapshot can
    // trust it (especially important for untested Windows host revisions).
    u32 page = 0;
    while (page < mem1_pages && !IsTrackedPage(page))
      ++page;
    if (page == mem1_pages && vmem_pages == 0)
      return ReleaseProtect();
    volatile u8* const byte =
        page < mem1_pages ? ram + static_cast<size_t>(page) * PAGE :
                            vmem + static_cast<size_t>(page - mem1_pages) * PAGE;
    const u8 saved = *byte;
    const u64 before = protect_faults.load(std::memory_order_relaxed);
    *byte = static_cast<u8>(saved ^ 1);
    *byte = saved;
    const bool self_test_ok = protect_dirty[page].load(std::memory_order_acquire) != 0 &&
                              protect_faults.load(std::memory_order_relaxed) > before &&
                              !protect_fault_failed.load(std::memory_order_acquire);
    if (!self_test_ok)
      return ReleaseProtect();
    if (!SetPortableProtection(true, true))
      return ReleaseProtect();
    for (u32 p = 0; p < mem1_pages + vmem_pages; ++p)
      protect_dirty[p].store(0, std::memory_order_relaxed);
    // Protection mode keeps the same keyed per-page hash cache that hash
    // mode exposes to StateDigest. Audit additionally needs a baseline before
    // the first rollback epoch assigns the agreed key.
    PrepareHashPages();
    if (exhaustive_audit)
      HashBaseline();
    std::fill(stamp.begin(), stamp.end(), epoch);
    pages_seen = 0;
    return true;
  }

  bool ReleaseProtect()
  {
    if (mode == Mode::Protect)
    {
      SetPortableProtection(false, false);
      EMM::SetMemoryFaultHandler(nullptr);
      active_protect_tracker = nullptr;
    }
    protect_dirty.reset();
    protect_observed.clear();
    views.clear();
    mode = Mode::Off;
    return false;
  }

  bool InitKernel()
  {
#if NSR_HAVE_UFFD
    seen_remaps = Memory::g_nsr_logical_remaps;
    views = ParseViews();
    if (views.empty())
      return false;
    uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
    if (uffd < 0)
      return ReleaseKernel();
    uffdio_api api{};
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
    pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (ioctl(uffd, UFFDIO_API, &api) != 0 || pagemap_fd < 0)
      return ReleaseKernel();
    for (const View& v : views)
    {
      if (!Register(v))
        return ReleaseKernel();
    }
    regions.resize(4096);
    mode = Mode::Kernel;
    if (!Sample())
    {
      mode = Mode::Off;
      return ReleaseKernel();
    }
    std::fill(stamp.begin(), stamp.end(), epoch);  // nothing synced yet
    return true;
#else
    return false;
#endif
  }

  // Undo a partial kernel set-up (old kernel, missing feature): closing the
  // userfaultfd drops its registrations. Returns false for the callers.
  bool ReleaseKernel()
  {
#if NSR_HAVE_UFFD
    if (uffd >= 0)
      close(uffd);
    if (pagemap_fd >= 0)
      close(pagemap_fd);
    uffd = pagemap_fd = -1;
    views.clear();
    regions.clear();
#endif
    return false;
  }

  bool InitHash(const u8* ram_base, const u8* vmem_base)
  {
    ram = const_cast<u8*>(ram_base);
    vmem = const_cast<u8*>(vmem_base);
    if (!ram || (vmem_pages && !vmem))
      return false;
    // Pages without rolled-back bytes (wholly inside keep ranges) are never
    // saved, restored or checked: don't hash them.
    PrepareHashPages();
    audit_pages = ParseU32(Env("NSR_RB_HASH_AUDIT")).value_or(16);
    mode = Mode::Hash;
    Sample();
    std::fill(stamp.begin(), stamp.end(), epoch);  // nothing synced yet
    pages_seen = 0;
    return true;
  }

  // Hash/protection mode, at a rollback epoch start: rekey with a secret every
  // peer derives from the agreed epoch, so the per-page hashes the snapshot
  // path computes are also the canonical state digest's page values for
  // whole restored pages (no second page hash on Windows). The
  // key stays unpredictable before the epoch exists and differs per epoch;
  // the collision argument above only needs the key not to be chosen by the
  // memory contents.  Every page counts as written once (synced slots were
  // just cleared by the session start anyway).
  void Rekey(u64 seed)
  {
    if (mode != Mode::Hash && mode != Mode::Protect)
      return;
    XXH3_generateSecret_fromSeed(secret.data(), seed);
    shared_key = seed;
    std::fill(hashes.begin(), hashes.end(), XXH128_hash_t{});
    if (mode == Mode::Hash)
    {
      Sample();
      std::fill(stamp.begin(), stamp.end(), epoch);
    }
    else
    {
      // Epoch entry clears the snapshot sync table, so this one full hash is
      // sufficient; later samples touch only first-write pages.
      HashBaseline();
    }
  }

  // Host bytes of state page p (MEM1, then the fake VMEM prefix).
  std::pair<const u8*, u32> PageBytes(u32 p) const
  {
    if (p < mem1_pages)
      return {ram + p * PAGE, std::min(PAGE, mem1_bytes - p * PAGE)};
    const u32 a = (p - mem1_pages) * PAGE;
    return {vmem + a, std::min(PAGE, vmem_bytes - a)};
  }

  // Hash mode's Sample(): stamp the pages whose content hash changed since
  // the previous sample. Workers write disjoint hashes[]/stamp[] entries.
  void SampleHash()
  {
    const u32 now = epoch;
    std::atomic<u64> changed{0};
    s_workers.Run(static_cast<u32>(hash_pages.size()), PAGE_CHUNK, [&](u32 a, u32 b) {
      u64 n = 0;
      for (u32 i = a; i < b; ++i)
      {
        const u32 p = hash_pages[i];
        if (p < mem1_pages && p >= fifo_pages.first && p < fifo_pages.second)
          continue;
        const auto [bytes, len] = PageBytes(p);
        const XXH128_hash_t h = XXH3_128bits_withSecret(bytes, len, secret.data(), secret.size());
        if (!XXH128_isEqual(h, hashes[p]))
        {
          hashes[p] = h;
          stamp[p] = now;
          ++n;
        }
      }
      changed.fetch_add(n, std::memory_order_relaxed);
    });
    pages_seen += changed.load();
  }

  // Hash mode: byte-compare a few pages of a just-saved slot with RAM. A
  // difference means a hash collision hid a change (or a tracking bug):
  // repair the slot, and stamp the page so other slots resync it too.
  void Audit(std::vector<u8>& slot, std::pair<u32, u32> fifo)
  {
    if (mode != Mode::Hash || !audit_pages || hash_pages.empty())
      return;
    for (u32 i = 0; i < audit_pages; ++i)
    {
      const u32 p = *RollbackLogic::NextRotatingAuditPage(hash_pages, &audit_next);
      if (p < mem1_pages && p >= fifo.first && p < fifo.second)
        continue;  // GX FIFO: left stale on purpose (a live range)
      ++audit_checked;
      bool differs = false;
      if (p < mem1_pages)
      {
        for (u32 c = page_first[p]; c < page_first[p + 1]; ++c)
        {
          const auto [a, b] = chunks[c];
          if (std::memcmp(slot.data() + a, ram + a, b - a) != 0)
          {
            std::memcpy(slot.data() + a, ram + a, b - a);
            differs = true;
          }
        }
      }
      else
      {
        const auto [bytes, len] = PageBytes(p);
        u8* dst = slot.data() + mem1_bytes + (p - mem1_pages) * PAGE;
        if (std::memcmp(dst, bytes, len) != 0)
        {
          std::memcpy(dst, bytes, len);
          differs = true;
        }
      }
      if (differs)
      {
        stamp[p] = epoch;
        if (audit_mismatches++ < 20)
          Log("SNAPSHOT HASH AUDIT: state page {:#x} differed from RAM after a hashed save "
              "(repaired)", p);
      }
    }
  }

  bool RefreshPortableViews()
  {
    // Old logical mappings may already be gone, so an unprotect failure here
    // is harmless. Canonical and physical aliases are made writable before
    // unregistering the callback, leaving no protected page without an owner.
    SetPortableProtection(false, false);
    EMM::SetMemoryFaultHandler(nullptr);
    active_protect_tracker = nullptr;
    BuildPortableViews();
    if (views.empty())
      return false;
    for (u32 page = 0; page < mem1_pages + vmem_pages; ++page)
      protect_dirty[page].store(IsTrackedPage(page) ? 1 : 0, std::memory_order_relaxed);
    active_protect_tracker = this;
    EMM::SetMemoryFaultHandler(&PortableFaultThunk);
    return SetPortableProtection(true, false);
  }

  bool ProtectFallbackToHash(const char* reason)
  {
    u8* const ram_base = ram;
    u8* const vmem_base = vmem;
    const std::optional<u64> epoch_key = shared_key;
    ReleaseProtect();
    protect_fallback = true;
    on = InitHash(ram_base, vmem_base);
    if (on && epoch_key)
      Rekey(*epoch_key);
    Log("snapshot protect tracker failed ({}); {}", reason,
        on ? "falling back to page hashing" : "falling back to full copies");
    return on;
  }

  // Release the kernel handles and forget everything (core shutdown; the
  // next session initialises again).
  void Reset()
  {
    ReleaseProtect();
    ReleaseKernel();
    std::destroy_at(this);
    std::construct_at(this);
  }

  // Stamp the pages written since the previous sample and protect them again.
  bool Sample()
  {
    if (mode == Mode::Protect)
    {
      const auto t0 = std::chrono::steady_clock::now();
      if (seen_remaps != Memory::g_nsr_logical_remaps)
      {
        seen_remaps = Memory::g_nsr_logical_remaps;
        ++remaps;
        if (!RefreshPortableViews())
          return ProtectFallbackToHash("view remap");
      }
      if (protect_fault_failed.load(std::memory_order_acquire))
        return ProtectFallbackToHash("fault handler could not restore write access");

      // Close every page that faulted during this epoch before consuming its
      // bit. A concurrent first write after protection sets the bit again and
      // is included by the exchange below.
      if (!SetPortableProtection(true, true))
        return ProtectFallbackToHash("page reprotection");
      ++epoch;
      std::fill(protect_observed.begin(), protect_observed.end(), 0);
      u64 dirty_pages = 0;
      for (u32 page = 0; page < mem1_pages + vmem_pages; ++page)
      {
        if (protect_dirty[page].exchange(0, std::memory_order_acq_rel) != 0)
        {
          protect_observed[page] = 1;
          stamp[page] = epoch;
          ++dirty_pages;
        }
      }
      pages_seen += dirty_pages;
      if (shared_key)
        HashObservedPages(protect_observed);
      if (exhaustive_audit && AuditPortable(protect_observed, epoch) != 0)
        return ProtectFallbackToHash("exhaustive hash audit mismatch");
      ++samples;
      sample_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
      return true;
    }
    if (mode == Mode::Hash)
    {
      const auto t0 = std::chrono::steady_clock::now();
      ++epoch;
      SampleHash();
      ++samples;
      sample_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
      return true;
    }
#if NSR_HAVE_UFFD
    // Dolphin remapped its logical views (a BAT write) since we last looked:
    // re-read the mappings first (RefreshViews samples again by itself).
    if (seen_remaps != Memory::g_nsr_logical_remaps)
    {
      seen_remaps = Memory::g_nsr_logical_remaps;
      ++remaps;
      RefreshViews(true);
    }
    const auto t0 = std::chrono::steady_clock::now();
    ++epoch;
    bool ok = true;
    for (const View& v : views)
    {
      const u64 end = v.host + u64{v.count} * PAGE;
      u64 start = v.host;
      while (start < end)
      {
        pm_scan_arg arg{};
        arg.size = sizeof(arg);
        arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
        arg.start = start;
        arg.end = end;
        arg.vec = reinterpret_cast<u64>(regions.data());
        arg.vec_len = regions.size();
        arg.category_mask = PAGE_IS_WRITTEN;
        arg.return_mask = PAGE_IS_WRITTEN;
        long n = ioctl(pagemap_fd, PAGEMAP_SCAN, &arg);
        if (n < 0 && errno == EPERM)
        {
          // Dolphin replaced this mapping (it remaps the logical views
          // whenever the game writes a BAT), so the new one isn't registered
          // yet. Register it; writes made through it before now are unknown,
          // so the whole view counts as written once.
          ++remaps;
          Register(v);
          std::fill(stamp.begin() + v.first, stamp.begin() + v.first + v.count, epoch);
          n = ioctl(pagemap_fd, PAGEMAP_SCAN, &arg);
        }
        if (n < 0)
        {
          if (failures++ < 5)
            Log("snapshot dirty tracking: PAGEMAP_SCAN of mapping {:x} failed: {}", v.host,
                std::strerror(errno));
          ok = false;  // be conservative: the whole view counts as written
          std::fill(stamp.begin() + v.first, stamp.begin() + v.first + v.count, epoch);
          break;
        }
        for (long i = 0; i < n; ++i)
        {
          const u32 a = static_cast<u32>((regions[i].start - v.host) / PAGE);
          const u32 b = static_cast<u32>((regions[i].end - v.host) / PAGE);
          for (u32 q = a; q < b && q < v.count; ++q)
            stamp[v.first + q] = epoch;
          pages_seen += b - a;
        }
        if (arg.walk_end <= start)
          break;
        start = arg.walk_end;
      }
    }
    ++samples;
    sample_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count();
    return ok;
#else
    return false;
#endif
  }

  // MEM1 pages wholly inside the GX FIFO, [first, end). The FIFO is a live
  // range (RbRestore always puts the current FIFO back), and the CPU writes
  // the frame's whole command stream into it, so saving or restoring those
  // pages is wasted work.
  // If the game ever moves its FIFO, pages skipped so far may be stale in
  // every slot: forget all slots' sync points (next saves are full copies).
  std::pair<u32, u32> FifoPages()
  {
    auto& fifo = Core::System::GetInstance().GetCommandProcessor().GetFifo();
    const u32 fb = fifo.CPBase.load() & 0x01FFFFFF, fe = (fifo.CPEnd.load() & 0x01FFFFFF) + 32;
    std::pair<u32, u32> pages{0, 0};
    if (fe > fb && fe <= mem1_bytes)
      pages = {(fb + PAGE - 1) / PAGE, fe / PAGE};
    if (pages != fifo_pages)
    {
      if (fifo_pages.second != 0)
        Log("snapshot dirty tracking: GX FIFO moved; all snapshots resync");
      if (mode == Mode::Protect)
      {
        // The FIFO is retained live across restores. Leaving its command
        // stream writable avoids hundreds of useless first-write faults per
        // frame. A former FIFO range becomes ordinary tracked state again.
        if (fifo_pages.first < fifo_pages.second)
        {
          for (u32 page = fifo_pages.first; page < fifo_pages.second; ++page)
            protect_dirty[page].store(1, std::memory_order_relaxed);
          if (!SetStateRangeProtection(fifo_pages.first, fifo_pages.second, true))
            protect_fault_failed.store(true, std::memory_order_release);
        }
        if (pages.first < pages.second)
        {
          if (!SetStateRangeProtection(pages.first, pages.second, false))
            protect_fault_failed.store(true, std::memory_order_release);
        }
      }
      fifo_pages = pages;
      synced.clear();
    }
    return pages;
  }
  std::pair<u32, u32> fifo_pages{0, 0};
  u64 fifo_skipped = 0;

  // Epoch at which `slot` equalled RAM, or nothing (slot needs a full copy).
  std::optional<u32> Synced(const std::vector<u8>& slot) const
  {
    const auto it = synced.find(slot.data());
    if (it == synced.end())
      return std::nullopt;
    return it->second;
  }
};
DirtyTracker s_dirty;

void RbInit(Core::System& system)
{
  Rollback& r = s_rb;
  RollbackConfig& config = ConfigForRollback();
  r.initialized = true;
  if (const std::string path = Env("NSR_LOG"); !path.empty() && s_session.log == stderr)
  {
    if (FILE* f = std::fopen(path.c_str(), "a"))
      s_session.log = f;
  }
  if (!config.ac_log_path.empty())
    r.ac_log = std::fopen(config.ac_log_path.c_str(), "w");
  if (!config.stacks_path.empty())
    r.stacks = std::fopen(config.stacks_path.c_str(), "w");
  r.force = ParseU32(Env("NSR_RB_FORCE")).value_or(0);
  if (const std::string path = Env("NSR_RB_REPLAY_FOLLOW"); !path.empty())
  {
    std::string error;
    if (!Env("NSR_RB_REPLAY").empty() || !Env("NSR_RB_RECORD").empty() ||
        (Env("NSR_RB_FORCE") != "0" && !Env("NSR_RB_FORCE").empty()) ||
        !Env("NSR_RB_MISPREDICT").empty() || !Env("NSR_RB_NET").empty() ||
        Config::Get(Config::MAIN_CPU_THREAD))
    {
      error = "follow requires single-core, no replay/record/forced rollback";
    }

    // Strict decimal parsing prevents malformed safety values from using a permissive default.
    auto option = [&error](const char* name, unsigned fallback, unsigned maximum) {
      const std::string text = Env(name);
      if (text.empty())
        return fallback;
      unsigned value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
          value > maximum)
      {
        error = "invalid follow option";
      }
      return value;
    };
    s_follow_delay = option("NSR_RB_REPLAY_DELAY", 0, 216000);
    const unsigned timeout = option("NSR_RB_REPLAY_TIMEOUT_MS", 30000, 3600000);
    if (!timeout)
      error = "follow timeout must be positive";
    if (error.empty())
    {
      s_follow = std::make_unique<ReplayFollow>(path, s_follow_delay, timeout);
      s_follow_speed = Config::Get(Config::MAIN_EMULATION_SPEED);
      error = s_follow->Error();
      if (error.empty() && !s_follow->Status(".ready", "1\n"))
        error = "cannot write follow acknowledgement";
    }
    if (error.empty())
    {
      Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
      Log("follow ready: {} delay {} timeout {}ms", path, s_follow_delay, timeout);
    }
    else
    {
      Log("follow init failed: {}", error);
      s_follow_failed = true;
      Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
    }
  }
  if (const std::string path = Env("NSR_RB_RECORD"); !path.empty())
    r.record = std::fopen(path.c_str(), "wb");
  if (const std::string path = Env("NSR_RB_REPLAY"); !path.empty())
  {
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
      u8 rec[4 + sizeof(Pads)];
      while (std::fread(rec, 1, sizeof(rec), f) == sizeof(rec))
      {
        u32 t;
        std::memcpy(&t, rec, 4);
        std::memcpy(r.replay[t].data(), rec + 4, sizeof(Pads));
      }
      std::fclose(f);
    }
    Log("replaying pads for {} ticks", r.replay.size());
  }
  r.force_from = ParseU32(Env("NSR_RB_FORCE_FROM")).value_or(0);
  r.mispredict = ParseU32(Env("NSR_RB_MISPREDICT")).value_or(0);
  r.net_enabled = !Env("NSR_RB_NET").empty();
  r.auto_handoff = r.net_enabled && Env("NSR_RB_VTIME_FROM").empty();
  r.net_delay = std::min<u32>(ParseU32(Env("NSR_RB_DELAY")).value_or(1), 8);
  r.max_prediction = std::min<u32>(ParseU32(Env("NSR_RB_MAX_PRED")).value_or(7), 60);
  r.time_sync = Env("NSR_RB_TIME_SYNC", "1") != "0";
  if (r.net_enabled)
  {
    // Network confirmation is the input source in this mode.  The synthetic
    // force/misprediction sources remain available unchanged when it is off.
    r.force = 0;
    r.mispredict = 0;
  }
  r.force_to = ParseU32(Env("NSR_RB_FORCE_TO")).value_or(0xFFFFFFFF);
  r.perturb_at = ParseU32(Env("NSR_RB_PERTURB_AT")).value_or(0);
  r.vtime_from = ParseU32(Env("NSR_RB_VTIME_FROM")).value_or(0);
  // Diagnostics: NSR_RB_MEMWATCH=addr logs every write to that word
  // (Dolphin's MEMMAP log; run with -C Logger.Logs.MEMMAP=True
  // -C Logger.Options.WriteToConsole=True -C Logger.Options.Verbosity=3).
  if (const auto w = Split(Env("NSR_RB_WATCH"), ':'); w.size() == 3)
  {
    r.watch = ParseU32(w[0]).value_or(0);
    r.watch_from = ParseU32(w[1]).value_or(0);
    r.watch_to = ParseU32(w[2]).value_or(0);
  }
  if (r.perturb_at)
    r.prev.resize(system.GetMemory().GetRamSizeReal());
  r.hash_log = !Env("NSR_RB_HASH_LOG").empty();
  for (const std::string& t : Split(Env("NSR_RB_DUMP_AT"), ','))
  {
    if (const auto v = ParseU32(t))
      r.dump_at.push_back(*v);
  }

  // Segments = MEM1 minus the keep ranges (defaults + NSR_RB_KEEP).
  std::vector<std::pair<u32, u32>> keep(RB_KEEP.begin(), RB_KEEP.end());
  r.audio_rollback = r.net_enabled || !Env("NSR_RB_AUDIO_ROLLBACK").empty();
  if (!r.audio_rollback)
    keep.insert(keep.end(), AUDIO_KEEP.begin(), AUDIO_KEEP.end());
  for (const std::string& item : Split(Env("NSR_RB_KEEP"), ','))
  {
    const auto parts = Split(item, '-');
    if (parts.size() != 2)
      continue;
    const auto lo = ParseU32(parts[0]), hi = ParseU32(parts[1]);
    if (lo && hi)
      keep.emplace_back(*lo & 0x01FFFFFF, *hi & 0x01FFFFFF);
  }
  std::sort(keep.begin(), keep.end());
  const u32 size = system.GetMemory().GetRamSizeReal();

  // Candidate ranges: all of MEM1, or the NSR_RB_RANGES list.
  std::vector<std::pair<u32, u32>> candidates;
  if (const std::string path = Env("NSR_RB_RANGES"); !path.empty())
  {
    if (FILE* f = std::fopen(path.c_str(), "r"))
    {
      char line[256];
      while (std::fgets(line, sizeof(line), f))
      {
        unsigned long lo = 0, hi = 0;
        if (line[0] != '#' && std::sscanf(line, "%lx %lx", &lo, &hi) == 2 && hi > lo)
          candidates.emplace_back(static_cast<u32>(lo) & 0x01FFFFFF,
                                  std::min<u32>(static_cast<u32>(hi - lo) + (lo & 0x01FFFFFF), size));
      }
      std::fclose(f);
    }
    std::sort(candidates.begin(), candidates.end());
    Log("restore ranges from {}: {}", path, candidates.size());
  }
  else
  {
    candidates.emplace_back(0, size);
  }
  // segments = candidates minus keep ranges
  for (auto [lo, hi] : candidates)
  {
    for (const auto& [klo, khi] : keep)
    {
      if (khi <= lo || klo >= hi)
        continue;
      if (klo > lo)
        r.segments.emplace_back(lo, klo);
      lo = std::max(lo, khi);
      if (lo >= hi)
        break;
    }
    if (lo < hi)
      r.segments.emplace_back(lo, hi);
  }
  u64 total = 0;
  for (const auto& [lo, hi] : r.segments)
    total += hi - lo;
  Log("rollback state: {} ranges, {} bytes", r.segments.size(), total);
  r.mem1_size = size;
  if (u8* vmem = system.GetMemory().GetFakeVMEM())
  {
    r.vmem = vmem;
    r.vmem_bytes = std::min<u32>(ParseU32(Env("NSR_RB_VMEM_BYTES")).value_or(0x104000),
                                 system.GetMemory().GetFakeVMemSize());
    Log("rollback state: + fake VMEM 7e000000..{:08x}", 0x7E000000 + r.vmem_bytes);
  }

  r.profile_path = Env("NSR_RB_PROFILE");
  if (!r.profile_path.empty())
  {
    r.force = 0;  // profiling only; NSR_RB_FORCE_FROM still sets where it starts
    r.at_start.resize(size);
    r.at_end.resize(size);
    r.pgran = std::clamp<u32>(ParseU32(Env("NSR_RB_GRANULE")).value_or(RB_GRANULE), 4, 4096);
    r.inside.assign(size / r.pgran, 0);
    r.outside.assign(size / r.pgran, 0);
  }

  r.io_idle_mode = r.net_enabled || !Env("NSR_RB_IO_IDLE").empty();
  if (r.force > 0 || r.net_enabled)
  {
    // Deep enough for the deepest rollback the session can ask for
    // (forced depth or misprediction delay), plus quiet-point slack.
    const u32 net_depth = r.net_enabled ? r.max_prediction + r.net_delay : 0;
    const u32 depth = std::max({r.force, r.mispredict, net_depth});
    r.ring.assign(r.io_idle_mode ? std::max<u32>(depth + 1, ParseU32(Env("NSR_RB_RING")).value_or(24))
                                 : depth + 1,
                  std::vector<u8>(size + r.vmem_bytes));
    // Snapshot page work: worker threads, then the dirty tracker (see
    // PageWorkers and DirtyTracker for the modes).
    const u32 hw = std::max(1u, std::thread::hardware_concurrency());
    const u32 threads =
        std::clamp<u32>(ParseU32(Env("NSR_RB_THREADS")).value_or(hw / 4), 1, 16);
    s_workers.Start(threads);
    s_dirty.Prepare(r.segments, size, r.vmem_bytes);
    const std::string dirty = Env("NSR_RB_DIRTY", "1");
    if (dirty != "0")
    {
      if (dirty == "protect")
      {
        s_dirty.on = s_dirty.InitProtect(system.GetMemory(), system.GetMemory().GetRAM(), r.vmem);
        if (!s_dirty.on)
        {
          s_dirty.protect_fallback = true;
          Log("snapshot protect tracker startup self-test failed; falling back to page hashing");
          s_dirty.on = s_dirty.InitHash(system.GetMemory().GetRAM(), r.vmem);
        }
      }
      else if (dirty == "hash")
      {
        s_dirty.on = s_dirty.InitHash(system.GetMemory().GetRAM(), r.vmem);
      }
      else if (dirty == "kernel")
      {
        s_dirty.on = s_dirty.InitKernel();
      }
      else
      {
#if defined(_WIN32) || defined(__APPLE__)
        s_dirty.on = s_dirty.InitProtect(system.GetMemory(), system.GetMemory().GetRAM(), r.vmem);
        if (!s_dirty.on)
        {
          s_dirty.protect_fallback = true;
          Log("snapshot protect tracker startup self-test failed; falling back to page hashing");
          s_dirty.on = s_dirty.InitHash(system.GetMemory().GetRAM(), r.vmem);
        }
#else
        s_dirty.on = s_dirty.InitKernel();
        if (!s_dirty.on)
          s_dirty.on = s_dirty.InitHash(system.GetMemory().GetRAM(), r.vmem);
#endif
      }
      if (!s_dirty.on && dirty != "kernel" && dirty != "protect" && dirty != "hash")
        s_dirty.on = s_dirty.InitHash(system.GetMemory().GetRAM(), r.vmem);
    }
    using Mode = DirtyTracker::Mode;
    Log("snapshot dirty-page tracking: {} ({} mappings of game memory, {} hashed pages); "
        "{} snapshot thread(s)",
        s_dirty.mode == Mode::Kernel ? "kernel" :
        s_dirty.mode == Mode::Protect ? "write protection" :
        s_dirty.mode == Mode::Hash   ? "page hashing" :
                                       "off, full copies",
        s_dirty.views.size(), s_dirty.hash_pages.size(), s_workers.Threads());
    for (const auto& v : s_dirty.views)
      Log("  mapping {:x}: state pages {:#x}+{:#x}", v.host, v.first, v.count);
  }
  if (r.net_enabled)
  {
    if (!r.auto_handoff && r.vtime_from == 0)
    {
      Log("rollback netplay requires the agreed NSR_RB_VTIME_FROM frame");
      s_session.failed = true;
      Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
    }
    else
    {
      if (!s_session.initialized)
        s_session.enabled = Setup();
      if (!s_session.enabled)
      {
        Log("rollback netplay transport setup failed");
        s_session.failed = true;
        Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
      }
      else if (r.auto_handoff)
      {
        Log("automatic lockstep-menu handoff enabled");
      }
    }
  }
  if (r.force > 0 || config.sample || r.stacks)
  {
    // The watchdog owns no emulator object.  CPU-thread hooks publish only the
    // counters it needs, so stopping/joining it never races PPC or RAM teardown.
    s_rb_watchdog = std::jthread(
        [sample = config.sample, stacks = r.stacks](std::stop_token stop) {
          while (!stop.stop_requested())
          {
            for (int i = 0; i < 20 && !stop.stop_requested(); ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (stop.stop_requested())
              return;

            const s64 last = s_rb_last_tick_ms.load();
            const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            const u32 live = s_rb_watch_live.load();
            const u64 rollbacks = s_rb_watch_rollbacks.load();
            if (stacks)
            {
              // PPC stacks may only be sampled on the CPU thread.  Retain a
              // useful host-side liveness trace without touching emulated state.
              std::fprintf(stacks, "%u rollbacks=%llu last_tick_ms=%lld\n", live,
                           static_cast<unsigned long long>(rollbacks),
                           static_cast<long long>(last));
              std::fflush(stacks);
            }
            if (sample)
              Log("sample live {} rollbacks {} last-tick-age {}ms", live, rollbacks,
                  last ? now - last : 0);
            if (rollbacks == 0 || last == 0 || now - last < 5000)
              continue;
            Log("HANG: no tick for {} ms after {} rollbacks (live {}); PPC/RAM sampling "
                "is deferred to CPU-thread diagnostics",
                now - last, rollbacks, live);
            NSRTelemetry::Event(NSRTelemetry::EventKind::Hang, live,  // NSR telemetry
                                {NSRTelemetry::Int("ms_since_tick", now - last),
                                 NSRTelemetry::Int("rollbacks", static_cast<s64>(rollbacks))});
            return;
          }
        });
  }
  Log("rollback image: force={} keep-ranges={} hash-log={} net={} delay={} max-pred={}", r.force,
      keep.size(), r.hash_log, r.net_enabled, r.net_delay, r.max_prediction);
}

// ---------------------------------------------------------------------------
// ARAM replay ring (final-review M1, rb-coverage-review C3)
//
// The game records every gameplay object for its instant replay: every second
// frame fn_800307C0 -> fn_8002FE34 -> fn_80199C94 serializes the object list
// (-0x5C58(r13)) into one of 4 rotating MRAM buffers and copies the record
// into ARAM with the synchronous ARQ writer fn_801C1D98. The records form a
// ring of `count` x `bytes` that fn_80030B30 -> fn_80199D58 allocates from the
// game's ARAM heap (fn_801D2BB0); Quick Play: 213 x 0xA40 at
// 0x466460..0x4EEBA0. The instant-replay viewer (X after a play) reads records
// back through fn_8002FECC -> fn_80199CF0 -> fn_801C1E78 into game memory.
//
// ARAM is outside the MEM1/VMEM snapshots, so a restore used to leave records
// written by abandoned (mispredicted) frames in the ring wherever the
// corrected timeline did not rewrite the same record; a replay viewed later
// would differ between peers (and copy that difference into MEM1).
//
// Coverage: an undo log (RollbackLogic::WindowUndoLog) of every ARAM write
// into the ring while a rollback ring exists. DSPManager reports each ARAM
// transfer before it happens (SetNSRARAMObserver); writes into the ring's
// window record the bytes they replace. Each snapshot stores the log's mark,
// and RbRestore undoes the newer records, so after a restore the ring equals
// the snapshot's frame exactly, including writes made during re-simulation
// (logged like live ones and undone by a later, deeper rollback). Records
// older than every ring slot's mark are trimmed. Session enter/leave clears
// the log and the marks (like the ring slots and their metadata).
//
// Why the window comes from the game's own ring descriptor (read from MEM1 at
// each transfer, not a fixed address range): only bytes of the ring's ARAM
// allocation are ever logged or undone, whatever the mode or allocation
// order, so audio banks and streamed samples (kept live; the audio model
// handles audio) can never be rolled back. The descriptor is itself restored
// game state, so the window always matches the timeline that writes into it.
// A window change inside a session is logged.
//
// Why an undo log rather than copying the ring into every slot: ~2.6 KB is
// written every second frame into a ~560 KB window, so the log holds tens of
// KB and costs one small copy per write; slot copies would be 24 x 560 KB.
//
// ARQ and quiet points: Dolphin performs an ARQ request's ARAM DMA in full
// when the transfer starts (completion is only a scheduled event), so a write
// is logged in the frame whose code (or interrupt) started it; I/O-idle quiet
// points already require the ARQ queues empty. Speed factors (16x re-sim
// machine) only change completion timing.
//
// Diagnostics: NSR_RB_ARAM=0 keeps the observer but does not log or undo
// (reproduces the uncovered behavior for tests); reads of the ring
// (ARAM -> MRAM DMA) are counted and the first ones logged
// (NSR_RB_ARAM_TRACE=1 logs every ring read and write); NSR_DUMP_PREFIX
// dumps write the ring to <dump>.aram (see WriteAramRingSidecar).
//
// Deferred: other ARAM contents (audio) stay live by design. RbAdopt's
// file-based state transplant does not adopt the ring (it is a single-machine
// diagnostic). Revisit both if a mode is found that keeps other gameplay data
// in ARAM (the read probe reports reads of the ring only).
constexpr u32 ARAM_RING_BASE_PTR = 0x803ECBEC;    // -0x56B4(r13): ARAM address, 0 = none
constexpr u32 ARAM_RING_COUNT = 0x803EC658;       // -0x5C48(r13): records in the ring
constexpr u32 ARAM_RING_RECORD_BYTES = 0x803EC650;  // -0x5C50(r13): bytes per record

struct AramRing
{
  const u8* mem1 = nullptr;  // set while a session observes ARAM
  u8* aram = nullptr;
  u32 aram_size = 0;
  bool undo = false;  // logging/undo on (a rollback ring exists, NSR_RB_ARAM != 0)
  bool trace = false;  // NSR_RB_ARAM_TRACE=1: log every ring access (diagnostics)
  RollbackLogic::WindowUndoLog log;
  std::map<const u8*, u64> marks;  // snapshot slot -> log mark when saved
  std::pair<u32, u32> window{0, 0};
  u64 writes = 0, write_bytes = 0, byte_writes = 0, reads = 0, read_bytes = 0;
  u64 restores = 0, undone = 0;
  u64 host_ns = 0;  // host time spent logging, undoing and trimming (cost report)
  u32 reads_logged = 0;
};

// Adds the host time of one logging/undo step to s_aram.host_ns.
struct AramTimer
{
  ~AramTimer();
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
AramRing s_aram;
AramTimer::~AramTimer()
{
  s_aram.host_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count());
}

// [lo, hi) of the ring in ARAM from the game's descriptor; {0, 0} if none.
std::pair<u32, u32> AramRingWindow(const u8* ram, u32 aram_size)
{
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  const u32 base = rd(ARAM_RING_BASE_PTR);
  const u64 end = u64{base} + u64{rd(ARAM_RING_COUNT)} * rd(ARAM_RING_RECORD_BYTES);
  if (base == 0 || end <= base || end > aram_size)
    return {0, 0};
  return {base, static_cast<u32>(end)};
}

void AramObserver(DSP::DSPManager& dsp, u32 address, u32 length,
                  DSP::DSPManager::NSRARAMAccess access)
{
  using Access = DSP::DSPManager::NSRARAMAccess;
  AramRing& a = s_aram;
  if (!a.mem1)
    return;
  const auto window = AramRingWindow(a.mem1, dsp.GetARAMSize());
  if (window != a.window)
  {
    Log("ARAM replay ring: window {:x}..{:x} (was {:x}..{:x}) at frame {}", window.first,
        window.second, a.window.first, a.window.second, s_rb.tick_running);
    a.window = window;
  }
  const auto [lo, hi] = window;
  if (lo == hi || address >= hi || u64{address} + length <= lo)
    return;
  if (a.trace)
  {
    Log("ARAM replay ring: {} {:x}+{:#x} frame {} re-sim {}",
        access == Access::DMARead ? "read" : "write", address, length, s_rb.tick_running,
        s_rb.in_resim);
  }
  if (access == Access::DMARead)
  {
    ++a.reads;
    a.read_bytes += length;
    if (a.reads_logged < 16)
    {
      ++a.reads_logged;
      Log("ARAM replay ring: read {:x}+{:#x} in frame {} (re-sim {})", address, length,
          s_rb.tick_running, s_rb.in_resim);
    }
    return;
  }
  ++(access == Access::ByteWrite ? a.byte_writes : a.writes);
  a.write_bytes += length;
  if (a.undo)
  {
    AramTimer timer;
    a.log.Record({a.aram, a.aram_size}, lo, hi, address, length);
  }
}

// RbSave: remember the log position of this snapshot and trim records that no
// rollback-ring slot can undo any more.
void AramRingSaved(const std::vector<u8>& slot)
{
  AramRing& a = s_aram;
  if (!a.undo)
    return;
  AramTimer timer;
  a.marks[slot.data()] = a.log.Mark();
  if (a.log.Records() <= 2 * s_rb.ring.size())
    return;
  u64 oldest = UINT64_MAX;
  for (const auto& ring_slot : s_rb.ring)
  {
    if (const auto it = a.marks.find(ring_slot.data()); it != a.marks.end())
      oldest = std::min(oldest, it->second);
  }
  if (oldest != UINT64_MAX)
    a.log.TrimThrough(oldest);
}

// RbRestore: put the ring back to the snapshot's frame. Slots this session
// never saved (RbAdopt's file state) leave ARAM alone.
void AramRingRestore(const std::vector<u8>& slot)
{
  AramRing& a = s_aram;
  if (!a.undo)
    return;
  const auto it = a.marks.find(slot.data());
  if (it == a.marks.end())
    return;
  AramTimer timer;
  ++a.restores;
  a.undone += a.log.UndoTo(it->second, {a.aram, a.aram_size});
}

void AramRingReport(const char* when)
{
  const AramRing& a = s_aram;
  if (!a.mem1)
    return;
  Log("ARAM replay ring ({}): window {:x}..{:x}, {} DMA writes + {} byte writes ({} bytes), "
      "{} reads ({} bytes); undo {}: {} restores undid {} records, log {} records / {} bytes, "
      "host {} us",
      when, a.window.first, a.window.second, a.writes, a.byte_writes, a.write_bytes, a.reads,
      a.read_bytes, a.undo ? "on" : "off", a.restores, a.undone, a.log.Records(), a.log.Bytes(),
      a.host_ns / 1000);
}

// Session enter (enter = true) / leave: start or stop observing ARAM, with an
// empty log either way.
void AramRingSession(Core::System& system, bool enter)
{
  if (!enter)
    AramRingReport("session leave");
  s_aram = {};
  auto& dsp = system.GetDSP();
  if (!enter)
  {
    dsp.SetNSRARAMObserver(nullptr);
    return;
  }
  s_aram.mem1 = system.GetMemory().GetRAM();
  s_aram.aram = dsp.GetARAMPtr();
  s_aram.aram_size = dsp.GetARAMSize();
  s_aram.undo = !s_rb.ring.empty() && Env("NSR_RB_ARAM") != "0" && s_aram.aram != nullptr;
  s_aram.trace = Env("NSR_RB_ARAM_TRACE") == "1";
  s_aram.window = AramRingWindow(s_aram.mem1, s_aram.aram_size);
  dsp.SetNSRARAMObserver(AramObserver);
  Log("ARAM replay ring: window {:x}..{:x} at session start, undo log {}", s_aram.window.first,
      s_aram.window.second, s_aram.undo ? "on" : "off");
}

// The ring as it is now: its ARAM address and bytes (empty if none).
std::pair<u32, std::span<const u8>> AramRingBytes(Core::System& system)
{
  const auto& dsp = system.GetDSP();
  const u8* aram = dsp.GetARAMPtr();
  if (!aram || !system.GetMemory().GetRAM())
    return {0, {}};
  const auto [lo, hi] = AramRingWindow(system.GetMemory().GetRAM(), dsp.GetARAMSize());
  return {lo, std::span<const u8>(aram + lo, hi - lo)};
}

// <dump>.aram next to a MEM1 dump: "NSRARAM1", the ring's ARAM address and
// length (u32 little-endian each), then its bytes (tools/rbdiff.py compares
// it with the other dump's sidecar). Written even when no ring exists
// (length 0), so a missing ring is visible in the comparison.
void WriteAramRingSidecar(Core::System& system, const std::string& dump)
{
  const auto [address, bytes] = AramRingBytes(system);
  FILE* f = std::fopen((dump + ".aram").c_str(), "wb");
  if (!f)
    return;
  const u32 length = static_cast<u32>(bytes.size());
  const u8 header[16] = {'N', 'S', 'R', 'A', 'R', 'A', 'M', '1',
                         static_cast<u8>(address), static_cast<u8>(address >> 8),
                         static_cast<u8>(address >> 16), static_cast<u8>(address >> 24),
                         static_cast<u8>(length), static_cast<u8>(length >> 8),
                         static_cast<u8>(length >> 16), static_cast<u8>(length >> 24)};
  std::fwrite(header, 1, sizeof(header), f);
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
}

// Per-machine heap blocks (sidecar, see WritePerMachineSidecar): blocks the
// game's allocator hands to audio code. The audio middleware, the game's sound
// layer and stream source, and PathFinder allocate from the shared game heap
// (descriptor 0x804CB810, id 1, together with game state) and from the small
// stream-source heap; which addresses they get depends on the mode (Quick Play
// vs Demo Mode), so they are found by watching the allocator, not by address.
// The set must follow rollback: snapshots remember the event sequence number
// when they were saved, and a restore undoes later allocations and frees.
struct AllocEvent
{
  u64 seq;
  bool alloc;
  u32 lo, hi;
};
std::map<u32, u32> s_audio_blocks;  // payload start -> end (virtual)
// Every heap descriptor an audio-owned allocation ever came from (sticky for
// the process, unlike s_audio_blocks; see DigestExclusions).
std::set<u32> s_audio_heaps;
std::vector<AllocEvent> s_alloc_log;
u64 s_alloc_seq = 0;
std::map<const u8*, u64> s_slot_seq;  // snapshot slot -> s_alloc_seq when saved

void AllocUndoTo(u64 seq)
{
  while (!s_alloc_log.empty() && s_alloc_log.back().seq > seq)
  {
    const AllocEvent e = s_alloc_log.back();
    s_alloc_log.pop_back();
    if (e.alloc)
      s_audio_blocks.erase(e.lo);
    else
      s_audio_blocks[e.lo] = e.hi;
  }
}

void RbSave(const u8* ram, std::vector<u8>& slot)
{
  s_slot_seq[slot.data()] = s_alloc_seq;
  AramRingSaved(slot);  // ARAM replay ring (final-review M1)
  // Keep the undo log bounded by the oldest snapshot still remembered.
  if (s_alloc_log.size() > 65536 && !s_slot_seq.empty())
  {
    u64 oldest = UINT64_MAX;
    for (const auto& [ptr, seq] : s_slot_seq)
      oldest = std::min(oldest, seq);
    std::erase_if(s_alloc_log, [&](const AllocEvent& e) { return e.seq <= oldest; });
  }
  PhaseTimer timer(PH_SAVE);
  DirtyTracker& d = s_dirty;
  const bool full_slot = s_rb.vmem_bytes == 0 || slot.size() >= s_rb.mem1_size + s_rb.vmem_bytes;
  // Per-page path: with dirty tracking (only pages stamped since the slot was
  // synced), or for full copies split over the snapshot worker threads.
  const bool parallel_full = !d.on && d.paged && s_workers.Threads() > 1 &&
                             slot.size() >= s_rb.mem1_size + s_rb.vmem_bytes;
  if ((d.on && full_slot) || parallel_full)
  {
    std::optional<u32> since;
    std::pair<u32, u32> fifo{0, 0};
    if (d.on)
    {
      d.Sample();
      fifo = d.FifoPages();
      since = d.Synced(slot);
    }
    const auto [fifo_lo, fifo_hi] = fifo;
    const u32 vmem_pages = s_rb.vmem_bytes ? d.vmem_pages : 0;
    std::atomic<u32> fifo_skipped{0};
    std::atomic<u64> copied{0};  // NSR telemetry
    s_workers.Run(d.mem1_pages + vmem_pages, PAGE_CHUNK, [&](u32 lo, u32 hi) {
      for (u32 p = lo; p < hi; ++p)
      {
        if (!RollbackLogic::PageNeedsCopy(d.stamp[p], since))
          continue;
        if (p < d.mem1_pages)
        {
          if (since && p >= fifo_lo && p < fifo_hi)
          {
            fifo_skipped.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          copied.fetch_add(1, std::memory_order_relaxed);
          for (u32 c = d.page_first[p]; c < d.page_first[p + 1]; ++c)
            std::memcpy(slot.data() + d.chunks[c].first, ram + d.chunks[c].first,
                        d.chunks[c].second - d.chunks[c].first);
        }
        else
        {
          copied.fetch_add(1, std::memory_order_relaxed);
          const u32 a = (p - d.mem1_pages) * DirtyTracker::PAGE;
          const u32 n = std::min(DirtyTracker::PAGE, s_rb.vmem_bytes - a);
          std::memcpy(slot.data() + s_rb.mem1_size + a, s_rb.vmem + a, n);
        }
      }
    });
    d.fifo_skipped += fifo_skipped.load();
    d.pages_copied += copied.load();  // NSR telemetry
    if (!d.on)
      return;
    d.synced[slot.data()] = d.epoch;
    if (since)
      d.Audit(slot, fifo);
    if (d.verify)
    {
      u32 missed = 0;
      for (const auto& [lo, hi] : s_rb.segments)
        for (u32 a = lo; a < hi; a += 4)
          missed += (a / DirtyTracker::PAGE < fifo_lo || a / DirtyTracker::PAGE >= fifo_hi) &&
                    std::memcmp(slot.data() + a, ram + a, std::min(4u, hi - a)) != 0;
      if (s_rb.vmem_bytes)
        missed += std::memcmp(slot.data() + s_rb.mem1_size, s_rb.vmem, s_rb.vmem_bytes) != 0;
      if (missed)
        Log("DIRTY VERIFY: save missed {} words (live {} cursor {})", missed, s_rb.live, s_rb.cursor);
    }
    return;
  }
  for (const auto& [lo, hi] : s_rb.segments)
    std::memcpy(slot.data() + lo, ram + lo, hi - lo);
  // Fake VMEM (EA database heap), only into slots sized for it (the ring).
  if (s_rb.vmem_bytes && slot.size() >= s_rb.mem1_size + s_rb.vmem_bytes)
    std::memcpy(slot.data() + s_rb.mem1_size, s_rb.vmem, s_rb.vmem_bytes);
}

// OS objects that live in game memory but follow real time, found by
// walking the OS's own lists in current RAM (physical [lo, hi) ranges):
//   - every thread on the active queue (0x800000DC, link at +0x2FC) except
//     the main thread: its OSThread (0x318 bytes) and its stack;
//   - every alarm on the alarm queue (head at 0x803ED078, next at +0x14,
//     0x28 bytes each);
//   - the GX FIFO, from the command processor's base/end registers.
std::vector<std::pair<u32, u32>> RbLiveRanges(Core::System& system, const u8* ram, bool all = false)
{
  // Layouts from the Dolphin SDK headers / decomp (checked by the Codex audit
  // in NFLStreet local/work/codex/live-objects-audit.md). Every walk is
  // bounded; ranges are clamped to MEM1.
  const u32 mem_size = system.GetMemory().GetRamSizeReal();
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  auto valid_range = [&](u32 a, u32 size) {
    if (a < 0x80003100)
      return false;
    const u32 offset = a - 0x80000000;
    return offset <= mem_size && size <= mem_size - offset;
  };
  auto valid = [&](u32 a) { return valid_range(a, 4); };
  std::vector<std::pair<u32, u32>> live;
  auto keep = [&](u32 a, u32 size) {
    if (valid_range(a, size))
      live.emplace_back(a & 0x01FFFFFF, (a & 0x01FFFFFF) + size);
  };
  // Threads (active queue at 0x800000DC, link +0x2FC): OSThread, stack, the
  // OSThreadQueue it sleeps on (+0x2DC), the mutex it waits for (+0x2F0) and
  // the mutexes it owns (list at +0x2F4, next +0x10). OSMutex = 0x18 bytes.
  u32 t = rd(0x800000DC);
  for (int n = 0; n < 32 && valid_range(t, 0x318); ++n, t = rd(t + 0x2FC))
  {
    // (all: every per-machine object, for the dump sidecar)
    if (t == DISC_WORKER_THREAD && s_rb.io_idle_mode && !all)
      continue;  // restored with everything else in I/O-idle mode
    if (t == AUDIO_THREAD && s_rb.audio_rollback && !all)
      continue;  // restored with the rest of audio
    if (t != 0x803A3F30)  // main thread: its OSThread and stack are kept statically
    {
      keep(t, 0x318);
      const u32 base = rd(t + 0x304), end = rd(t + 0x308);
      // base is the one-past-the-top address, which may equal the end of MEM1.
      const bool base_ok = base > 0x80003100 && base <= 0x80000000 + mem_size;
      if (valid(end) && base_ok && end < base && base - end <= 0x100000)
        live.emplace_back(end & 0x01FFFFFF, base & 0x01FFFFFF);
    }
    keep(rd(t + 0x2DC), 8);
    keep(rd(t + 0x2F0), 0x18);
    u32 m = rd(t + 0x2F4);
    for (int k = 0; k < 64 && valid_range(m, 0x18); ++k, m = rd(m + 0x10))
      keep(m, 0x18);
  }
  // Alarms (queue head 0x803ED078, next +0x14, 0x28 bytes).
  u32 al = rd(0x803ED078);
  for (int n = 0; n < 256 && valid_range(al, 0x28); ++n, al = rd(al + 0x14))
    keep(al, 0x28);
  // DVD: four priority queues (circular, sentinels at 0x803A55D8 + 8 * i,
  // next at +0) and the executing block (0x803ED118). DVDCommandBlock is
  // 0x30 bytes; 0x3C for the DVDFileInfo wrapper (callback 0x8023F464).
  auto dvd = [&](u32 cb) {
    if (valid_range(cb, 0x30))
      keep(cb, rd(cb + 0x28) == 0x8023F464 ? 0x3C : 0x30);
  };
  for (u32 i = 0; i < 4; ++i)
  {
    const u32 sentinel = 0x803A55D8 + 8 * i;
    u32 cb = rd(sentinel);
    for (int n = 0; n < 64 && valid_range(cb, 0x30) && cb != sentinel; ++n, cb = rd(cb))
      dvd(cb);
  }
  if (const u32 cb = rd(0x803ED118); valid(cb))
    dvd(cb);
  // ARQ: queued requests (heads 0x803ED2D8 / 0x803ED2E0, next +0) and the
  // pending one per priority (0x803ED2F0 / 0x803ED2F4). ARQRequest = 0x20.
  for (const u32 head : {0x803ED2D8u, 0x803ED2E0u})
  {
    u32 q = rd(head);
    for (int n = 0; n < 256 && valid_range(q, 0x20); ++n, q = rd(q))
      keep(q, 0x20);
  }
  keep(rd(0x803ED2F0), 0x20);
  keep(rd(0x803ED2F4), 0x20);
  // Audio middleware callback list (GN7E69): head at 0x803A6BCC (kept), walked
  // by fn_80269C70 on the audio thread, nodes {next, ?, callback, arg} living
  // in game memory. A restore that rewound a still-linked node left a null
  // callback (crash on the audio thread).
  if (!s_rb.audio_rollback || all)
  {
    u32 node = rd(0x803A6BCC);
    for (int n = 0; n < 256 && valid_range(node, 0x10); ++n, node = rd(node))
      keep(node, 0x10);
  }
  // The game's real-time tick accumulator (see TICK_CLOCK_PTR).
  if (const u32 outer = rd(TICK_CLOCK_PTR); valid_range(outer, 0x10))
    keep(rd(outer + 0xC), 0x10);
  // GX FIFO, from the command processor's registers.
  if (!s_rb.io_idle_mode || all)
  {
    for (const auto& [lo, hi] : s_io_ranges)
      if (hi <= mem_size)
        live.emplace_back(lo, hi);
    for (const auto& [lo, hi] : DISC_KEEP)
      live.emplace_back(lo, hi);
  }
  auto& fifo = system.GetCommandProcessor().GetFifo();
  const u32 fb = fifo.CPBase.load() & 0x01FFFFFF, fe = fifo.CPEnd.load() & 0x01FFFFFF;
  if (fe > fb && fe < mem_size)
    live.emplace_back(fb, std::min<u32>(fe + 32, mem_size));
  // A thread object, queue, stack, and FIFO can overlap. Normalize them once
  // so snapshots and sidecars describe each per-machine byte only once.
  std::vector<RollbackLogic::Range> ranges;
  ranges.reserve(live.size());
  for (const auto& [lo, hi] : live)
    ranges.push_back({lo, hi});
  const std::vector<RollbackLogic::Range> merged = RollbackLogic::MergeRanges(std::move(ranges));
  live.clear();
  live.reserve(merged.size());
  for (const RollbackLogic::Range& range : merged)
    live.emplace_back(range.lo, range.hi);
  return live;
}

// Disc system idle: no DVD command executing or queued, the game's disc
// worker asleep on its request queue, no game loads in flight.
bool IsIoIdle(const u8* ram)
{
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  if (rd(0x803ED118) != 0)
    return false;
  // ARQ (audio-RAM DMA, shared by audio streaming and game copies): no queued
  // or pending request, or a restore could desync the middleware from the
  // hardware (rb-coverage-review L4).
  for (const u32 a : {0x803ED2D8u, 0x803ED2E0u, 0x803ED2F0u, 0x803ED2F4u})
    if (rd(a) != 0)
      return false;
  for (u32 i = 0; i < 4; ++i)
  {
    const u32 sentinel = 0x803A55D8 + 8 * i;
    if (rd(sentinel) != sentinel && rd(sentinel) != 0)
      return false;
  }
  const u16 state = static_cast<u16>(rd(DISC_WORKER_THREAD + 0x2C8) >> 16);
  if (state != 4 || rd(DISC_WORKER_THREAD + 0x2DC) != DISC_WORKER_QUEUE)
    return false;
  // No read anywhere in the worker's pipeline (a read can be finished at the
  // DVD but not yet consumed by the worker: that gap lost completions).
  if (rd(DISC_SEM_B) != 0 || rd(DISC_SEM_A) != DISC_SLOTS_IDLE)
    return false;
  if (rd(PENDING_LOADS) != 0)
    return false;
  if (s_rb.audio_rollback)
  {
    const u16 astate = static_cast<u16>(rd(AUDIO_THREAD + 0x2C8) >> 16);
    if (astate != 4 || rd(AUDIO_THREAD + 0x2DC) != AUDIO_THREAD_QUEUE)
      return false;
  }
  return true;
}

void RbRestore(Core::System& system, u8* ram, const std::vector<u8>& slot)
{
  // Restore the game state, then put the live OS objects back as they are now.
  // Copy page by page and tell the JIT about every page whose bytes changed:
  // restored memory can hold code (e.g. code the audio engine generates in the
  // heap), and compiled blocks for the old bytes must not keep running.
  if (const auto it = s_slot_seq.find(slot.data()); it != s_slot_seq.end())
    AllocUndoTo(it->second);
  AramRingRestore(slot);  // ARAM replay ring (final-review M1)
  std::optional<PhaseTimer> timer(std::in_place, PH_LIVE);
  const auto live = RbLiveRanges(system, ram);
  std::vector<std::vector<u8>> keep;
  for (const auto& [lo, hi] : live)
    keep.emplace_back(ram + lo, ram + hi);
  timer.emplace(PH_RESTORE);
  constexpr u32 PAGE = 0x1000;
  std::vector<u32> changed;
  DirtyTracker& d = s_dirty;
  std::optional<u32> since;
  bool vmem_restored = false;
  std::pair<u32, u32> fifo{0, 0};
  if (d.on)
  {
    d.Sample();
    fifo = d.FifoPages();
    since = d.Synced(slot);
  }
  const bool sized = slot.size() >= s_rb.mem1_size + s_rb.vmem_bytes;
  if (since || (d.paged && s_workers.Threads() > 1 && sized))
  {
    // Page by page, split over the snapshot workers. With a sync point, only
    // pages written since the slot equalled RAM can differ from it, and GX
    // FIFO pages are skipped (the live FIFO is put back below anyway);
    // without one, every page is compared (the full restore, in parallel).
    const auto [fifo_lo, fifo_hi] = fifo;
    const u32 vmem_pages = s_rb.vmem_bytes ? d.vmem_pages : 0;
    std::vector<u8> page_changed(d.mem1_pages, 0);
    std::atomic<bool> any_vmem{false};
    s_workers.Run(d.mem1_pages + vmem_pages, PAGE_CHUNK, [&](u32 lo, u32 hi) {
      for (u32 p = lo; p < hi; ++p)
      {
        if (!RollbackLogic::PageNeedsCopy(d.stamp[p], since))
          continue;
        if (p >= d.mem1_pages)
        {
          const u32 a = (p - d.mem1_pages) * PAGE, n = std::min(PAGE, s_rb.vmem_bytes - a);
          std::memcpy(s_rb.vmem + a, slot.data() + s_rb.mem1_size + a, n);
          any_vmem.store(true, std::memory_order_relaxed);
          continue;
        }
        if (since && p >= fifo_lo && p < fifo_hi)
          continue;
        for (u32 c = d.page_first[p]; c < d.page_first[p + 1]; ++c)
        {
          const auto [a, b] = d.chunks[c];
          if (std::memcmp(ram + a, slot.data() + a, b - a) != 0)
          {
            std::memcpy(ram + a, slot.data() + a, b - a);
            page_changed[p] = 1;
          }
        }
      }
    });
    for (u32 p = 0; p < d.mem1_pages; ++p)
    {
      if (page_changed[p])
        changed.push_back(p * PAGE);
    }
    vmem_restored = any_vmem.load();
  }
  else
  {
    for (const auto& [lo, hi] : s_rb.segments)
    {
      for (u32 a = lo; a < hi;)
      {
        const u32 b = std::min(hi, (a & ~(PAGE - 1)) + PAGE);
        if (std::memcmp(ram + a, slot.data() + a, b - a) != 0)
        {
          std::memcpy(ram + a, slot.data() + a, b - a);
          if (changed.empty() || changed.back() != (a & ~(PAGE - 1)))
            changed.push_back(a & ~(PAGE - 1));
        }
        a = b;
      }
    }
    // Fake VMEM holds data only (the database heap): no JIT invalidation needed.
    if (s_rb.vmem_bytes && slot.size() >= s_rb.mem1_size + s_rb.vmem_bytes)
    {
      std::memcpy(s_rb.vmem, slot.data() + s_rb.mem1_size, s_rb.vmem_bytes);
      vmem_restored = true;
    }
  }
  if (since && d.verify)
  {
    u32 missed = 0;
    for (const auto& [lo, hi] : s_rb.segments)
      for (u32 a = lo; a < hi; a += PAGE - (a % PAGE))
      {
        const u32 b = std::min(hi, a + PAGE - (a % PAGE));
        bool in_live = false;
        for (const auto& [llo, lhi] : live)
          in_live |= a < lhi && llo < b;
        if (!in_live && std::memcmp(ram + a, slot.data() + a, b - a) != 0)
          ++missed;
      }
    if (s_rb.vmem_bytes && std::memcmp(s_rb.vmem, slot.data() + s_rb.mem1_size, s_rb.vmem_bytes) != 0)
      ++missed;
    if (missed)
      Log("DIRTY VERIFY: restore missed {} pages (live {} cursor {})", missed, s_rb.live, s_rb.cursor);
  }
  timer.emplace(PH_LIVE);
  for (size_t i = 0; i < live.size(); ++i)
    std::memcpy(ram + live[i].first, keep[i].data(), keep[i].size());
  timer.emplace(PH_JIT);
  auto& jit = system.GetJitInterface();
  // One call per run of consecutive changed pages (the per-call address
  // translation and block-map lookups dominate for single pages).
  for (size_t i = 0; i < changed.size();)
  {
    size_t j = i + 1;
    while (j < changed.size() && changed[j] == changed[j - 1] + PAGE)
      ++j;
    jit.InvalidateICache(0x80000000 | changed[i], static_cast<u32>(j - i) * PAGE, true);
    i = j;
  }
  // The emulated instruction cache too: recompilation fetches through it, so
  // stale lines would keep the old code bytes (rb-coverage-review C5).
  // Only the cache contents are dropped: InstructionCache::Reset would also
  // clear the whole JIT block cache (ClearSafe), and recompiling every block
  // after every rollback cost ~200 ms of host time in the next frame. Blocks
  // for the changed pages are already invalidated above; blocks for unchanged
  // pages were compiled from bytes the restore did not touch, so they stay
  // valid. NSR_RB_JIT_CLEAR=1 restores the full clear (diagnostics).
  //
  // Only the lines of changed pages are dropped (what icbi does line by line);
  // Cache::Reset also refilled ~2 MB of lookup tables on every rollback.
  // Lines of untouched pages hold the same bytes as before the restore.
  // Fake VMEM is data only, but its lines are dropped too if it was restored.
  static const bool jit_clear = !Env("NSR_RB_JIT_CLEAR").empty();
  if (jit_clear && !changed.empty())
  {
    system.GetPPCState().iCache.Reset(jit);
  }
  else if (!changed.empty() || vmem_restored)
  {
    auto& memory = system.GetMemory();
    auto& ic = system.GetPPCState().iCache;
    std::vector<bool> page_changed(memory.GetRamSize() / PAGE + 1, false);
    for (const u32 page : changed)
      page_changed[page / PAGE] = true;
    for (u32 set = 0; set < PowerPC::CACHE_SETS; ++set)
    {
      for (u32 way = 0; way < PowerPC::CACHE_WAYS; ++way)
      {
        if (!(ic.valid[set] & (1u << way)))
          continue;
        const u32 a = ic.addrs[set][way];
        std::vector<u8>* table;
        u32 index;
        if (a & PowerPC::CACHE_VMEM_BIT)
        {
          if (!vmem_restored)
            continue;
          table = &ic.lookup_table_vmem;
          index = (a & memory.GetFakeVMemMask()) >> 5;
        }
        else if (a & PowerPC::CACHE_EXRAM_BIT)
        {
          continue;  // Wii only
        }
        else
        {
          if (!page_changed[(a & memory.GetRamMask()) / PAGE])
            continue;
          table = &ic.lookup_table;
          index = (a & memory.GetRamMask()) >> 5;
        }
        ic.valid[set] &= ~(1u << way);
        ic.modified[set] &= ~(1u << way);
        (*table)[index] = 0xFF;
      }
    }
  }
}

// Compare RAM (re-simulated state before tick t) with the original state
// before tick t, granule by granule; record and log what differs.
void RbCheck(u32 tick, const u8* ram, const std::vector<u8>& original)
{
  Rollback& r = s_rb;
  ++r.checks;
  u32 differing = 0;
  std::vector<std::pair<u32, u32>> ranges;  // coalesced, for the log
  // Dirty tracking: pages not written since `original` equalled RAM can't differ.
  DirtyTracker& d = s_dirty;
  std::optional<u32> since;
  if (d.on)
  {
    d.Sample();
    since = d.Synced(original);
  }
  for (const auto& [lo, hi] : r.segments)
  {
    for (u32 g = lo; g < hi; g += RB_GRANULE)
    {
      if (!RollbackLogic::PageNeedsCopy(d.stamp[g / DirtyTracker::PAGE], since))
      {
        g = std::min(hi, (g / DirtyTracker::PAGE + 1) * DirtyTracker::PAGE) - RB_GRANULE;
        continue;
      }
      const u32 len = std::min(RB_GRANULE, hi - g);
      if (std::memcmp(ram + g, original.data() + g, len) == 0)
        continue;
      ++differing;
      ++r.granule_hits[g / RB_GRANULE];
      if (!ranges.empty() && ranges.back().second == g)
        ranges.back().second = g + len;
      else
        ranges.emplace_back(g, g + len);
    }
  }
  // Fake VMEM: counted with the rest, reported by its own address range.
  u32 vmem_differing = 0, vmem_first = 0;
  if (r.vmem_bytes && original.size() >= r.mem1_size + r.vmem_bytes)
  {
    for (u32 g = 0; g < r.vmem_bytes; g += RB_GRANULE)
    {
      if (!RollbackLogic::PageNeedsCopy(d.stamp[d.mem1_pages + g / DirtyTracker::PAGE], since))
        continue;
      const u32 len = std::min(RB_GRANULE, r.vmem_bytes - g);
      if (std::memcmp(r.vmem + g, original.data() + r.mem1_size + g, len) == 0)
        continue;
      if (vmem_differing++ == 0)
        vmem_first = 0x7E000000 + g;
    }
    if (vmem_differing && r.logged_mismatches < 20)
      Log("re-sim mismatch before tick {}: fake VMEM {} granules from {:08x}", tick,
          vmem_differing, vmem_first);
  }
  differing += vmem_differing;
  if (differing == 0)
    return;
  ++r.mismatched_checks;
  if (r.logged_mismatches < 20)
  {
    ++r.logged_mismatches;
    std::string list;
    for (size_t i = 0; i < ranges.size() && i < 12; ++i)
      list += fmt::format(" {:08x}-{:08x}", 0x80000000 | ranges[i].first, 0x80000000 | ranges[i].second);
    Log("re-sim mismatch before tick {}: {} granules in {} ranges:{}{}", tick, differing,
        ranges.size(), list, ranges.size() > 12 ? " ..." : "");
  }
}

void RbReport(size_t limit)
{
  Rollback& r = s_rb;
  Log("rollback summary: live ticks {}, rollbacks {}, re-simulated ticks {}, checks {}, "
      "mismatched {}",
      r.live, r.rollbacks, r.resim_ticks, r.checks, r.mismatched_checks);
  // Coalesce granules into ranges, ordered by how often they differed.
  std::vector<std::tuple<u32, u32, u32>> ranges;  // lo, hi, max hits
  for (const auto& [g, hits] : r.granule_hits)
  {
    const u32 lo = g * RB_GRANULE;
    if (!ranges.empty() && std::get<1>(ranges.back()) == lo)
    {
      std::get<1>(ranges.back()) = lo + RB_GRANULE;
      std::get<2>(ranges.back()) = std::max(std::get<2>(ranges.back()), hits);
    }
    else
    {
      ranges.emplace_back(lo, lo + RB_GRANULE, hits);
    }
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const auto& a, const auto& b) { return std::get<2>(a) > std::get<2>(b); });
  for (size_t i = 0; i < ranges.size() && i < limit; ++i)
  {
    const auto& [lo, hi, hits] = ranges[i];
    Log("  differs {:08x}-{:08x} ({} bytes) in {} checks", 0x80000000 | lo, 0x80000000 | hi,
        hi - lo, hits);
  }
}

u64 RbHash(const u8* ram)
{
  u64 h = 1469598103934665603ULL;
  for (const auto& [lo, hi] : s_rb.segments)
  {
    for (u32 off = lo; off + 8 <= hi; off += 8)
    {
      u64 w;
      std::memcpy(&w, ram + off, 8);
      h = (h ^ w) * 1099511628211ULL;
    }
  }
  for (u32 off = 0; off + 8 <= s_rb.vmem_bytes; off += 8)
  {
    u64 w;
    std::memcpy(&w, s_rb.vmem + off, 8);
    h = (h ^ w) * 1099511628211ULL;
  }
  return h;
}

void RbWritePads(const Core::CPUThreadGuard& guard, u32 buf, const Pads& pads)
{
  for (u32 port = 0; port < MAX_PLAYERS; ++port)
  {
    for (u32 i = 0; i < PAD_SIZE; ++i)
      PowerPC::MMU::HostWrite<u8>(guard, pads[port][i], buf + port * PAD_SIZE + i);
  }
}

// Count granules (within segments) that differ between RAM and `before`.
void RbCountWrites(const u8* ram, const std::vector<u8>& before, std::vector<u32>& counts)
{
  const u32 gran = s_rb.pgran;
  for (const auto& [lo, hi] : s_rb.segments)
  {
    for (u32 g = lo - lo % gran; g < hi; g += gran)
    {
      const u32 a = std::max(g, lo), b = std::min(g + gran, hi);
      if (std::memcmp(ram + a, before.data() + a, b - a) != 0)
        ++counts[g / gran];
    }
  }
}

void RbWriteProfile()
{
  Rollback& r = s_rb;
  FILE* f = std::fopen(r.profile_path.c_str(), "w");
  if (!f)
    return;
  std::fprintf(f, "# NSR rollback write profile after %u ticks: lo hi inside outside "
               "(%u-byte granules coalesced while both counts stay equal)\n", r.live, r.pgran);
  u32 g = 0;
  const u32 n = static_cast<u32>(r.inside.size());
  while (g < n)
  {
    if (r.inside[g] == 0 && r.outside[g] == 0)
    {
      ++g;
      continue;
    }
    u32 end = g + 1;
    while (end < n && r.inside[end] == r.inside[g] && r.outside[end] == r.outside[g])
      ++end;
    std::fprintf(f, "%08x %08x %u %u\n", 0x80000000u | (g * r.pgran),
                 0x80000000u | (end * r.pgran), r.inside[g], r.outside[g]);
    g = end;
  }
  std::fclose(f);
}

// Per-session RNG seed: install it once, at the first poll, which happens at
// the same emulated instant on every machine (before any match loads and
// GRand re-seeds from it).
void InstallSeedOnce(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  if (r.seed_written)
    return;
  r.seed_written = true;
  if (const auto value = ConfigForRollback().seed)
    PowerPC::MMU::HostWrite<u32>(guard, *value, SEED_ADDRESS);
}
}  // namespace

long long DiscLatency(long long ticks)
{
  return ConfigForRollback().instant_disc && FrameDrivenLifecycleActive() ? 0 : ticks;
}

void NoteDiscCommand()
{
  ++s_rb.disc_commands;
}

void LogMemHit(u32 pc, u32 lr, bool write, size_t size, u64 value, u32 addr)
{
  if (!s_session.log)
    return;
  Log("MBP live {} cursor {} rolled {}: pc {:08x} lr {:08x} {}{} {:x} at {:08x}", s_rb.live,
      s_rb.cursor, s_rb.rolled, pc, lr, write ? "write" : "read", size * 8, value, addr);
}

bool IsEnabled()
{
  return ConfigForRollback().nsr_enabled;
}

bool ValidateRollbackImage(Core::System& system)
{
  // tools/isopatch.py verifies the GN7E69-r0 originals before writing this
  // footprint.  Treat the complete footprint as the patch signature: a
  // mismatch rejects every invasive NSR hook rather than installing a
  // partial fixed-address set into an unknown executable.
  constexpr std::pair<u32, u32> signature[] = {
      {0x80416900, 0x4E800020}, {0x80416904, 0x4E800020}, {0x80416908, 0x4E800020},
      {0x8041690C, 0x4E800020}, {0x80416910, 0x4E800020}, {0x80416914, 0x4E800020},
      {0x80416918, 0x4E800020}, {0x80026B64, 0x483EFF1D}, {0x80198924, 0x4827DFE9},
      {0x801E271C, 0x48234245}, {0x801E2720, 0x60000000}, {0x801E2724, 0x60000000},
      {0x801E2728, 0x60000000}, {0x801E272C, 0x60000000}, {0x801E2730, 0x60000000},
      {0x80028654, 0x483EE3BD}, {0x8002865C, 0x60000000}, {0x80416A80, 0x9421FFE0},
      {0x80416A84, 0x7C0802A6}, {0x80416A88, 0x90010024}, {0x80416A8C, 0x93E1001C},
      {0x80416A90, 0x4BFFFE85}, {0x80416A94, 0x7C7F1B78}, {0x80416A98, 0x4BDE135D},
      {0x80416A9C, 0x4BC10035}, {0x80416AA0, 0x4BFFFE79}, {0x80416AA4, 0x73E00001},
      {0x80416AA8, 0x4082FFE8}, {0x80416AAC, 0x83E1001C}, {0x80416AB0, 0x80010024},
      {0x80416AB4, 0x7C0803A6}, {0x80416AB8, 0x38210020}, {0x80416ABC, 0x4E800020},
  };
  const auto& memory = system.GetMemory();
  for (const auto& [address, expected] : signature)
  {
    const u32 actual = memory.Read_U32(address);
    if (actual != expected)
    {
      Log("rollback hook signature mismatch at {:08x}: expected {:08x}, got {:08x}; hooks disabled",
          address, expected, actual);
      return false;
    }
  }
  return true;
}

// Run milestone-1 raw-pad lockstep for one poll.  Keeping this independent
// from either game-side stub lets the rollback image use the same proven
// ordering in menus: PADRead -> agreement -> Reborn remap/PADClamp.
static bool LockstepExchange(const Core::CPUThreadGuard& guard, u32 buf, u32 frame)
{
  auto& system = guard.GetSystem();
  InstallSeedOnce(guard);
  if (!ConfigForRollback().netplay_enabled)
    return false;  // seed-only mode (offline practice)

  Session& s = s_session;
  if (!s.initialized)
    s.enabled = Setup();
  if (!s.enabled || s.failed)
    return false;
  if (!AllConnected() && !WaitForConnection())
    return false;

  // The pad poll can run more than once for the same retrace (the game also
  // polls once directly at init). Re-apply what was agreed for that frame.
  if (auto it = s.applied.find(frame); it != s.applied.end())
  {
    WritePads(guard, buf, it->second);
    return true;
  }

  // 1. Our controller is always local port 0 (raw, straight from PADRead;
  //    remap/PADClamp run in-game on the agreed pads afterwards), or the test
  //    script if one is loaded. Record it for frame + delay.
  Pad local;
  if (const auto scripted = ScriptPad(guard, frame))
  {
    local = *scripted;
  }
  else
  {
    for (u32 i = 0; i < PAD_SIZE; ++i)
      local[i] = PowerPC::MMU::HostRead<u8>(guard, buf + i);
  }
  local[10] = 0;  // err: connected

  // The first `delay` frames (from the first hook call, identical on every
  // machine) use neutral pads. Every target frame from the last recorded one
  // up to frame + delay gets a pad, so a retrace that skips the poll can't
  // leave a hole a peer would wait on forever.
  if (!s.started)
  {
    s.started = true;
    s.first_frame = frame;
    s.next_target = frame;
    for (u32 i = 0; i < s.players; ++i)
    {
      for (u32 f = frame; f < frame + s.delay; ++f)
        s.remote_pads[i].try_emplace(f, NeutralPad());
    }
  }
  for (u32 t = s.next_target; t <= frame + s.delay; ++t)
    s.local_pads.try_emplace(t, t < s.first_frame + s.delay ? NeutralPad() : local);
  s.next_target = std::max(s.next_target, frame + s.delay + 1);

  // 2. Send, then block (host-side, no emulated time) until every other
  //    player's pad for this frame is in.
  SendLatest(frame + s.delay);
  const auto start = std::chrono::steady_clock::now();
  auto last_send = start;
  while (!HaveAllPads(frame))
  {
    Service(1);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_send > std::chrono::milliseconds(8))
    {
      SendLatest(frame + s.delay);  // cover packet loss
      last_send = now;
    }
    // Until the first frame completes, allow the full connect window: in a
    // star, a player can't see whether the others have joined the hub yet.
    const int limit_ms = s.exchanged_once ? PEER_TIMEOUT_MS : CONNECT_TIMEOUT_MS;
    if (s.failed || now - start > std::chrono::milliseconds(limit_ms))
    {
      Log("a player went silent at frame {}", frame);
      s.failed = true;
      StopForRollbackTransportFailure();
      return false;
    }
  }

  s.exchanged_once = true;
  if (s.star && s.me == 0)
    enet_host_flush(s.host);  // push relayed packets out promptly

  // 3. Agree on every player's pad and write them to ports 0..players-1.
  std::array<Pad, MAX_PLAYERS> pads{};
  for (u32 i = 0; i < s.players; ++i)
    pads[i] = i == s.me ? s.local_pads.at(frame) : s.remote_pads[i].at(frame);
  s.applied.emplace(frame, pads);
  WritePads(guard, buf, pads);

  // 4. Periodic desync check: hash MEM1 after the agreed pads are in the
  //    buffer (same emulated instant on every machine).
  if (frame % CHECKSUM_INTERVAL == 0 && frame > 0 && s_rb.session_generation == 0)
  {
    s.local_hashes[frame] = HashMem1(system);
    s.last_sent_hash_frame = frame;
    if (!Env("NSR_DUMP_PREFIX").empty() && !s.dumped)
    {
      auto& memory = system.GetMemory();
      s.snapshots[frame].assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSizeReal());
      while (s.snapshots.size() > 4)
        s.snapshots.erase(s.snapshots.begin());
    }
    for (u32 i = 0; i < s.players; ++i)
    {
      if (i != s.me)
        CheckHash(i, frame);
    }
  }

  // 5. Trim history.
  if (frame > 600)
  {
    const u32 cutoff = frame - 600;
    const auto old = [&](const auto& kv) { return kv.first < cutoff; };
    std::erase_if(s.local_pads, old);
    for (auto& m : s.remote_pads)
      std::erase_if(m, old);
    std::erase_if(s.applied, old);
    std::erase_if(s.local_hashes, old);
    for (auto& m : s.remote_hashes)
      std::erase_if(m, old);
  }
  if (frame % 600 == 0)
    Log("frame {} ok (desyncs so far: {})", frame, s.desyncs);
  return true;
}

void Hook(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();
  const u32 buf = ppc_state.gpr[3];
  const u32 frame = ppc_state.gpr[4];
  ppc_state.npc = LR(ppc_state);  // Replace hook: return to caller
  LockstepExchange(guard, buf, frame);
}

void RollbackPollHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  const u32 scratch = ppc_state.gpr[3];
  const u32 retrace = ppc_state.gpr[4];
  const u32 phase = ppc_state.gpr[5];
  ppc_state.npc = LR(ppc_state);
  InstallSeedOnce(guard);
  Rollback& r = s_rb;
  if (!r.initialized)
    RbInit(system);

  RbSampleIo(guard);
  if (phase == 0)
  {
    // Automatic stage-4 sessions use the milestone-1 exchange outside the
    // match.  It runs before Reborn's remap so different local controllers
    // cannot change guest instruction timing or menu state.
    if (r.auto_handoff && !r.vtime_on && !r.menu_frame_lockstep)
    {
      LockstepExchange(guard, scratch, retrace);
      return;
    }
    // Raw pads, before Reborn's remap: the test script replaces port 0.
    if (auto scripted = ScriptPad(guard, retrace))
    {
      (*scripted)[10] = 0;  // err: connected
      for (u32 i = 0; i < PAD_SIZE; ++i)
        PowerPC::MMU::HostWrite<u8>(guard, (*scripted)[i], scratch + i);
    }
    return;
  }

  // Processed pads: the local sample for the next live tick. The game's pad
  // buffer gets them now, as in the unpatched poll, unless ticks are being
  // re-simulated (then each re-simulated tick has its own pads in there).
  for (u32 port = 0; port < MAX_PLAYERS; ++port)
  {
    for (u32 i = 0; i < PAD_SIZE; ++i)
      r.latest[port][i] = PowerPC::MMU::HostRead<u8>(guard, scratch + port * PAD_SIZE + i);
  }
  // While the game is ticking (a match), the pad buffer belongs to the
  // ticks: each tick writes its own pads, and UI code that reads the buffer
  // between ticks (e.g. the play-call menu, in the frame tail) sees the last
  // tick's pads, not whatever the poll sampled at that retrace. Otherwise UI
  // input would depend on frame timing, which rollback re-simulation (and
  // every machine) changes. Two polls without a tick end tick mode (menus,
  // loading), where the poll feeds the buffer as in the unpatched game.
  if (r.ticks_since_poll > 0)
  {
    r.tick_mode = true;
    r.idle_polls = 0;
  }
  else if (r.tick_mode && ++r.idle_polls >= 2)
  {
    r.tick_mode = false;
  }
  r.ticks_since_poll = 0;
  // fix/modal: in a frame-mode session only RbStep writes the buffer (agreed
  // pads), so a long frame (Restart/Quit from the pause menu) never exposes
  // this machine's local sample to the game. See PollFeedsPadBuffer.
  if (RollbackLogic::PollFeedsPadBuffer(r.tick_mode, r.cursor == r.live, r.menu_frame_lockstep,
                                        r.vtime_on && r.frame_mode))
    RbWritePads(guard, RB_PAD_BUF, r.latest);
}

// One step of the rollback state machine, run at the start of every rollback
// unit (a sim tick, or a whole main-loop frame in frame mode) with the game's pad
// buffer address. Returns 1 if another unit must run right after this one
// (re-simulation in progress), else 0.
// Diagnostics: NSR_RB_TRANSPLANT=dump:ranges:when copies the listed ranges
// ("lo hi" per line) from a MEM1 dump into RAM right before a unit runs, to
// find which state differences matter (bisect the ranges). `when` is a live
// tick ("3637"), or "live/frame" for a re-simulated frame during that live
// tick's step ("3637/3633").
static void RbTransplant(Core::System& system, u8* ram, u32 live, u32 frame)
{
  const auto& spec = ConfigForRollback().transplant;
  if (spec.size() != 3)
    return;
  const std::string key =
      live == frame ? std::to_string(live) : std::to_string(live) + "/" + std::to_string(frame);
  if (spec[2] != key)
    return;
  std::vector<u8> dump(system.GetMemory().GetRamSizeReal());
  size_t moved = 0;
  if (FILE* f = std::fopen(spec[0].c_str(), "rb"))
  {
    const size_t n = std::fread(dump.data(), 1, dump.size(), f);
    std::fclose(f);
    if (FILE* rf = n == dump.size() ? std::fopen(spec[1].c_str(), "r") : nullptr)
    {
      char line[128];
      while (std::fgets(line, sizeof(line), rf))
      {
        unsigned long lo = 0, hi = 0;
        if (std::sscanf(line, "%lx %lx", &lo, &hi) != 2)
          continue;
        lo &= 0x01FFFFFF;
        hi = std::min<unsigned long>(hi & 0x01FFFFFF, static_cast<unsigned long>(dump.size()));
        if (hi > lo)
        {
          std::memcpy(ram + lo, dump.data() + lo, hi - lo);
          moved += hi - lo;
        }
      }
      std::fclose(rf);
    }
  }
  Log("transplanted {} bytes before {}", moved, key);
}

// ---------------------------------------------------------------------------
// Per-machine sidecar: next to every MEM1 dump (NSR_RB_DUMP_AT,
// NSR_RB_DUMP_UNIT), <dump>.permachine lists memory that follows this
// machine's own emulated time and is never read by the sim, found
// structurally at dump time, so NFLStreet tools/rbdiff.py can compare dumps
// of any mode (its fixed Quick Play addresses were wrong in Demo Mode, where
// the same audio objects sit elsewhere in the heap). Lines: "lo hi kind",
// virtual addresses, [lo, hi). Kinds:
//   audio-heap   heap blocks handed to audio code (AllocHook), header included;
//   os-live      RbLiveRanges(all): non-main threads (OSThread + stack, incl.
//                the disc worker and the audio thread), thread queues and
//                mutexes of every thread, alarms, DVD command blocks, ARQ
//                requests, the audio callback-list nodes, the tick
//                accumulator, disc staging/descriptor, disc stream
//                destinations, the GX FIFO (CP registers).
//   io-object, audio-data, heap-counters, heap-free: see the writer below.
// Static per-machine words (.data/.bss, the same in every mode) stay in
// rbdiff's own list.
// Format v2 (first line "# nsr-permachine v2"; final review M3) adds the
// complete heap block map ("#heap"/"#block" records, see WalkHeap), so rbdiff
// can exclude a per-machine block only where both dumps agree on its
// boundaries and owner and report an allocation-ownership difference instead
// of hiding it. Older sidecars lack the map; rbdiff still reads them (union
// semantics) and says the comparison has reduced confidence.
// ---------------------------------------------------------------------------
namespace
{
// Code whose allocations are audio state: the audio middleware, the game's
// sound layer and stream source (fn_801F3000..), PathFinder music; plus
// anything allocated on the audio thread.
constexpr std::array<std::pair<u32, u32>, 5> AUDIO_ALLOC_OWNERS{{
    {0x80263000, 0x80285A90},  // audio middleware text
    {0x801F3000, 0x801F7800},  // sound layer, stream source
    {0x801B1000, 0x801B9200},  // PathFinder
    // fn_801AA220: 0x180-byte buffers for world objects' sounds, sample data
    // with a word (+0x40) that changes with audio timing; written by DMA (a
    // memory watch on it never fires), not by game code.
    {0x801AA220, 0x801AA24C},
    // fn_801E10F0: the audio system's allocator object (EA IAllocator method,
    // reached only through its vtable) used by the game's sound code and
    // PathFinder/AEMS for their objects and handles.
    {0x801E10F0, 0x801E1168},
}};
// Allocator internals (GN7E69): fn_801D28A8 does the work; its three callers
// resume at these addresses with the payload in r3. Heap block header: 16
// bytes before the payload, next block's payload - 0x10 at payload - 4.
constexpr std::array<u32, 3> ALLOC_RETURNS{0x801D2BA0, 0x801D2BC0, 0x801D300C};
constexpr u32 FREE_FN = 0x801D2BD0;

bool AudioOwnedAllocation(const Core::CPUThreadGuard& guard, const PowerPC::PowerPCState& ppc)
{
  using PowerPC::MMU;
  if (MMU::HostRead<u32>(guard, 0x800000E4) == AUDIO_THREAD)
    return true;
  auto in_owner = [](u32 a) {
    return std::any_of(AUDIO_ALLOC_OWNERS.begin(), AUDIO_ALLOC_OWNERS.end(),
                       [a](const auto& r) { return a >= r.first && a < r.second; });
  };
  // Return addresses up the stack (EABI back chain: the LR save word of a
  // frame is at back_chain + 4).
  u32 sp = ppc.gpr[1];
  for (int i = 0; i < 16; ++i)
  {
    const u32 next = MMU::HostRead<u32>(guard, sp);
    if ((next >> 24) != 0x80 || next <= sp)
      break;
    if (in_owner(MMU::HostRead<u32>(guard, next + 4)))
      return true;
    sp = next;
  }
  return false;
}

// Payload end of an allocated block: its header's last word is the address
// of the next block's header (the payload runs right up to it).
u32 BlockEnd(const u8* ram, u32 p)
{
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  const u32 next = rd(p - 4);
  if ((next >> 24) == 0x80 || (next >> 24) == 0x81)
    if (next > p && next - p < 0x01000000)
      return next;
  return 0;
}
}  // namespace

// HookType::Start at ALLOC_RETURNS: record audio-owned allocations.
void AllocHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  const u32 p = ppc.gpr[3];
  if ((p >> 24) != 0x80 && (p >> 24) != 0x81)
    return;
  // Diagnostics: NSR_RB_ALLOC_TRACE=lo-hi logs every allocation overlapping
  // that range with its return-address chain (who owns a block?).
  static const std::pair<u32, u32> trace = [] {
    const auto parts = Split(Env("NSR_RB_ALLOC_TRACE"), '-');
    if (parts.size() == 2)
      return std::pair<u32, u32>{ParseU32(parts[0]).value_or(0), ParseU32(parts[1]).value_or(0)};
    return std::pair<u32, u32>{0, 0};
  }();
  if (trace.second)
  {
    const u32 e = BlockEnd(system.GetMemory().GetRAM(), p);
    if (p < trace.second && (e > trace.first || p >= trace.first))
    {
      std::string chain;
      u32 sp = ppc.gpr[1];
      for (int i = 0; i < 12; ++i)
      {
        const u32 next = PowerPC::MMU::HostRead<u32>(guard, sp);
        if ((next >> 24) != 0x80 || next <= sp)
          break;
        chain += fmt::format(" {:08x}", PowerPC::MMU::HostRead<u32>(guard, next + 4));
        sp = next;
      }
      Log("alloc {:08x}..{:08x} thread {:08x} frame {} audio {}:{}", p, e,
          PowerPC::MMU::HostRead<u32>(guard, 0x800000E4), s_rb.live,
          AudioOwnedAllocation(guard, ppc), chain);
    }
  }
  if (!AudioOwnedAllocation(guard, ppc))
    return;
  // The header's next link may not be final yet (e.g. the block was carved
  // from the end of the free area): then the extent is read at dump time.
  const u32 end = BlockEnd(system.GetMemory().GetRAM(), p);
  s_audio_blocks[p] = end;
  if (const u32 heap = PowerPC::MMU::HostRead<u32>(guard, p - 8);
      heap >= 0x80003100 && heap < 0x817FFFF0 && PowerPC::MMU::HostRead<u32>(guard, heap + 4) == heap)
  {
    s_audio_heaps.insert(heap);
  }
  s_alloc_log.push_back({++s_alloc_seq, true, p, end});
}

// HookType::Start at FREE_FN (r3 = payload).
void FreeHook(const Core::CPUThreadGuard& guard)
{
  const u32 p = guard.GetSystem().GetPPCState().gpr[3];
  const auto it = s_audio_blocks.find(p);
  if (it == s_audio_blocks.end())
    return;
  s_alloc_log.push_back({++s_alloc_seq, false, p, it->second});
  s_audio_blocks.erase(it);
}

std::vector<unsigned int> AllocHookAddresses()
{
  return {ALLOC_RETURNS.begin(), ALLOC_RETURNS.end()};
}

namespace
{
// Disc/stream I/O objects (the game's stream source and disc worker: file
// readers, read requests, the worker's buffers) are allocated at boot, before
// the allocator watch is installed, and are shared with game loads, so they
// are found from their static roots instead: words in .data/.sbss that point
// at (or into) them. Measured in Quick Play; the roots are fixed addresses in
// the DOL, the blocks move with the mode.
constexpr std::array<u32, 5> IO_ROOT_WORDS{0x803EBC24, 0x803EBC28, 0x803ED628, 0x803D55E8, 0x803D55EC};
constexpr std::pair<u32, u32> IO_ROOT_TABLE{0x802C8840, 0x802C89E0};  // stream source request table

// The allocated heap block containing p: scan back for a header whose heap
// descriptor word points at a descriptor that points at itself (+4), whose
// "used" byte is set and whose next-header word lies beyond p. [header, next).
std::optional<std::pair<u32, u32>> EnclosingBlock(const u8* ram, u32 p)
{
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  auto ptr = [](u32 a) { return a >= 0x80003100 && a < 0x817FFFF0; };  // (reads up to +0xC stay in MEM1)
  if (!ptr(p))
    return std::nullopt;
  for (u32 h = (p & ~3u) - 0x10, n = 0; ptr(h) && n < 0x10000; h -= 4, ++n)
  {
    const u32 heap = rd(h + 8), next = rd(h + 0xC);
    if (ptr(heap) && rd(heap + 4) == heap && (rd(h + 4) & 0xFF00) == 0x0100 && next > p &&
        ptr(next) && next - h < 0x00400000)
      return std::pair<u32, u32>{h, next};
  }
  return std::nullopt;
}

// --- Heap block map (sidecar v2; final review M3) ------------------------------
// The per-machine heap ranges above say what THIS dump thinks is audio/I/O
// memory. Comparing two dumps by the union of those claims let one dump's
// "audio" label hide the other dump's gameplay object at the same address
// (an allocation-ownership divergence). So the sidecar also records every
// block of every heap with its owner, and rbdiff excludes a per-machine block
// only when both dumps agree on it, reporting any owner/boundary difference.
//
// Allocator layout (GN7E69, fn_801D28A8 family), from dumps: a block starts
// with a back link (absent in a heap's first block), then a word whose high
// half `off` is the header length counted from that word: the flags word
// (used byte, mask 0xFF00) is at +off-12, the heap descriptor at +off-8, the
// next block's start at +off-4 and the payload at +off. Seen: 0x0C (plain
// block, the classic 16-byte header), 0x10, 0x1C (aligned block with a
// 16-byte pre-header). A heap's chain ends with a 0x14-byte sentinel block.
// Heap descriptors: +4 points at itself, +0x10 first child heap, +0x14/+0x18
// the heap's [start, end). Top-level heaps are listed in a static table of
// {start, end, flags, descriptor} entries (0x80384BC0.. in GN7E69: the ARAM
// heap, the fake-VMEM heap, then the MEM1 heaps); nested heaps live inside a
// block of their parent and are found from the descriptor's child link and
// from used blocks whose payload holds a descriptor.
// Deferred: only the first child link is followed (no sibling link has been
// seen; a second nested heap would still be found from its parent's block).
// Revisit if a sidecar's "#heap" lines ever disagree with a full-MEM1
// descriptor scan.
constexpr std::pair<u32, u32> HEAP_TABLE{0x80384B00, 0x80384D00};

struct HeapBlock
{
  u32 lo, hi, payload;
  bool used;
};

struct HeapWalk
{
  u32 desc, start, end;
  std::vector<HeapBlock> blocks;
  bool complete;  // reached the end sentinel through valid headers
};

// MEM1 virtual address with `extent` readable bytes.
bool InMem1(u32 a, u32 extent)
{
  return a >= 0x80003100 && a < 0x81800000 && extent <= 0x81800000 - a;
}

bool HeapDescriptor(const u8* ram, u32 desc)
{
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  if (!InMem1(desc, 0x1C) || rd(desc + 4) != desc)
    return false;
  const u32 start = rd(desc + 0x14), end = rd(desc + 0x18);
  return InMem1(start, 0x20) && end > start && end <= 0x81800000;
}

HeapWalk WalkHeap(const u8* ram, u32 desc)
{
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  HeapWalk w{desc, rd(desc + 0x14), rd(desc + 0x18), {}, false};
  u32 h = w.start;
  while (h + 0x20 <= w.end && w.blocks.size() < 200000)
  {
    // The class word is at +4 after a back link, or at +0 in the first block.
    u32 pos = 0, off = 0;
    for (const u32 cand : {h + 4, h})
    {
      const u32 o = InMem1(cand, 4) ? rd(cand) >> 16 : 0;
      if (o >= 0xC && o <= 0x40 && (o & 3) == 0 && InMem1(cand, o + 4) && rd(cand + o - 8) == desc)
      {
        pos = cand;
        off = o;
        break;
      }
    }
    if (!pos)
      return w;
    const u32 next = rd(pos + off - 4);
    if (next <= h || next > w.end)
      return w;
    w.blocks.push_back({h, next, pos + off, (rd(pos + off - 12) & 0xFF00) != 0});
    h = next;
  }
  w.complete = h + 0x20 > w.end;
  return w;
}

std::vector<HeapWalk> WalkAllHeaps(const u8* ram)
{
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  std::vector<u32> todo;
  for (u32 e = HEAP_TABLE.first; e < HEAP_TABLE.second; e += 16)
  {
    const u32 desc = rd(e + 12);
    if (HeapDescriptor(ram, desc) && rd(desc + 0x14) == rd(e) && rd(desc + 0x18) == rd(e + 4))
      todo.push_back(desc);
  }
  std::set<u32> seen;
  std::vector<HeapWalk> heaps;
  while (!todo.empty() && heaps.size() < 64)
  {
    const u32 desc = todo.back();
    todo.pop_back();
    if (!seen.insert(desc).second)
      continue;
    HeapWalk w = WalkHeap(ram, desc);
    if (const u32 child = rd(desc + 0x10); HeapDescriptor(ram, child))
      todo.push_back(child);
    for (const HeapBlock& b : w.blocks)
    {
      if (!b.used)
        continue;
      for (const u32 c : {b.payload, b.payload + 0x10})
      {
        if (c + 0x1C < b.hi && HeapDescriptor(ram, c) && rd(c + 0x14) >= b.lo &&
            rd(c + 0x18) <= b.hi)
          todo.push_back(c);
      }
    }
    heaps.push_back(std::move(w));
  }
  std::sort(heaps.begin(), heaps.end(),
            [](const HeapWalk& a, const HeapWalk& b) { return a.start < b.start; });
  return heaps;
}
}  // namespace

// Boot-time disc/stream I/O objects: the heap blocks enclosing what the I/O
// root words point at now (block header -> next header, virtual).
static std::map<u32, u32> IoObjectBlocks(const u8* ram)
{
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  std::vector<u32> roots(IO_ROOT_WORDS.begin(), IO_ROOT_WORDS.end());
  for (u32 a = IO_ROOT_TABLE.first; a < IO_ROOT_TABLE.second; a += 4)
    roots.push_back(a);
  std::map<u32, u32> io;
  for (const u32 root : roots)
  {
    if (const auto b = EnclosingBlock(ram, rd(root)))
      io[b->first] = b->second;
  }
  return io;
}

// Audio data blocks the allocator watch cannot attribute (loaded by the
// game's file loader, e.g. PathFinder project data 'PFDx', whose variables
// the audio thread updates): heap blocks that an audio-heap block points into
// and whose payload starts with a known audio data magic.  [header, next).
static std::map<u32, u32> AudioDataBlocks(const u8* ram)
{
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  constexpr std::array<u32, 1> AUDIO_DATA_MAGICS{0x50464478};  // 'PFDx'
  std::map<u32, u32> data;
  for (const auto& [p, recorded] : s_audio_blocks)
  {
    const u32 end = BlockEnd(ram, p) ? BlockEnd(ram, p) : recorded;
    for (u32 a = p; a + 4 <= end && a < p + 0x40000; a += 4)
    {
      const u32 q = rd(a);
      if (q >= p && q < end)
        continue;
      // Only pointers to a block's payload start (O(1) header check).
      if (q < 0x80003110 || q >= 0x817FFFF0 || (q & 3) || data.contains(q - 0x10) ||
          std::find(AUDIO_DATA_MAGICS.begin(), AUDIO_DATA_MAGICS.end(), rd(q)) == AUDIO_DATA_MAGICS.end())
        continue;
      const u32 heap = rd(q - 8), next = rd(q - 4);
      if (heap >= 0x80003100 && heap < 0x817FFFF0 && rd(heap + 4) == heap && next > q &&
          next < 0x81800000 && next - q < 0x00400000)
        data[q - 0x10] = next;
    }
  }
  return data;
}

// The frontend's UI command scratch buffer (GN7E69): fn_80189F68 computes
// its size with fn_802188A0, allocates it, initializes its command arrays in
// fn_802188D4, and stores the payload at r13-0x6C18.  Render traversal writes
// transient clipping/primitive data here; at confirmed frame 8260 two honest
// peers differed only at 0x80B8BD54..5C in this block while their heap maps,
// RNG, semantic state, and later whole-RAM dumps agreed.  Classify the block
// through its fixed root instead of its observed Quick Play address so a
// later session may move it.  The range list itself is hashed, and sidecar v2
// ownership compares its boundaries, so a real allocation-layout divergence
// remains visible.
constexpr u32 UI_RENDER_SCRATCH_ROOT = 0x803E9688;  // -0x6C18(r13)

static std::map<u32, u32> UiRenderBlocks(const u8* ram)
{
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  std::map<u32, u32> blocks;
  if (const auto block = EnclosingBlock(ram, rd(UI_RENDER_SCRATCH_ROOT)))
    blocks[block->first] = block->second;
  return blocks;
}

// One per-machine range of the classification below: virtual [lo, hi) and
// its sidecar kind (a string literal).
struct PerMachineRange
{
  u32 lo, hi;
  const char* kind;
};

// The dynamic per-machine classification of the state in `ram` (dumps'
// .permachine sidecars and the canonical state digest share it, so the
// netplay oracle and the runtime desync check exclude the same bytes).  In
// sidecar order: audio heap blocks, live OS objects (every thread, alarm, DVD
// and ARQ request, audio callback node, tick accumulator, disc staging/stream
// destinations, GX FIFO), boot-time disc/stream I/O objects, PathFinder audio
// data blocks, the frontend UI render scratch, then each audio-touched heap's
// counters and free blocks.
// `io_blocks` / `audio_data` replace the I/O objects / audio data blocks found
// from the current state, and `with_heaps` = false leaves out the heap
// counters/free blocks (the state digest passes its fixed sets and applies a
// superset heap rule; see DigestExclusions).
static std::vector<PerMachineRange> PerMachineRanges(Core::System& system, const u8* ram,
                                                     const std::map<u32, u32>* io_blocks = nullptr,
                                                     const std::map<u32, u32>* audio_data = nullptr,
                                                     bool with_heaps = true)
{
  std::vector<PerMachineRange> out;
  for (const auto& [p, recorded] : s_audio_blocks)
  {
    // The header's links can change as neighbours are allocated/freed; the
    // block's own extent is fixed while it is allocated.
    const u32 end = BlockEnd(ram, p) ? BlockEnd(ram, p) : recorded;
    if (!end)
      continue;
    out.push_back({p - 0x10, end, "audio-heap"});
  }
  for (const auto& [lo, hi] : RbLiveRanges(system, ram, true))
    out.push_back({0x80000000 | lo, 0x80000000 | hi, "os-live"});
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  for (const auto& [lo, hi] : io_blocks ? *io_blocks : IoObjectBlocks(ram))
    out.push_back({lo, hi, "io-object"});
  for (const auto& [lo, hi] : audio_data ? *audio_data : AudioDataBlocks(ram))
    out.push_back({lo, hi, "audio-data"});
  for (const auto& [lo, hi] : UiRenderBlocks(ram))
    out.push_back({lo, hi, "ui-render"});
  // Heaps that hold audio blocks also hold game blocks (e.g. the small heap
  // 0x804C4810 the audio allocator object fn_801E10F0 uses is shared with UI
  // text), and audio code allocates and frees there on audio time. Its effect
  // on such a heap is bookkeeping, not game state: the descriptor's block
  // counters (+0x24..+0x30) and the stale bytes in free blocks (rbdiff only
  // ignores a free range where it is free in both dumps). Free blocks
  // are found by walking the heap's block chain from its start (+0x14) to its
  // end (+0x18) through each header's next link; a header whose "used" byte
  // is clear is free.
  if (!with_heaps)
    return out;
  std::set<u32> heaps;
  for (const auto& [p, recorded] : s_audio_blocks)
  {
    const u32 heap = rd(p - 8);
    if (heap >= 0x80003100 && heap < 0x817FFFF0 && rd(heap + 4) == heap)
      heaps.insert(heap);
  }
  for (const u32 heap : heaps)
  {
    out.push_back({heap + 0x24, heap + 0x30, "heap-counters"});
    const u32 start = rd(heap + 0x14), end = rd(heap + 0x18);
    u32 h = start;
    for (int n = 0; n < 200000 && h >= start && h + 0x20 <= end && h < 0x817FFFF0; ++n)
    {
      // Aligned blocks (size class 0x1C) start with a 16-byte pre-header
      // {back link, 0x001C0000, 0, 0}; their real header follows it.
      u32 hdr = h;
      if (rd(h + 8) != heap && rd(h + 0xC) == 0 && (rd(h + 4) & 0xFFFF0000) == 0x001C0000)
        hdr = h + 0x10;
      const u32 next = rd(hdr + 0xC);
      if (rd(hdr + 8) == heap && (rd(hdr + 4) & 0xFF00) == 0 && next > hdr + 0x10 && next <= end)
        out.push_back({hdr + 0x10, next, "heap-free"});
      if (next <= h || next > end)
        break;
      h = next;
    }
  }
  return out;
}

static void WritePerMachineSidecar(Core::System& system, const u8* ram, const std::string& dump)
{
  FILE* f = std::fopen((dump + ".permachine").c_str(), "w");
  if (!f)
    return;
  std::fprintf(f, "# nsr-permachine v2: per-machine ranges at dump time: lo hi kind (virtual, "
                  "[lo, hi)); heap block map in '#heap'/'#block' lines; "
                  "NSRNetplay WritePerMachineSidecar\n");
  size_t blocks = 0;
  for (const PerMachineRange& range : PerMachineRanges(system, ram))
  {
    std::fprintf(f, "%08x %08x %s\n", range.lo, range.hi, range.kind);
    if (std::strcmp(range.kind, "audio-heap") == 0)
      ++blocks;
  }
  // Heap block map (v2, see WalkHeap). Written as '#'-prefixed records so a
  // pre-v2 rbdiff, which treats every non-comment line as per-machine memory,
  // keeps its old behaviour instead of excluding game blocks:
  //   #heap  desc start end blocks complete|partial
  //   #block lo hi used|free owner
  // lo/hi are the block's own boundaries (back link .. next block); owner is
  // the per-machine kind above for the block whose header is listed there
  // (payload - 0x10), "game" for any other used block, "-" when free.
  std::map<u32, const char*> owner;  // payload - 0x10 -> kind
  for (const auto& [lo, hi] : IoObjectBlocks(ram))
    owner[lo] = "io-object";
  for (const auto& [lo, hi] : AudioDataBlocks(ram))
    owner[lo] = "audio-data";
  for (const auto& [lo, hi] : UiRenderBlocks(ram))
    owner[lo] = "ui-render";
  for (const auto& [p, recorded] : s_audio_blocks)
    owner[p - 0x10] = "audio-heap";
  size_t mapped = 0;
  for (const HeapWalk& w : WalkAllHeaps(ram))
  {
    if (w.blocks.empty())
      continue;  // not a heap after all (a self-pointing word in some object)
    std::fprintf(f, "#heap %08x %08x %08x %zu %s\n", w.desc, w.start, w.end, w.blocks.size(),
                 w.complete ? "complete" : "partial");
    for (const HeapBlock& b : w.blocks)
    {
      const auto it = b.used ? owner.find(b.payload - 0x10) : owner.end();
      std::fprintf(f, "#block %08x %08x %s %s\n", b.lo, b.hi, b.used ? "used" : "free",
                   !b.used ? "-" : it != owner.end() ? it->second : "game");
    }
    mapped += w.blocks.size();
  }
  std::fclose(f);
  Log("wrote {}.permachine ({} audio heap blocks, {} heap blocks mapped)", dump, blocks, mapped);
}

// Diagnostics: NSR_RB_DUMP_UNIT=live:frame,... writes all of MEM1 right
// before frame `frame` runs during live frame `live`'s step (live == frame
// for the live frame itself), to NSR_DUMP_PREFIX-unit-<live>-<frame>.bin.
static void RbDumpUnit(Core::System& system, const u8* ram, u32 live, u32 frame)
{
  const auto& wanted = ConfigForRollback().dump_units;
  const std::string key = std::to_string(live) + ":" + std::to_string(frame);
  if (std::find(wanted.begin(), wanted.end(), key) == wanted.end())
    return;
  const std::string path = Env("NSR_DUMP_PREFIX", "local/nsr") + "-unit-" + std::to_string(live) +
                           "-" + std::to_string(frame) + ".bin";
  if (FILE* f = std::fopen(path.c_str(), "wb"))
  {
    std::fwrite(ram, 1, system.GetMemory().GetRamSizeReal(), f);
    std::fclose(f);
    Log("wrote {}", path);
    WritePerMachineSidecar(system, ram, path);
    WriteAramRingSidecar(system, path);  // ARAM replay ring (final-review M1)
  }
  if (s_rb.vmem_bytes)
  {
    if (FILE* f = std::fopen((path + ".vmem").c_str(), "wb"))
    {
      std::fwrite(s_rb.vmem, 1, s_rb.vmem_bytes, f);
      std::fclose(f);
    }
  }
}

// Diagnostics: NSR_RB_ADOPT=dump:frame makes this run adopt another run's
// game state right before live frame `frame` runs (taken there with
// NSR_RB_DUMP_UNIT=frame:frame; dump.vmem is used too if present), the way a
// rollback restore does (live OS objects and kept ranges stay). Two runs
// with different emulated timing (e.g. CPU overclock) then start the session
// from identical game state, like netplay peers after a lockstep phase, and
// can be compared frame by frame inside the session.
static void RbAdopt(Core::System& system, u8* ram, u32 live)
{
  const auto& spec = ConfigForRollback().adopt;
  if (spec.size() != 2 || ParseU32(spec[1]).value_or(0xFFFFFFFF) != live)
    return;
  std::vector<u8> slot(s_rb.mem1_size + s_rb.vmem_bytes);
  size_t got = 0, got_vmem = 0;
  if (FILE* f = std::fopen(spec[0].c_str(), "rb"))
  {
    got = std::fread(slot.data(), 1, s_rb.mem1_size, f);
    std::fclose(f);
  }
  if (s_rb.vmem_bytes)
  {
    if (FILE* f = std::fopen((spec[0] + ".vmem").c_str(), "rb"))
    {
      got_vmem = std::fread(slot.data() + s_rb.mem1_size, 1, s_rb.vmem_bytes, f);
      std::fclose(f);
    }
  }
  if (got != s_rb.mem1_size)
  {
    Log("adopt: could not read {}", spec[0]);
    return;
  }
  if (got_vmem != s_rb.vmem_bytes)
    slot.resize(s_rb.mem1_size);  // MEM1 only
  RbRestore(system, ram, slot);
  Log("adopted {} before live frame {} (io idle now {}; vmem {})", spec[0], live, IsIoIdle(ram),
      got_vmem == s_rb.vmem_bytes);
}

// Forced-rollback depth for a live unit: NSR_RB_FORCE, or with
// NSR_RB_FORCE_RANDOM=seed a pseudo-random depth in 0..NSR_RB_FORCE per unit
// (0 = no rollback), like netplay's varying misprediction depths. Two runs
// with different seeds model two peers with different rollback patterns.
static u32 ForcedDepth(u32 live)
{
  return RollbackLogic::ForcedDepth(s_rb.force, ConfigForRollback().force_random_seed, live);
}

// Misprediction mode: the pads frame t really had (the replayed recording).
static Pads TruePads(u32 t)
{
  if (const auto it = s_rb.replay.find(t); it != s_rb.replay.end())
    return it->second;
  return s_rb.latest;
}

// Hash only semantic words known to follow the deterministic game frame.
// Audio, SDK queues, card state, raw SI buffers and other per-machine ranges
// are intentionally absent.  The list is small by design: a false positive
// would make every valid rollback history look desynced.
static u64 RbSemanticChecksum(const Core::CPUThreadGuard& guard)
{
  using PowerPC::MMU;
  u64 hash = 1469598103934665603ULL;
  auto add = [&](u32 value) { hash = (hash ^ value) * 1099511628211ULL; };
  auto valid = [](u32 address) { return address >= 0x80003100 && address < 0x81800000; };

  // EA RNG streams 0/1 are the strongest current sim oracle.
  add(MMU::HostRead<u32>(guard, 0x80481888));
  add(MMU::HostRead<u32>(guard, 0x80481904));
  const u32 clock = MMU::HostRead<u32>(guard, TICK_CLOCK_PTR);
  for (u32 offset : {0u, 4u, 8u})
    add(valid(clock + offset) ? MMU::HostRead<u32>(guard, clock + offset) : 0);
  const u32 flow = MMU::HostRead<u32>(guard, 0x803EABA8);
  for (u32 offset : {0u, 0x20u, 0xBCu})
    add(valid(flow + offset) ? MMU::HostRead<u32>(guard, flow + offset) : 0);
  const u32 play = MMU::HostRead<u32>(guard, 0x803EAB88);
  add(valid(play + 0xE0) ? MMU::HostRead<u32>(guard, play + 0xE0) : 0);
  return hash;
}

// ---------------------------------------------------------------------------
// Canonical state digest (final review H4).
//
// Peers compare a digest of the deterministic game state at the start of
// each finalized frame (the frame whose inputs every peer has confirmed;
// Exchange::UpdateFinalized), and at stage-4 enter/leave barriers.  The old
// value covered nine semantic words (two RNG streams, the tick clock, flow
// and play state), so a divergence anywhere else stayed silent until it
// happened to reach one of them.
//
// What is hashed: every restored MEM1 byte (r.segments: MEM1 minus RB_KEEP,
// plus AUDIO_KEEP when audio is kept live), the snapshotted fake-VMEM prefix
// (the EA database heap) and the game's ARAM gameplay replay ring (restored
// by its undo log; GameplayAramRingHash), plus the old semantic words and the
// list of excluded ranges.  What is excluded, all from existing audited lists so the
// runtime check and the whole-RAM netplay oracle (tools/rbdiff.py) agree:
//   - RB_KEEP (never restored) and AUDIO_KEEP (rbdiff excludes it in every
//     mode: middleware .data follows each machine's audio time);
//   - rbdiff's static per-machine words (STATIC_PER_MACHINE below);
//   - the dynamic per-machine classification of the current state, exactly
//     what the dump sidecars list (PerMachineRanges: audio heap blocks, live
//     OS objects incl. the GX FIFO and disc staging, I/O objects, audio data
//     blocks, audio-touched heaps' counters and free blocks);
//   - the game's PADStatus buffer (RB_PAD_BUF): input, not state.  The frame
//     stub writes the frame's (possibly predicted) pads there before the
//     digest is taken; they are covered by the input exchange itself.
// The dynamic part is derived from the state being hashed, so peers in the
// same state exclude the same bytes; the merged list is hashed too, so a
// difference in heap ownership (final review M3) is a mismatch as well.
// Bytes are hashed as stored (guest big-endian), page hashes combined in
// host order: every supported host is little-endian.
//
// Cost: per-page XXH3-64 values are cached between digests and only pages
// the dirty tracker stamped since the cached state are rehashed, from the
// ring slot that was just saved (it equals RAM at that epoch).  Pages that
// overlap a dynamic exclusion are hashed directly each time (only their
// included pieces).  The digest itself hashes the ~6,700 page values.
// Without a dirty tracker (NSR_RB_DIRTY=0, a diagnostic setting) every page
// is rehashed on every digest.  Handoff barriers hash everything once.
//
// Deferred: hashing
// the frame's full CPU context is unnecessary: digests are taken at the frame
// stub, where the main thread's state is the same call path on every peer.
// ---------------------------------------------------------------------------
namespace
{
// Version of the exclusion rules; part of the config fingerprint, so peers
// with different rules refuse to play instead of reporting false desyncs.
constexpr u32 STATE_DIGEST_RANGES_VERSION = 4;  // 4: + audio callback's transient stream pointer

// tools/rbdiff.py PER_MACHINE entries below its STATIC_END (.data/.bss words
// that follow audio/disc time in every mode; physical [lo, hi)).  Keep the
// two lists in sync (and bump STATE_DIGEST_RANGES_VERSION) when either moves.
constexpr std::array<std::pair<u32, u32>, 12> STATIC_PER_MACHINE{{
    {0x0037F8C0, 0x00381C20},  // disc worker thread/stack
    {0x0039B42C, 0x0039B430},  // disc worker owner word
    {0x003D51A0, 0x003D51A4},  // disc worker owner word
    {0x002C8840, 0x002C89E0},  // audio streaming I/O
    {0x002C8A30, 0x002C8A34},  // audio callback's transient stream pointer
    {0x003EBC3C, 0x003EBC40},  // audio streaming I/O
    {0x003ECFA0, 0x003ECFA4},  // audio streaming I/O
    {0x002F481C, 0x002F4834},  // PathFinder music globals (audio thread)
    {0x0039B3F0, 0x0039B3F4},  // thread owner word (audio/disc)
    {0x003D55D0, 0x003D55DC},  // thread owner/link words (audio/disc)
    {0x003D55E0, 0x003DD600},  // disc/stream staging
    {0x003EBB24, 0x003EBB78},  // PathFinder music globals (audio thread)
}};

struct StateDigest
{
  // Cache: per state page, XXH3-64 of its restored bytes, valid where
  // valid[p] (the page has not changed since `epoch`).
  std::vector<u64> page;
  std::vector<u8> valid;
  // The same for partly excluded MEM1 pages: their included-pieces hash, valid
  // while the page is unchanged and its exclusions have the same signature.
  std::vector<u64> part;
  std::vector<u64> part_sig;
  std::vector<u8> part_valid;
  std::optional<u32> epoch;
  std::pair<u32, u32> fifo{0, 0};  // tracker's GX FIFO pages when cached
  // Scratch, reused between digests.
  std::vector<u8> kind;  // per MEM1 page: 0 cached, 1 partly excluded, 2 excluded
  std::vector<u64> sig;  // per MEM1 page: signature of its exclusion pieces
  std::vector<RollbackLogic::Range> excluded;  // merged, physical
  std::vector<u64> values;                     // per state page, combined in order
  // Diagnostics, per ring slot: the frame it describes and its page values.
  std::vector<u32> slot_frame;
  std::vector<std::vector<u64>> slot_values;
  std::vector<u64> slot_semantic;
  std::vector<std::vector<RollbackLogic::Range>> slot_excluded;
  u32 dumped_through = RollbackNet::NO_FRAME;  // NSR_RB_DIGEST_DUMP_EVERY progress
  // The boot-time I/O objects, fixed at the first rollback entry (see
  // DigestExclusions); empty until then.
  std::optional<std::map<u32, u32>> io_blocks;
  // The audio data blocks, fixed per epoch from its agreed first state:
  // scanning every audio block for pointers cost ~0.2 ms per digest, and the
  // data is loaded with the match.  Revisit if PathFinder loads project data
  // mid-match (it would show as a digest mismatch inside such a block).
  std::optional<std::map<u32, u32>> audio_data;
  // ARAM gameplay ring hash cache: valid while no ring write or undo happened
  // (the session's ARAM observer counts both) and the window is the same.
  std::optional<std::array<u64, 6>> aram_key;
  u64 aram_hash = 0;
  // Page values within an epoch: XXH3-128 (folded) with the secret derived
  // from the agreed epoch seed, identical to the hash/protection tracker's
  // page hashes after DirtyTracker::Rekey. Windows therefore hashes only
  // first-write pages; a kernel-tracker peer computes the same values here.
  alignas(64) std::array<u8, XXH3_SECRET_DEFAULT_SIZE> secret{};
  std::optional<u64> secret_seed;
  u64 reused = 0;  // page values taken from the dirty tracker
  u64 digests = 0, total_ns = 0, classify_ns = 0, rehashed = 0, direct = 0;
  u64 per_machine_ns = 0, heap_ns = 0;  // parts of classify_ns

  void Invalidate()
  {
    epoch.reset();
    std::fill(valid.begin(), valid.end(), u8{0});
    std::fill(part_valid.begin(), part_valid.end(), u8{0});
    std::fill(slot_frame.begin(), slot_frame.end(), RollbackNet::NO_FRAME);
    dumped_through = RollbackNet::NO_FRAME;
    audio_data.reset();
    aram_key.reset();
  }
};
StateDigest s_digest;

// Per-machine words the state digest found beyond the lists above: each one
// differed between two peers' finalized states in the two-human netplay gate
// while all game state was equal (local/work/statehash, NSR_RB_DESYNC_DUMPS),
// and both are written on audio time.  Also add them to rbdiff.py when it is
// next touched; they were invisible to it only because its dumps are sampled
// at frames where both machines happened to agree.
constexpr std::array<std::pair<u32, u32>, 3> DIGEST_PER_MACHINE{{
    // Audio-middleware .data 0x802FFE00..0x80300800 (stream/voice config and
    // the handle generation byte 0x80300038, which differed: c4 vs c2, fb vs
    // fd).  The Codex audio ownership audit (local/work/codex/audio-hardening-
    // patches, 0002) found it referenced only by middleware code and proposed
    // it for AUDIO_KEEP; that patch is unmerged, so the digest excludes it.
    {0x002FFE00, 0x00300800},
    // Two thread words just before PADStatus: main (0x803A3F30) on one peer,
    // the audio thread (0x803AA828) on the other at the same frame start,
    // always together with an audio allocation in the small heap.
    {0x003E92C0, 0x003E92CC},
    // +0x10 of the game's sound manager (object at 0x8030A518, *(r13-0x7C00),
    // set up by fn_8006E710 next to the sound-start rate limiter): a ms-like
    // value, 0x13E8 vs 0x13E0 at one frame start in ~57,000 compared frames,
    // equal again by the next whole-RAM dump.  Revisit (find its writer with
    // a memory watch) if more of this object shows up in a .pages diff.
    {0x0030A528, 0x0030A52C},
}};

// The excluded ranges for the state in `ram` (current RAM: the classification
// walks OS lists whose heads live in RB_KEEP, which slots don't hold).
//
// One deliberate difference from the sidecar: the I/O objects.  The sidecar
// finds them from root words that the stream source rewrites on audio time
// (the request table is itself a static per-machine range), so at a given
// frame two peers can point at different blocks (first netplay gate: a
// 0x210-byte block excluded on one peer only, a false desync 24 frames into
// the match).  rbdiff hides that by uniting both dumps' sidecars; a digest
// cannot.  The objects are boot-time allocations that never move, so the
// digest uses the set found in the first agreed state (the first entry
// barrier, or the first epoch start without one) for the whole process.
// Transient references to audio buffers are covered by audio-heap anyway.
// Revisit if a non-audio block reached only through the request table shows
// per-machine bytes (a false desync whose .pages diff points there).
void DigestExclusions(Core::System& system, const u8* ram, u32 mem1_size,
                      std::vector<RollbackLogic::Range>* out)
{
  if (!s_digest.io_blocks)
    s_digest.io_blocks = IoObjectBlocks(ram);
  if (!s_digest.audio_data)
    s_digest.audio_data = AudioDataBlocks(ram);
  std::vector<RollbackLogic::Range> ranges;
  auto add = [&](u32 lo, u64 len) {
    if (lo < mem1_size && len > 0)
      ranges.push_back({lo, static_cast<u32>(std::min<u64>(u64{lo} + len, mem1_size))});
  };
  const auto t0 = std::chrono::steady_clock::now();
  for (const PerMachineRange& range : PerMachineRanges(system, ram, &*s_digest.io_blocks, &*s_digest.audio_data, false))
  {
    if (range.hi > range.lo)
      add(range.lo & 0x01FFFFFF, range.hi - range.lo);
  }
  for (const auto& [lo, hi] : AUDIO_KEEP)
    add(lo, hi - lo);
  for (const auto& [lo, hi] : STATIC_PER_MACHINE)
    add(lo, hi - lo);
  for (const auto& [lo, hi] : DIGEST_PER_MACHINE)
    add(lo, hi - lo);
  add(RB_PAD_BUF & 0x01FFFFFF, MAX_PLAYERS * PAD_SIZE);
  // Allocator metadata of every heap audio code has allocated from.  The
  // audio thread allocates and frees there on audio time (e.g. a 0x20-byte
  // block in the small heap 0x804C4810 that one peer holds at a frame start
  // and the other does not), which moves block headers, back links, free
  // block headers and the descriptor's bookkeeping.  Excluding every block
  // header and aligned pre-header, every free block whole, and the
  // descriptor's counters/bins (+0x24..+0x70: block/byte counts and an
  // operation count, observed differing) makes both layouts exclude the same
  // merged ranges: a transient audio block is excluded as audio-heap on one
  // peer and as free space on the other.  Game blocks' payloads stay hashed.
  // The heap set is sticky for the process (audio allocates in these heaps
  // from the first menus on), so it does not depend on whether a transient
  // block exists right now.
  const auto t1 = std::chrono::steady_clock::now();
  // Heap ranges come out of each walk in address order: keep them apart so
  // the final merge is linear instead of sorting ~3,700 ranges every digest.
  std::vector<RollbackLogic::Range> misc = std::move(ranges);
  ranges.clear();
  ranges.reserve(4096);
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  for (const u32 heap : s_audio_heaps)
  {
    misc.push_back({(heap & 0x01FFFFFF) + 0x24, (heap & 0x01FFFFFF) + 0x70});
    const u32 first = rd(heap + 0x14), last = rd(heap + 0x18);
    u32 h = first;
    for (int n = 0; n < 200000 && h >= first && h + 0x20 <= last && h < 0x817FFFF0; ++n)
    {
      u32 hdr = h;
      if (rd(h + 8) != heap && rd(h + 0xC) == 0 && (rd(h + 4) & 0xFFFF0000) == 0x001C0000)
        hdr = h + 0x10;  // aligned block: 16-byte pre-header first
      const u32 next = rd(hdr + 0xC);
      if (rd(hdr + 8) != heap || next <= hdr || next > last)
        break;
      const bool free_block = (rd(hdr + 4) & 0xFF00) == 0;
      const u32 lo = h & 0x01FFFFFF, hi = ((free_block ? next : hdr + 0x10) & 0x01FFFFFF);
      if (!ranges.empty() && ranges.back().hi == lo)
        ranges.back().hi = std::min(hi, mem1_size);  // adjacent: extend (fewer ranges)
      else
        add(lo, hi - lo);
      h = next;
    }
  }
  const auto t2 = std::chrono::steady_clock::now();
  s_digest.per_machine_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  s_digest.heap_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
  const auto by_lo = [](const RollbackLogic::Range& a, const RollbackLogic::Range& b) {
    return a.lo < b.lo || (a.lo == b.lo && a.hi < b.hi);
  };
  std::sort(misc.begin(), misc.end(), by_lo);
  if (!std::is_sorted(ranges.begin(), ranges.end(), by_lo))
    std::sort(ranges.begin(), ranges.end(), by_lo);  // nested heaps interleave
  std::vector<RollbackLogic::Range>& merged = *out;
  merged.clear();
  merged.reserve(misc.size() + ranges.size());
  auto coalesce = [&merged](const RollbackLogic::Range& x) {
    if (x.lo >= x.hi)
      return;
    if (!merged.empty() && x.lo <= merged.back().hi)
      merged.back().hi = std::max(merged.back().hi, x.hi);
    else
      merged.push_back(x);
  };
  size_t i = 0, j = 0;
  while (i < misc.size() || j < ranges.size())
  {
    if (j == ranges.size() || (i < misc.size() && by_lo(misc[i], ranges[j])))
      coalesce(misc[i++]);
    else
      coalesce(ranges[j++]);
  }
  // Same result as RollbackLogic::MergeRanges over both lists.
}
}  // namespace

// The rollback-owned ARAM gameplay replay ring (final-review M1; restored by
// its undo log), as GameplayAramRingHash.  Cached between digests while the
// session's ARAM observer saw no write or undo and the window is unchanged.
static u64 RbAramDigest(Core::System& system, bool use_cache)
{
  StateDigest& sd = s_digest;
  const auto [address, bytes] = AramRingBytes(system);
  const AramRing& a = s_aram;
  const std::array<u64, 6> key{address, bytes.size(), a.writes, a.byte_writes, a.restores,
                               a.undone};
  if (use_cache && a.mem1 && sd.aram_key == key)
    return sd.aram_hash;
  const u64 hash = XXH3_64bits_withSeed(bytes.data(), bytes.size(), address);
  if (use_cache && a.mem1)
  {
    sd.aram_key = key;
    sd.aram_hash = hash;
  }
  return hash;
}

// Start an epoch's page-hash key (both the digest's and, in hash mode, the
// dirty tracker's): call at every rollback session start, before its first
// snapshot, with a seed all peers agree on.
static void RbDigestEpochKey(u64 seed)
{
  StateDigest& sd = s_digest;
  XXH3_generateSecret_fromSeed(sd.secret.data(), seed);
  sd.secret_seed = seed;
  std::fill(sd.valid.begin(), sd.valid.end(), u8{0});
  std::fill(sd.part_valid.begin(), sd.part_valid.end(), u8{0});
  sd.epoch.reset();
  s_dirty.Rekey(seed);
}

// Digest of the state at the start of `frame`.  `slot` is the ring slot just
// saved for that frame (incremental: the cache source), or null for a one-off
// full digest of RAM (handoff barriers).  Call it after the frame's pads are
// written and nothing else has run.
static u64 RbStateDigest(Core::System& system, const std::vector<u8>* slot, u64 semantic,
                         u32 frame)
{
  Rollback& r = s_rb;
  DirtyTracker& d = s_dirty;
  if (r.config.digest_semantic_only || !d.paged)
    return semantic;
  constexpr u32 PAGE = DirtyTracker::PAGE;
  const auto start = std::chrono::steady_clock::now();
  StateDigest& sd = s_digest;
  const u8* ram = system.GetMemory().GetRAM();
  const u32 mem1_pages = d.mem1_pages;
  const bool vmem_in_slot = slot && slot->size() >= r.mem1_size + r.vmem_bytes;
  const u32 vmem_pages = r.vmem_bytes && (vmem_in_slot || !slot) ? d.vmem_pages : 0;
  const u32 pages = mem1_pages + vmem_pages;

  DigestExclusions(system, ram, r.mem1_size, &sd.excluded);
  const auto classified = std::chrono::steady_clock::now();
  sd.kind.assign(mem1_pages, 0);
  sd.sig.assign(mem1_pages, 0);
  for (const RollbackLogic::Range& x : sd.excluded)
  {
    for (u32 p = x.lo / PAGE; p <= (x.hi - 1) / PAGE && p < mem1_pages; ++p)
    {
      const u32 plo = p * PAGE, phi = std::min(plo + PAGE, r.mem1_size);
      if (x.lo <= plo && x.hi >= phi)
      {
        sd.kind[p] = 2;
        continue;
      }
      if (sd.kind[p] == 0)
        sd.kind[p] = 1;
      const u64 piece = (u64{std::max(x.lo, plo)} << 32) | std::min(x.hi, phi);
      sd.sig[p] = (sd.sig[p] ^ piece) * 0x100000001B3ULL + 0x9E3779B97F4A7C15ULL;
    }
  }

  // Cache bookkeeping (incremental digests only).  A moved GX FIFO resyncs
  // every slot (DirtyTracker::FifoPages), so pages it skipped may be stale.
  std::optional<u32> since;
  if (slot)
  {
    if (sd.page.size() != pages)
    {
      sd.page.assign(pages, 0);
      sd.valid.assign(pages, 0);
      sd.part.assign(mem1_pages, 0);
      sd.part_sig.assign(mem1_pages, 0);
      sd.part_valid.assign(mem1_pages, 0);
      sd.epoch.reset();
    }
    if (d.fifo_pages != sd.fifo)
    {
      sd.fifo = d.fifo_pages;
      sd.Invalidate();
    }
    since = d.on ? sd.epoch : std::nullopt;
  }
  sd.values.assign(pages, 0);
  const u8* cache_src = slot ? slot->data() : ram;
  const u8* vmem_src = slot ? slot->data() + r.mem1_size : r.vmem;
  const std::vector<RollbackLogic::Range>& excluded = sd.excluded;
  std::atomic<u64> rehashed{0}, direct{0}, reused{0};
  const bool keyed = slot && sd.secret_seed.has_value();
  const bool reuse_tracker =
      keyed && d.on &&
      (d.mode == DirtyTracker::Mode::Hash || d.mode == DirtyTracker::Mode::Protect) &&
      d.shared_key == sd.secret_seed;
  s_workers.Run(pages, PAGE_CHUNK, [&](u32 lo, u32 hi) {
    std::array<u8, PAGE> buf;
    u64 n_rehashed = 0, n_direct = 0, n_reused = 0;
    // Hash page p's restored bytes (its segment pieces) from `base`.
    // Epoch digests use the keyed 128-bit hash (see StateDigest::secret);
    // one-off barrier digests the plain one (no epoch key exists yet).
    auto hash_bytes = [&](const u8* bytes, size_t len) -> u64 {
      if (!keyed)
        return XXH3_64bits(bytes, len);
      const XXH128_hash_t h = XXH3_128bits_withSecret(bytes, len, sd.secret.data(), sd.secret.size());
      return h.low64 ^ h.high64;
    };
    auto hash_page = [&](const u8* base, u32 p) -> u64 {
      if (p >= mem1_pages)
      {
        const u32 a = (p - mem1_pages) * PAGE;
        return hash_bytes(vmem_src + a, std::min(PAGE, r.vmem_bytes - a));
      }
      const u32 c0 = d.page_first[p], c1 = d.page_first[p + 1];
      if (c1 == c0 + 1)
        return hash_bytes(base + d.chunks[c0].first, d.chunks[c0].second - d.chunks[c0].first);
      size_t n = 0;
      for (u32 c = c0; c < c1; ++c)
      {
        const auto [a, b] = d.chunks[c];
        std::memcpy(buf.data() + n, base + a, b - a);
        n += b - a;
      }
      return hash_bytes(buf.data(), n);
    };
    for (u32 p = lo; p < hi; ++p)
    {
      if (slot && (!since || d.stamp[p] > *since))
      {
        sd.valid[p] = 0;
        if (p < mem1_pages)
          sd.part_valid[p] = 0;
      }
      const u8 k = p < mem1_pages ? sd.kind[p] : u8{0};
      if (k == 2)
        continue;  // value stays 0
      if (k == 1 && slot && sd.part_valid[p] && sd.part_sig[p] == sd.sig[p])
      {
        sd.values[p] = sd.part[p];
        continue;
      }
      if (k == 1)
      {
        // Only the included pieces, read from RAM (the exclusions are RAM's;
        // RAM equals the slot here except for the excluded pad buffer).
        size_t n = 0;
        auto it = std::upper_bound(excluded.begin(), excluded.end(), p * PAGE,
                                   [](u32 v, const RollbackLogic::Range& x) { return v < x.hi; });
        for (u32 c = d.page_first[p]; c < d.page_first[p + 1]; ++c)
        {
          u32 a = d.chunks[c].first;
          const u32 b = d.chunks[c].second;
          while (a < b)
          {
            while (it != excluded.end() && it->hi <= a)
              ++it;
            if (it != excluded.end() && it->lo <= a)
            {
              a = it->hi;
              continue;
            }
            const u32 e = it != excluded.end() && it->lo < b ? it->lo : b;
            std::memcpy(buf.data() + n, ram + a, e - a);
            n += e - a;
            a = e;
          }
        }
        sd.values[p] = hash_bytes(buf.data(), n);
        if (slot)
        {
          sd.part[p] = sd.values[p];
          sd.part_sig[p] = sd.sig[p];
          sd.part_valid[p] = 1;
        }
        ++n_direct;
        continue;
      }
      if (!slot)
      {
        sd.values[p] = hash_page(ram, p);
        continue;
      }
      if (reuse_tracker && (p >= mem1_pages || (d.page_first[p + 1] == d.page_first[p] + 1 &&
                                                d.chunks[d.page_first[p]].first == p * PAGE &&
                                                d.chunks[d.page_first[p]].second == (p + 1) * PAGE)))
      {
        // A whole restored page: the tracker hashed exactly these bytes at
        // the save's sample (the slot equals RAM there) with the same key.
        sd.values[p] = d.hashes[p].low64 ^ d.hashes[p].high64;
        ++n_reused;
        continue;
      }
      if (!sd.valid[p])
      {
        sd.page[p] = hash_page(cache_src, p);
        sd.valid[p] = 1;
        ++n_rehashed;
      }
      sd.values[p] = sd.page[p];
    }
    rehashed.fetch_add(n_rehashed, std::memory_order_relaxed);
    direct.fetch_add(n_direct, std::memory_order_relaxed);
    reused.fetch_add(n_reused, std::memory_order_relaxed);
  });
  if (slot)
  {
    // The cache now describes the state at the slot's sync epoch; without a
    // tracker there is none, and the next digest rehashes everything.
    sd.epoch = d.on ? d.Synced(*slot) : std::nullopt;
  }

  XXH3_state_t state;
  XXH3_64bits_reset_withSeed(&state, STATE_DIGEST_RANGES_VERSION);
  XXH3_64bits_update(&state, &semantic, sizeof(semantic));
  const u64 aram = RbAramDigest(system, slot != nullptr);
  XXH3_64bits_update(&state, &aram, sizeof(aram));
  for (const RollbackLogic::Range& x : sd.excluded)
  {
    const std::array<u32, 2> pair{x.lo, x.hi};
    XXH3_64bits_update(&state, pair.data(), sizeof(pair));
  }
  XXH3_64bits_update(&state, sd.values.data(), sd.values.size() * sizeof(u64));
  const u64 digest = XXH3_64bits_digest(&state);

  if (slot && !r.ring.empty())
  {
    const size_t slots = r.ring.size();
    if (sd.slot_frame.size() != slots)
    {
      sd.slot_frame.assign(slots, RollbackNet::NO_FRAME);
      sd.slot_values.assign(slots, {});
      sd.slot_semantic.assign(slots, 0);
      sd.slot_excluded.assign(slots, {});
    }
    sd.slot_frame[frame % slots] = frame;
    sd.slot_values[frame % slots] = sd.values;
    sd.slot_semantic[frame % slots] = semantic;
    sd.slot_excluded[frame % slots] = sd.excluded;
  }
  const auto end = std::chrono::steady_clock::now();
  ++sd.digests;
  sd.total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  sd.classify_ns +=
      std::chrono::duration_cast<std::chrono::nanoseconds>(classified - start).count();
  sd.rehashed += rehashed.load();
  sd.direct += direct.load();
  sd.reused += reused.load();
  return digest;
}

// NSR_RB_TEST_CORRUPT=frame:addr (diagnostics; set it on one peer only):
// at the start of `frame`, before its snapshot, set the byte at `addr` to the
// complement of what the first (live) run found there.  Idempotent, so a
// re-simulation of the frame (or a restore of its own snapshot) reproduces
// the same divergence instead of undoing or doubling it.
static void RbTestCorrupt(u8* ram, u32 frame)
{
  static std::optional<u8> corrupted;  // the byte written (one test per process)
  const auto& spec = s_rb.config.test_corrupt;
  if (!spec || spec->first != frame)
    return;
  const u32 offset = spec->second & 0x01FFFFFF;
  if (offset >= s_rb.mem1_size)
    return;
  if (!corrupted)
  {
    corrupted = static_cast<u8>(~ram[offset]);
    Log("TEST CORRUPT: byte {:08x} {:02x} -> {:02x} at the start of frame {} (semantic words "
        "untouched)",
        spec->second, ram[offset], *corrupted, frame);
  }
  ram[offset] = *corrupted;
}

// Diagnostics on a confirmed digest mismatch: with NSR_DUMP_PREFIX set, write
// the local state at that frame start (the ring slot, if it still holds the
// frame; RB_KEEP bytes in it are stale), its fake VMEM, a sidecar of the
// current RAM and the per-page digest values ("page value" lines, for
// diffing against the other peer's file to find the diverged pages).
static void RbDumpDesync(Core::System& system, u32 frame)
{
  const std::string prefix = Env("NSR_DUMP_PREFIX");
  const Rollback& r = s_rb;
  const StateDigest& sd = s_digest;
  if (prefix.empty() || r.ring.empty() || sd.slot_frame.size() != r.ring.size())
    return;
  const size_t index = frame % r.ring.size();
  if (sd.slot_frame[index] != frame)
  {
    Log("desync dump: frame {} is no longer in the snapshot ring", frame);
    return;
  }
  const std::string path = fmt::format("{}-desync-{}.bin", prefix, frame);
  const std::vector<u8>& slot = r.ring[index];
  if (FILE* f = std::fopen(path.c_str(), "wb"))
  {
    std::fwrite(slot.data(), 1, std::min<size_t>(slot.size(), r.mem1_size), f);
    std::fclose(f);
  }
  if (r.vmem_bytes && slot.size() >= r.mem1_size + r.vmem_bytes)
  {
    if (FILE* f = std::fopen((path + ".vmem").c_str(), "wb"))
    {
      std::fwrite(slot.data() + r.mem1_size, 1, r.vmem_bytes, f);
      std::fclose(f);
    }
  }
  WritePerMachineSidecar(system, system.GetMemory().GetRAM(), path);
  if (FILE* f = std::fopen((path + ".excl").c_str(), "w"))
  {
    // The digest's own exclusions at that frame (physical [lo, hi)).
    for (const RollbackLogic::Range& x : sd.slot_excluded[index])
      std::fprintf(f, "%08x %08x\n", x.lo, x.hi);
    std::fclose(f);
  }
  if (FILE* f = std::fopen((path + ".pages").c_str(), "w"))
  {
    std::fprintf(f, "# state digest page values at the start of frame %u (semantic %016llx); "
                    "page index (MEM1 then fake VMEM) value\n",
                 frame, static_cast<unsigned long long>(sd.slot_semantic[index]));
    const std::vector<u64>& values = sd.slot_values[index];
    for (size_t p = 0; p < values.size(); ++p)
      std::fprintf(f, "%zx %016llx\n", p, static_cast<unsigned long long>(values[p]));
    std::fclose(f);
  }
  Log("wrote {} (+ .vmem, .permachine, .excl, .pages) for the desync at frame {}", path, frame);
}

// A handoff mismatch happens outside the snapshot ring, so preserve the live
// state that produced the barrier digest.  This is the lifecycle equivalent
// of RbDumpDesync and is intentionally failure-only.
static void RbDumpHandoffDesync(Core::System& system, u32 frame)
{
  const std::string prefix = Env("NSR_DUMP_PREFIX");
  if (prefix.empty())
    return;
  const std::string path = fmt::format("{}-handoff-desync-{}.bin", prefix, frame);
  const u8* ram = system.GetMemory().GetRAM();
  if (FILE* f = std::fopen(path.c_str(), "wb"))
  {
    std::fwrite(ram, 1, system.GetMemory().GetRamSizeReal(), f);
    std::fclose(f);
  }
  if (s_rb.vmem_bytes)
  {
    if (FILE* f = std::fopen((path + ".vmem").c_str(), "wb"))
    {
      std::fwrite(s_rb.vmem, 1, s_rb.vmem_bytes, f);
      std::fclose(f);
    }
  }
  WritePerMachineSidecar(system, ram, path);
  WriteAramRingSidecar(system, path);
  if (FILE* f = std::fopen((path + ".excl").c_str(), "w"))
  {
    for (const RollbackLogic::Range& x : s_digest.excluded)
      std::fprintf(f, "%08x %08x\n", x.lo, x.hi);
    std::fclose(f);
  }
  if (FILE* f = std::fopen((path + ".pages").c_str(), "w"))
  {
    for (size_t p = 0; p < s_digest.values.size(); ++p)
      std::fprintf(f, "%zx %016llx\n", p,
                   static_cast<unsigned long long>(s_digest.values[p]));
    std::fclose(f);
  }
  Log("wrote {} (+ .vmem, .permachine, .aram, .excl, .pages) for handoff mismatch", path);
}

// Diagnostics: NSR_RB_DIGEST_DUMP_EVERY=n (with NSR_DUMP_PREFIX) writes the
// digested state of every finalized frame start that is a multiple of n:
// <prefix>-dfin-<frame>.bin (the ring slot's MEM1; RB_KEEP bytes in it are
// stale) and .excl (the digest's excluded physical ranges).  Unlike
// NSR_RB_DUMP_AT dumps of live (possibly mispredicted) frames, these are
// exactly what peers compared, so two peers' files show which bytes a
// mismatch is made of.
static void RbDumpFinalized(u32 finalized)
{
  static const u32 every = ParseU32(Env("NSR_RB_DIGEST_DUMP_EVERY")).value_or(0);
  const std::string prefix = Env("NSR_DUMP_PREFIX");
  StateDigest& sd = s_digest;
  const Rollback& r = s_rb;
  if (!every || prefix.empty() || finalized == RollbackNet::NO_FRAME || r.ring.empty() ||
      sd.slot_frame.size() != r.ring.size())
    return;
  u32 frame = sd.dumped_through == RollbackNet::NO_FRAME || sd.dumped_through > finalized ?
                  finalized :
                  sd.dumped_through + 1;
  for (; frame <= finalized; ++frame)
  {
    const size_t index = frame % r.ring.size();
    if (frame % every != 0 || sd.slot_frame[index] != frame)
      continue;
    const std::string path = fmt::format("{}-dfin-{}.bin", prefix, frame);
    if (FILE* f = std::fopen(path.c_str(), "wb"))
    {
      std::fwrite(r.ring[index].data(), 1, r.mem1_size, f);
      std::fclose(f);
    }
    if (FILE* f = std::fopen((path + ".excl").c_str(), "w"))
    {
      for (const RollbackLogic::Range& x : sd.slot_excluded[index])
        std::fprintf(f, "%08x %08x\n", x.lo, x.hi);
      std::fclose(f);
    }
  }
  sd.dumped_through = finalized;
}

// ---------------------------------------------------------------------------
// Config fingerprint (final review M2): everything that changes how a peer
// simulates a rollback epoch or what its digest means.  See ConfigField.
// Emulated-timing options (CPU overclock, dual core, disc speed with instant
// disc on) are deliberately absent: the design tolerates timing differences
// between machines (a 0.6x CPU peer passes the whole-RAM gate), and so is the
// CPU core (JIT64 vs JITARM64 peers are a goal).
// ---------------------------------------------------------------------------
static RollbackNet::ConfigFingerprint LocalConfigFingerprint(Core::System& system)
{
  using RollbackNet::ConfigField;
  const Rollback& r = s_rb;
  const RollbackConfig& c = ConfigForRollback();
  RollbackNet::ConfigFingerprint fp;
  auto hash_text = [](const std::string& text) { return XXH3_64bits(text.data(), text.size()); };
  auto env_list = [&](std::initializer_list<const char*> names) {
    std::string text;
    for (const char* name : names)
      text += fmt::format("{}={};", name, Env(name));
    return hash_text(text);
  };
  auto list = [](const std::vector<u32>& values) {
    std::string text;
    for (const u32 value : values)
      text += fmt::format("{:08x},", value);
    return text;
  };
  fp[ConfigField::Protocol] = (u64{RollbackNet::VERSION} << 8) | RollbackNet::HANDOFF_VERSION;
  fp[ConfigField::StateDigest] = (u64{c.digest_every} << 16) |
                                 (u64{STATE_DIGEST_RANGES_VERSION} << 8) |
                                 (c.digest_semantic_only ? 1u : 0u);
  constexpr u64 TB_PER_FRAME = 40500000 / 60;  // as EnterRollbackSession
  fp[ConfigField::VtimeBase] =
      ParseU64(Env("NSR_RB_VTIME_BASE")).value_or(TB_PER_FRAME << 37) / TB_PER_FRAME * TB_PER_FRAME;
  fp[ConfigField::VmemBytes] = r.vmem_bytes;
  fp[ConfigField::ResimSpeed] =
      env_list({"NSR_RB_RESIM_SPEED", "NSR_RB_RESIM_GPU_SPEED", "NSR_RB_RESIM_ARAM_SPEED",
                "NSR_RB_RESIM_AUDIO_SPEED", "NSR_RB_RESIM_DRAW"});
  fp[ConfigField::InstantDisc] = c.instant_disc ? 1 : 0;
  fp[ConfigField::IoIdle] = r.io_idle_mode ? 1 : 0;
  fp[ConfigField::AudioRollback] = r.audio_rollback ? 1 : 0;
  {
    // The executable text as loaded (GN7E69: 0x80003100 up to the end of the
    // audio middleware's text), so a different Reborn build or code patch is
    // named rather than surfacing as a state mismatch.
    const u8* ram = system.GetMemory().GetRAM();
    constexpr u32 TEXT_LO = 0x00003100, TEXT_HI = 0x00285A90;
    const std::string id = fmt::format("{}-r{}-{:016x}", SConfig::GetInstance().GetGameID(),
                                       SConfig::GetInstance().GetRevision(),
                                       XXH3_64bits(ram + TEXT_LO, TEXT_HI - TEXT_LO));
    fp[ConfigField::GameImage] = hash_text(id);
  }
  fp[ConfigField::LauncherImage] = hash_text(Env("NSR_FINGERPRINT"));
  fp[ConfigField::Seed] = c.seed ? u64{*c.seed} : ~u64{0};
  {
    std::string text;
    for (const auto& [lo, hi] : r.segments)
      text += fmt::format("{:x}-{:x};", lo, hi);
    fp[ConfigField::RestoreRanges] = hash_text(text);
  }
  {
    std::string text = fmt::format(
        "{} {} {} {} {} {} {} {} {};", static_cast<int>(c.ac_mode), c.ac_speech_frames,
        c.ac_speech_lead, c.ac_sfx_default, c.ac_sfx_tail, c.ac_bank_frames, c.ac_polls_per_tick,
        c.ac_song_frames, c.ac_song_gap);
    for (const auto& [lo, hi] : c.ac_hash_ranges)
      text += fmt::format("{:x}-{:x};", lo, hi);
    fp[ConfigField::AudioClock] = hash_text(text);
  }
  fp[ConfigField::ResimGates] = hash_text(fmt::format(
      "gate {} effect {} skip {} vsync {} async {} peekz {}", list(c.audio_gate_only),
      list(c.effect_gates), list(c.skip_resim), c.keep_resim_vsync, c.async_present, c.peek_z));
  fp[ConfigField::SessionRules] = hash_text(
      fmt::format("auto {} from {} end {} reenter {}", r.auto_handoff,
                  r.auto_handoff ? 0 : r.vtime_from, c.session_end, c.auto_reenter));
  fp[ConfigField::DolphinCore] = hash_text(fmt::format(
      "cheats {} mmu {} ram {} {} fprf {} nans {} rtc {} {}",
      Config::Get(Config::MAIN_ENABLE_CHEATS), Config::Get(Config::MAIN_MMU),
      Config::Get(Config::MAIN_RAM_OVERRIDE_ENABLE), Config::Get(Config::MAIN_MEM1_SIZE),
      Config::Get(Config::MAIN_FPRF), Config::Get(Config::MAIN_ACCURATE_NANS),
      Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE), Config::Get(Config::MAIN_CUSTOM_RTC_VALUE)));
  return fp;
}

static std::string DescribeConfig(const RollbackNet::ConfigFingerprint& fp)
{
  std::string text;
  for (size_t i = 0; i < RollbackNet::CONFIG_FIELDS; ++i)
  {
    const auto field = static_cast<RollbackNet::ConfigField>(i);
    text += fmt::format("{}{}={:x}", i ? " " : "", RollbackNet::ConfigFieldName(field), fp[field]);
  }
  return text;
}

// Refuse a differently configured peer: name every differing field, raise a
// telemetry event and stop cleanly (detrun counts "bad config" as fatal).
static void FailConfigMismatch(u32 player, const RollbackNet::ConfigFingerprint* local,
                               const RollbackNet::ConfigFingerprint* remote, u64 local_digest,
                               u64 remote_digest)
{
  std::string fields;
  if (local && remote)
  {
    for (const auto field : RollbackNet::ConfigDifferences(*local, *remote))
    {
      fields += fmt::format("{}{} (local {:x}, remote {:x})", fields.empty() ? "" : ", ",
                            RollbackNet::ConfigFieldName(field), (*local)[field],
                            (*remote)[field]);
    }
  }
  else
  {
    fields = fmt::format("config digest (local {:016x}, remote {:016x}); compare the "
                         "'config fingerprint' log lines",
                         local_digest, remote_digest);
  }
  Log("bad config fingerprint from player {}: refusing rollback; differing: {}", player, fields);
  NSRTelemetry::Event(NSRTelemetry::EventKind::Note, TelemetryLiveFrame(),  // NSR telemetry
                      {NSRTelemetry::Int("config_mismatch_player", static_cast<s64>(player)),
                       NSRTelemetry::Hex("local_config", local_digest),
                       NSRTelemetry::Hex("remote_config", remote_digest)},
                      "config mismatch: " + fields);
  s_session.failed = true;
  Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
}

// Stop the emulation after a transition protocol failure.  Continuing would
// turn a detected boundary disagreement into an opaque menu or match desync.
static bool FailHandoff(const std::string& message)
{
  Log("DESYNC: HANDOFF ABORT: {}", message);
  s_session.failed = true;
  s_rb_handoff.reset();
  Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
  return false;
}

// Before rollback can be disabled, every input used by the last live frame
// must be confirmed.  A differing late input would require re-simulating the
// resource-teardown frame, which the lifecycle tests proved unsafe; fail
// loudly instead of carrying speculative state into lockstep menus.
static bool ConfirmRollbackTail(u32 completed_frame)
{
  if (!s_rb_exchange)
    return FailHandoff("rollback exchange missing at leave");
  const auto start = std::chrono::steady_clock::now();
  auto last_send = start;
  while ((s_rb_exchange->ConfirmedThrough() == RollbackNet::NO_FRAME ||
          s_rb_exchange->ConfirmedThrough() < completed_frame) &&
         !s_session.failed)
  {
    Service(1);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_send >= std::chrono::milliseconds(8))
    {
      SendRollbackLatest(s_rb.live);
      last_send = now;
    }
    if (now - start >= std::chrono::milliseconds(PEER_TIMEOUT_MS))
      return FailHandoff(fmt::format("timeout confirming leave frame {}", completed_frame));
  }
  if (s_session.failed)
    return FailHandoff(fmt::format("transport failed confirming leave frame {}", completed_frame));
  if (const auto pending = s_rb_exchange->PendingRollback())
  {
    return FailHandoff(fmt::format("leave frame {} revealed a misprediction at {}",
                                   completed_frame, *pending));
  }
  return true;
}

// Exchange and compare the canonical state digest (RbStateDigest, a full
// one-off hash of RAM here) and the config fingerprint at an agreed absolute
// frame.  This is a host-side barrier: no guest time advances while waiting,
// and each proposal is resent until every player has acknowledged the same
// kind/frame/hash/config tuple.  A differing config is refused by name before
// any rollback epoch starts (final review M2).
static bool HandoffBarrier(const Core::CPUThreadGuard& guard, RollbackNet::HandoffKind kind,
                           u32 frame, u32* host_clock = nullptr,
                           u32* host_aux_clock = nullptr)
{
  Session& s = s_session;
  if (!s.enabled || s.failed || (!AllConnected() && !WaitForConnection()))
    return FailHandoff(fmt::format("transport is not connected at frame {}", frame));

  const RollbackNet::ConfigFingerprint config = LocalConfigFingerprint(guard.GetSystem());
  u64 semantic = RbStateDigest(guard.GetSystem(), nullptr, RbSemanticChecksum(guard), frame);
  if (kind == RollbackNet::HandoffKind::LeaveRollback && s_rb_exchange)
  {
    // Leave: the teardown frame just freed audio/heap memory on each
    // machine's own schedule, so a fresh RAM hash here differs between honest
    // peers (4-player regress: three distinct values after a pause-menu
    // quit, with no per-frame digest mismatch during the match).  Prove the
    // last confirmed in-match state instead: ConfirmRollbackTail ensured no
    // pending rollback, so this per-frame digest is final on every peer, and
    // digest_every is part of the config fingerprint, so all peers pick the
    // same frame.
    if (const auto last = s_rb_exchange->LocalStateChecksumAtOrBefore(s_rb.tick_running))
    {
      semantic = last->second ^ (static_cast<u64>(last->first) * 0x9E3779B97F4A7C15ULL);
      Log("handoff leave proves the in-match digest of frame {}", last->first);
    }
  }
  // Entry 0, leave 1, re-entry 2, ... is derived solely from the completed
  // session generation.  It prevents a late unsequenced packet from an old
  // match from poisoning a later barrier.
  const u32 transition = kind == RollbackNet::HandoffKind::EnterRollback ?
                             s_rb.session_generation * 2 :
                             s_rb.session_generation * 2 - 1;
  const u32 sync_clock = PowerPC::MMU::HostRead<u32>(guard, VI_POST_COUNTER);
  const u32 sync_aux_clock = PowerPC::MMU::HostRead<u32>(guard, TRANSITION_AUX_CLOCK);
  s_rb_handoff.emplace(static_cast<u8>(s.players), static_cast<u8>(s.me), kind, transition, frame,
                       semantic, config, sync_clock, sync_aux_clock);
  SendHandoff();
  const auto start = std::chrono::steady_clock::now();
  auto last_send = start;
  while (!s_rb_handoff->Complete() && !s_rb_handoff->Mismatched() && !s.failed)
  {
    Service(1);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_send >= std::chrono::milliseconds(8))
    {
      SendHandoff();
      last_send = now;
    }
    if (now - start >= std::chrono::milliseconds(PEER_TIMEOUT_MS))
      return FailHandoff(fmt::format(
          "timeout at {} frame {} state digest {:016x}",
          kind == RollbackNet::HandoffKind::EnterRollback ? "entry" : "leave", frame,
          semantic));
  }
  if (const auto& other = s_rb_handoff->ConfigMismatch())
  {
    const RollbackNet::ConfigFingerprint remote = other->config;
    const u32 player = other->sender;
    s_rb_handoff.reset();
    FailConfigMismatch(player, &config, &remote, RollbackNet::ConfigDigest(config),
                       RollbackNet::ConfigDigest(remote));
    return false;  // FailConfigMismatch stopped the session
  }
  if (s_rb_handoff->Mismatched())
  {
    const RollbackNet::HandoffPacket& other = *s_rb_handoff->Mismatch();
    RbDumpHandoffDesync(guard.GetSystem(), frame);
    return FailHandoff(fmt::format(
        "state/frame mismatch from player {}: local kind {} frame {} state digest {:016x}, "
        "remote kind {} frame {} state digest {:016x}",
        other.sender, static_cast<u32>(kind), frame, semantic, static_cast<u32>(other.kind),
        other.frame, other.semantic_hash));
  }
  if (s.failed)
    return FailHandoff(fmt::format("transport failed at handoff frame {}", frame));
  const std::optional<u32> agreed_clock = s_rb_handoff->HostClock();
  const std::optional<u32> agreed_aux_clock = s_rb_handoff->HostAuxClock();
  if (!agreed_clock || !agreed_aux_clock)
    return FailHandoff(fmt::format("handoff at frame {} completed without player 0", frame));
  if (host_clock)
    *host_clock = *agreed_clock;
  if (host_aux_clock)
    *host_aux_clock = *agreed_aux_clock;
  Log("handoff {} transition {} agreed at frame {} state digest {:016x} host clock {}",
      kind == RollbackNet::HandoffKind::EnterRollback ? "entry" : "leave", transition, frame,
      semantic, *agreed_clock);
  s_rb_handoff.reset();
  return true;
}

// Re-entry needs one small phase before its full-state barrier: real audio
// completion may make the preceding presentation loop take a few different
// virtual retraces on each peer. Exchange only the authenticated clock
// baselines, adopt player 0's domain, then compute the full digest. A
// high-bit transition id keeps delayed clock packets distinct from the
// subsequent entry proposal.
static bool HandoffClockBarrier(const Core::CPUThreadGuard& guard, u32 frame, u32* host_clock,
                                u32* host_aux_clock)
{
  Session& s = s_session;
  if (!s.enabled || s.failed || (!AllConnected() && !WaitForConnection()))
    return FailHandoff(fmt::format("transport is not connected at clock sync frame {}", frame));
  const RollbackNet::ConfigFingerprint config = LocalConfigFingerprint(guard.GetSystem());
  const u32 transition = 0x80000000u | s_rb.session_generation;
  const u32 sync_clock = PowerPC::MMU::HostRead<u32>(guard, VI_POST_COUNTER);
  const u32 sync_aux_clock = PowerPC::MMU::HostRead<u32>(guard, TRANSITION_AUX_CLOCK);
  s_rb_handoff.emplace(static_cast<u8>(s.players), static_cast<u8>(s.me),
                       RollbackNet::HandoffKind::SyncClocks, transition, frame, 0, config,
                       sync_clock, sync_aux_clock);
  SendHandoff();
  const auto start = std::chrono::steady_clock::now();
  auto last_send = start;
  while (!s_rb_handoff->Complete() && !s_rb_handoff->Mismatched() && !s.failed)
  {
    Service(1);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_send >= std::chrono::milliseconds(8))
    {
      SendHandoff();
      last_send = now;
    }
    if (now - start >= std::chrono::milliseconds(PEER_TIMEOUT_MS))
      return FailHandoff(fmt::format("timeout synchronizing re-entry clocks at frame {}", frame));
  }
  if (const auto& other = s_rb_handoff->ConfigMismatch())
  {
    const RollbackNet::ConfigFingerprint remote = other->config;
    const u32 player = other->sender;
    s_rb_handoff.reset();
    FailConfigMismatch(player, &config, &remote, RollbackNet::ConfigDigest(config),
                       RollbackNet::ConfigDigest(remote));
    return false;
  }
  if (s_rb_handoff->Mismatched() || s.failed)
    return FailHandoff(fmt::format("clock synchronization mismatch at frame {}", frame));
  const std::optional<u32> agreed_clock = s_rb_handoff->HostClock();
  const std::optional<u32> agreed_aux_clock = s_rb_handoff->HostAuxClock();
  if (!agreed_clock || !agreed_aux_clock)
    return FailHandoff(fmt::format("clock sync at frame {} completed without player 0", frame));
  *host_clock = *agreed_clock;
  *host_aux_clock = *agreed_aux_clock;
  Log("handoff clock sync transition {} agreed at frame {} host clocks {}/{}", transition, frame,
      *agreed_clock, *agreed_aux_clock);
  s_rb_handoff.reset();
  return true;
}

// Adopt player 0's retrace-derived clock domain at a lifecycle barrier.
// Defined beside the affected model below; the declaration keeps handoff
// transport independent from the game's clock layout.
namespace
{
void NormalizeTransitionClocks(const Core::CPUThreadGuard& guard, u32 host_clock,
                               u32 host_aux_clock);
}

// Convert the transport-neutral input arrays without coupling that component
// to Dolphin's CommonTypes aliases.
static RollbackNet::Pad ToNetPad(const Pad& pad)
{
  RollbackNet::Pad out{};
  std::copy(pad.begin(), pad.end(), out.begin());
  return out;
}

static Pads FromNetPads(const RollbackNet::Pads& pads)
{
  Pads out{};
  for (u32 player = 0; player < MAX_PLAYERS; ++player)
    std::copy(pads[player].begin(), pads[player].end(), out[player].begin());
  return out;
}

// Drain and resend while prediction is at its configured bound.  This is a
// host wait: no guest frame, clock, audio query, or disc completion advances,
// so all peers still simulate the same numbered frame once input arrives.
// Set by RbStep for a live frame that starts with the pause menu open.  Such a
// frame waits for every player's confirmed input instead of predicting: the
// pause menu is where a match is quit, and the session-leave teardown frame
// must never run on a guess (ConfirmRollbackTail refuses to re-simulate it;
// a 4-player regress run aborted when a predicted A on "Quit Game" was later
// corrected).  The menu is static, so the added input latency is invisible.
static bool s_rb_confirm_live = false;

static bool RbNetworkPreflight(u32 live)
{
  Rollback& r = s_rb;
  Session& s = s_session;
  if (!r.net_enabled || !s_rb_exchange)
    return true;
  s_rb_exchange->SubmitLocal(live, ToNetPad(r.latest[0]));
  // Publish the current local frame before draining arrivals so timing
  // samples compare progress at the same live-frame boundary.
  Service(0);
  s_rb_exchange->UpdateFinalized();
  SendRollbackLatest(live);
  {
    // NSR telemetry: ENet link quality per peer and the prediction window.
    namespace T = NSRTelemetry;
    for (u32 i = 0; i < s.players; ++i)
    {
      if (i == s.me || !Linked(i))
        continue;
      const ENetPeer* peer = s.peers[i];
      T::SetGauge(T::Gauge::NetRttMs, peer->roundTripTime, static_cast<int>(i));
      T::SetGauge(T::Gauge::NetRttVarMs, peer->roundTripTimeVariance, static_cast<int>(i));
      T::SetGauge(T::Gauge::NetLossPct,
                  100.0 * peer->packetLoss / static_cast<double>(ENET_PEER_PACKET_LOSS_SCALE),
                  static_cast<int>(i));
    }
    T::SetGauge(T::Gauge::FrameAdvantage, s_rb_exchange->FrameAdvantage(live));
    T::SetGauge(T::Gauge::InputDelay, r.net_delay);
    T::SetGauge(T::Gauge::MaxPrediction, r.max_prediction);
    if (const u32 confirmed = s_rb_exchange->ConfirmedThrough();
        confirmed != RollbackNet::NO_FRAME && confirmed <= live)
      T::Observe(T::Hist::PredictionFrames, live - confirmed);
  }
  const auto waited_us = [](std::chrono::steady_clock::time_point since) {  // NSR telemetry
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - since)
                                .count());
  };

  auto wait_until = [&](auto predicate, std::chrono::microseconds limit, bool starvation) {
    const auto start = std::chrono::steady_clock::now();
    auto last_send = start;
    while (predicate() && !s.failed)
    {
      Service(1);
      const auto now = std::chrono::steady_clock::now();
      if (now - last_send >= std::chrono::milliseconds(8))
      {
        SendRollbackLatest(live);
        last_send = now;
      }
      if (now - start >= limit)
        break;
    }
    if (predicate() && starvation)
    {
      Log("rollback input timeout at frame {} (confirmed through {})", live,
          s_rb_exchange->ConfirmedThrough());
      NSRTelemetry::Event(NSRTelemetry::EventKind::InputTimeout, live,  // NSR telemetry
                          {NSRTelemetry::Int("confirmed_through",
                                             s_rb_exchange->ConfirmedThrough())});
      s.failed = true;
      Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
      return false;
    }
    return true;
  };

  if (s_rb_exchange->ShouldStall(live))
  {
    ++r.net_stalls;
    const auto stall_start = std::chrono::steady_clock::now();  // NSR telemetry
    const bool ok = wait_until([&] { return s_rb_exchange->ShouldStall(live); },
                               std::chrono::milliseconds(PEER_TIMEOUT_MS), true);
    NSRTelemetry::Count(NSRTelemetry::Counter::NetStalls);
    NSRTelemetry::Observe(NSRTelemetry::Hist::NetStallUs, waited_us(stall_start));
    // A stall is time the peers agreed to lose; without this the throttle
    // would replay it as fast frames and push this peer straight back ahead.
    Core::System::GetInstance().GetCoreTiming().NSRDeferThrottle(
        std::chrono::steady_clock::now() - stall_start);
    if (!ok)
      return false;
  }
  if (s_rb_confirm_live)
  {
    const auto is_unconfirmed = [&] {
      const u32 confirmed = s_rb_exchange->ConfirmedThrough();
      return confirmed == RollbackNet::NO_FRAME || confirmed < live;
    };
    if (is_unconfirmed())
    {
      const auto confirm_start = std::chrono::steady_clock::now();
      const bool ok = wait_until(is_unconfirmed, std::chrono::milliseconds(PEER_TIMEOUT_MS), true);
      Core::System::GetInstance().GetCoreTiming().NSRDeferThrottle(
          std::chrono::steady_clock::now() - confirm_start);
      if (!ok)
        return false;
    }
  }
  // Check twice per second after compensating remote progress for one-way
  // latency.  Only the genuinely faster peer stretches this frame, by less
  // than one frame; game state and virtual time are untouched.
  const u32 sync_wait_us = r.time_sync ? s_rb_exchange->TimeSyncWaitUs(live) : 0;
  if (sync_wait_us != 0)
  {
    ++r.sync_waits;
    const auto sync_start = std::chrono::steady_clock::now();  // NSR telemetry
    Log("time sync frame {}: advantage {:.3f} frames, stretch {} us", live,
        s_rb_exchange->FrameAdvantage(), sync_wait_us);
    wait_until([] { return true; }, std::chrono::microseconds(sync_wait_us), false);
    NSRTelemetry::Count(NSRTelemetry::Counter::SyncWaits);
    NSRTelemetry::Observe(NSRTelemetry::Hist::TimeSyncWaitUs, waited_us(sync_start));
    // Keep the stretch: Dolphin's speed limiter otherwise treats it as lag
    // (anything under MaxFallback, 100 ms by default) and runs the next frames
    // fast to catch up, which cancelled every stretch and left the faster
    // peer 2-3 frames ahead for the whole match (2026-10-11 two-human runs).
    Core::System::GetInstance().GetCoreTiming().NSRDeferThrottle(
        std::chrono::steady_clock::now() - sync_start);
  }
  RbDumpFinalized(s_rb_exchange->Finalized());
  if (const auto desync = s_rb_exchange->FirstDesync();
      desync && (!s_rb_logged_desync || *desync < *s_rb_logged_desync))
  {
    s_rb_logged_desync = desync;
    const RollbackNet::DesyncInfo info =
        s_rb_exchange->FirstDesyncInfo().value_or(RollbackNet::DesyncInfo{0, *desync, 0, 0});
    const StateDigest& sd = s_digest;
    const size_t index = r.ring.empty() ? 0 : *desync % r.ring.size();
    const bool have_semantic = index < sd.slot_frame.size() && sd.slot_frame[index] == *desync;
    Log("DESYNC: first confirmed state digest mismatch at frame {} with player {}: local "
        "{:016x} remote {:016x} (local semantic words {})",
        *desync, info.player, info.local, info.remote,
        have_semantic ? fmt::format("{:016x}", sd.slot_semantic[index]) : "n/a");
    NSRTelemetry::Desync(*desync, info.local, info.remote, info.player);  // NSR telemetry
  }
  {
    // Dump the first mismatching frame, or the first NSR_RB_DESYNC_DUMPS of
    // them (diagnostics: later ones show whether a divergence persists or
    // was a transient per-machine difference).
    static const size_t dumps = ParseU32(Env("NSR_RB_DESYNC_DUMPS")).value_or(1);
    const auto& all = s_rb_exchange->Desyncs();
    for (; s_rb_desyncs_dumped < all.size() && s_rb_desyncs_dumped < dumps; ++s_rb_desyncs_dumped)
    {
      if (s_rb_desyncs_dumped > 0)
        Log("digest mismatch at frame {} with player {}: local {:016x} remote {:016x}",
            all[s_rb_desyncs_dumped].frame, all[s_rb_desyncs_dumped].player,
            all[s_rb_desyncs_dumped].local, all[s_rb_desyncs_dumped].remote);
      RbDumpDesync(Core::System::GetInstance(), all[s_rb_desyncs_dumped].frame);
    }
  }
  return !s.failed;
}

u32 RbStep(const Core::CPUThreadGuard& guard, u32 buf)
{
  auto& system = guard.GetSystem();
  Rollback& r = s_rb;
  if (!r.initialized)
    RbInit(system);
  s_rb_last_tick_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  // Wait before touching live-unit state. Host sleep advances no guest clocks, refreshes the
  // watchdog, and observes Stopping before the core joins this CPU thread.
  Pads follow_pads{};
  if (s_follow || s_follow_failed)
  {
    bool terminal = s_follow_failed;
    while (Core::IsRunningOrStarting(system))
    {
      s_rb_last_tick_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
      if (!terminal)
      {
        std::array<unsigned char, 48> bytes{};
        const auto result = s_follow->Poll(r.live, bytes);
        if (result == ReplayFollow::Result::Pads)
        {
          const auto backlog = s_follow->Backlog(r.live);
          // Hysteresis avoids toggling unlimited speed at the edge of the delayed head.
          const bool catching = s_follow_catchup.load() ? backlog > s_follow_delay + 2 :
                                                         backlog > s_follow_delay + 4;
          if (catching != s_follow_catchup.exchange(catching))
          {
            Config::SetCurrent(Config::MAIN_EMULATION_SPEED, catching ? 0.0f : 1.0f);
            Log("follow catch-up {} at unit {} backlog {}", catching, r.live, backlog);
          }
          std::memcpy(follow_pads.data(), bytes.data(), bytes.size());
          break;
        }
        terminal = result == ReplayFollow::Result::Complete ||
                   result == ReplayFollow::Result::Error;
        if (result == ReplayFollow::Result::Complete)
        {
          Log("follow complete after {} units", r.live);
          Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
        }
        if (result == ReplayFollow::Result::Error)
        {
          Log("follow failed at unit {}: {}", r.live, s_follow->Error());
          s_follow->Status(".error", s_follow->Error() + "\n");
          Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!Core::IsRunningOrStarting(system))
      return 0;
  }
  u8* ram = system.GetMemory().GetRAM();
  const u32 t = r.live;
  ++r.ticks_since_poll;
  r.tick_running = t;
  const bool fixed_entry = !r.session_ended && r.vtime_from && t == r.vtime_from;
  const bool test_reentry = r.enter_pending && r.config.auto_reenter;
  const bool automatic_entry = r.enter_pending && r.auto_handoff;
  if (!r.vtime_on && (fixed_entry || test_reentry || automatic_entry))
  {
    // A failed session (e.g. a refused config) is already stopping: don't
    // retry the barrier every frame until the core stops.
    u32 host_clock = 0;
    u32 host_aux_clock = 0;
    bool clocks_ready = true;
    if (automatic_entry && r.session_generation > 0)
    {
      clocks_ready = HandoffClockBarrier(guard, t, &host_clock, &host_aux_clock);
      if (clocks_ready)
        NormalizeTransitionClocks(guard, host_clock, host_aux_clock);
    }
    if (!automatic_entry ||
        (clocks_ready && !s_session.failed &&
         HandoffBarrier(guard, RollbackNet::HandoffKind::EnterRollback, t, &host_clock,
                        &host_aux_clock)))
    {
      // Lockstep proves the semantic state, but VI retrace callbacks are
      // scheduled independently on each host. The game reads this counter
      // for timers and sound rate limiting, so every peer adopts player 0's
      // natural baseline before the rollback epoch starts.
      if (automatic_entry)
        NormalizeTransitionClocks(guard, host_clock, host_aux_clock);
      EnterRollbackSession(guard, t);
    }
  }
  if (!r.vtime_on)
  {
    // The rollback image still routes every main-loop iteration through this
    // stub.  Outside a match it is deliberately a thin pass-through: sample
    // this frame's input, never snapshot/predict/restore, and leave every
    // gate disabled so menus and loading use the game's original clocks.
    Pads pass_pads = r.latest;
    if (s_follow)
      pass_pads = follow_pads;
    else if (const auto it = r.replay.find(t); it != r.replay.end())
      pass_pads = it->second;
    else if (r.menu_frame_lockstep)
    {
      // Post-match menus cannot return to retrace-keyed lockstep because
      // rollback histories intentionally differ.  MAX_PRED=0 turns the same
      // absolute-frame exchange into delay-based lockstep: this host wait
      // completes only after every player's input for t is confirmed.
      if (!RbNetworkPreflight(t))
        return 0;
      pass_pads = FromNetPads(s_rb_exchange->Assemble(t));
    }
    if (r.record)
    {
      std::fwrite(&t, 4, 1, r.record);
      std::fwrite(pass_pads.data(), 1, sizeof(Pads), r.record);
      std::fflush(r.record);
    }
    RbWritePads(guard, buf, pass_pads);
    // Post-match lockstep is frame-driven even though rollback ownership is
    // off: game time/audio queries and instant DVD completion must observe
    // the same frame on every peer while the next match is loading.
    r.in_unit = r.menu_frame_lockstep;
    r.in_resim = false;
    r.frame_tick_done = false;
    r.vtime_calls = 0;
    r.live = t + 1;
    r.cursor = r.live;
    r.rolled = false;
    s_rb_watch_live = r.live;
    return 0;
  }
  const u32 slots = r.ring.empty() ? 1 : static_cast<u32>(r.ring.size());
  RbSampleIo(guard);
  s_rb_confirm_live = r.net_enabled && r.cursor == r.live && !r.rolled &&
                      RollbackLogic::PausedFrame(PowerPC::MMU::HostRead<u32>(guard, PAUSE_DEPTH));
  if (r.net_enabled && r.cursor == r.live && !r.rolled && !RbNetworkPreflight(r.live))
    return 0;
  if (r.disc_commands != r.disc_commands_seen)
  {
    if (r.config.disc_trace && r.vtime_on)
      Log("disc commands before unit live {} cursor {} rolled {}: {}", r.live, r.cursor, r.rolled,
          r.disc_commands - r.disc_commands_seen);
    r.disc_commands_seen = r.disc_commands;
  }

  // NSR_RB_MEMWATCH: armed at the start of the NSR_RB_WATCH window (memchecks
  // make the JIT very slow, so not from boot).
  if (!r.memwatch_armed && r.watch && r.live >= r.watch_from)
  {
    r.memwatch_armed = true;
    if (const auto addr = ParseU32(Env("NSR_RB_MEMWATCH")))
    {
      TMemCheck mc;
      mc.start_address = *addr;
      mc.end_address = *addr + 3;
      mc.is_ranged = true;
      mc.is_break_on_write = true;
      mc.log_on_hit = true;
      system.GetPowerPC().GetMemChecks().Add(std::move(mc));
      Log("watching writes to {:08x} from tick {}", *addr, r.live);
    }
  }
  if (r.watch && r.live >= r.watch_from && r.live <= r.watch_to)
    Log("watch tick-start live {} cursor {} rolled {}: [{:08x}] = {:08x}", r.live, r.cursor,
        r.rolled, r.watch, PowerPC::MMU::HostRead<u32>(guard, r.watch));
  std::array<u32, TICK_COUNTERS.size()> counter_addr{};
  for (size_t i = 0; i < TICK_COUNTERS.size() && !r.frame_mode; ++i)
  {
    const u32 obj = PowerPC::MMU::HostRead<u32>(guard, TICK_COUNTERS[i].cell);
    counter_addr[i] = (obj >> 24) == 0x80 ? obj + TICK_COUNTERS[i].offset : 0;
  }
  if (r.cursor == r.live && !r.rolled)
  {
    auto& rec = r.counters[r.live];
    for (size_t i = 0; i < TICK_COUNTERS.size(); ++i)
      rec[i] = counter_addr[i] ? PowerPC::MMU::HostRead<u32>(guard, counter_addr[i]) : 0;
  }

  if (r.cursor == r.live)
  {
    if (r.rolled)
    {
      // Re-simulation reached the live tick again: the state must match what
      // it was before the rollback.
      if (r.mispredict == 0 && !r.net_enabled)  // corrected net pads intentionally differ
      {
        PhaseTimer timer(PH_CHECK);
        for (const auto& [lo, hi] : RbLiveRanges(system, ram))
          std::memcpy(r.ring[r.live % slots].data() + lo, ram + lo, hi - lo);
        RbCheck(r.live, ram, r.ring[r.live % slots]);
      }
    }
    else if (r.net_enabled && s_rb_exchange && s_rb_exchange->PendingRollback())
    {
      const u32 pending = *s_rb_exchange->PendingRollback();
      u32 target = pending;
      const u32 lower = RollbackLogic::RollbackLowerBound(r.live, slots, r.vtime_from, r.barrier);
      bool reachable = pending >= lower;
      while (reachable && target > lower &&
             !(r.io_idle.contains(target) && r.io_idle[target]))
      {
        --target;
      }
      const bool quiet = reachable && IsIoIdle(ram) && r.io_idle.contains(target) &&
                         r.io_idle[target];
      if (!reachable || (!quiet && r.live - pending >= slots - 2))
      {
        Log("UNCORRECTABLE network input at {} (live {}, lower {}, quiet {})", pending, r.live,
            lower, quiet);
        ++r.uncorrectable;
        s_session.failed = true;
        Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
      }
      else if (quiet)
      {
        RbSave(ram, r.ring[r.live % slots]);
        RbRestore(system, ram, r.ring[target % slots]);
        r.cursor = target;
        r.rolled = true;
        ++r.rollbacks;
        ++r.mispredictions;
        s_rb_exchange->BeginRollback();
        // Digests of later frame starts describe the abandoned timeline;
        // re-simulation records them again (H4).
        s_rb_exchange->DiscardChecksumsAfter(target);
      }
      else
      {
        ++r.skipped_busy;
      }
    }
    else if (r.mispredict > 0 && r.force > 0)
    {
      // Misprediction mode: confirm frame c = live - d.
      const u32 now = r.live, d = r.mispredict;
      const u32 start = std::max(r.force_from, r.vtime_from);
      if (now >= start + d)
      {
        const u32 c = now - d;
        if (r.pads.contains(c) && r.pads[c] != TruePads(c))
        {
          r.mispredict_pending = std::min(r.mispredict_pending, c);
          ++r.mispredictions;
        }
      }
      if (r.mispredict_pending != UINT32_MAX)
      {
        u32 target = r.mispredict_pending;
        // Never restore before the session, the ring or a load barrier.
        const u32 lower = RollbackLogic::RollbackLowerBound(now, slots, start, r.barrier);
        bool ok = target >= lower;
        if (ok && r.io_idle_mode)
        {
          while (target > lower && !(r.io_idle.contains(target) && r.io_idle[target]))
            --target;
          ok = IsIoIdle(ram) && target >= lower && r.io_idle.contains(target) && r.io_idle[target];
        }
        if (!ok && r.mispredict_pending < lower)
        {
          // Uncorrectable (in netplay: needs a state resync). Report it and
          // drop it instead of retrying forever while running on wrong pads.
          Log("UNCORRECTABLE misprediction at {} (live {}, lower bound {})", r.mispredict_pending, now,
              lower);
          ++r.uncorrectable;
          r.mispredict_pending = UINT32_MAX;
        }
        if (ok)
        {
          // Corrected pads: true up to the newest confirmed frame, then the
          // new prediction (its pads) for the unconfirmed ones.
          const u32 confirmed = now >= d ? now - d : 0;
          for (u32 j = target; j < now; ++j)
            r.pads[j] = TruePads(std::min(j, confirmed));
          // No save of the state before frame `now` here: nothing reads it (the
          // re-simulation check is off in this mode) and the live frame `now`
          // saves its slot again when re-simulation reaches it.
          RbRestore(system, ram, r.ring[target % slots]);
          r.cursor = target;
          r.rolled = true;
          ++r.rollbacks;
          r.mispredict_pending = UINT32_MAX;
        }
        else if (r.mispredict_pending != UINT32_MAX)
        {
          ++r.skipped_busy;  // retry next frame (deeper rollback then)
        }
      }
    }
    else if (r.force > 0 && r.live >= r.force + r.force_from && r.live <= r.force_to &&
             ForcedDepth(r.live) > 0)
    {
      // Forced rollback (stage 1 test): keep the state before this unit, go
      // back `depth` units (not past the load barrier), and re-run them with
      // the pads they had.
      const u32 depth = std::min(ForcedDepth(r.live), r.live - std::min(r.live, r.vtime_from));
      u32 target = std::max(r.live - depth, r.barrier);
      if (r.io_idle_mode)
      {
        // Only while idle now, to the newest idle snapshot at or before the
        // target still in the ring.
        target = r.live - depth;
        // Never search past the ring, the session start or a load barrier.
        const u32 lower =
            RollbackLogic::RollbackLowerBound(r.live, slots, r.vtime_from, r.barrier);
        while (target > lower && !(r.io_idle.contains(target) && r.io_idle[target]))
          --target;
        if (!IsIoIdle(ram) || target < lower || !r.io_idle.contains(target) || !r.io_idle[target])
        {
          ++r.skipped_busy;
          target = r.live;  // no rollback this unit
        }
      }
      if (target < r.live)
      {
        if (target != r.live - depth)
          ++r.clamped;
        RbSave(ram, r.ring[r.live % slots]);
        RbRestore(system, ram, r.ring[target % slots]);
        r.cursor = target;
        r.rolled = true;
        ++r.rollbacks;
      }
    }
  }

  if (const auto it = r.counters.find(r.cursor); it != r.counters.end())
  {
    for (size_t i = 0; i < TICK_COUNTERS.size(); ++i)  // harmless when unchanged
    {
      if (counter_addr[i])
        PowerPC::MMU::HostWrite<u32>(guard, it->second[i], counter_addr[i]);
    }
  }

  if (r.cursor < r.live)
  {
    // Re-simulated unit. Its snapshot is re-saved, so its I/O-idle flag must
    // be re-evaluated too: re-simulation runs faster (no drawing), so disc
    // I/O can complete at different unit boundaries than live.
    RbTestCorrupt(ram, r.cursor);
    RbSave(ram, r.ring[r.cursor % slots]);
    if (r.io_idle_mode)
      r.io_idle[r.cursor] = IsIoIdle(ram);
    if (r.net_enabled && s_rb_exchange)
      r.pads[r.cursor] = FromNetPads(s_rb_exchange->Assemble(r.cursor));
    RbWritePads(guard, buf, r.pads.at(r.cursor));
    if (r.net_enabled && s_rb_exchange)
    {
      // Same point as the live unit's digest: snapshot saved, pads written.
      if (r.cursor % r.config.digest_every == 0)
        s_rb_exchange->SetStateChecksum(
            r.cursor, RbStateDigest(system, &r.ring[r.cursor % slots],
                                    RbSemanticChecksum(guard), r.cursor));
      s_rb_exchange->UpdateFinalized();
    }
    RbDumpUnit(system, ram, r.live, r.cursor);
    RbTransplant(system, ram, r.live, r.cursor);
    r.in_unit = true;
    r.in_resim = true;
    r.tick_running = r.cursor;
    r.vtime_calls = 0;
    r.gate_index = 0;
    ++r.cursor;
    ++r.resim_ticks;
    return 1;  // run another unit after this one
  }

  // Live tick.
  RbTransplant(system, ram, t, t);
  if (r.perturb_at && t + 1 == r.perturb_at)
    RbSave(ram, r.prev);
  if (r.perturb_at && t == r.perturb_at)
  {
    // Dependency probe (no rollback): put the NSR_RB_RANGES state back to
    // what it was one tick earlier, then carry on. If the game state later
    // differs from a normal run, the sim reads something in those ranges
    // that changed in between (e.g. written by the rest of the frame).
    RbRestore(system, ram, r.prev);
    Log("perturbed {} ranges at tick {}", r.segments.size(), t);
  }
  if (!r.profile_path.empty() && t >= r.force_from)  // NSR_RB_FORCE_FROM = profile start
  {
    if (r.have_end)
      RbCountWrites(ram, r.at_end, r.outside);
    RbSave(ram, r.at_start);
    if (t % 600 == 0 && t > 0)
      RbWriteProfile();
  }
  if (std::find(r.dump_at.begin(), r.dump_at.end(), t) != r.dump_at.end())
  {
    const std::string path = Env("NSR_DUMP_PREFIX", "local/nsr") + "-tick-" + std::to_string(t) + ".bin";
    if (FILE* f = std::fopen(path.c_str(), "wb"))
    {
      std::fwrite(ram, 1, system.GetMemory().GetRamSizeReal(), f);
      std::fclose(f);
      Log("wrote {}", path);
      WritePerMachineSidecar(system, ram, path);
      WriteAramRingSidecar(system, path);  // ARAM replay ring (final-review M1)
    }
  }
  if (r.hash_log && t % 60 == 0)
    Log("tick {} hash {:016x}", t, RbHash(ram));
  if (r.config.io_trace && t % 5 == 0)
  {
    auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
    Log("io unit {}: dvd exec {:08x} q0 {:08x} worker state {} queue {:08x} srr0 {:08x} semA {} semB {} "
        "pending {} idle {}",
        t, rd(0x803ED118), rd(0x803A55D8), rd(DISC_WORKER_THREAD + 0x2C8) >> 16,
        rd(DISC_WORKER_THREAD + 0x2DC), rd(DISC_WORKER_THREAD + 0x198), rd(0x80381C00),
        rd(0x80381C0C), rd(PENDING_LOADS), IsIoIdle(ram));
    Log("io unit {}: mq@80381C00 {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
        t, rd(0x80381C00), rd(0x80381C04), rd(0x80381C08), rd(0x80381C0C), rd(0x80381C10),
        rd(0x80381C14), rd(0x80381C18), rd(0x80381C1C), rd(0x80381C20), rd(0x80381C24));
  }
  if (r.io_idle_mode)
  {
    r.io_idle[t] = IsIoIdle(ram);
    if (r.config.quiet_trace)
    {
      auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
      const bool dvd = rd(0x803ED118) == 0;
      const bool worker = (rd(DISC_WORKER_THREAD + 0x2C8) >> 16) == 4 && rd(DISC_SEM_B) == 0 &&
                          rd(DISC_SEM_A) == DISC_SLOTS_IDLE;
      const bool loads = rd(PENDING_LOADS) == 0;
      const bool audio = (rd(AUDIO_THREAD + 0x2C8) >> 16) == 4 && rd(AUDIO_THREAD + 0x2DC) == AUDIO_THREAD_QUEUE;
      Log("q {} {} dvd {} worker {} loads {} audio {}", t, r.io_idle[t] ? 1 : 0, dvd, worker, loads, audio);
    }
    if (t >= 600)
      r.io_idle.erase(r.io_idle.begin(), r.io_idle.lower_bound(t - 600));
  }
  RbTestCorrupt(ram, t);
  if (r.force > 0 || r.net_enabled)
    RbSave(ram, r.ring[t % slots]);
  if (s_follow)
    r.pads[t] = follow_pads;
  else if (r.net_enabled && s_rb_exchange)
    r.pads[t] = FromNetPads(s_rb_exchange->Assemble(t));
  else if (const auto it = r.replay.find(t); it != r.replay.end())
    r.pads[t] = it->second;  // same inputs per tick as a recorded run
  else
    r.pads[t] = r.latest;
  if (r.mispredict > 0 && r.force > 0 && t >= std::max(r.force_from, r.vtime_from) + r.mispredict)
    r.pads[t] = TruePads(t - r.mispredict);  // prediction: last confirmed pads
  if (r.record)
  {
    std::fwrite(&t, 4, 1, r.record);
    std::fwrite(r.pads[t].data(), 1, sizeof(Pads), r.record);
    std::fflush(r.record);
  }
  RbWritePads(guard, buf, r.pads[t]);
  RbAdopt(system, ram, t);
  RbDumpUnit(system, ram, t, t);
  if (t == r.vtime_from)
  {
    AudioClockSessionStart(guard);
    // The model lives in restored RAM, so the first snapshot must contain
    // its initialized magic and queues.  RbAdopt intentionally precedes it.
    if (!r.ring.empty())
      RbSave(ram, r.ring[t % slots]);
  }
  if (r.net_enabled && s_rb_exchange)
  {
    // The digest describes exactly what a re-simulation of t starts from:
    // the final snapshot of t (after the session-start model) with t's pads
    // written (the pad buffer itself is excluded).
    if (t % r.config.digest_every == 0)
      s_rb_exchange->SetStateChecksum(
          t, RbStateDigest(system, &r.ring[t % slots], RbSemanticChecksum(guard), t));
    s_rb_exchange->UpdateFinalized();
  }
  r.in_unit = true;
  r.in_resim = false;
  r.tick_running = t;
  r.vtime_calls = 0;
  r.gate_index = 0;
  r.gate_log.erase(t);  // a live tick runs once; record it afresh
  r.live = t + 1;
  r.cursor = r.live;
  r.rolled = false;
  s_rb_watch_live = r.live;
  s_rb_watch_rollbacks = r.rollbacks;
  if (t >= 600)
  {
    r.pads.erase(r.pads.begin(), r.pads.lower_bound(t - 600));
    r.counters.erase(r.counters.begin(), r.counters.lower_bound(t - 600));
    r.gate_log.erase(r.gate_log.begin(), r.gate_log.lower_bound(t - 600));
    if (s_rb_exchange)
      s_rb_exchange->TrimBefore(t - 600);
  }
  if (s_dirty.mode == DirtyTracker::Mode::Kernel && (t % 600 == 0 || t == r.vtime_from))
    s_dirty.RefreshViews();
  if (t % 600 == 0)
  {
    if (s_dirty.on && s_dirty.samples)
      Log("snapshot dirty tracking: {} samples, {:.1f} us/sample, {} remapped views, {:.1f} written "
          "pages/sample, {} GX FIFO page copies skipped",
          s_dirty.samples, s_dirty.sample_ns / 1e3 / s_dirty.samples, s_dirty.remaps,
          static_cast<double>(s_dirty.pages_seen) / s_dirty.samples, s_dirty.fifo_skipped);
    if (s_dirty.mode == DirtyTracker::Mode::Hash)
      Log("snapshot hash audit: {} pages compared, {} differed", s_dirty.audit_checked,
          s_dirty.audit_mismatches);
    if (s_dirty.mode == DirtyTracker::Mode::Protect && s_dirty.exhaustive_audit)
      Log("snapshot protect audit: {} pages hashed, {} tracker misses", s_dirty.protect_audit_checked,
          s_dirty.protect_audit_misses);
    if (s_digest.digests)
      Log("state digest: {} digests, {:.1f} us/digest (classify {:.1f} us: per-machine {:.1f}, "
          "heaps {:.1f}), {:.1f} pages "
          "rehashed/digest, {:.1f} partly excluded pages/digest, {:.1f} tracker hashes reused/"
          "digest, {} excluded ranges",
          s_digest.digests, s_digest.total_ns / 1e3 / s_digest.digests,
          s_digest.classify_ns / 1e3 / s_digest.digests,
          s_digest.per_machine_ns / 1e3 / s_digest.digests, s_digest.heap_ns / 1e3 / s_digest.digests,
          static_cast<double>(s_digest.rehashed) / s_digest.digests,
          static_cast<double>(s_digest.direct) / s_digest.digests,
          static_cast<double>(s_digest.reused) / s_digest.digests, s_digest.excluded.size());
    Log("tick {}: rollbacks {}, re-simulated {}, mismatched checks {}/{}; audio gate: "
        "recorded {}, replayed {}, mismatched {}; load barriers {} (clamped {}, in re-sim {}); "
        "io-idle skips {}; mispredictions {} (uncorrectable {}); net stalls {} sync waits {}",
        t, r.rollbacks, r.resim_ticks, r.mismatched_checks, r.checks, r.gate_recorded,
        r.gate_replayed, r.gate_mismatched, r.barrier_events, r.clamped, r.barrier_resim,
        r.skipped_busy, r.mispredictions, r.uncorrectable, r.net_stalls, r.sync_waits);
    AramRingReport(fmt::format("tick {}", t).c_str());  // final-review M1
    if (r.mismatched_checks > 0)
      RbReport(15);  // Dolphin is usually killed, not shut down: report as we go
    // Revisit NSR_RB_VMEM_BYTES if the game ever uses fake VMEM past it.
    if (r.vmem && r.vmem_bytes)
    {
      const u32 total = system.GetMemory().GetFakeVMemSize();
      for (u32 off = r.vmem_bytes; off < total; off += 0x1000)
      {
        const u8* p = r.vmem + off;
        if (std::any_of(p, p + 0x1000, [](u8 b) { return b != 0; }))
        {
          Log("WARNING: fake VMEM in use at {:08x}, past the rolled-back {:#x} bytes",
              0x7E000000 + off, r.vmem_bytes);
          break;
        }
      }
    }
    if (r.gate_recorded > 0)
    {
      std::map<u32, u32> census;  // gated entry -> calls in the kept window
      for (const auto& [tick, calls] : r.gate_log)
        for (const auto& c : calls)
          ++census[c.entry];
      std::string line;
      for (const auto& [entry, n] : census)
        line += fmt::format(" {:08x}x{}", entry, n);
      Log("audio gate calls:{}", line);
    }
  }
  r.frame_tick_done = false;
  return 0;  // this was the last unit for now
}

void RollbackTickHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();
  const u32 buf = ppc_state.gpr[3];
  ppc_state.npc = LR(ppc_state);
  Rollback& r = s_rb;
  if (r.frame_mode)
  {
    // The frame stub already handled rollback for this frame; the tick just
    // gets the frame's pads (one tick per frame during the session).
    ++r.ticks_since_poll;
    if (const auto it = r.pads.find(r.tick_running); it != r.pads.end())
      RbWritePads(guard, buf, it->second);
    ppc_state.gpr[3] = 0;
    return;
  }
  ppc_state.gpr[3] = RbStep(guard, buf);
}

// ---------------------------------------------------------------------------
// Re-simulation cost diagnostics (NSR_RB_COST=1, NSR_RB_PROF=<cycles>).
// Per unit (frame stub -> frame-end stub): emulated time base ticks (40.5 MHz),
// CPU cycles the idle-skip fast-forwarded (all threads waiting), emulated GPU
// command cycles, host microseconds, and host time of RbStep (snapshot work).
// The profiler samples, every <cycles> emulated CPU cycles while a unit runs,
// what the CPU is doing: the PC when the main thread runs, otherwise the
// running thread and the main thread's saved call stack (why it waits).
// NSR_RB_COST is host-side only (reads counters, writes the log). NSR_RB_PROF
// schedules a CoreTiming event: it changes nothing the game sees directly,
// but shorter slices move the points where interrupts are taken, so use it
// for profiling, not for gate runs.
}  // namespace NSRNetplay
extern u64 g_nsr_gpu_cycles;
extern bool g_nsr_skip_vertices;  // VideoCommon/VertexLoaderManager.cpp
extern bool g_nsr_host_timing;
extern u64 g_nsr_host_ns[2];  // [0] GPU command processing, [1] DSP slices + audio DMA
namespace NSRNetplay
{
namespace
{
struct CostState
{
  bool active = false;
  u64 ph_step[PH_COUNT] = {};  // phase totals before RbStep
  u64 ph_unit[PH_COUNT] = {};  // ... after RbStep (unit start)
  u64 dev_unit[2] = {};        // GPU/DSP host totals at unit start
  u64 digest_step = 0;         // state digest host ns before RbStep
  u64 digest_us = 0;           // ... spent in this unit's RbStep
  std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
  u64 tb = 0, idle = 0, gpu = 0;
  s64 step_us = 0;
  u32 frame = 0;
  bool resim = false;
  std::chrono::steady_clock::time_point host;
  // profiler
  CoreTiming::EventType* prof_event = nullptr;
  s64 prof_interval = 0;
  bool prof_scheduled = false;
  std::map<std::string, u64> prof[2];  // [resim]
  u64 prof_n[2] = {0, 0};
};
CostState s_cost;

void ProfDump()
{
  for (int k = 0; k < 2; ++k)
  {
    std::vector<std::pair<u64, std::string>> v;
    for (const auto& [key, n] : s_cost.prof[k])
      v.emplace_back(n, key);
    std::sort(v.rbegin(), v.rend());
    Log("prof {} samples {}", k ? "resim" : "live", s_cost.prof_n[k]);
    for (size_t i = 0; i < v.size() && i < 60; ++i)
      Log("prof {} {} {}", k ? "resim" : "live", v[i].first, v[i].second);
    s_cost.prof[k].clear();
    s_cost.prof_n[k] = 0;
  }
}

void ProfSample(Core::System& system, u64, s64)
{
  s_cost.prof_scheduled = false;
  Rollback& r = s_rb;
  if (!r.vtime_on)
    return;
  if (r.in_unit)
  {
    const u8* ram = system.GetMemory().GetRAM();
    auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
    const auto& ppc = system.GetPPCState();
    const u32 cur = rd(0x800000E4);
    std::string key;
    if (cur == 0x803A3F30)
      key = fmt::format("run {:08x}", ppc.pc);
    else
    {
      key = fmt::format("wait cur {:08x} pc {:08x} main", cur, ppc.pc);
      u32 sp = rd(0x803A3F30 + 4);  // main OSThread: OSContext gpr[1]
      for (int i = 0; i < 6 && (sp >> 24) == 0x80; ++i)
      {
        sp = rd(sp);
        if ((sp >> 24) != 0x80)
          break;
        key += fmt::format(" {:08x}", rd(sp + 4));
      }
    }
    ++s_cost.prof[r.in_resim ? 1 : 0][key];
    ++s_cost.prof_n[r.in_resim ? 1 : 0];
  }
  system.GetCoreTiming().ScheduleEvent(s_cost.prof_interval, s_cost.prof_event);
  s_cost.prof_scheduled = true;
}

void CostFrameStart(const Core::CPUThreadGuard& guard,
                    std::chrono::steady_clock::time_point step_start)
{
  static const bool cost = !Env("NSR_RB_COST").empty();
  static const s64 prof = static_cast<s64>(ParseU32(Env("NSR_RB_PROF")).value_or(0));
  static const u32 prof_every = ParseU32(Env("NSR_RB_PROF_EVERY")).value_or(1000);
  auto& system = guard.GetSystem();
  const Rollback& r = s_rb;
  if (prof > 0 && r.vtime_on)
  {
    if (!s_cost.prof_event)
    {
      s_cost.prof_interval = prof;
      s_cost.prof_event = system.GetCoreTiming().RegisterEvent("NSRProfile", ProfSample);
    }
    if (!s_cost.prof_scheduled)
    {
      system.GetCoreTiming().ScheduleEvent(prof, s_cost.prof_event);
      s_cost.prof_scheduled = true;
    }
    if (!r.in_resim && r.tick_running % prof_every == 0 && s_cost.prof_n[0] + s_cost.prof_n[1])
      ProfDump();
  }
  if (!cost || !r.vtime_on)
    return;
  s_cost.active = true;
  s_cost.step_us = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - step_start)
                       .count();
  s_cost.frame = r.tick_running;
  s_cost.resim = r.in_resim;
  s_cost.tb = system.GetSystemTimers().GetFakeTimeBase();
  s_cost.idle = system.GetCoreTiming().GetIdleTicks();
  s_cost.gpu = g_nsr_gpu_cycles;
  g_nsr_host_timing = true;
  std::copy(std::begin(s_phase_ns), std::end(s_phase_ns), s_cost.ph_unit);
  std::copy(std::begin(g_nsr_host_ns), std::end(g_nsr_host_ns), s_cost.dev_unit);
  s_cost.digest_us = (s_digest.total_ns - s_cost.digest_step) / 1000;
  s_cost.host = std::chrono::steady_clock::now();
}

void CostFrameEnd(const Core::CPUThreadGuard& guard)
{
  if (!s_cost.active)
    return;
  s_cost.active = false;
  auto& system = guard.GetSystem();
  const auto now = std::chrono::steady_clock::now();
  const auto us = [](u64 ns) { return ns / 1000; };
  const u64 tb = system.GetSystemTimers().GetFakeTimeBase();
  // Per unit: emulated time, idle-skipped and GPU cycles; host time of the
  // unit (host_us; of which gpu/dsp emulation), of RbStep before it (step_us;
  // of which save/restore/live/jit/check), and the host clock at its end.
  Log("cost frame {} resim {} tb {} idle_cyc {} gpu_cyc {} host_us {} step_us {} at {} "
      "save {} restore {} live {} jit {} check {} gpu_us {} dsp_us {} hat {} digest {}",
      s_cost.frame, s_cost.resim ? 1 : 0, tb - s_cost.tb,
      system.GetCoreTiming().GetIdleTicks() - s_cost.idle, g_nsr_gpu_cycles - s_cost.gpu,
      std::chrono::duration_cast<std::chrono::microseconds>(now - s_cost.host).count(),
      s_cost.step_us, tb, us(s_cost.ph_unit[PH_SAVE] - s_cost.ph_step[PH_SAVE]),
      us(s_cost.ph_unit[PH_RESTORE] - s_cost.ph_step[PH_RESTORE]),
      us(s_cost.ph_unit[PH_LIVE] - s_cost.ph_step[PH_LIVE]),
      us(s_cost.ph_unit[PH_JIT] - s_cost.ph_step[PH_JIT]),
      us(s_cost.ph_unit[PH_CHECK] - s_cost.ph_step[PH_CHECK]),
      us(g_nsr_host_ns[0] - s_cost.dev_unit[0]), us(g_nsr_host_ns[1] - s_cost.dev_unit[1]),
      std::chrono::duration_cast<std::chrono::microseconds>(now - s_cost.epoch).count(),
      s_cost.digest_us);
}
// Re-simulated frames run on a faster emulated machine: NSR_RB_RESIM_SPEED
// (default 16; 1 = off) times faster CPU, and the devices whose work the
// CPU's frame waits on or hands off are sped up by the same factor, so their
// progress per re-simulated instruction stays what it was:
//   - single-core GPU command processing (NSR_RB_RESIM_GPU_SPEED);
//   - ARAM DMA (NSR_RB_RESIM_ARAM_SPEED);
//   - the audio DMA cadence that drives AX frames, and the DSP slice
//     (NSR_RB_RESIM_AUDIO_SPEED); re-simulated audio is not sent to the
//     speakers (it was heard live).
// VI, the time base/decrementer (OS alarms) and SI stay at real speed.
//
// Why: a re-simulated frame waits for nothing any more (no retrace wait, no
// present fence; measured 0% idle), so its ~9 ms of emulated time was pure
// CPU work of the full frame (sim + draw-list building, ~4.5M cycles at
// 486 MHz): at k = 7 about 5 retraces of emulated time per live frame.
//
// Why the devices too: audio rolls back with the game, and the AX callbacks
// that advance it run at interrupt level during re-simulation. With only the
// CPU faster, a re-simulated frame got ~16x fewer AX frames and ARAM DMA
// steps than before, so under sustained rollback the rolled-back audio engine
// fell behind (whole-RAM gate: ~430 bytes of audio-engine/bank state differ
// at frame 5000; EA audio library read garbage at frame 5491).
//
// Determinism: game code sees virtual, per-frame time inside units
// (VirtualTime), never cycles. What changes is where real-time hardware
// events (VI, alarms) land relative to re-simulated instructions; that
// already differs between machines and between live and re-simulated runs,
// and what the sim reads that follows it is answered per frame (audio clock,
// music events, VI counters). Verified with the RNG and whole-RAM gates.
void ResimSpeed(const Core::CPUThreadGuard& guard, bool resim)
{
  static const float factor = [] {
    const std::string v = Env("NSR_RB_RESIM_SPEED");
    return v.empty() ? 16.0f : std::max(1.0f, std::strtof(v.c_str(), nullptr));
  }();
  static const float gpu_factor = [] {
    const std::string v = Env("NSR_RB_RESIM_GPU_SPEED");
    return v.empty() ? factor : std::max(1.0f, std::strtof(v.c_str(), nullptr));
  }();
  if (factor != 1.0f)
    guard.GetSystem().GetCoreTiming().SetNSRSpeedFactor(resim ? factor : 1.0f);
  if (gpu_factor != 1.0f)
    guard.GetSystem().GetFifo().SetNSRSpeedFactor(resim ? gpu_factor : 1.0f);
  static const float aram_factor = [] {
    const std::string v = Env("NSR_RB_RESIM_ARAM_SPEED");
    return v.empty() ? factor : std::max(1.0f, std::strtof(v.c_str(), nullptr));
  }();
  if (aram_factor != 1.0f)
    guard.GetSystem().GetDSP().SetNSRSpeedFactor(resim ? aram_factor : 1.0f);
  static const float audio_factor = [] {
    const std::string v = Env("NSR_RB_RESIM_AUDIO_SPEED");
    return v.empty() ? factor : std::max(1.0f, std::strtof(v.c_str(), nullptr));
  }();
  if (audio_factor != 1.0f)
    guard.GetSystem().GetSystemTimers().SetNSRSpeedFactor(resim ? audio_factor : 1.0f);
  // Host time: re-simulated frames are never shown, so the GPU skips their
  // primitives (vertex loading and the backend's draw path: ~3 ms of host time
  // per frame even with the Null backend). Command parsing, register state,
  // EFB copies and the emulated GPU timing still run. NSR_RB_RESIM_DRAW=1
  // keeps them.
  static const bool resim_draw = !Env("NSR_RB_RESIM_DRAW").empty();
  g_nsr_skip_vertices = resim && !resim_draw;
}
}  // namespace

// Host profiler (diagnostics, NSR_HOST_PROF=<microseconds of CPU time>,
// NSR_HOST_PROF_OUT=<file>): a per-thread CPU-time timer interrupts the CPU
// thread and records the host instruction pointer, tagged with what the
// rollback code was doing (RbStep, a live unit, a re-simulated unit). Every
// 600 frames the cumulative counts are written as "phase module offset count"
// lines; module offsets of the Dolphin binary are symbolized offline (nm).
// Addresses outside any loaded file are JIT-compiled game code.
#if defined(__linux__) && defined(__x86_64__)
namespace
{
constexpr u32 HP_MAX = 1u << 20;
std::atomic<u32> s_hp_n{0};
u64 s_hp_pc[HP_MAX];
u8 s_hp_tag[HP_MAX];
volatile sig_atomic_t s_hp_phase = 0;  // 0 other, 1 live unit, 2 re-sim unit, 3 RbStep
std::map<std::tuple<int, std::string, u64>, u64> s_hp_counts;

void HostProfHandler(int, siginfo_t*, void* uc)
{
  const u32 i = s_hp_n.fetch_add(1, std::memory_order_relaxed);
  if (i < HP_MAX)
  {
    s_hp_pc[i] = static_cast<u64>(static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RIP]);
    s_hp_tag[i] = static_cast<u8>(s_hp_phase);
  }
}

void HostProfStart()
{
  static bool started = false;
  static const u32 us = ParseU32(Env("NSR_HOST_PROF")).value_or(0);
  if (started || us == 0)
    return;
  started = true;
  struct sigaction sa{};
  sa.sa_sigaction = HostProfHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  const int signo = SIGRTMIN + 7;
  sigaction(signo, &sa, nullptr);
  sigevent sev{};
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = signo;
  sev._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
  timer_t timer;
  if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer) != 0)
    return;
  itimerspec its{};
  its.it_interval.tv_nsec = static_cast<long>(us) * 1000;
  its.it_value = its.it_interval;
  timer_settime(timer, 0, &its, nullptr);
  Log("host profiler: every {} us of CPU-thread time", us);
}

void HostProfDump()
{
  static const std::string out = Env("NSR_HOST_PROF_OUT");
  const u32 n = std::min(s_hp_n.exchange(0), HP_MAX);
  if (n == 0 || out.empty())
    return;
  for (u32 i = 0; i < n; ++i)
  {
    Dl_info info{};
    std::string module = "jit";
    u64 offset = s_hp_pc[i];
    if (dladdr(reinterpret_cast<void*>(s_hp_pc[i]), &info) && info.dli_fname)
    {
      module = info.dli_fname;
      offset = s_hp_pc[i] - reinterpret_cast<u64>(info.dli_fbase);
    }
    else
    {
      offset = 0;  // JIT code: one bucket
    }
    ++s_hp_counts[{s_hp_tag[i], module, offset}];
  }
  if (FILE* f = std::fopen(out.c_str(), "w"))
  {
    for (const auto& [key, count] : s_hp_counts)
      std::fprintf(f, "%d %s %llx %llu\n", std::get<0>(key), std::get<1>(key).c_str(),
                   static_cast<unsigned long long>(std::get<2>(key)),
                   static_cast<unsigned long long>(count));
    std::fclose(f);
  }
}
}  // namespace
#else
namespace
{
volatile sig_atomic_t s_hp_phase = 0;
void HostProfStart()
{
}
void HostProfDump()
{
}
}  // namespace
#endif

void RollbackFrameHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();
  ppc_state.npc = LR(ppc_state);
  Rollback& r = s_rb;
  r.frame_mode = true;
  r.frame_tick_done = false;
  r.in_game_seen = false;
  std::copy(std::begin(s_phase_ns), std::end(s_phase_ns), s_cost.ph_step);
  s_cost.digest_step = s_digest.total_ns;
  if (r.vtime_on)
  {
    HostProfStart();
    if (!r.in_resim && r.live % 600 == 0)
      HostProfDump();
  }
  s_hp_phase = 3;
  const auto step_start = std::chrono::steady_clock::now();
  ppc_state.gpr[3] = RbStep(guard, RB_PAD_BUF);
  s_hp_phase = r.in_resim ? 2 : 1;
  AudioClockFrameLog(guard);
  ResimSpeed(guard, r.in_unit && r.in_resim);
  CostFrameStart(guard, step_start);
  TelemetryUnitStart(step_start);  // NSR telemetry
}

void RollbackFrameEndHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();
  ppc_state.npc = LR(ppc_state);
  Rollback& r = s_rb;
  if (FrameDrivenLifecycleActive() && !r.config.async_present)
    PowerPC::MMU::HostWrite<u32>(guard, PowerPC::MMU::HostRead<u32>(guard, VI_POST_COUNTER) + 1,
                                 VI_POST_COUNTER);
  CostFrameEnd(guard);
  TelemetryUnitEnd(guard);  // NSR telemetry
  s_hp_phase = 0;
  ResimSpeed(guard, false);
  r.in_unit = false;
  if (!r.vtime_on)
  {
    if (r.auto_handoff)
    {
      // InGame is observed during this completed lockstep frame.  Enter on
      // the next frame boundary, whose absolute number is therefore derived
      // from the confirmed lockstep timeline on every peer.
      if (r.in_game_seen)
        r.enter_pending = true;
      return;
    }
    if (r.session_ended && r.config.auto_reenter)
    {
      if (!r.in_game_seen)
        r.reenter_ready = true;
      else if (r.reenter_ready)
        r.enter_pending = true;
    }
    return;
  }
  if (r.in_resim)
    return;
  if (r.config.session_end && !r.session_end_consumed &&
      r.tick_running >= r.config.session_end)
  {
    r.session_end_consumed = true;
    LeaveRollbackSession(guard, "NSR_RB_SESSION_END");
  }
  else if (r.session_saw_in_game && !r.in_game_seen)
  {
    // The next absolute frame is the lockstep fallback boundary.  Confirm
    // the just-completed transition input first, then prove equal semantic
    // state before disabling rollback and resetting the poll stream.
    u32 host_clock = 0;
    u32 host_aux_clock = 0;
    if (!r.auto_handoff ||
        (ConfirmRollbackTail(r.tick_running) &&
         HandoffBarrier(guard, RollbackNet::HandoffKind::LeaveRollback, r.live, &host_clock,
                        &host_aux_clock)))
    {
      if (r.auto_handoff)
        NormalizeTransitionClocks(guard, host_clock, host_aux_clock);
      LeaveRollbackSession(guard, "InGame group inactive");
    }
  }
}

// The active InGame group calls this once per main-loop iteration.  Losing
// that call after a session has observed it is the match/results boundary;
// seeing it again after at least one inactive frame can arm an opt-in test
// re-entry at the following frame boundary.
void InGameHook(const Core::CPUThreadGuard& guard)
{
  (void)guard;
  Rollback& r = s_rb;
  r.in_game_seen = true;
  if (r.vtime_on)
    r.session_saw_in_game = true;
}

// Frame mode, during the session: exactly one tick per unpaused frame.
// Replaces the real-time accumulator drain fn_802378C8 (which returns the
// ticks due and adds them to the game clock outer+4): the first call in a
// frame returns the fixed step and advances the clock; later calls return 0.
// A frame with the pause menu open (PAUSE_DEPTH != 0) gets no tick, as in
// stock. Frames, not wall-clock time, then drive the game, as on every
// machine (Slippi's model).
void TickCountHook(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  auto& ppc = guard.GetSystem().GetPPCState();
  if (!r.frame_mode || !FrameDrivenLifecycleActive())
    return;  // run the original
  using PowerPC::MMU;
  const u32 outer = MMU::HostRead<u32>(guard, TICK_CLOCK_PTR);
  if ((outer >> 24) != 0x80)
    return;
  const u32 inner = MMU::HostRead<u32>(guard, outer + 0xC);
  u32 step = (inner >> 24) == 0x80 ? MMU::HostRead<u32>(guard, inner + 4) : 1;
  if (step == 0)
    step = 1;
  u32 n = 0;
  if (!r.frame_tick_done && RollbackLogic::PausedFrame(MMU::HostRead<u32>(guard, PAUSE_DEPTH)))
  {
    // Paused (fix/pausetime): stock skipped the accumulator feed this frame,
    // so its drain finds nothing due and returns 0 (fn_802379DC also stores
    // the 0 at inner+8). Handing out the frame's tick anyway ran the sim
    // under the pause menu: the play clock kept counting while its display
    // was frozen, so after resume the CPU snapped on a clock that still
    // showed time. One drain call per frame either way: the loop stops on 0.
    // Returning 0 also keeps the pause menu stock (fix/modal): with no tick,
    // InGame runs its no-tick input flush (fn_801C601C/fn_801C60BC(-1)), as
    // the stock pause menu and its Restart/Quit confirmation expect. Verified
    // stock-equivalent single-machine and in 2-peer netplay (NFLStreet
    // tests/scripts/*pause-restart*, *pause-quit*). Handing a paused frame a
    // tick body instead (an experiment on fix/reentry) left the confirmation
    // dialog stuck on Cancel in netplay.
    r.frame_tick_done = true;
    if ((inner >> 24) == 0x80)
      MMU::HostWrite<u32>(guard, 0, inner + 8);
    MusicHoldPausedFrame(guard);
  }
  else if (!r.frame_tick_done)
  {
    r.frame_tick_done = true;
    n = step;
    if ((inner >> 24) == 0x80)
    {
      // Keep the live accumulator drained so the game doesn't catch up later.
      MMU::HostWrite<u32>(guard, MMU::HostRead<u32>(guard, inner + 0xC) % step, inner + 0xC);
      MMU::HostWrite<u32>(guard, step, inner + 8);
    }
    MMU::HostWrite<u32>(guard, MMU::HostRead<u32>(guard, outer + 4) + n, outer + 4);
  }
  ppc.gpr[3] = n;
  ppc.npc = LR(ppc);
}

namespace
{
u32 s_hook_pc = 0;
}

// Frame mode: re-simulated frames skip the world draw (RENDER_FN; the render
// pass costs ~3x the sim and is mostly this; Slippi skips rendering too)
// and don't wait for the next retrace (VIWaitForRetrace returns at once). Everything else in the frame runs,
// including the present's bookkeeping (framebuffer flip): skipping
// the whole present (fn_801CE82C) left that game state out of step and broke
// matches. Extra flips within one retrace are invisible: VI shows the last.
void ViCallbackHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  const u32 lr = LR(ppc);
  const bool from_frame_routine = lr >= RB_FRAME_CODE && lr < RB_FRAME_CODE + 0x80;
  if (lr == 0x80416AF4 && FrameDrivenLifecycleActive())
    // SIM_WAIT_TRAMP: a retrace wait inside deterministic game/menu work.
    return;
  if (from_frame_routine != FrameDrivenLifecycleActive())
  {
    ppc.npc = lr;  // skip this call
    return;
  }
  // Session, frame routine: deliver due music events first (audio clock,
  // MusicInjectEvent); the callback itself runs when none is left.
  if (from_frame_routine)
    MusicInjectEvent(guard);
}

void PeekZHook(const Core::CPUThreadGuard& guard)
{
  if (!FrameDrivenLifecycleActive())
    return;  // outside the session: run the real peek
  auto& ppc = guard.GetSystem().GetPPCState();
  ppc.gpr[3] = s_rb.config.peek_z;
  ppc.npc = LR(ppc);
}

// Retrace waits inside the sim (session). The frame-driven clocks (the VI
// pre-retrace callback's retrace-driven clock 0x8039B430, the post-retrace
// counter, the audio clock model) advance once per frame, from the frame
// routine. Game code that waits for one of them to pass a deadline inside a
// frame, e.g. the EA Trax fade-out at the end of a game (fn_80072C90 waits
// in fn_800731D0 -> VIWaitForRetrace until fn_800728F8 sees the retrace clock
// pass the fade end), would wait forever (Demo Mode soak: every run hung at
// frame 101554, the final whistle). So each such wait is one virtual
// retrace: the VI pre-retrace callback runs once (from here, through
// SIM_WAIT_TRAMP), the post-retrace counter and the audio clock advance by
// one, then the real wait runs (live) or is skipped (re-simulation). The
// number of waits is decided by game state, so every machine and every
// re-simulation counts the same. The per-frame present wait
// (PRESENT_BREAKPT_RETURN) is not a sim wait.
namespace
{
constexpr u32 SIM_WAIT_TRAMP = 0x80416AF4;  // `blr` written by Dolphin (free patch data)
u32 s_sim_wait_lr = 0;
bool s_sim_wait_pass = false;
u64 s_sim_waits = 0;

void SimWaitBegin(const Core::CPUThreadGuard& guard)
{
  using PowerPC::MMU;
  auto& ppc = guard.GetSystem().GetPPCState();
  MMU::HostWrite<u32>(guard, MMU::HostRead<u32>(guard, VI_POST_COUNTER) + 1, VI_POST_COUNTER);
  // The presenter's teardown wait (fn_801CE82C, call at 0x801CE7DC) is
  // host/GX pacing. It needs a virtual VI callback to make progress, but it
  // is not elapsed game-audio time: the number of waits depends on each
  // peer's rollback-history cycle phase. Advancing AcNow here made otherwise
  // identical peers expire model deadlines on different transition frames.
  const u32 caller = LR(ppc) - 4;
  if (caller < 0x801CE000 || caller >= 0x801CE900)
    AudioClockSimWait(guard);
  if (MMU::HostRead<u32>(guard, SIM_WAIT_TRAMP) != 0x4E800020)
    MMU::HostWrite<u32>(guard, 0x4E800020, SIM_WAIT_TRAMP);
  if (++s_sim_waits % 600 == 1)
    Log("sim retrace wait #{} in frame {} from {:08x}", s_sim_waits, s_rb.tick_running, LR(ppc) - 4);
  s_sim_wait_lr = LR(ppc);
  LR(ppc) = SIM_WAIT_TRAMP;
  ppc.npc = VI_PRE_CB;  // ViCallbackHook lets it run (LR == SIM_WAIT_TRAMP)
}
}  // namespace

// HookType::Replace at SIM_WAIT_TRAMP: after the callback, the wait itself.
void SimWaitTrampHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  LR(ppc) = s_sim_wait_lr;
  if (s_rb.in_resim)
  {
    ppc.npc = s_sim_wait_lr;  // re-simulation: no real wait
    return;
  }
  s_sim_wait_pass = true;  // PresentHook lets this one call through
  ppc.npc = VI_WAIT_FOR_RETRACE;
}

void PresentHook(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  auto& ppc = guard.GetSystem().GetPPCState();
  const bool async_present = r.config.async_present;  // old behavior
  if (s_hook_pc == VI_WAIT_FOR_RETRACE && FrameDrivenLifecycleActive() && r.frame_mode && r.in_unit &&
      !async_present &&
      LR(ppc) != PRESENT_BREAKPT_RETURN &&
      PowerPC::MMU::HostRead<u32>(guard, 0x800000E4) == 0x803A3F30)  // main thread
  {
    if (s_sim_wait_pass && LR(ppc) == s_sim_wait_lr)
    {
      s_sim_wait_pass = false;
      return;  // the real wait (live frame)
    }
    SimWaitBegin(guard);
    return;
  }
  if (s_hook_pc == VI_POST_COUNTER_FN)
  {
    if (!async_present && FrameDrivenLifecycleActive())
      ppc.npc = LR(ppc);  // counted per frame in RollbackFrameEndHook
    return;
  }
  if (s_hook_pc == GX_ENABLE_BREAKPT)
  {
    if (!async_present && r.frame_mode && r.in_unit && FrameDrivenLifecycleActive() &&
        LR(ppc) == PRESENT_BREAKPT_RETURN)
      ppc.npc = r.in_resim ? LR(ppc) : VI_WAIT_FOR_RETRACE;
    return;
  }
  // Diagnostics: NSR_RB_RESIM_VSYNC=1 keeps the vsync wait in re-simulated
  // frames (each then takes a live frame's emulated time).
  if (r.frame_mode && r.in_unit && r.in_resim &&
      !(r.config.keep_resim_vsync && s_hook_pc == VI_WAIT_FOR_RETRACE))
    ppc.npc = LR(ppc);
}

void RollbackTickEndHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  ppc_state.npc = LR(ppc_state);
  Rollback& r = s_rb;
  if (!r.frame_mode)
    r.in_unit = false;
  if (r.watch && r.live >= r.watch_from && r.live <= r.watch_to)
    Log("watch tick-end   live {} cursor {}: [{:08x}] = {:08x}", r.live, r.cursor, r.watch,
        PowerPC::MMU::HostRead<u32>(guard, r.watch));
  if (r.profile_path.empty() || r.cursor != r.live || r.live <= r.force_from)
    return;
  const u8* ram = system.GetMemory().GetRAM();
  RbCountWrites(ram, r.at_start, r.inside);
  RbSave(ram, r.at_end);
  r.have_end = true;
}

namespace
{
// Virtual time for a time read from game code inside a tick, or nothing.
std::optional<u64> VirtualTime(const Core::CPUThreadGuard& guard, const PowerPC::PowerPCState& ppc)
{
  Rollback& r = s_rb;
  if (!FrameDrivenLifecycleActive() || !r.in_unit || LR(ppc) >= 0x80230000)
    // SDK callers remain on real time; only game decisions are frame-driven.
    return std::nullopt;
  // Spin-waits on ARQ (audio-shared DMA queue; fn_801C1D98..fn_801C1F54) read
  // the time once per iteration, a timing-dependent count: real time, and
  // they don't consume a virtual call slot (rb-coverage-review L1).
  if (LR(ppc) >= 0x801C1D98 && LR(ppc) < 0x801C1F54)
    return std::nullopt;
  // Only the main thread runs the sim (rb-coverage-review L1c).
  if (PowerPC::MMU::HostRead<u32>(guard, 0x800000E4) != 0x803A3F30)
    return std::nullopt;
  constexpr u64 TB_PER_TICK = 40500000 / 60;  // GameCube time base, one 60 Hz frame
  return r.vtime_base + u64(r.tick_running - r.vtime_from) * TB_PER_TICK + 64 * u64(r.vtime_calls++);
}
}  // namespace

void OSGetTimeHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  const u64 t = VirtualTime(guard, ppc).value_or(system.GetSystemTimers().GetFakeTimeBase());
  ppc.gpr[3] = static_cast<u32>(t >> 32);
  ppc.gpr[4] = static_cast<u32>(t);
  ppc.npc = LR(ppc);
}

void OSGetTickHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  ppc.gpr[3] = static_cast<u32>(VirtualTime(guard, ppc).value_or(system.GetSystemTimers().GetFakeTimeBase()));
  ppc.npc = LR(ppc);
}

void RollbackTimeHook(const Core::CPUThreadGuard& guard)
{
  OSGetTickHook(guard);  // the patched `mftb r3` wants TBL in r3
}


void SetHookPc(unsigned int pc)
{
  s_hook_pc = pc;
}

std::vector<unsigned int> AudioGateList()
{
  return ConfigForRollback().audio_gate_only;
}

void AudioGateHook(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  auto& ppc = guard.GetSystem().GetPPCState();
  // Only calls from game code on the main thread, inside a tick, during the
  // rollback session. Audio's own thread and interrupt callbacks run normally.
  if (!r.vtime_on || !r.in_unit || LR(ppc) >= 0x8025E000 ||
      PowerPC::MMU::HostRead<u32>(guard, 0x800000E4) != 0x803A3F30)
    return;  // npc unchanged: run the function
  const u32 entry = s_hook_pc;
  // The sound-start path (0x8026AAF0, called from fn_801F4050) is gated by
  // default. Reissuing it during re-simulation made the audio heap diverge
  // and eventually corrupted both this middleware and frontend transitions.
  // NSR_RB_AUDIO_GATE=0 disables it for diagnostics; =addr,... gates exactly
  // those entries. Gating every entry crashes (some return pointers or fill
  // out-parameters in game memory that a skipped call leaves unwritten).
  const auto& only = r.config.audio_gate_only;
  const auto& effects = r.config.effect_gates;
  if (!only.empty() && std::find(only.begin(), only.end(), entry) == only.end() &&
      std::find(effects.begin(), effects.end(), entry) == effects.end())
    return;
  if (!r.in_resim)
  {
    auto& log = r.gate_log[r.tick_running];
    r.gate_pending.push_back({LR(ppc), r.tick_running, log.size()});
    log.push_back({entry});
    LR(ppc) = RB_GATE_RETURN;  // record the result when it returns
    ++r.gate_recorded;
    return;
  }
  const auto it = r.gate_log.find(r.tick_running);
  const size_t k = r.gate_index++;
  if (it != r.gate_log.end() && k < it->second.size() && it->second[k].entry == entry &&
      it->second[k].done)
  {
    const auto& c = it->second[k];
    ppc.gpr[3] = c.r3;
    ppc.gpr[4] = c.r4;
    ppc.ps[1].SetPS0(c.f1);
    ppc.npc = LR(ppc);  // skip the call
    ++r.gate_replayed;
    return;
  }
  ++r.gate_mismatched;  // different calls than the live tick made: run it
}

void AudioGateReturnHook(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  auto& ppc = guard.GetSystem().GetPPCState();
  if (r.gate_pending.empty())
  {
    ppc.npc = LR(ppc);  // shouldn't happen
    return;
  }
  const auto p = r.gate_pending.back();
  r.gate_pending.pop_back();
  if (const auto it = r.gate_log.find(p.tick); it != r.gate_log.end() && p.index < it->second.size())
  {
    auto& c = it->second[p.index];
    c.r3 = ppc.gpr[3];
    c.r4 = ppc.gpr[4];
    c.f1 = ppc.ps[1].PS0AsU64();
    c.done = true;
  }
  ppc.npc = p.lr;
  LR(ppc) = p.lr;
}

// ---------------------------------------------------------------------------
// Audio clock (see NSRNetplay.h, "Audio clock").
// ---------------------------------------------------------------------------
namespace
{
struct AcPending
{
  u32 lr, entry, site, r3, r4, r5;
  u32 frame;
  bool resim;
};
std::vector<AcPending> s_ac_pending;

FILE* AcLog()
{
  return s_rb.ac_log;
}

// The return stub is a `blr` in free patch data (zero in the image); its
// HLE hook does the work, the word only has to decode.
void AcEnsureReturnStub(const Core::CPUThreadGuard& guard)
{
  if (PowerPC::MMU::HostRead<u32>(guard, AC_RETURN_STUB) != 0x4E800020)
    PowerPC::MMU::HostWrite<u32>(guard, 0x4E800020, AC_RETURN_STUB);
}

bool AcGameCaller(const Core::CPUThreadGuard& guard, const PowerPC::PowerPCState& ppc)
{
  const Rollback& r = s_rb;
  return FrameDrivenLifecycleActive() && r.in_unit && LR(ppc) < 0x8025E000 &&
         PowerPC::MMU::HostRead<u32>(guard, 0x800000E4) == 0x803A3F30;
}
}  // namespace

void AudioCallLogHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  if (!AcLog() || !AcGameCaller(guard, ppc))
    return;
  // The wrapper's own return address: saved in its caller's frame (EABI).
  const u32 back = PowerPC::MMU::HostRead<u32>(guard, ppc.gpr[1]);
  const u32 site = (back >> 24) == 0x80 ? PowerPC::MMU::HostRead<u32>(guard, back + 4) : 0;
  s_ac_pending.push_back({LR(ppc), s_hook_pc, site, ppc.gpr[3], ppc.gpr[4], ppc.gpr[5],
                          s_rb.tick_running, s_rb.in_resim});
  AcEnsureReturnStub(guard);
  LR(ppc) = AC_RETURN_STUB;
}

void AudioCallReturnHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  if (s_ac_pending.empty())
  {
    ppc.npc = LR(ppc);
    return;
  }
  const AcPending p = s_ac_pending.back();
  s_ac_pending.pop_back();
  if (FILE* f = AcLog())
  {
    std::string out;
    u32 words = 0;
    if (p.entry == 0x8026F1E8)
      words = 3;
    else if (p.entry == 0x8026EF88)
      words = 4;
    for (u32 i = 0; i < words; ++i)
      out += fmt::format(" {:08x}", PowerPC::MMU::HostRead<u32>(guard, p.r4 + 4 * i));
    std::fprintf(f, "C %u %d %08x %08x %08x %08x %08x %08x -> %08x%s\n", p.frame, p.resim ? 1 : 0,
                 p.entry, p.lr - 4, p.site - 4, p.r3, p.r4, p.r5, ppc.gpr[3], out.c_str());
  }
  ppc.npc = p.lr;
  LR(ppc) = p.lr;
}

void AudioClockFrameLog(const Core::CPUThreadGuard& guard)
{
  FILE* f = AcLog();
  if (!f || !FrameDrivenLifecycleActive())
    return;
  // NSR_AC_HASH=lo-hi,...: also a hash of each range (diagnostics: find the
  // first frame where some state differs between two runs).
  std::string hashes;
  for (const auto& [lo, hi] : s_rb.config.ac_hash_ranges)
  {
    u32 h = 2166136261u;
    for (u32 a = lo; a < hi; ++a)
      h = (h ^ PowerPC::MMU::HostRead<u8>(guard, a)) * 16777619u;
    hashes += fmt::format(" {:08x}", h);
  }
  std::fprintf(f, "F %u %d %08x %08x%s\n", s_rb.tick_running, s_rb.in_resim ? 1 : 0,
               PowerPC::MMU::HostRead<u32>(guard, 0x80481888),
               PowerPC::MMU::HostRead<u32>(guard, 0x80481904), hashes.c_str());
}

std::vector<unsigned int> AudioTraceList()
{
  return ConfigForRollback().audio_trace;
}

// ---------------------------------------------------------------------------
// Audio clock model: what the sim sees of audio, as a function of frames.
//
// Why: the audio thread (OSThread 0x803AA828) and the DSP advance on emulated
// time, which differs between peers and between a live frame and its
// re-simulation. Game code polls audio progress (speech queue drained? sound
// handle finished? bank uploaded?) and branches on it; the sound-start rate
// limiter fn_8006C8A4 even draws RNG stream 1 only while a handle still plays.
// Rolling audio memory back with the game cannot fix that: the answers depend
// on *when* the audio thread ran relative to game code.
//
// What: during the rollback session, every audio *query* the game makes
// through its sound-layer wrappers is answered from this model instead of the
// middleware, and every handle/id the game gets back is a virtual one the
// model hands out. Real calls still run for every *effect* (start, stop,
// queue, volume), with virtual ids translated back to this machine's real
// ones, so audio output keeps working; it may drift from what the sim
// believes (e.g. a sound the model thinks ended still plays): an audible
// difference, never a logic one.
//
// The model state lives in MEM1 free patch data (AC_BASE, inside the Reborn
// DOL's zero-filled data10 section, unreferenced), so rollback restores it
// with the game and every peer computes the same bytes from the same frames.
// The only per-machine state is the host-side map from virtual to real ids,
// which a re-simulated start simply overwrites.
//
// Durations are deterministic functions of game data:
//   - sound effects: the patch's sample count and rate (EA 'PT' tags 0x85 /
//     0x84, default 22050 Hz) from the BNKb bank header in RAM; a patch with
//     a loop start (tag 0x86) never ends by itself;
//   - speech items: a fixed NSR_AC_SPEECH_FRAMES (default 75; see
//     AcSpeechFrames for why and what is deferred);
//   - bank uploads: ready NSR_AC_BANK_FRAMES frames after the game first polls.
//
// Env: NSR_AC=0 disables (real answers, nondeterministic); NSR_AC=shadow
// computes and logs model answers (NSR_AC_LOG) but returns the real ones
// (calibration). Deferred: exact speech durations from the stream headers on
// disc (revisit if commentary pacing feels off), pitch-dependent SFX length.
// ---------------------------------------------------------------------------
namespace
{
constexpr u32 AC_BASE = 0x80416B00;
constexpr u32 AC_END = 0x80417000;
constexpr u32 AC_MAGIC = 0x41434C4B;  // 'ACLK'
// +0x00 magic, +0x04 next sound seq, +0x08 next speech seq
constexpr u32 AC_STREAMS = 8, AC_ITEMS = 6, AC_STREAM_SIZE = 0x40;
// Per stream: +0x00 head start frame, +0x04 queued count, +0x08 volume,
// +0x10 items[AC_ITEMS] {virtual id, frames}
constexpr u32 AC_STREAM = AC_BASE + 0x10;
constexpr u32 AC_BANK = AC_STREAM + AC_STREAMS * AC_STREAM_SIZE;  // {record, first poll frame}
constexpr u32 AC_BANK_SLOTS = 8;
constexpr u32 AC_SFX = AC_BANK + AC_BANK_SLOTS * 8;  // {virtual handle, start, end}
constexpr u32 AC_SFX_SLOTS = 48;
static_assert(AC_SFX + AC_SFX_SLOTS * 12 <= AC_END);
constexpr u32 AC_LOOP = 0xFFFFFFFF;  // end frame of a sound that never ends by itself
constexpr u32 AC_VIRTUAL_SFX = 0x40000000;
constexpr u32 SND_FLAGS = 0x8039AEC4;  // the sound layer's "initialised/enabled" bytes (+1, +2)

// Game sound-layer wrappers (GN7E69) and the call sites whose results are made virtual.
constexpr u32 W_SPEECH_STATUS = 0x801F4834;  // (stream*, out[3]: count, head id, ms)
constexpr u32 W_EVENT_STATUS = 0x801F4934;   // (item id, out[4]) -> r3
constexpr u32 W_SFX_FINISHED = 0x801F41A0;   // (handle) -> 1 finished/invalid, 0 playing
constexpr u32 W_STREAM_VOLUME = 0x801F48B8;  // (stream*) -> volume
constexpr u32 C_BANK_READY = 0x801F360C;     // `bl 0x8026A96C` in the bank service fn_801F35B4
constexpr u32 W_SFX_START = 0x801F4050;      // (bank*, sound, opts) -> handle
constexpr u32 R_SFX_START = 0x8006C988;      // after its only call (rate limiter)
constexpr u32 W_SPEECH_QUEUE = 0x801F4580;   // (stream*, desc {archive, index}, ...) -> id
constexpr std::array<u32, 2> R_SPEECH_QUEUE{0x80073D6C, 0x801861A8};
constexpr u32 W_SPEECH_SKIP = 0x801F4AB0;    // (item id, ...)
constexpr u32 W_STREAM_STOP = 0x801F4638;    // (stream*)
constexpr u32 W_STREAM_DESTROY = 0x801F45D4;  // (stream*)
constexpr u32 W_STREAM_SETVOL = 0x801F4770;  // (stream*, volume)
constexpr u32 W_STREAM_FADE = 0x801F4678;    // (stream*, time, volume)
constexpr u32 W_SFX_FADE = 0x801F4294;       // (handle, time, volume)
constexpr u32 W_SFX_STOP = 0x801F421C;       // (handle, ...)
constexpr u32 W_SFX_PARAM = 0x801F4314;      // (handle, value)
constexpr u32 W_SFX_STOP2 = 0x801F4130;      // (handle)
// After calls of the handle wrappers whose result reaches game code.
constexpr std::array<u32, 4> R_SFX_HANDLE{0x8006CA70, 0x8006CA08, 0x8006E2E4, 0x8006E9E0};

AcMode AcModeNow()
{
  return ConfigForRollback().ac_mode;
}

struct AcMem
{
  const Core::CPUThreadGuard& guard;
  u32 rd(u32 a) const { return PowerPC::MMU::HostRead<u32>(guard, a); }
  u16 rd16(u32 a) const { return PowerPC::MMU::HostRead<u16>(guard, a); }
  u8 rd8(u32 a) const { return PowerPC::MMU::HostRead<u8>(guard, a); }
  void wr(u32 a, u32 v) const { PowerPC::MMU::HostWrite<u32>(guard, v, a); }
};

bool AcPtr(u32 a)
{
  return a >= 0x80003100 && a < 0x81800000;
}

std::map<u32, u32> s_ac_real;  // virtual id/handle -> this machine's real one
struct AcStash
{
  bool valid = false;
  u32 frame = 0, a = 0, b = 0, c = 0;
};
AcStash s_ac_sfx_start, s_ac_queue, s_ac_handle;

void AcLogLine(const std::string& line)
{
  if (FILE* f = AcLog())
    std::fprintf(f, "%s\n", line.c_str());
}

// Model active for this hook: session on, inside a frame, main thread, game caller.
bool AcActive(const Core::CPUThreadGuard& guard, const PowerPC::PowerPCState& ppc)
{
  if (AcModeNow() == AcMode::Off || !AcGameCaller(guard, ppc))
    return false;
  return PowerPC::MMU::HostRead<u32>(guard, AC_BASE) == AC_MAGIC;
}

bool AcSoundLayerOn(const AcMem& m)
{
  return (m.rd(SND_FLAGS) & 0x00FFFF00) == 0x00010100;
}

// The model's clock: the frame number plus the retrace waits game code made
// inside frames during the session (AC_EXTRA_TICKS, counted by
// SimWaitBegin), so a sim loop that waits retraces for audio progress sees
// the model advance, the same way on every machine.
constexpr u32 AC_EXTRA_TICKS = AC_BASE + 0xC;
u32 AcNow(const AcMem& m)
{
  return RollbackLogic::AudioNow(s_rb.tick_running, m.rd(AC_EXTRA_TICKS), 0);
}

u32 AcFramesToMs(u32 frames)
{
  return RollbackLogic::FramesToMs(frames);
}

// --- speech streams --------------------------------------------------------
u32 AcStreamBase(u32 s)
{
  return AC_STREAM + s * AC_STREAM_SIZE;
}

// Pops the items that have finished by frame f (items play back to back).
void AcSpeechAdvance(const AcMem& m, u32 s, u32 f)
{
  const u32 b = AcStreamBase(s);
  u32 start = m.rd(b), count = std::min(m.rd(b + 4), AC_ITEMS);
  bool changed = false;
  while (count > 0)
  {
    const u32 dur = m.rd(b + 0x14);
    if (static_cast<s32>(f - start) < static_cast<s32>(dur))
      break;
    start += dur;
    for (u32 i = 0; i + 1 < count; ++i)
    {
      m.wr(b + 0x10 + 8 * i, m.rd(b + 0x18 + 8 * i));
      m.wr(b + 0x14 + 8 * i, m.rd(b + 0x1C + 8 * i));
    }
    --count;
    m.wr(b + 0x10 + 8 * count, 0);
    m.wr(b + 0x14 + 8 * count, 0);
    changed = true;
  }
  if (changed)
  {
    m.wr(b, start);
    m.wr(b + 4, count);
  }
}

// Frames a speech item plays. Deferred: the real length is in the clip's
// EA stream header (SCHl, 'PT' tag 0x85 samples / 0x84 rate) on disc, which
// the host could read once the stream source has resolved the clip's disc
// position (fn_801F10B0); the DIR1 entry sizes do not predict it (one entry
// holds several clips: same entry, 46..86 frames in a reference run). Until
// then a fixed length near the median of a reference match (75 frames;
// lines measured 25..491). Revisit if commentary pacing feels off.
u32 AcSpeechFrames()
{
  return s_rb.config.ac_speech_frames;
}

void AcSpeechQueue(const AcMem& m, u32 s, u32 vid, u32 frames, u32 f)
{
  const u32 lead = s_rb.config.ac_speech_lead;
  AcSpeechAdvance(m, s, f);
  const u32 b = AcStreamBase(s);
  const u32 count = m.rd(b + 4);
  if (count >= AC_ITEMS)
    return;  // the game never queues this deep; drop (deterministically)
  if (count == 0)
    m.wr(b, f + lead);
  m.wr(b + 0x10 + 8 * count, vid);
  m.wr(b + 0x14 + 8 * count, frames);
  m.wr(b + 4, count + 1);
}

void AcSpeechRemove(const AcMem& m, u32 vid, u32 f)
{
  const u32 s = vid & 0xFF;
  if (s >= AC_STREAMS)
    return;
  AcSpeechAdvance(m, s, f);
  const u32 b = AcStreamBase(s);
  const u32 count = m.rd(b + 4);
  for (u32 i = 0; i < count; ++i)
  {
    if (m.rd(b + 0x10 + 8 * i) != vid)
      continue;
    if (i == 0)
      m.wr(b, f);  // the next item starts now
    for (u32 j = i; j + 1 < count; ++j)
    {
      m.wr(b + 0x10 + 8 * j, m.rd(b + 0x18 + 8 * j));
      m.wr(b + 0x14 + 8 * j, m.rd(b + 0x1C + 8 * j));
    }
    m.wr(b + 0x10 + 8 * (count - 1), 0);
    m.wr(b + 0x14 + 8 * (count - 1), 0);
    m.wr(b + 4, count - 1);
    return;
  }
}

void AcStreamClear(const AcMem& m, u32 s)
{
  if (s >= AC_STREAMS)
    return;
  const u32 b = AcStreamBase(s);
  for (u32 a = b; a < b + AC_STREAM_SIZE; a += 4)
  {
    if (a != b + 8)  // keep the volume
      m.wr(a, 0);
  }
}

// --- sound effects ---------------------------------------------------------
// Frames a sound plays: longest sample of the patch (EA 'PT' tags), or
// AC_LOOP if it loops. Bank table: *(0x803A6DF4) + 0xC * bank, header 'BNKb',
// patch offsets at +0x14 relative to their own slot (fn_8026A6A8).
u32 AcSfxFrames(const AcMem& m, u32 bank_struct, u32 sound)
{
  const u32 fallback = s_rb.config.ac_sfx_default;
  const u32 tail = s_rb.config.ac_sfx_tail;
  if (!AcPtr(bank_struct))
    return fallback;
  const u32 bank = m.rd(bank_struct + 0x14);
  const u32 table = m.rd(0x803A6DF4);
  if (bank >= m.rd16(0x803A6C48) || !AcPtr(table))
    return fallback;
  const u32 hdr = m.rd(table + 0xC * bank);
  if (!AcPtr(hdr) || m.rd(hdr) != 0x424E4B62 || sound >= m.rd16(hdr + 6))
    return fallback;
  const u32 slot = hdr + 0x14 + 4 * sound;
  const u32 off = m.rd(slot);
  if (off == 0 || !AcPtr(slot + off))
    return fallback;
  const u32 p = slot + off;
  if (m.rd16(p) != 0x5054)  // 'PT'
    return fallback;
  u32 best = 0, samples = 0, rate = 22050;
  bool loop = false, any = false;
  auto close = [&] {
    if (samples)
      best = std::max<u32>(best, static_cast<u32>((u64(samples) * 60 + rate - 1) / rate));
    samples = 0;
    rate = 22050;
  };
  for (u32 i = p + 4, n = 0; i < p + 0x400 && n < 256; ++n)
  {
    const u8 t = m.rd8(i++);
    if (t == 0xFF)
      break;
    if (t == 0xFD)
    {
      close();
      any = true;
      continue;
    }
    if (t == 0xFC || t == 0xFE)
      continue;
    const u8 len = m.rd8(i++);
    u32 v = 0;
    for (u32 k = 0; k < len && k < 4; ++k)
      v = (v << 8) | m.rd8(i + k);
    i += len;
    if (t == 0x85)
      samples = v;
    else if (t == 0x84 && v)
      rate = v;
    else if (t == 0x86)
      loop = true;
  }
  close();
  if (loop)
    return AC_LOOP;
  return any && best ? best + tail : fallback;
}

// Slot index of a live virtual handle, or -1.
s32 AcSfxFind(const AcMem& m, u32 vh)
{
  if ((vh & 0xFF000000) != AC_VIRTUAL_SFX)
    return -1;
  for (u32 i = 0; i < AC_SFX_SLOTS; ++i)
  {
    if (m.rd(AC_SFX + 12 * i) == vh)
      return static_cast<s32>(i);
  }
  return -1;
}

bool AcSfxPlaying(const AcMem& m, s32 slot, u32 f)
{
  if (slot < 0)
    return false;
  const u32 end = m.rd(AC_SFX + 12 * slot + 8);
  return end == AC_LOOP || static_cast<s32>(f - end) < 0;
}

u32 AcSfxStart(const AcMem& m, u32 frames, u32 f)
{
  const u32 seq = m.rd(AC_BASE + 4);
  m.wr(AC_BASE + 4, seq + 1);
  const u32 vh = AC_VIRTUAL_SFX | (seq & 0x00FFFFFF);
  // A free or finished slot, else the oldest start.
  u32 pick = 0, oldest = 0xFFFFFFFF;
  for (u32 i = 0; i < AC_SFX_SLOTS; ++i)
  {
    const u32 a = AC_SFX + 12 * i;
    if (m.rd(a) == 0 || !AcSfxPlaying(m, static_cast<s32>(i), f))
    {
      pick = i;
      break;
    }
    if (m.rd(a + 4) < oldest)
    {
      oldest = m.rd(a + 4);
      pick = i;
    }
  }
  const u32 a = AC_SFX + 12 * pick;
  m.wr(a, vh);
  m.wr(a + 4, f);
  m.wr(a + 8, frames == AC_LOOP ? AC_LOOP : f + frames);
  return vh;
}

u32 AcReal(u32 vid)
{
  const auto it = s_ac_real.find(vid);
  return it == s_ac_real.end() ? 0xFFFFFFFF : it->second;
}

// --- bank uploads ----------------------------------------------------------
bool AcBankReady(const AcMem& m, u32 record, u32 f)
{
  const u32 wait = s_rb.config.ac_bank_frames;
  s32 free_slot = -1, oldest_slot = 0;
  u32 oldest = 0xFFFFFFFF;
  for (u32 i = 0; i < AC_BANK_SLOTS; ++i)
  {
    const u32 a = AC_BANK + 8 * i;
    if (m.rd(a) == record)
    {
      if (f - m.rd(a + 4) < wait)
        return false;
      m.wr(a, 0);
      m.wr(a + 4, 0);
      return true;
    }
    if (m.rd(a) == 0 && free_slot < 0)
      free_slot = static_cast<s32>(i);
    if (m.rd(a) != 0 && m.rd(a + 4) < oldest)
    {
      oldest = m.rd(a + 4);
      oldest_slot = static_cast<s32>(i);
    }
  }
  const u32 a = AC_BANK + 8 * static_cast<u32>(free_slot >= 0 ? free_slot : oldest_slot);
  m.wr(a, record);
  m.wr(a + 4, f);
  return wait == 0;
}

// --- bank uploads: hold the commit until this machine's upload is done -----
// (fix/k7loop) The model's "ready" is only the sim-visible half of a bank
// load. On ready, the bank service commits the bank: fn_801F3544 ->
// fn_801F5034 asks the middleware for the bank's resident size
// (fn_8026A9F4), allocates a right-sized block, moves the bank there
// (fn_8026A980) and frees the load buffer. That commit is only valid once the
// middleware's upload has finished (its busy byte, read by fn_8026A96C, is
// clear). Committing while the upload is still in flight leaves the
// middleware's bank table pointing at moved/freed data; a later sound start
// from that bank parses a garbage event stream in fn_8027823C, forever
// (regress forced-k7 and RC1 forced-k2: live frame 14,382; NSR_RB_FORCE=7
// first committed early at frame 12,064, bank 1, in re-simulation).
//
// Live, real uploads finish in <= 4 polls, inside the model's
// NSR_AC_BANK_FRAMES = 6. After a restore, though, the upload resumes from the
// snapshot and re-simulated frames give the audio engine only about half a
// live frame of emulated time each, so a rollback across a bank load (any
// depth that reaches back to within a few frames of the first poll) can reach
// the model's ready frame with the upload still running. A busy-polling loop
// (AC_EXTRA_TICKS) can do the same within one frame.
//
// So when the model says ready while the real upload is busy, the main
// thread waits right here: the hook re-runs the real fn_8026A96C (5
// instructions; it returns to this call site, so the hook sees the poll
// again) until the busy byte clears, then answers ready. Emulated time
// passes, the upload's DMA and interrupt work completes, and nothing the sim
// can observe changes: the model state was already updated by the one
// AcBankReady/AcCountPoll of this poll, and in-frame time is virtual. The
// wait is per machine and costs host time only when an upload is late.
// Bounded by AC_BANK_HOLD_MAX_TICKS of emulated time (an upload that never
// completes, e.g. a load the middleware rejected): then it logs and answers
// ready as before. Revisit if that log line ever appears in a session.
constexpr u32 MW_BANK_BUSY = 0x803A6C10;      // byte: an upload is in flight
constexpr u32 MW_BANK_READY_FN = 0x8026A96C;  // returns !busy
constexpr u64 AC_BANK_HOLD_MAX_TICKS = 486'000'000;  // ~1 s of emulated CPU time
struct AcBankHold
{
  bool active = false;
  u32 record = 0, frame = 0;
  u64 start = 0;
};
AcBankHold s_ac_bank_hold;
u64 s_ac_bank_holds = 0;  // holds this session (diagnostics)

// Returns true when this poll is held (npc/LR set to run the real check).
bool AcBankHoldStep(const Core::CPUThreadGuard& guard, PowerPC::PowerPCState& ppc, const AcMem& m,
                    u32 record)
{
  const u64 now = guard.GetSystem().GetCoreTiming().GetTicks();
  AcBankHold& h = s_ac_bank_hold;
  if (!h.active)
  {
    h = {true, record, s_rb.tick_running, now};
    ++s_ac_bank_holds;
    if (s_ac_bank_holds <= 16 || s_ac_bank_holds % 256 == 0)
      Log("audio clock: bank {:08x} ready at frame {} but its upload is still running{}; "
          "waiting (hold {})",
          record, s_rb.tick_running, s_rb.in_resim ? " (re-sim)" : "", s_ac_bank_holds);
  }
  if ((m.rd8(MW_BANK_BUSY) != 0) && now - h.start < AC_BANK_HOLD_MAX_TICKS)
  {
    LR(ppc) = C_BANK_READY;  // the real check returns to this poll
    ppc.npc = MW_BANK_READY_FN;
    return true;
  }
  if (m.rd8(MW_BANK_BUSY) != 0)
    Log("audio clock: bank {:08x} upload still busy after {} ticks at frame {}; committing anyway",
        h.record, now - h.start, h.frame);
  h.active = false;
  return false;
}
}  // namespace

void AudioClockSimWait(const Core::CPUThreadGuard& guard)
{
  const AcMem m{guard};
  if (AcModeNow() != AcMode::Off && m.rd(AC_BASE) == AC_MAGIC)
    m.wr(AC_EXTRA_TICKS, m.rd(AC_EXTRA_TICKS) + 1);
}

void AudioClockSessionStart(const Core::CPUThreadGuard& guard)
{
  if (AcModeNow() == AcMode::Off)
    return;
  const AcMem m{guard};
  for (u32 a = AC_BASE; a < AC_END - 4; a += 4)  // the last word: MusicDispatchHook's pre-session note
    m.wr(a, 0);
  m.wr(AC_BASE, AC_MAGIC);
  s_ac_real.clear();
  s_ac_bank_hold = {};
  s_ac_bank_holds = 0;
  MusicSessionStart(guard, s_rb.tick_running);
  Log("audio clock: model at {:08x}..{:08x} ({})", AC_BASE, AC_END,
      AcModeNow() == AcMode::Shadow ? "shadow" : "on");
}

// Busy-wait guard. Game code can also poll the model in a loop without
// waiting for a retrace, e.g. the sound loader fn_8006C1E8 spins on
// fn_8006C120 + SndUpdate until its bank queue drains, which needs the bank
// service's bank-ready query (fn_801F35B4) to say ready: with answers that
// only change from frame to frame, that loop never ends (Demo Mode: frame
// 106071, loading the next demo). So every NSR_AC_POLLS_PER_TICK model
// queries within one frame (default 1000; a normal frame makes far fewer)
// count as one more frame-equivalent of model time (AC_EXTRA_TICKS). The
// count is decided by game code, so it is the same on every machine and in
// re-simulation; normal frames are unaffected.
constexpr u32 AC_POLLS = AC_BASE + 0x4C8;       // queries so far in AC_POLL_FRAME
constexpr u32 AC_POLL_FRAME = AC_BASE + 0x4CC;  // (inside the music block's spare words)

void AcCountPoll(const AcMem& m)
{
  const u32 per_tick = s_rb.config.ac_polls_per_tick;
  const u32 frame = s_rb.tick_running;
  u32 polls = m.rd(AC_POLL_FRAME) == frame ? m.rd(AC_POLLS) : 0;
  ++polls;
  m.wr(AC_POLL_FRAME, frame);
  m.wr(AC_POLLS, polls);
  if (polls % per_tick == 0)
  {
    m.wr(AC_EXTRA_TICKS, m.rd(AC_EXTRA_TICKS) + 1);
    if (polls == per_tick)
      AcLogLine(fmt::format("P {} busy poll: {} model queries in one frame", frame, polls));
  }
}

// Queries (HookType::Gate): answer from the model and skip the wrapper.
void AudioClockQueryHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  const u32 pc = s_hook_pc;
  if (!AcActive(guard, ppc))
    return;
  const AcMem m{guard};
  // A held bank poll (see AcBankHoldStep) re-enters here after each real
  // check: it must not count as another query or ask the model again.
  if (pc == C_BANK_READY && s_ac_bank_hold.active && s_ac_bank_hold.frame != s_rb.tick_running)
    s_ac_bank_hold.active = false;  // (defensive: a hold always ends inside its frame)
  if (pc == C_BANK_READY && s_ac_bank_hold.active)
  {
    if (AcBankHoldStep(guard, ppc, m, s_ac_bank_hold.record))
      return;
    LR(ppc) = C_BANK_READY + 4;  // what the poll's own `bl` leaves
    ppc.gpr[3] = 1;
    ppc.npc = C_BANK_READY + 4;
    return;
  }
  AcCountPoll(m);
  const u32 f = AcNow(m);
  const bool shadow = AcModeNow() == AcMode::Shadow;
  if (pc == C_BANK_READY)
  {
    const u32 record = ppc.gpr[9];
    const bool ready = AcBankReady(m, record, f);
    if (shadow)
    {
      AcLogLine(fmt::format("V {} bank {:08x} {}", f, record, ready ? 1 : 0));
      return;
    }
    if (ready && m.rd8(MW_BANK_BUSY) != 0 && AcBankHoldStep(guard, ppc, m, record))
      return;  // ready for the sim; this machine's upload finishes first
    ppc.gpr[3] = ready ? 1 : 0;
    ppc.npc = C_BANK_READY + 4;
    return;
  }
  if (!AcSoundLayerOn(m))
    return;  // the wrapper returns at once without touching audio
  const u32 r3 = ppc.gpr[3], r4 = ppc.gpr[4];
  u32 ret = 0;
  switch (pc)
  {
  case W_SPEECH_STATUS:
  {
    const u32 s = AcPtr(r3) ? m.rd(r3) : 0xFFFFFFFF;
    u32 count = 0, head = 0, ms = 0;
    if (s < AC_STREAMS)
    {
      AcSpeechAdvance(m, s, f);
      const u32 b = AcStreamBase(s);
      count = m.rd(b + 4);
      if (count)
      {
        head = m.rd(b + 0x10);
        const u32 start = m.rd(b), dur = m.rd(b + 0x14);
        ms = AcFramesToMs(static_cast<s32>(f - start) < 0 ? dur : start + dur - f);
      }
    }
    if (shadow)
    {
      AcLogLine(fmt::format("V {} speech {} {} {:08x} {}", f, s, count, head, ms));
      return;
    }
    m.wr(r4, count);
    m.wr(r4 + 4, head);
    m.wr(r4 + 8, ms);
    ret = 0;
    break;
  }
  case W_EVENT_STATUS:
  {
    u32 out[4] = {0, 0, 0, 0};
    ret = 0xFFFFFFF8;
    const u32 s = r3 & 0xFF;
    if (s < AC_STREAMS && static_cast<s32>(r3) >= 0)
    {
      AcSpeechAdvance(m, s, f);
      const u32 b = AcStreamBase(s);
      u32 at = m.rd(b);
      for (u32 i = 0, n = m.rd(b + 4); i < n; ++i)
      {
        const u32 dur = m.rd(b + 0x14 + 8 * i);
        if (m.rd(b + 0x10 + 8 * i) == r3)
        {
          const u32 elapsed = i == 0 && static_cast<s32>(f - at) > 0 ? f - at : 0;
          out[0] = i == 0 ? 2 : 1;
          out[1] = AcFramesToMs(elapsed);
          out[2] = AcFramesToMs(dur - std::min(dur, elapsed));
          out[3] = AcFramesToMs(dur);
          ret = 0;
          break;
        }
        at += dur;
      }
    }
    if (shadow)
    {
      AcLogLine(fmt::format("V {} event {:08x} {} {} {}", f, r3, out[0], out[1], ret));
      return;
    }
    for (u32 i = 0; i < 4; ++i)
      m.wr(r4 + 4 * i, out[i]);
    break;
  }
  case W_SFX_FINISHED:
  {
    ret = AcSfxPlaying(m, AcSfxFind(m, r3), f) ? 0 : 1;
    if (shadow)
    {
      AcLogLine(fmt::format("V {} finished {:08x} {}", f, r3, ret));
      return;
    }
    break;
  }
  case W_STREAM_VOLUME:
  {
    const u32 s = AcPtr(r3) ? m.rd(r3) : 0xFFFFFFFF;
    ret = s < AC_STREAMS ? m.rd(AcStreamBase(s) + 8) : 0;
    if (shadow)
    {
      AcLogLine(fmt::format("V {} volume {} {}", f, s, ret));
      return;
    }
    break;
  }
  default:
    return;
  }
  ppc.gpr[3] = ret;
  ppc.npc = LR(ppc);
}

// Effects (HookType::Start): keep the model in step, translate ids.
void AudioClockEffectHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  const u32 pc = s_hook_pc;
  // Return sites are inside game functions: LR there is not the wrapper's
  // caller, so check the session conditions without the LR test.
  const bool return_site =
      pc == R_SFX_START || std::find(R_SPEECH_QUEUE.begin(), R_SPEECH_QUEUE.end(), pc) != R_SPEECH_QUEUE.end() ||
      std::find(R_SFX_HANDLE.begin(), R_SFX_HANDLE.end(), pc) != R_SFX_HANDLE.end();
  const Rollback& r = s_rb;
  if (AcModeNow() == AcMode::Off || !FrameDrivenLifecycleActive() || !r.in_unit ||
      PowerPC::MMU::HostRead<u32>(guard, 0x800000E4) != 0x803A3F30 ||
      PowerPC::MMU::HostRead<u32>(guard, AC_BASE) != AC_MAGIC)
    return;
  if (!return_site && LR(ppc) >= 0x8025E000)
    return;
  const AcMem m{guard};
  const u32 f = AcNow(m);
  const bool shadow = AcModeNow() == AcMode::Shadow;
  const u32 r3 = ppc.gpr[3];
  switch (pc)
  {
  case W_SFX_START:
  {
    // fn_801F4050 calls the middleware only if the sound layer is on and the
    // bank's ready byte (+0x1C, set by the virtualised bank service) is set.
    const bool calls = m.rd8(SND_FLAGS + 1) && m.rd8(SND_FLAGS + 2) && AcPtr(r3) && m.rd8(r3 + 0x1C);
    s_ac_sfx_start = {calls, f, r3, ppc.gpr[4], 0};
    return;
  }
  case R_SFX_START:
  {
    if (!s_ac_sfx_start.valid || s_ac_sfx_start.frame != f)
      return;
    s_ac_sfx_start.valid = false;
    const u32 frames = AcSfxFrames(m, s_ac_sfx_start.a, s_ac_sfx_start.b);
    const u32 vh = AcSfxStart(m, frames, f);
    s_ac_real[vh] = r3;
    AcLogLine(fmt::format("E {} sfx {:08x} {:x} frames {} -> {:08x} real {:08x}", f, s_ac_sfx_start.a,
                          s_ac_sfx_start.b, frames == AC_LOOP ? -1 : static_cast<s32>(frames), vh, r3));
    if (!shadow)
      ppc.gpr[3] = vh;
    return;
  }
  case W_SPEECH_QUEUE:
  {
    const u32 s = AcPtr(r3) ? m.rd(r3) : 0xFFFFFFFF;
    s_ac_queue = {AcSoundLayerOn(m) && s < AC_STREAMS, f, s, 0, 0};
    return;
  }
  case W_SPEECH_SKIP:
  {
    const u32 real = AcReal(r3);
    AcLogLine(fmt::format("E {} skip {:08x} real {:08x}", f, r3, real));
    AcSpeechRemove(m, r3, f);
    if (!shadow)
      ppc.gpr[3] = real;
    return;
  }
  case W_STREAM_STOP:
  case W_STREAM_DESTROY:
  {
    const u32 s = AcPtr(r3) ? m.rd(r3) : 0xFFFFFFFF;
    AcLogLine(fmt::format("E {} stream-stop {}", f, s));
    AcStreamClear(m, s);
    return;
  }
  case W_STREAM_SETVOL:
  case W_STREAM_FADE:
  {
    const u32 s = AcPtr(r3) ? m.rd(r3) : 0xFFFFFFFF;
    if (s < AC_STREAMS)
      m.wr(AcStreamBase(s) + 8, pc == W_STREAM_SETVOL ? ppc.gpr[4] : ppc.gpr[5]);
    return;
  }
  case W_SFX_FADE:
  case W_SFX_STOP:
  case W_SFX_PARAM:
  case W_SFX_STOP2:
  {
    const s32 slot = AcSfxFind(m, r3);
    const bool playing = AcSfxPlaying(m, slot, f);
    s_ac_handle = {true, f, playing ? static_cast<u32>(slot) : 0xFFFFFFF8, 0, 0};
    if ((pc == W_SFX_STOP || pc == W_SFX_STOP2) && slot >= 0 && playing)
      m.wr(AC_SFX + 12 * slot + 8, f);  // stopped: finished from now on
    if ((r3 & 0xFF000000) == AC_VIRTUAL_SFX && !shadow)
      ppc.gpr[3] = AcReal(r3);
    return;
  }
  default:
    break;
  }
  if (std::find(R_SPEECH_QUEUE.begin(), R_SPEECH_QUEUE.end(), pc) != R_SPEECH_QUEUE.end())
  {
    if (!s_ac_queue.valid || s_ac_queue.frame != f)
      return;
    s_ac_queue.valid = false;
    const u32 s = s_ac_queue.a;
    const u32 seq = m.rd(AC_BASE + 8);
    m.wr(AC_BASE + 8, seq + 1);
    const u32 vid = ((seq + 1) & 0x00FFFFFF) << 8 | s;
    const u32 frames = AcSpeechFrames();
    AcSpeechQueue(m, s, vid, frames, f);
    s_ac_real[vid] = r3;
    AcLogLine(fmt::format("E {} queue {} frames {} -> {:08x} real {:08x}", f, s, frames, vid, r3));
    if (!shadow)
      ppc.gpr[3] = vid;
    return;
  }
  if (std::find(R_SFX_HANDLE.begin(), R_SFX_HANDLE.end(), pc) != R_SFX_HANDLE.end())
  {
    if (s_ac_handle.valid && s_ac_handle.frame == f && !shadow)
      ppc.gpr[3] = s_ac_handle.a;
    s_ac_handle.valid = false;
  }
}

// ---------------------------------------------------------------------------
// Audio clock, music: EA Trax song changes follow frames.
//
// Why: in-game music is EA PathFinder (0x801B1000..0x801B9200) driven by the
// audio thread (handler 0x801B3AAC, registered with the middleware; its clock
// and stream queries follow DSP playback). At segment boundaries PathFinder
// calls the game's listener lists (dispatchers fn_8006D91C / fn_8006D98C /
// fn_8006D9FC, registered by fn_8006DAA8 through fn_801B2F18) *on the audio
// thread*. The EA Trax manager (fn_8007272C, state *(0x803EA704), 0x130
// bytes) listens on the first list (fn_80071C3C): a song's end segment
// (id 0x7C*k + 0x75) makes it pick the next playlist song and tell PathFinder
// to play it (fn_80072424); a song's start segment (id 0x7C*k) builds the
// now-playing banner and posts it to the UI (fn_800724F4 -> fn_8021D7B8),
// which allocates UI objects from shared heaps. So song changes, the
// manager's state and the banner happened on audio time: different frames
// per peer and between a live frame and its re-simulation.
//
// What: during the session the dispatchers run only when this model calls
// them, on the main thread from the frame routine (the frame routine's
// VI_PRE_CB call is redirected through the dispatcher first, then resumes;
// see MusicInjectEvent). PathFinder's own calls are skipped. The model, in
// MEM1 (restored by rollback, identical on every peer):
//   - when the game picks a song (fn_80072424, from the model's end event or
//     any main-thread caller), the song "starts" NSR_AC_SONG_GAP frames later
//     (default 380: end-to-start delay in a reference match) and "ends"
//     NSR_AC_SONG_FRAMES after that (default 10800 = 3:00);
//   - at a start the dispatcher gets (0x11000000, 0x7C): banner;
//   - at an end it gets (0x11000000, 0x75): next song, which PathFinder then
//     plays (audible switch);
//   - the song playing when the session starts started at the last real start
//     event before the session (recorded pre-session, when every peer still
//     shares one timeline).
// Music stays audible: PathFinder still plays every song the game picks. A
// real song shorter than the model's length is followed by silence until the
// model's end event; a longer one is cut by the switch. Deferred: real song
// lengths (PathFinder segment data / stream headers; songs are 39 segments,
// ids step 3), revisit if the fixed length sounds wrong.
// ---------------------------------------------------------------------------
namespace
{
constexpr std::array<u32, 3> MUSIC_DISPATCH{0x8006D91C, 0x8006D98C, 0x8006D9FC};
constexpr u32 MUSIC_PICK_SONG = 0x80072424;  // EA Trax: next playlist song -> PathFinder
constexpr u32 MUSIC_MANAGER = 0x803EA704;    // -0x7B9C(r13): EA Trax manager state
constexpr u32 MUSIC_TRAMP = 0x80416AF8;      // injected dispatcher calls return here (blr)
constexpr u32 AC_MUSIC = 0x80416FC0;         // +0 song end frame, +4 song start frame (+8/+C: AcCountPoll)
constexpr u32 AC_MUSIC_LAST_START = 0x80416FFC;  // last real start before the session (kept)
static_assert(AC_SFX + AC_SFX_SLOTS * 12 <= AC_MUSIC);

void NormalizeTransitionClocks(const Core::CPUThreadGuard& guard, u32 host_clock,
                               u32 host_aux_clock)
{
  using PowerPC::MMU;
  const u32 local_clock = MMU::HostRead<u32>(guard, VI_POST_COUNTER);
  const u32 delta = local_clock - host_clock;
  MMU::HostWrite<u32>(guard, host_clock, VI_POST_COUNTER);
  MMU::HostWrite<u32>(guard, host_aux_clock, TRANSITION_AUX_CLOCK);

  // These GN7E69 words are advanced by the same VI callbacks as the post
  // counter. Preserve their established offsets while moving the clock
  // domain to player 0. The pre-retrace object's counter is reached through
  // its game-owned root so this remains valid if that object moves.
  const auto shift = [&](u32 address) {
    MMU::HostWrite<u32>(guard, MMU::HostRead<u32>(guard, address) - delta, address);
  };
  shift(0x802D55BC);
  shift(0x803EBE58);
  const u32 pre_retrace = MMU::HostRead<u32>(guard, 0x803EBE44);
  if (pre_retrace >= 0x80003100 && pre_retrace < 0x817FFFE0)
    shift(pre_retrace + 0x20);

  Log("handoff clocks normalized: VI {} -> {}, aux -> {}, delta {}", local_clock, host_clock,
      host_aux_clock, delta);
}

constexpr u32 MUSIC_EVENT = 0x11000000;  // track event flags as PathFinder sends them
constexpr u32 MUSIC_START_ID = 0x7C, MUSIC_END_ID = 0x75;
u32 s_music_lr = 0, s_music_r3 = 0;  // the frame routine's VI_PRE_CB call being resumed

bool MusicStartId(s32 id)
{
  return id >= 0 && id % 0x7C == 0;
}

u32 MusicSongFrames()
{
  return s_rb.config.ac_song_frames;
}

u32 MusicGapFrames()
{
  return s_rb.config.ac_song_gap;
}

void MusicEnsureTramp(const Core::CPUThreadGuard& guard)
{
  if (PowerPC::MMU::HostRead<u32>(guard, MUSIC_TRAMP) != 0x4E800020)
    PowerPC::MMU::HostWrite<u32>(guard, 0x4E800020, MUSIC_TRAMP);
}
}  // namespace

// Session start: the song playing now started at the last real start event.
void MusicSessionStart(const Core::CPUThreadGuard& guard, u32 frame)
{
  const AcMem m{guard};
  const u32 last = m.rd(AC_MUSIC_LAST_START);
  u32 end = (last ? last : frame) + MusicSongFrames();
  if (static_cast<s32>(end - frame) <= 0)
    end = frame + 1;
  m.wr(AC_MUSIC, end);
  m.wr(AC_MUSIC + 4, 0);
  AcLogLine(fmt::format("M {} session: last start {} -> end {}", frame, last, end));
}

// HookType::Gate at the three PathFinder -> game dispatchers.
void MusicDispatchHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  const Rollback& r = s_rb;
  if (AcModeNow() == AcMode::Off)
    return;
  if (!FrameDrivenLifecycleActive())
  {
    // Before the session (one shared timeline in netplay: lockstep): note
    // when the current song started, for MusicSessionStart.
    if (s_hook_pc == MUSIC_DISPATCH[0] && (ppc.gpr[3] & 0x01000000) &&
        MusicStartId(static_cast<s32>(ppc.gpr[4])))
      PowerPC::MMU::HostWrite<u32>(guard, r.tick_running, AC_MUSIC_LAST_START);
    return;
  }
  if (LR(ppc) == MUSIC_TRAMP || AcModeNow() == AcMode::Shadow)
    return;  // the model's own call (or shadow mode): run it
  AcLogLine(fmt::format("M {} blocked {:08x} {:08x} {:08x}", r.tick_running, s_hook_pc, ppc.gpr[3],
                        ppc.gpr[4]));
  ppc.npc = LR(ppc);  // PathFinder's call on audio time: skipped
}

// HookType::Start at fn_80072424 (the game picks the next song).
void MusicPickHook(const Core::CPUThreadGuard& guard)
{
  const Rollback& r = s_rb;
  if (AcModeNow() != AcMode::On || !FrameDrivenLifecycleActive() || !r.in_unit ||
      PowerPC::MMU::HostRead<u32>(guard, 0x800000E4) != 0x803A3F30)
    return;
  const AcMem m{guard};
  if (m.rd(AC_BASE) != AC_MAGIC)
    return;
  const u32 mgr = m.rd(MUSIC_MANAGER);
  if (!AcPtr(mgr) || m.rd8(mgr + 0x126) != 0)
    return;  // music off: fn_80072424 returns without picking
  const u32 f = AcNow(m);
  const RollbackLogic::SongTimeline song =
      RollbackLogic::ScheduleSong(f, MusicGapFrames(), MusicSongFrames());
  m.wr(AC_MUSIC + 4, song.start);
  m.wr(AC_MUSIC, song.end);
  AcLogLine(fmt::format("M {} pick: start {} end {}", f, song.start, song.end));
}

// Called by TickCountHook once per paused frame (fix/pausetime): push the
// pending song start/end one frame later. In stock the pause menu stops the
// game's sounds, music included (a stock run paused for ~1.3 s plays the
// rest of the match audio exactly 1.3 s later than an unpaused one), but the
// model counts frames, which keep running under the menu: its end event
// then cut the audible song early by each pause and the next song and its
// now-playing banner arrived ahead of the track. Holding the deadlines keeps
// the model's remaining song time equal to the real stream's. Frame-driven
// and in restored MEM1, so every peer and every re-simulation agree.
// Deferred: speech/SFX model durations (AcNow) still run during a pause; a
// stopped commentary line can count as finished on resume. Revisit if
// commentary overlaps or skips right after unpausing.
void MusicHoldPausedFrame(const Core::CPUThreadGuard& guard)
{
  if (AcModeNow() != AcMode::On)
    return;
  const AcMem m{guard};
  if (m.rd(AC_BASE) != AC_MAGIC)
    return;
  const RollbackLogic::SongTimeline held =
      RollbackLogic::HoldSongOneFrame({m.rd(AC_MUSIC + 4), m.rd(AC_MUSIC)});
  m.wr(AC_MUSIC + 4, held.start);
  m.wr(AC_MUSIC, held.end);
}

// Called from ViCallbackHook for the frame routine's VI_PRE_CB call during the
// session: if a music event is due, call the dispatcher with it first
// (return to MUSIC_TRAMP, which re-enters VI_PRE_CB with the original LR).
void MusicInjectEvent(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  const Rollback& r = s_rb;
  if (AcModeNow() != AcMode::On || !r.in_unit)
    return;
  const AcMem m{guard};
  if (m.rd(AC_BASE) != AC_MAGIC)
    return;
  const u32 f = AcNow(m);
  const u32 end = m.rd(AC_MUSIC), start = m.rd(AC_MUSIC + 4);
  u32 id;
  if (start && static_cast<s32>(f - start) >= 0)
  {
    id = MUSIC_START_ID;
    m.wr(AC_MUSIC + 4, 0);
  }
  else if (end && static_cast<s32>(f - end) >= 0)
  {
    id = MUSIC_END_ID;
    m.wr(AC_MUSIC, f + MusicSongFrames());  // if the game doesn't pick, try again later
  }
  else
  {
    return;
  }
  AcLogLine(fmt::format("M {} inject {:x}", f, id));
  MusicEnsureTramp(guard);
  s_music_lr = LR(ppc);
  s_music_r3 = ppc.gpr[3];
  LR(ppc) = MUSIC_TRAMP;
  ppc.gpr[3] = MUSIC_EVENT;
  ppc.gpr[4] = id;
  ppc.npc = MUSIC_DISPATCH[0];
}

// HookType::Replace at MUSIC_TRAMP: back into the frame routine's VI_PRE_CB call.
void MusicTrampHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  LR(ppc) = s_music_lr;
  ppc.gpr[3] = s_music_r3;
  ppc.npc = VI_PRE_CB;
}

std::vector<unsigned int> MusicDispatchers()
{
  return {MUSIC_DISPATCH.begin(), MUSIC_DISPATCH.end()};
}

std::vector<unsigned int> AudioClockQueryHooks()
{
  return {W_SPEECH_STATUS, W_EVENT_STATUS, W_SFX_FINISHED, W_STREAM_VOLUME, C_BANK_READY};
}

std::vector<unsigned int> AudioClockEffectHooks()
{
  std::vector<unsigned int> v{W_SFX_START,   R_SFX_START,      W_SPEECH_QUEUE,  W_SPEECH_SKIP,
                              W_STREAM_STOP, W_STREAM_DESTROY, W_STREAM_SETVOL, W_STREAM_FADE,
                              W_SFX_FADE,    W_SFX_STOP,       W_SFX_PARAM,     W_SFX_STOP2};
  v.insert(v.end(), R_SPEECH_QUEUE.begin(), R_SPEECH_QUEUE.end());
  v.insert(v.end(), R_SFX_HANDLE.begin(), R_SFX_HANDLE.end());
  return v;
}

std::vector<unsigned int> ResimSkipList()
{
  // Default: skip nothing; re-simulated frames draw fully. Skipping world
  // draws left CPU-side render state (a ring allocator cursor, view/object
  // matrices, EA draw state) permanently different from a no-rollback run,
  // so peers with different rollback histories could never hash equal
  // (rb-coverage-review C1). Each frame costs one retrace either way.
  return ConfigForRollback().skip_resim;
}

void LoadBarrierHook(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  if (!r.initialized || !r.vtime_on || r.io_idle_mode)
    return;
  if (r.in_resim)
  {
    // A re-simulated unit reached a load: with identical inputs this can't
    // happen (the barrier forbids re-running load frames); with corrected
    // inputs it means the rollback crossed into new I/O. Count it.
    ++r.barrier_resim;
    return;
  }
  // The event happens during unit live-1 (or between units): snapshots taken
  // before it are no longer valid rollback targets.
  r.barrier = std::max(r.barrier, r.live);
  ++r.barrier_events;
}

std::vector<unsigned int> EffectGateList()
{
  return ConfigForRollback().effect_gates;
}

std::vector<unsigned int> ProbeAddresses()
{
  return ConfigForRollback().probes;
}

void ProbeHook(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  if (!r.watch || r.live < r.watch_from || r.live > r.watch_to)
    return;
  auto& ppc = guard.GetSystem().GetPPCState();
  const u32 v = PowerPC::MMU::HostRead<u32>(guard, r.watch);
  if (r.config.probe_all)
    Log("probe hit {:08x} from {:08x}: [{:08x}] = {:08x}; live {} cursor {} resim {} r3 {:08x} r4 {:08x}",
        s_hook_pc, LR(ppc) - 4, r.watch, v, r.live, r.cursor, r.in_resim, ppc.gpr[3], ppc.gpr[4]);
  if (v != r.probe_last_value)
    Log("probe: [{:08x}] {:08x} -> {:08x} after {:08x} (called from {:08x}), before {:08x} "
        "(called from {:08x}); live {} cursor {}",
        r.watch, r.probe_last_value, v, r.probe_last_pc, r.probe_last_lr - 4, s_hook_pc,
        LR(ppc) - 4, r.live, r.cursor);
  r.probe_last_value = v;
  r.probe_last_pc = s_hook_pc;
  r.probe_last_lr = LR(ppc);
}

namespace
{
// Start a match-scoped rollback epoch without changing the absolute frame
// index used by recordings.  All history and host mappings are discarded so
// no correction can cross a prior match or menu transition.
void EnterRollbackSession(const Core::CPUThreadGuard& guard, u32 frame)
{
  (void)guard;
  Rollback& r = s_rb;
  if (r.net_enabled)
  {
    if (!s_session.enabled || s_session.failed ||
        (!AllConnected() && !WaitForConnection()))
    {
      Log("rollback session cannot start: transport is not connected");
      Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
      return;
    }
    const u32 session = ConfigForRollback().seed.value_or(0x4E535200) ^ frame;
    // Every datagram carries the config digest: a fixed-epoch session (no
    // handoff barrier) still refuses a differently configured peer (M2).
    const RollbackNet::ConfigFingerprint config = LocalConfigFingerprint(guard.GetSystem());
    Log("config fingerprint {:016x}: {}", RollbackNet::ConfigDigest(config),
        DescribeConfig(config));
    s_rb_exchange = std::make_unique<RollbackNet::Exchange>(
        frame, static_cast<u8>(s_session.players), static_cast<u8>(s_session.me), r.net_delay,
        r.max_prediction, session, RollbackNet::ConfigDigest(config));
    s_rb_logged_desync.reset();
    s_rb_bad_session_logged = false;
    s_rb_violation_logged = false;
    s_rb_peer_desync_logged = false;
    s_rb_desyncs_dumped = 0;
    Log("rollback network epoch {:08x}: player {} of {}, delay {}, max prediction {}", session,
        s_session.me, s_session.players, r.net_delay, r.max_prediction);
  }
  r.vtime_from = frame;
  r.vtime_on = true;
  r.menu_frame_lockstep = false;
  r.in_unit = false;
  r.in_resim = false;
  r.frame_tick_done = false;
  r.rolled = false;
  r.cursor = frame;
  r.tick_running = frame;
  r.vtime_calls = 0;
  r.enter_pending = false;
  r.reenter_ready = false;
  r.session_ended = false;
  r.in_game_seen = false;
  r.session_saw_in_game = false;
  ++r.session_generation;
  r.mispredict_pending = UINT32_MAX;
  r.mispredictions = 0;
  r.uncorrectable = 0;
  r.net_stalls = 0;
  r.sync_waits = 0;
  r.io_idle.clear();
  r.skipped_busy = 0;
  r.barrier = frame;
  r.barrier_events = 0;
  r.barrier_resim = 0;
  r.clamped = 0;
  r.gate_log.clear();
  r.gate_pending.clear();
  r.gate_index = 0;
  r.gate_replayed = 0;
  r.gate_mismatched = 0;
  r.gate_recorded = 0;
  r.pads.clear();
  r.counters.clear();
  r.checks = 0;
  r.mismatched_checks = 0;
  r.rollbacks = 0;
  r.resim_ticks = 0;
  r.granule_hits.clear();
  r.logged_mismatches = 0;
  r.disc_commands_seen = r.disc_commands;
  s_io_ranges.clear();
  for (auto& slot : r.ring)
    std::fill(slot.begin(), slot.end(), 0);
  RollbackLogic::ClearSessionSlotMetadata(&s_dirty.synced, &s_slot_seq);
  s_digest.Invalidate();  // the state digest cache describes the cleared slots
  if (s_rb_exchange)
  {
    // The epoch's page-hash key (digest and hash-mode tracker), from values
    // every peer agreed: the epoch's session id and config digest.
    RbDigestEpochKey((u64{s_rb_exchange->Session()} << 32) ^ s_rb_exchange->Config());
  }
  AramRingSession(guard.GetSystem(), true);  // ARAM replay ring (final-review M1)
  s_ac_pending.clear();
  s_ac_real.clear();
  s_ac_sfx_start = {};
  s_ac_queue = {};
  s_ac_handle = {};
  s_music_lr = 0;
  s_music_r3 = 0;
  s_sim_wait_lr = 0;
  s_sim_wait_pass = false;
  s_sim_waits = 0;

  // The same deterministic base is used for every epoch unless the future
  // netplay handshake supplies an agreed override.
  constexpr u64 TB_PER_FRAME = 40500000 / 60;
  r.vtime_base = ParseU64(Env("NSR_RB_VTIME_BASE")).value_or(TB_PER_FRAME << 37) /
                 TB_PER_FRAME * TB_PER_FRAME;
  s_rb_watch_rollbacks = 0;
  Log("rollback session {} entered at frame {} (base {:016x})", r.session_generation, frame,
      r.vtime_base);
  TelemetrySessionEnter();  // NSR telemetry
}

// End the rollback epoch at a completed live frame.  Clearing the guest model
// and every host-side trampoline/mapping makes all fixed hooks pass through
// immediately and leaves the next epoch independent of this one.
void LeaveRollbackSession(const Core::CPUThreadGuard& guard, const char* reason)
{
  Rollback& r = s_rb;
  if (!r.vtime_on)
    return;
  Log("rollback session {} left after frame {}: {}", r.session_generation, r.tick_running, reason);
  TelemetrySessionLeave(reason);  // NSR telemetry (before the counters reset)
  if (r.force > 0)
    RbReport(40);

  // Automatic stage 4 keeps the audio model and its virtual-to-real handle
  // map alive through the post-match transition.  Clearing it here made
  // loading/audio queries fall back to each peer's emulated-cycle schedule,
  // so a quick Restart/Quit -> next match could allocate identical game
  // objects in different orders.  Non-stage-4 sessions still tear it down.
  const bool continue_frame_driven = r.auto_handoff && !s_session.failed;
  if (!continue_frame_driven)
  {
    const AcMem model{guard};
    for (u32 address = AC_BASE; address < AC_END; address += 4)
      model.wr(address, 0);
    s_ac_pending.clear();
    s_ac_real.clear();
    s_ac_sfx_start = {};
    s_ac_queue = {};
    s_ac_handle = {};
    s_music_lr = 0;
    s_music_r3 = 0;
    s_sim_wait_lr = 0;
    s_sim_wait_pass = false;
    s_sim_waits = 0;
  }

  r.vtime_on = false;
  s_rb_exchange.reset();
  s_rb_handoff.reset();
  s_rb_logged_desync.reset();
  s_rb_bad_session_logged = false;
  s_rb_violation_logged = false;
  s_rb_peer_desync_logged = false;
  r.in_unit = false;
  r.in_resim = false;
  r.frame_tick_done = false;
  r.rolled = false;
  r.cursor = r.live;
  r.vtime_calls = 0;
  r.tick_mode = false;
  r.ticks_since_poll = 0;
  r.idle_polls = 0;
  r.mispredict_pending = UINT32_MAX;
  r.io_idle.clear();
  r.barrier = 0;
  r.gate_log.clear();
  r.gate_pending.clear();
  r.gate_index = 0;
  r.pads.clear();
  r.counters.clear();
  r.enter_pending = false;
  r.reenter_ready = false;
  r.session_ended = true;
  r.in_game_seen = false;
  r.session_saw_in_game = false;
  if (continue_frame_driven)
  {
    ResetLockstepStream();
    const u32 session = ConfigForRollback().seed.value_or(0x4E535200) ^ r.live ^ 0x4D454E55;
    s_rb_exchange = std::make_unique<RollbackNet::Exchange>(
        r.live, static_cast<u8>(s_session.players), static_cast<u8>(s_session.me),
        s_session.delay, 0, session);
    r.menu_frame_lockstep = true;
    Log("post-match frame lockstep epoch {:08x} starts at frame {} with delay {}; "
        "frame-driven time/disc/audio retained",
        session, r.live, s_session.delay);
  }
  s_io_ranges.clear();
  for (auto& slot : r.ring)
    std::fill(slot.begin(), slot.end(), 0);
  RollbackLogic::ClearSessionSlotMetadata(&s_dirty.synced, &s_slot_seq);
  s_digest.Invalidate();  // the state digest cache describes the cleared slots
  AramRingSession(guard.GetSystem(), false);  // ARAM replay ring (final-review M1)
  r.rollbacks = 0;
  r.resim_ticks = 0;
  s_rb_watch_rollbacks = 0;
}
}  // namespace

AramRingView GameplayAramRing(Core::System& system)
{
  const auto [address, bytes] = AramRingBytes(system);
  return {address, bytes};
}

u64 GameplayAramRingHash(Core::System& system)
{
  const auto [address, bytes] = AramRingBytes(system);
  return XXH3_64bits_withSeed(bytes.data(), bytes.size(), address);
}

// Let the video thread omit historical catch-up frames without skipping game-side draws.
bool IsReplayCatchingUp()
{
  return s_follow_catchup.load();
}

void Shutdown(const Core::CPUThreadGuard& guard)
{
  if (s_rb_watchdog.joinable())
  {
    s_rb_watchdog.request_stop();
    s_rb_watchdog.join();
  }
  TelemetryShutdown();  // NSR telemetry: after the watchdog, which can report HANG
  if (s_follow)
    Config::SetCurrent(Config::MAIN_EMULATION_SPEED, s_follow_speed);
  s_follow.reset();
  s_follow_catchup = false;
  s_follow_failed = false;
  s_follow_delay = 0;
  s_follow_speed = 1.0f;
  Session& s = s_session;
  if (s_rb.vtime_on)
    LeaveRollbackSession(guard, "core shutdown");
  else if (s_rb.initialized && s_rb.force > 0)
    RbReport(40);
  if (s_rb.initialized)
  {
    const AcMem model{guard};
    for (u32 address = AC_BASE; address < AC_END; address += 4)
      model.wr(address, 0);
  }
  if (s.host)
  {
    for (ENetPeer* peer : s.peers)
    {
      if (peer)
        enet_peer_disconnect_now(peer, 0);
    }
    enet_host_destroy(s.host);
  }
  s_dirty.Reset();
  s_digest = StateDigest{};
  s_workers.Stop();
  if (s_rb.record)
    std::fclose(s_rb.record);
  if (s_rb.ac_log)
    std::fclose(s_rb.ac_log);
  if (s_rb.stacks)
    std::fclose(s_rb.stacks);
  if (s.log && s.log != stderr)
    std::fclose(s.log);
  s_ac_pending.clear();
  s_ac_real.clear();
  s_ac_sfx_start = {};
  s_ac_queue = {};
  s_ac_handle = {};
  s_music_lr = 0;
  s_music_r3 = 0;
  s_sim_wait_lr = 0;
  s_sim_wait_pass = false;
  s_sim_waits = 0;
  s_hook_pc = 0;
  s = Session{};
  s_rb = Rollback{};
  s_rb_exchange.reset();
  s_rb_handoff.reset();
  s_rb_logged_desync.reset();
  s_rb_bad_session_logged = false;
  s_rb_violation_logged = false;
  s_rb_peer_desync_logged = false;
  s_io_ranges.clear();
  s_rb_last_tick_ms = 0;
  s_rb_watch_live = 0;
  s_rb_watch_rollbacks = 0;
}
}  // namespace NSRNetplay

// ===========================================================================
// NSR telemetry glue (NSRTelemetry.h; NFLStreet docs/telemetry.md).
// Everything the rollback code reports lives here, so the call sites above are
// one line each.  Per live frame it turns the rollback counters into deltas
// and host times into histograms; it only reads host-side counters and, for
// the audio clock, one word of the session model (no writes, no guest-visible
// effect).  All functions are CPU-thread only.
// Deferred: stage-2 network metrics (RTT, loss, lateness, time sync, desync
// checksums) are fed by the netcode itself through NSRTelemetry's API; see the
// integration list in NSRTelemetry.h.  Revisit when stage 2 lands.
// ===========================================================================
namespace NSRNetplay
{
namespace
{
struct TelemetryGlue
{
  bool step_open = false;     // a live step (restore + re-sims + live unit) is running
  bool have_last_end = false;
  std::chrono::steady_clock::time_point step_begin, live_begin, last_live_end;
  std::array<u64, PH_COUNT> phase{};
  u64 rollbacks = 0, resim = 0, mispredictions = 0, uncorrectable = 0, skipped_busy = 0;
  u64 pages_copied = 0, pages_seen = 0;
  u32 ac_extra = 0;
};
TelemetryGlue s_tm;

void TelemetryBaseline()
{
  const Rollback& r = s_rb;
  std::copy(std::begin(s_phase_ns), std::end(s_phase_ns), s_tm.phase.begin());
  s_tm.rollbacks = r.rollbacks;
  s_tm.resim = r.resim_ticks;
  s_tm.mispredictions = r.mispredictions;
  s_tm.uncorrectable = r.uncorrectable;
  s_tm.skipped_busy = r.skipped_busy;
  s_tm.pages_copied = s_dirty.pages_copied;
  s_tm.pages_seen = s_dirty.pages_seen;
  s_tm.ac_extra = 0;  // the audio model restarts with the session
  s_tm.step_open = false;
  s_tm.have_last_end = false;
}

void TelemetryUnitStart(std::chrono::steady_clock::time_point step_start)
{
  const Rollback& r = s_rb;
  if (!r.vtime_on || !r.in_unit || !NSRTelemetry::Enabled())
    return;
  if (!s_tm.step_open)
  {
    s_tm.step_open = true;
    s_tm.step_begin = step_start;
  }
  if (!r.in_resim)
    s_tm.live_begin = std::chrono::steady_clock::now();
}

void TelemetryUnitEnd(const Core::CPUThreadGuard& guard)
{
  namespace T = NSRTelemetry;
  const Rollback& r = s_rb;
  if (!r.vtime_on || r.in_resim || !s_tm.step_open || !T::Enabled())
    return;
  const auto now = std::chrono::steady_clock::now();
  const auto us = [](auto d) {
    return static_cast<u64>(std::max<s64>(
        0, std::chrono::duration_cast<std::chrono::microseconds>(d).count()));
  };
  T::Observe(T::Hist::StepUs, us(s_tm.live_begin - s_tm.step_begin));
  T::Observe(T::Hist::LiveUnitUs, us(now - s_tm.live_begin));
  if (s_tm.have_last_end)
  {
    const u64 interval = us(now - s_tm.last_live_end);
    T::Observe(T::Hist::FrameIntervalUs, interval);
    if (interval > 17535)  // 16.7 ms frame budget + 5%
      T::Count(T::Counter::FramesOverBudget);
    if (interval > 50000)
      T::Count(T::Counter::HostStalls);
  }
  constexpr std::array<std::pair<HostPhase, T::Hist>, PH_COUNT> PHASES{{
      {PH_SAVE, T::Hist::SaveUs},
      {PH_RESTORE, T::Hist::RestoreUs},
      {PH_LIVE, T::Hist::LiveRangesUs},
      {PH_JIT, T::Hist::JitUs},
      {PH_CHECK, T::Hist::CheckUs},
  }};
  for (const auto& [phase, hist] : PHASES)
  {
    const u64 delta = s_phase_ns[phase] - s_tm.phase[phase];
    s_tm.phase[phase] = s_phase_ns[phase];
    if (delta > 0 || phase == PH_SAVE)
      T::Observe(hist, delta / 1000);
  }

  const u64 d_rb = r.rollbacks - s_tm.rollbacks;
  const u64 d_resim = r.resim_ticks - s_tm.resim;
  if (d_rb > 0)
  {
    T::Count(T::Counter::Rollbacks, d_rb);
    for (u64 i = 0; i < d_rb; ++i)  // normally one restore per live step
      T::Observe(T::Hist::RollbackDepth, d_resim / d_rb);
  }
  T::Count(T::Counter::ResimFrames, d_resim);
  T::Count(T::Counter::Mispredictions, r.mispredictions - s_tm.mispredictions);
  T::Count(T::Counter::QuietSkips, r.skipped_busy - s_tm.skipped_busy);
  if (r.uncorrectable > s_tm.uncorrectable)
  {
    T::Count(T::Counter::Uncorrectable, r.uncorrectable - s_tm.uncorrectable);
    T::Event(T::EventKind::Uncorrectable, r.tick_running,
             {T::Int("total", static_cast<s64>(r.uncorrectable)),
              T::Int("ring_slots", static_cast<s64>(r.ring.size())),
              T::Int("barrier", r.barrier)});
  }
  s_tm.rollbacks = r.rollbacks;
  s_tm.resim = r.resim_ticks;
  s_tm.mispredictions = r.mispredictions;
  s_tm.uncorrectable = r.uncorrectable;
  s_tm.skipped_busy = r.skipped_busy;
  if (s_dirty.on)
  {
    T::Count(T::Counter::DirtyPagesCopied, s_dirty.pages_copied - s_tm.pages_copied);
    T::Count(T::Counter::DirtyPagesWritten, s_dirty.pages_seen - s_tm.pages_seen);
    s_tm.pages_copied = s_dirty.pages_copied;
    s_tm.pages_seen = s_dirty.pages_seen;
  }
  if (AcModeNow() != AcMode::Off)
  {
    const AcMem m{guard};
    if (m.rd(AC_BASE) == AC_MAGIC)
    {
      const u32 extra = m.rd(AC_EXTRA_TICKS);
      if (extra > s_tm.ac_extra)
        T::Count(T::Counter::AudioClockCatchups, extra - s_tm.ac_extra);
      s_tm.ac_extra = extra;
    }
  }
  s_tm.step_open = false;
  s_tm.have_last_end = true;
  s_tm.last_live_end = now;
  T::LiveFrame(r.tick_running, now);
}

void TelemetrySessionEnter()
{
  NSRTelemetry::Rearm();  // a new boot after a previous core shutdown
  if (!NSRTelemetry::Enabled())
    return;
  const Rollback& r = s_rb;
  TelemetryBaseline();
  NSRTelemetry::RollbackInfo info;
  info.enter_frame = r.vtime_from;
  info.generation = r.session_generation;
  info.force = r.force;
  info.mispredict = r.mispredict;
  info.ring_slots = static_cast<u32>(r.ring.size());
  info.io_idle = r.io_idle_mode;
  info.audio_rollback = r.audio_rollback;
  info.dirty_tracking = s_dirty.on;
  info.dirty_mode = static_cast<u32>(s_dirty.mode);
  info.frame_mode = r.frame_mode;
  info.vmem_bytes = r.vmem_bytes;
  NSRTelemetry::SessionEnter(info);
  if (s_dirty.protect_fallback)
  {
    NSRTelemetry::Event(NSRTelemetry::EventKind::Note, r.vtime_from,
                        {NSRTelemetry::Int("dirty_protect_fallback", 1)});
  }
}

void TelemetrySessionLeave(const char* reason)
{
  namespace T = NSRTelemetry;
  if (!T::Enabled())
    return;
  const Rollback& r = s_rb;
  T::SessionLeave(r.tick_running, reason ? reason : "",
                  {T::Int("generation", r.session_generation),
                   T::Int("rollbacks", static_cast<s64>(r.rollbacks)),
                   T::Int("resim_frames", static_cast<s64>(r.resim_ticks)),
                   T::Int("mispredictions", static_cast<s64>(r.mispredictions)),
                   T::Int("uncorrectable", static_cast<s64>(r.uncorrectable)),
                   T::Int("quiet_skips", static_cast<s64>(r.skipped_busy))});
  s_tm.step_open = false;
  s_tm.have_last_end = false;
}

void TelemetryShutdown()
{
  NSRTelemetry::Shutdown();
  s_tm = TelemetryGlue{};
}

u32 TelemetryLiveFrame()
{
  return s_rb.live;
}
}  // namespace
}  // namespace NSRNetplay
// ===== end NSR telemetry glue =====
