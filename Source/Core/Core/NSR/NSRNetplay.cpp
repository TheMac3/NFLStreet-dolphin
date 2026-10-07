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
#include <vector>

#include <enet/enet.h>
#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

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
  Log("player {} of {} delay={} port={} topology={}", s.me, s.players, s.delay, addr.port,
      s.star ? "star" : "mesh");
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
  Log("player {} of {} delay={} topology=star punch via rendezvous (local port {})", s.me,
      s.players, s.delay, local.port);

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
}  // namespace

bool IsEnabled()
{
  static const bool enabled = std::getenv("NSR_NETPLAY") != nullptr;
  return enabled;
}

void Hook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  const u32 buf = ppc_state.gpr[3];
  const u32 frame = ppc_state.gpr[4];
  ppc_state.npc = LR(ppc_state);  // Replace hook: return to caller

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

void Shutdown()
{
  Session& s = s_session;
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
}
}  // namespace NSRNetplay
