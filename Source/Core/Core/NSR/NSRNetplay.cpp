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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <atomic>
#include <vector>

#include <enet/enet.h>
#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"

namespace NSRNetplay
{
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
  if (const std::string path = Env("NSR_LOG"); !path.empty())
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

void HandlePacket(const ENetPacket* packet)
{
  Session& s = s_session;
  if (packet->dataLength < offsetof(Packet, pads))
    return;
  Packet p;
  std::memcpy(&p, packet->data, std::min(sizeof(Packet), packet->dataLength));
  if (p.magic != PACKET_MAGIC || p.count > REDUNDANCY || p.sender >= s.players ||
      p.sender == s.me)
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
      if (index < s.players && index != s.me)
      {
        s.peers[index] = event.peer;
        Log("connected to player {}", index);
      }
      break;
    }
    case ENET_EVENT_TYPE_RECEIVE:
      HandlePacket(event.packet);
      enet_packet_destroy(event.packet);
      break;
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
      Log("not all {} players connected within {} ms; giving up (playing offline)", s.players,
          CONNECT_TIMEOUT_MS);
      s.failed = true;
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
constexpr std::array<std::pair<u32, u32>, 22> RB_KEEP{{
    {0x003EE4C0, 0x003FE4C0},  // main-thread stack
    {0x00384C00, 0x00398E00},  // memory-card work areas
    {0x00411A00, 0x00412000},  // NFLStreet patch area (counters, scripts)
    {0x00416900, 0x00416A80},  // rollback patch area
    {0x004132C0, 0x00413E00},  // trace ring
    {0x00000000, 0x00003100},  // low memory
    // SDK .data, only the words SDK code references (lis/addi pairs, see
    // NFLStreet docs/research-log.md 2026-10-09). The old blocks
    // (0x802B6000.., 0x802FB400..0x802FFE00, 0x80300800.., 0x80302400..) also
    // held game/EA-engine data (restored) and audio-middleware data (moved to
    // AUDIO_KEEP); keeping those live leaked state set by later frames into
    // re-simulated ones (EA floats at 0x802FB45C changed at a play start).
    {0x002B7240, 0x002B7260},
    {0x002C7000, 0x002C7200},  // no direct references found; kept as before
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

struct Rollback
{
  bool initialized = false;
  u32 force = 0;          // NSR_RB_FORCE: ticks to roll back before every live tick
  // NSR_RB_MISPREDICT=d (with NSR_RB_REPLAY, and NSR_RB_FORCE > 0 for the ring):
  // netplay with input delay d on one machine. Each live frame runs with the
  // last confirmed pads (frame t-d) as its prediction; when frame c = t-d's
  // true pads differ from what c ran with, roll back to c (or the newest
  // I/O-idle snapshot before it) and re-run with the corrected pads.
  u32 mispredict = 0;
  u32 mispredict_pending = UINT32_MAX;  // earliest frame still to correct
  u64 mispredictions = 0;
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
};

Rollback s_rb;

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

bool IsIoIdle(const u8* ram);  // defined with the restore helpers

void RbInit(Core::System& system)
{
  Rollback& r = s_rb;
  r.initialized = true;
  if (const std::string path = Env("NSR_LOG"); !path.empty() && s_session.log == stderr)
  {
    if (FILE* f = std::fopen(path.c_str(), "a"))
      s_session.log = f;
  }
  r.force = ParseU32(Env("NSR_RB_FORCE")).value_or(0);
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
  r.audio_rollback = !Env("NSR_RB_AUDIO_ROLLBACK").empty();
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

  r.io_idle_mode = !Env("NSR_RB_IO_IDLE").empty();
  if (r.force > 0 || !Env("NSR_RB_SAMPLE").empty() || !Env("NSR_RB_STACKS").empty())
  {
    if (r.force > 0)
      r.ring.assign(r.io_idle_mode ? std::max<u32>(r.force + 1, ParseU32(Env("NSR_RB_RING")).value_or(24))
                                   : r.force + 1,
                    std::vector<u8>(size + r.vmem_bytes));
    // Diagnostics: if ticks stop after rollbacks began (the game hung), log
    // where the CPU is once and dump MEM1 (NSR_DUMP_PREFIX-hang.bin).
    std::thread([&system] {
      for (;;)
      {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const s64 last = s_rb_last_tick_ms.load();
        const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        // NSR_RB_SAMPLE: profile where the CPU is during rollbacks (every 2 s).
        static const bool sample = !Env("NSR_RB_SAMPLE").empty();
        // NSR_RB_STACKS=path: append raw call stacks (pc, then return
        // addresses) for an offline call-tree profile, 400 samples every 2 s.
        static FILE* stacks = [] {
          const std::string path = Env("NSR_RB_STACKS");
          return path.empty() ? nullptr : std::fopen(path.c_str(), "w");
        }();
        if (stacks)
        {
          auto& p = system.GetPPCState();
          const u8* m = system.GetMemory().GetRAM();
          auto rd2 = [&](u32 a) { return Common::swap32(m + (a & 0x01FFFFFF)); };
          for (int k = 0; k < 400; ++k)
          {
            std::string line = fmt::format("{:08x} {:08x}", p.pc, LR(p));
            u32 sp = p.gpr[1];
            for (int i = 0; i < 24 && (sp >> 24) == 0x80; ++i)
            {
              sp = rd2(sp);
              if ((sp >> 24) != 0x80)
                break;
              line += fmt::format(" {:08x}", rd2(sp + 4));
            }
            std::fprintf(stacks, "%u %s\n", s_rb.live, line.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
          }
          std::fflush(stacks);
          continue;
        }
        if (sample && (s_rb.rollbacks > 0 || s_rb.force == 0))
        {
          auto& p = system.GetPPCState();
          const u8* m = system.GetMemory().GetRAM();
          auto rd2 = [&](u32 a) { return Common::swap32(m + (a & 0x01FFFFFF)); };
          std::map<u32, int> hits;
          for (int k = 0; k < 200; ++k)
          {
            // Attribute to the InGame call site on the stack (its LR inside
            // InGame::fn_8002854C), or the PC if not inside InGame.
            u32 sp = p.gpr[1], site = 0;
            u32 lr0 = LR(p);
            if (lr0 >= 0x8002854C && lr0 < 0x80028744)
              site = lr0;
            for (int i = 0; i < 24 && !site && (sp >> 24) == 0x80; ++i)
            {
              sp = rd2(sp);
              if ((sp >> 24) != 0x80)
                break;
              const u32 ra = rd2(sp + 4);
              if (ra >= 0x8002854C && ra < 0x80028744)
                site = ra;
            }
            ++hits[site ? site : (p.pc | 1)];
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
          std::vector<std::pair<int, u32>> top;
          for (const auto& [pc, n] : hits)
            top.emplace_back(n, pc);
          std::sort(top.rbegin(), top.rend());
          std::string line;
          for (size_t i = 0; i < top.size() && i < 8; ++i)
            line += fmt::format(" {:08x}x{}", top[i].second, top[i].first);
          Log("sample (InGame call site, odd = outside InGame) live {}:{}", s_rb.live, line);
          continue;
        }
        if (s_rb.rollbacks == 0 || last == 0 || now - last < 5000)
          continue;
        auto& ppc = system.GetPPCState();
        const u8* ram = system.GetMemory().GetRAM();
        auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
        // Sample the CPU 50 times: a busy loop shows one hot PC, an interrupt
        // storm shows handler PCs, a blocked thread shows the idle loop.
        std::map<std::string, int> samples;
        for (int k = 0; k < 50; ++k)
        {
          std::string chain = fmt::format("pc {:08x} lr {:08x} msr {:08x}", ppc.pc, LR(ppc), ppc.msr.Hex);
          u32 sp = ppc.gpr[1];
          for (int i = 0; i < 12 && (sp >> 24) == 0x80; ++i)
          {
            sp = rd(sp);
            if ((sp >> 24) != 0x80)
              break;
            chain += fmt::format(" <- {:08x}", rd(sp + 4));
          }
          ++samples[chain];
          std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        Log("HANG: no tick for {} ms after {} rollbacks; CPU samples:", now - last, s_rb.rollbacks);
        {
          // Thread dump: every active thread's state, wait queue, saved pc/lr.
          u32 th = rd(0x800000DC);
          for (int n = 0; n < 16 && (th >> 24) == 0x80; ++n, th = rd(th + 0x2FC))
            Log("  thread {:08x} state {} prio {} queue {:08x} srr0 {:08x} lr {:08x} sp {:08x}", th,
                rd(th + 0x2C8) >> 16, rd(th + 0x2D0), rd(th + 0x2DC), rd(th + 0x198), rd(th + 0x84),
                rd(th + 4));
          Log("  dvd executing {:08x}; pending loads {}; io idle {}; disc worker sem counts {} {}",
              rd(0x803ED118), rd(PENDING_LOADS), IsIoIdle(ram), rd(0x80381C00), rd(0x80381C0C));
        }
        // In __OSUnhandledException (GN7E69 0x8023AF0C): r24 = exception,
        // r25 = context, r26 = DSISR, r27 = DAR.
        const u32 c = ppc.gpr[25];
        Log("  r24 {:08x} r25 {:08x} r26 {:08x} r27 {:08x}; context srr0 {:08x} srr1 {:08x} lr {:08x}",
            ppc.gpr[24], c, ppc.gpr[26], ppc.gpr[27], (c >> 24) == 0x80 ? rd(c + 0x198) : 0,
            (c >> 24) == 0x80 ? rd(c + 0x19C) : 0, (c >> 24) == 0x80 ? rd(c + 0x84) : 0);
        if ((c >> 24) == 0x80)
        {
          // The faulting context's registers and stack.
          std::string regs;
          for (int g = 0; g < 32; ++g)
            regs += fmt::format(" r{}={:08x}", g, rd(c + 4 * g));
          Log("  context:{} ctr={:08x}", regs, rd(c + 0x88));
          std::string chain;
          u32 sp = rd(c + 4);
          for (int i = 0; i < 16 && (sp >> 24) == 0x80; ++i)
          {
            chain += fmt::format(" [{:08x}: lr {:08x}]", sp, rd(sp + 4));
            sp = rd(sp);
          }
          Log("  context stack:{}", chain);
        }
        for (const auto& [chain, n] : samples)
          Log("  {:2}x {}", n, chain);
        const std::string prefix = Env("NSR_DUMP_PREFIX");  // never dump into the cwd
        if (FILE* f = prefix.empty() ? nullptr : std::fopen((prefix + "-hang.bin").c_str(), "wb"))
        {
          std::fwrite(ram, 1, system.GetMemory().GetRamSizeReal(), f);
          std::fclose(f);
        }
        return;
      }
    }).detach();
  }
  Log("rollback image: force={} keep-ranges={} hash-log={}", r.force, keep.size(), r.hash_log);
}

void RbSave(const u8* ram, std::vector<u8>& slot)
{
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
std::vector<std::pair<u32, u32>> RbLiveRanges(Core::System& system, const u8* ram)
{
  // Layouts from the Dolphin SDK headers / decomp (checked by the Codex audit
  // in NFLStreet local/work/codex/live-objects-audit.md). Every walk is
  // bounded; ranges are clamped to MEM1.
  const u32 mem_size = system.GetMemory().GetRamSizeReal();
  auto rd = [&](u32 a) { return Common::swap32(ram + (a & 0x01FFFFFF)); };
  auto valid = [&](u32 a) { return a >= 0x80003100 && a < 0x80000000 + mem_size - 4; };
  std::vector<std::pair<u32, u32>> live;
  auto keep = [&](u32 a, u32 size) {
    if (valid(a))
      live.emplace_back(a & 0x01FFFFFF, std::min<u32>((a & 0x01FFFFFF) + size, mem_size));
  };
  // Threads (active queue at 0x800000DC, link +0x2FC): OSThread, stack, the
  // OSThreadQueue it sleeps on (+0x2DC), the mutex it waits for (+0x2F0) and
  // the mutexes it owns (list at +0x2F4, next +0x10). OSMutex = 0x18 bytes.
  u32 t = rd(0x800000DC);
  for (int n = 0; n < 32 && valid(t); ++n, t = rd(t + 0x2FC))
  {
    if (t == DISC_WORKER_THREAD && s_rb.io_idle_mode)
      continue;  // restored with everything else in I/O-idle mode
    if (t == AUDIO_THREAD && s_rb.audio_rollback)
      continue;  // restored with the rest of audio
    if (t != 0x803A3F30)  // main thread: its OSThread and stack are kept statically
    {
      keep(t, 0x318);
      const u32 base = rd(t + 0x304), end = rd(t + 0x308);
      if (valid(end) && valid(base) && end < base && base - end <= 0x100000)
        live.emplace_back(end & 0x01FFFFFF, base & 0x01FFFFFF);
    }
    keep(rd(t + 0x2DC), 8);
    keep(rd(t + 0x2F0), 0x18);
    u32 m = rd(t + 0x2F4);
    for (int k = 0; k < 64 && valid(m); ++k, m = rd(m + 0x10))
      keep(m, 0x18);
  }
  // Alarms (queue head 0x803ED078, next +0x14, 0x28 bytes).
  u32 al = rd(0x803ED078);
  for (int n = 0; n < 256 && valid(al); ++n, al = rd(al + 0x14))
    keep(al, 0x28);
  // DVD: four priority queues (circular, sentinels at 0x803A55D8 + 8 * i,
  // next at +0) and the executing block (0x803ED118). DVDCommandBlock is
  // 0x30 bytes; 0x3C for the DVDFileInfo wrapper (callback 0x8023F464).
  auto dvd = [&](u32 cb) { keep(cb, rd(cb + 0x28) == 0x8023F464 ? 0x3C : 0x30); };
  for (u32 i = 0; i < 4; ++i)
  {
    const u32 sentinel = 0x803A55D8 + 8 * i;
    u32 cb = rd(sentinel);
    for (int n = 0; n < 64 && valid(cb) && cb != sentinel; ++n, cb = rd(cb))
      dvd(cb);
  }
  if (const u32 cb = rd(0x803ED118); valid(cb))
    dvd(cb);
  // ARQ: queued requests (heads 0x803ED2D8 / 0x803ED2E0, next +0) and the
  // pending one per priority (0x803ED2F0 / 0x803ED2F4). ARQRequest = 0x20.
  for (const u32 head : {0x803ED2D8u, 0x803ED2E0u})
  {
    u32 q = rd(head);
    for (int n = 0; n < 256 && valid(q); ++n, q = rd(q))
      keep(q, 0x20);
  }
  keep(rd(0x803ED2F0), 0x20);
  keep(rd(0x803ED2F4), 0x20);
  // Audio middleware callback list (GN7E69): head at 0x803A6BCC (kept), walked
  // by fn_80269C70 on the audio thread, nodes {next, ?, callback, arg} living
  // in game memory. A restore that rewound a still-linked node left a null
  // callback (crash on the audio thread).
  if (!s_rb.audio_rollback)
  {
    u32 node = rd(0x803A6BCC);
    for (int n = 0; n < 256 && valid(node); ++n, node = rd(node))
      keep(node, 0x10);
  }
  // The game's real-time tick accumulator (see TICK_CLOCK_PTR).
  if (const u32 outer = rd(TICK_CLOCK_PTR); valid(outer))
    keep(rd(outer + 0xC), 0x10);
  // GX FIFO, from the command processor's registers.
  if (!s_rb.io_idle_mode)
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
  const auto live = RbLiveRanges(system, ram);
  std::vector<std::vector<u8>> keep;
  for (const auto& [lo, hi] : live)
    keep.emplace_back(ram + lo, ram + hi);
  constexpr u32 PAGE = 0x1000;
  std::vector<u32> changed;
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
  for (size_t i = 0; i < live.size(); ++i)
    std::memcpy(ram + live[i].first, keep[i].data(), keep[i].size());
  // Fake VMEM holds data only (the database heap): no JIT invalidation needed.
  if (s_rb.vmem_bytes && slot.size() >= s_rb.mem1_size + s_rb.vmem_bytes)
    std::memcpy(s_rb.vmem, slot.data() + s_rb.mem1_size, s_rb.vmem_bytes);
  auto& jit = system.GetJitInterface();
  for (const u32 page : changed)
    jit.InvalidateICache(0x80000000 | page, PAGE, true);
  // The emulated instruction cache too: recompilation fetches through it, so
  // stale lines would keep the old code bytes (rb-coverage-review C5).
  if (!changed.empty())
    system.GetPPCState().iCache.Reset(jit);
}

// Compare RAM (re-simulated state before tick t) with the original state
// before tick t, granule by granule; record and log what differs.
void RbCheck(u32 tick, const u8* ram, const std::vector<u8>& original)
{
  Rollback& r = s_rb;
  ++r.checks;
  u32 differing = 0;
  std::vector<std::pair<u32, u32>> ranges;  // coalesced, for the log
  for (const auto& [lo, hi] : r.segments)
  {
    for (u32 g = lo; g < hi; g += RB_GRANULE)
    {
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
  static bool seed_written = false;
  if (seed_written)
    return;
  seed_written = true;
  if (const auto value = ParseU32(Env("NSR_SEED")))
    PowerPC::MMU::HostWrite<u32>(guard, *value, SEED_ADDRESS);
}
}  // namespace

long long DiscLatency(long long ticks)
{
  static const bool instant = !Env("NSR_RB_INSTANT_DISC").empty();
  return instant && s_rb.vtime_on ? 0 : ticks;
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
  static const bool enabled =
      std::getenv("NSR_NETPLAY") != nullptr || std::getenv("NSR_SEED") != nullptr;
  return enabled;
}

void Hook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  const u32 buf = ppc_state.gpr[3];
  const u32 frame = ppc_state.gpr[4];
  ppc_state.npc = LR(ppc_state);  // Replace hook: return to caller

  InstallSeedOnce(guard);
  if (std::getenv("NSR_NETPLAY") == nullptr)
    return;  // seed-only mode (offline practice)

  Session& s = s_session;
  if (!s.initialized)
    s.enabled = Setup();
  if (!s.enabled || s.failed)
    return;
  if (!AllConnected() && !WaitForConnection())
    return;

  // The pad poll can run more than once for the same retrace (the game also
  // polls once directly at init). Re-apply what was agreed for that frame.
  if (auto it = s.applied.find(frame); it != s.applied.end())
  {
    WritePads(guard, buf, it->second);
    return;
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
      Log("a player went silent at frame {}; continuing offline", frame);
      s.failed = true;
      return;
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
  if (frame % CHECKSUM_INTERVAL == 0 && frame > 0)
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
  }
  if (frame % 600 == 0)
    Log("frame {} ok (desyncs so far: {})", frame, s.desyncs);
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
  if (!r.tick_mode && r.cursor == r.live)
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
  static const auto spec = Split(Env("NSR_RB_TRANSPLANT"), ':');
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
        hi = std::min<unsigned long>(hi & 0x01FFFFFF, dump.size());
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

// Diagnostics: NSR_RB_DUMP_UNIT=live:frame,... writes all of MEM1 right
// before frame `frame` runs during live frame `live`'s step (live == frame
// for the live frame itself), to NSR_DUMP_PREFIX-unit-<live>-<frame>.bin.
static void RbDumpUnit(Core::System& system, const u8* ram, u32 live, u32 frame)
{
  static const std::vector<std::string> wanted = Split(Env("NSR_RB_DUMP_UNIT"), ',');
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
  static const auto spec = Split(Env("NSR_RB_ADOPT"), ':');
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
  static const std::optional<u32> seed = ParseU32(Env("NSR_RB_FORCE_RANDOM"));
  if (!seed)
    return s_rb.force;
  u64 z = (u64(*seed) << 32 | live) + 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;
  return static_cast<u32>(z % (s_rb.force + 1));
}

// Misprediction mode: the pads frame t really had (the replayed recording).
static Pads TruePads(u32 t)
{
  if (const auto it = s_rb.replay.find(t); it != s_rb.replay.end())
    return it->second;
  return s_rb.latest;
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
  u8* ram = system.GetMemory().GetRAM();
  const u32 slots = r.ring.empty() ? 1 : static_cast<u32>(r.ring.size());
  ++r.ticks_since_poll;
  RbSampleIo(guard);
  if (r.disc_commands != r.disc_commands_seen)
  {
    if (static const bool trace = !Env("NSR_RB_DISC_TRACE").empty(); trace && r.vtime_on)
      Log("disc commands before unit live {} cursor {} rolled {}: {}", r.live, r.cursor, r.rolled,
          r.disc_commands - r.disc_commands_seen);
    r.disc_commands_seen = r.disc_commands;
  }

  // NSR_RB_MEMWATCH: armed at the start of the NSR_RB_WATCH window (memchecks
  // make the JIT very slow, so not from boot).
  static bool memwatch_armed = false;
  if (!memwatch_armed && r.watch && r.live >= r.watch_from)
  {
    memwatch_armed = true;
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
      if (r.mispredict == 0)  // with corrected pads the state is meant to differ
      {
        for (const auto& [lo, hi] : RbLiveRanges(system, ram))
          std::memcpy(r.ring[r.live % slots].data() + lo, ram + lo, hi - lo);
        RbCheck(r.live, ram, r.ring[r.live % slots]);
      }
    }
    else if (r.mispredict > 0 && r.force > 0)
    {
      // Misprediction mode: confirm frame c = live - d.
      const u32 t = r.live, d = r.mispredict;
      const u32 start = std::max(r.force_from, r.vtime_from);
      if (t >= start + d)
      {
        const u32 c = t - d;
        if (r.pads.contains(c) && r.pads[c] != TruePads(c))
        {
          r.mispredict_pending = std::min(r.mispredict_pending, c);
          ++r.mispredictions;
        }
      }
      if (r.mispredict_pending != UINT32_MAX)
      {
        u32 target = r.mispredict_pending;
        const u32 oldest = t >= slots - 1 ? t - (slots - 1) : 0;
        bool ok = target >= oldest && target >= start;
        if (ok && r.io_idle_mode)
        {
          while (target >= oldest && target > start && !(r.io_idle.contains(target) && r.io_idle[target]))
            --target;
          ok = IsIoIdle(ram) && target >= oldest && r.io_idle.contains(target) && r.io_idle[target];
        }
        if (ok)
        {
          // Corrected pads: true up to the newest confirmed frame, then the
          // new prediction (its pads) for the unconfirmed ones.
          const u32 confirmed = t >= d ? t - d : 0;
          for (u32 j = target; j < t; ++j)
            r.pads[j] = TruePads(std::min(j, confirmed));
          RbSave(ram, r.ring[t % slots]);
          RbRestore(system, ram, r.ring[target % slots]);
          r.cursor = target;
          r.rolled = true;
          ++r.rollbacks;
          r.mispredict_pending = UINT32_MAX;
        }
        else
        {
          ++r.skipped_busy;  // retry next frame (deeper rollback then)
          if (target < oldest)
            Log("misprediction at {} fell out of the snapshot ring (live {})", r.mispredict_pending, t);
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
        const u32 oldest = r.live >= slots - 1 ? r.live - (slots - 1) : 0;
        while (target >= oldest && target > 0 && !(r.io_idle.contains(target) && r.io_idle[target]))
          --target;
        if (!IsIoIdle(ram) || target < oldest || !r.io_idle.contains(target))
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
    RbSave(ram, r.ring[r.cursor % slots]);
    if (r.io_idle_mode)
      r.io_idle[r.cursor] = IsIoIdle(ram);
    RbWritePads(guard, buf, r.pads.at(r.cursor));
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
  const u32 t = r.live;
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
    }
  }
  if (r.hash_log && t % 60 == 0)
    Log("tick {} hash {:016x}", t, RbHash(ram));
  if (static const bool trace = !Env("NSR_RB_IO_TRACE").empty(); trace && t % 5 == 0)
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
    if (static const bool qt = !Env("NSR_RB_QUIET_TRACE").empty(); qt)
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
  if (r.force > 0)
    RbSave(ram, r.ring[t % slots]);
  if (const auto it = r.replay.find(t); it != r.replay.end())
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
  if (r.vtime_from && t == r.vtime_from && !r.vtime_on)
  {
    r.vtime_on = true;
    // The same base on every machine (each machine's own time base differs,
    // and game frame timers floor time*60/clock), a whole number of frames.
    // NSR_RB_VTIME_BASE overrides it (netplay: agreed at session start).
    constexpr u64 TB_PER_FRAME = 40500000 / 60;
    r.vtime_base = ParseU64(Env("NSR_RB_VTIME_BASE")).value_or(TB_PER_FRAME << 37) /
                   TB_PER_FRAME * TB_PER_FRAME;
    Log("virtual tick time from tick {} (base {:016x})", t, r.vtime_base);
    AudioClockSessionStart(guard);
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
  if (t >= 600)
  {
    r.pads.erase(r.pads.begin(), r.pads.lower_bound(t - 600));
    r.counters.erase(r.counters.begin(), r.counters.lower_bound(t - 600));
    r.gate_log.erase(r.gate_log.begin(), r.gate_log.lower_bound(t - 600));
  }
  if (t % 600 == 0)
  {
    Log("tick {}: rollbacks {}, re-simulated {}, mismatched checks {}/{}; audio gate: "
        "recorded {}, replayed {}, mismatched {}; load barriers {} (clamped {}, in re-sim {}); "
        "io-idle skips {}; mispredictions {}",
        t, r.rollbacks, r.resim_ticks, r.mismatched_checks, r.checks, r.gate_recorded,
        r.gate_replayed, r.gate_mismatched, r.barrier_events, r.clamped, r.barrier_resim,
        r.skipped_busy, r.mispredictions);
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

void RollbackFrameHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();
  ppc_state.npc = LR(ppc_state);
  Rollback& r = s_rb;
  r.frame_mode = true;
  r.frame_tick_done = false;
  ppc_state.gpr[3] = RbStep(guard, RB_PAD_BUF);
  AudioClockFrameLog(guard);
}

void RollbackFrameEndHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();
  ppc_state.npc = LR(ppc_state);
  static const bool async_present = !Env("NSR_RB_ASYNC_PRESENT").empty();
  if (s_rb.vtime_on && !async_present)
    PowerPC::MMU::HostWrite<u32>(guard, PowerPC::MMU::HostRead<u32>(guard, VI_POST_COUNTER) + 1,
                                 VI_POST_COUNTER);
  s_rb.in_unit = false;
}

// Frame mode, during the session: exactly one tick per frame. Replaces the
// real-time accumulator drain fn_802378C8 (which returns the ticks due and
// adds them to the game clock outer+4): the first call in a frame returns
// the fixed step and advances the clock; later calls return 0. Frames, not
// wall-clock time, then drive the game, as on every machine (Slippi's model).
void TickCountHook(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  auto& ppc = guard.GetSystem().GetPPCState();
  if (!r.frame_mode || !r.vtime_on)
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
  if (!r.frame_tick_done)
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
  if (from_frame_routine != s_rb.vtime_on)
    ppc.npc = lr;  // skip this call
}

void PeekZHook(const Core::CPUThreadGuard& guard)
{
  if (!s_rb.vtime_on)
    return;  // outside the session: run the real peek
  static const u32 value = ParseU32(Env("NSR_RB_PEEKZ")).value_or(0);
  auto& ppc = guard.GetSystem().GetPPCState();
  ppc.gpr[3] = value;
  ppc.npc = LR(ppc);
}

void PresentHook(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  auto& ppc = guard.GetSystem().GetPPCState();
  static const bool async_present = !Env("NSR_RB_ASYNC_PRESENT").empty();  // old behavior
  if (s_hook_pc == VI_POST_COUNTER_FN)
  {
    if (!async_present && r.vtime_on)
      ppc.npc = LR(ppc);  // counted per frame in RollbackFrameEndHook
    return;
  }
  if (s_hook_pc == GX_ENABLE_BREAKPT)
  {
    if (!async_present && r.frame_mode && r.in_unit && r.vtime_on &&
        LR(ppc) == PRESENT_BREAKPT_RETURN)
      ppc.npc = r.in_resim ? LR(ppc) : VI_WAIT_FOR_RETRACE;
    return;
  }
  // Diagnostics: NSR_RB_RESIM_VSYNC=1 keeps the vsync wait in re-simulated
  // frames (each then takes a live frame's emulated time).
  static const bool keep_vsync = !Env("NSR_RB_RESIM_VSYNC").empty();
  if (r.frame_mode && r.in_unit && r.in_resim && !(keep_vsync && s_hook_pc == VI_WAIT_FOR_RETRACE))
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
  if (!r.vtime_on || !r.in_unit || LR(ppc) >= 0x80230000)  // SDK callers: real time
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
  // NSR_RB_AUDIO_GATE=1 gates the sound-start path only (0x8026AAF0, called
  // from fn_801F4050); =addr,... gates exactly those entries. Gating every
  // entry crashes (some return pointers or fill out-parameters in game
  // memory that a skipped call leaves unwritten).
  static const std::vector<unsigned int> only = [] {
    std::vector<unsigned int> v;
    const std::string spec = Env("NSR_RB_AUDIO_GATE");
    if (spec == "1")
      return std::vector<unsigned int>{0x8026AAF0};
    for (const std::string& a : Split(spec, ','))
      if (const auto x = ParseU32(a))
        v.push_back(*x);
    return v;
  }();
  static const std::vector<unsigned int> effects = EffectGateList();
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
  static FILE* f = [] {
    const std::string path = Env("NSR_AC_LOG");
    return path.empty() ? nullptr : std::fopen(path.c_str(), "w");
  }();
  return f;
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
  return r.vtime_on && r.in_unit && LR(ppc) < 0x8025E000 &&
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
  if (!f || !s_rb.vtime_on)
    return;
  std::fprintf(f, "F %u %d %08x %08x\n", s_rb.tick_running, s_rb.in_resim ? 1 : 0,
               PowerPC::MMU::HostRead<u32>(guard, 0x80481888),
               PowerPC::MMU::HostRead<u32>(guard, 0x80481904));
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

enum class AcMode
{
  Off,
  On,
  Shadow
};
AcMode AcModeNow()
{
  static const AcMode mode = [] {
    const std::string v = Env("NSR_AC", "1");
    return v == "0" ? AcMode::Off : v == "shadow" ? AcMode::Shadow : AcMode::On;
  }();
  return mode;
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

u32 AcFramesToMs(u32 frames)
{
  return static_cast<u32>(u64(frames) * 1000 / 60);
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
  static const u32 frames = ParseU32(Env("NSR_AC_SPEECH_FRAMES")).value_or(75);
  return std::max<u32>(1, frames);
}

void AcSpeechQueue(const AcMem& m, u32 s, u32 vid, u32 frames, u32 f)
{
  static const u32 lead = ParseU32(Env("NSR_AC_SPEECH_LEAD")).value_or(0);
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
  static const u32 fallback = ParseU32(Env("NSR_AC_SFX_DEFAULT")).value_or(30);
  static const u32 tail = ParseU32(Env("NSR_AC_SFX_TAIL")).value_or(2);
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
  static const u32 wait = ParseU32(Env("NSR_AC_BANK_FRAMES")).value_or(6);
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
}  // namespace

void AudioClockSessionStart(const Core::CPUThreadGuard& guard)
{
  if (AcModeNow() == AcMode::Off)
    return;
  const AcMem m{guard};
  for (u32 a = AC_BASE; a < AC_END; a += 4)
    m.wr(a, 0);
  m.wr(AC_BASE, AC_MAGIC);
  s_ac_real.clear();
  Log("audio clock: model at {:08x}..{:08x} ({})", AC_BASE, AC_END,
      AcModeNow() == AcMode::Shadow ? "shadow" : "on");
}

// Queries (HookType::Gate): answer from the model and skip the wrapper.
void AudioClockQueryHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  const u32 pc = s_hook_pc;
  if (!AcActive(guard, ppc))
    return;
  const AcMem m{guard};
  const u32 f = s_rb.tick_running;
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
  if (AcModeNow() == AcMode::Off || !r.vtime_on || !r.in_unit ||
      PowerPC::MMU::HostRead<u32>(guard, 0x800000E4) != 0x803A3F30 ||
      PowerPC::MMU::HostRead<u32>(guard, AC_BASE) != AC_MAGIC)
    return;
  if (!return_site && LR(ppc) >= 0x8025E000)
    return;
  const AcMem m{guard};
  const u32 f = r.tick_running;
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
  const std::string spec = Env("NSR_RB_SKIP_RESIM");
  if (spec.empty())
    return {};
  std::vector<unsigned int> out;
  for (const std::string& a : Split(spec, ','))
    if (const auto v = ParseU32(a))
      out.push_back(*v);
  return out;
}

void LoadBarrierHook(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  if (!r.initialized || r.io_idle_mode)
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
  std::vector<unsigned int> out;
  for (const std::string& a : Split(Env("NSR_RB_EFFECT_GATE"), ','))
    if (const auto v = ParseU32(a))
      out.push_back(*v);
  return out;
}

std::vector<unsigned int> ProbeAddresses()
{
  std::vector<unsigned int> out;
  for (const std::string& a : Split(Env("NSR_RB_PROBES"), ','))
  {
    if (const auto v = ParseU32(a))
      out.push_back(*v);
  }
  return out;
}

void ProbeHook(const Core::CPUThreadGuard& guard)
{
  Rollback& r = s_rb;
  if (!r.watch || r.live < r.watch_from || r.live > r.watch_to)
    return;
  static u32 last_value = 0, last_pc = 0, last_lr = 0;
  auto& ppc = guard.GetSystem().GetPPCState();
  const u32 v = PowerPC::MMU::HostRead<u32>(guard, r.watch);
  static const bool all = !Env("NSR_RB_PROBE_ALL").empty();
  if (all)
    Log("probe hit {:08x} from {:08x}: [{:08x}] = {:08x}; live {} cursor {} resim {} r3 {:08x} r4 {:08x}",
        s_hook_pc, LR(ppc) - 4, r.watch, v, r.live, r.cursor, r.in_resim, ppc.gpr[3], ppc.gpr[4]);
  if (v != last_value)
    Log("probe: [{:08x}] {:08x} -> {:08x} after {:08x} (called from {:08x}), before {:08x} "
        "(called from {:08x}); live {} cursor {}",
        r.watch, last_value, v, last_pc, last_lr - 4, s_hook_pc, LR(ppc) - 4, r.live, r.cursor);
  last_value = v;
  last_pc = s_hook_pc;
  last_lr = LR(ppc);
}

void Shutdown()
{
  Session& s = s_session;
  if (s_rb.initialized && s_rb.force > 0)
    RbReport(40);
  if (s.host)
  {
    for (ENetPeer* peer : s.peers)
    {
      if (peer)
        enet_peer_disconnect_now(peer, 0);
    }
    enet_host_destroy(s.host);
  }
  if (s.log && s.log != stderr)
    std::fclose(s.log);
  s = Session{};
  s_rb = Rollback{};
  s_io_ranges.clear();
}
}  // namespace NSRNetplay
