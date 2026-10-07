// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// See NSRNetplay.h for the design. This file holds:
//   - session setup over ENet (host listens, client connects),
//   - the per-retrace exchange (send local pad for frame f + delay with
//     redundancy, block until the peer's pad for frame f arrives),
//   - periodic whole-MEM1 hashes to detect desyncs.

#include "Core/NSR/NSRNetplay.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
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
constexpr u32 PAD_COUNT = 4;
constexpr u32 REDUNDANCY = 32;        // past pads repeated in every packet
constexpr u32 CHECKSUM_INTERVAL = 60; // frames between MEM1 hashes
constexpr u32 PACKET_MAGIC = 0x4E535250;  // 'NSRP'
constexpr int CONNECT_TIMEOUT_MS = 60000;
constexpr int PEER_TIMEOUT_MS = 10000;
// Our own patch area: script table, freeze/loop words. These legitimately
// differ between machines (e.g. test scripts), so leave them out of hashes.
// Also skip the SDK memory-card work area (CARDMount at fn_801D45DC uses
// 0x80384DE0 + chan * 0xA000, plus its control block just before): it holds
// per-mount card values that differ between machines but aren't game state.
constexpr std::array<std::pair<u32, u32>, 3> HASH_SKIP{{
    {0x00384C00, 0x00398E00},  // memory-card control block + work areas
    {0x003A69C0, 0x003A6A00},  // SDK raw SI controller input (pre-PADRead, local)
    {0x00411A00, 0x00412000},  // NFLStreet patch area (scripts, counters)
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
  u32 latest_frame;       // frame of pads[count - 1]
  u32 count;              // pads for frames latest-count+1 .. latest
  u32 checksum_frame;     // 0 = none
  u64 checksum;
  Pad pads[REDUNDANCY];
};

enum class Role
{
  Off,
  Host,
  Client
};

struct Session
{
  bool initialized = false;
  Role role = Role::Off;
  u32 delay = 2;
  FILE* log = stderr;
  ENetHost* host = nullptr;
  ENetPeer* peer = nullptr;
  bool connected = false;
  bool failed = false;

  std::map<u32, Pad> local_pads;   // by frame they apply to
  std::map<u32, Pad> remote_pads;
  std::map<u32, std::pair<Pad, Pad>> applied;  // frame -> (host pad, client pad)
  std::map<u32, u64> local_hashes;
  std::map<u32, u64> remote_hashes;
  u32 last_sent_hash_frame = 0;
  bool started = false;
  u32 first_frame = 0;   // frame of the first hook call (same on both sides)
  u32 next_target = 0;   // next frame index we haven't recorded a pad for
  u32 desyncs = 0;
  u32 last_desync_frame = 0;  // log each mismatched frame once
  // Diagnostics: MEM1 copies for recent checksum frames; on the first
  // mismatch, the copy for that frame is written to NSR_DUMP_PREFIX-<frame>.bin
  // on both machines so they can be diffed at the exact frame.
  std::map<u32, std::vector<u8>> snapshots;
  bool dumped = false;
};

Session s_session;

Role ReadRole()
{
  const char* v = std::getenv("NSR_NETPLAY");
  if (!v)
    return Role::Off;
  if (std::strcmp(v, "host") == 0)
    return Role::Host;
  if (std::strcmp(v, "client") == 0)
    return Role::Client;
  return Role::Off;
}

template <typename... Args>
void Log(fmt::format_string<Args...> format, Args&&... args)
{
  const std::string line = fmt::format(format, std::forward<Args>(args)...);
  std::fprintf(s_session.log, "[NSR] %s\n", line.c_str());
  std::fflush(s_session.log);
}

Pad NeutralPad()
{
  Pad p{};  // no buttons, centred sticks, no triggers, err = 0 (connected)
  return p;
}

u64 HashMem1(Core::System& system)
{
  // FNV-1a over 64-bit words of MEM1, skipping our patch area.
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

bool Setup()
{
  Session& s = s_session;
  s.initialized = true;
  s.role = ReadRole();
  if (const char* d = std::getenv("NSR_DELAY"))
    s.delay = static_cast<u32>(std::max(0, std::atoi(d)));
  if (const char* path = std::getenv("NSR_LOG"))
  {
    if (FILE* f = std::fopen(path, "w"))
      s.log = f;
  }
  if (enet_initialize() != 0)
  {
    Log("enet_initialize failed");
    return false;
  }

  ENetAddress addr{};
  addr.host = ENET_HOST_ANY;
  if (s.role == Role::Host)
  {
    const char* port = std::getenv("NSR_PORT");
    addr.port = static_cast<u16>(port ? std::atoi(port) : 51515);
    s.host = enet_host_create(&addr, 1, 1, 0, 0);
  }
  else
  {
    s.host = enet_host_create(nullptr, 1, 1, 0, 0);
    const char* peer = std::getenv("NSR_PEER");
    std::string p = peer ? peer : "127.0.0.1:51515";
    const auto colon = p.rfind(':');
    ENetAddress target{};
    enet_address_set_host(&target, p.substr(0, colon).c_str());
    target.port = static_cast<u16>(std::atoi(p.substr(colon + 1).c_str()));
    if (s.host)
      s.peer = enet_host_connect(s.host, &target, 1, 0);
  }
  if (!s.host)
  {
    Log("enet_host_create failed");
    return false;
  }
  Log("role={} delay={}", s.role == Role::Host ? "host" : "client", s.delay);
  return true;
}

void DumpSnapshot(u32 frame)
{
  Session& s = s_session;
  const char* prefix = std::getenv("NSR_DUMP_PREFIX");
  if (!prefix || s.dumped)
    return;
  auto it = s.snapshots.find(frame);
  if (it == s.snapshots.end())
    return;
  const std::string path = std::string(prefix) + "-" + std::to_string(frame) + ".bin";
  if (FILE* f = std::fopen(path.c_str(), "wb"))
  {
    std::fwrite(it->second.data(), 1, it->second.size(), f);
    std::fclose(f);
    s.dumped = true;
    Log("wrote {}", path);
  }
}

void HandlePacket(const ENetPacket* packet)
{
  Session& s = s_session;
  if (packet->dataLength < offsetof(Packet, pads))
    return;
  Packet p;
  std::memcpy(&p, packet->data, std::min(sizeof(Packet), packet->dataLength));
  if (p.magic != PACKET_MAGIC || p.count > REDUNDANCY)
    return;
  for (u32 i = 0; i < p.count; ++i)
  {
    const u32 frame = p.latest_frame - (p.count - 1 - i);
    s.remote_pads.try_emplace(frame, p.pads[i]);
  }
  if (p.checksum_frame)
  {
    s.remote_hashes[p.checksum_frame] = p.checksum;
    auto it = s.local_hashes.find(p.checksum_frame);
    if (it != s.local_hashes.end() && it->second != p.checksum &&
        p.checksum_frame != s.last_desync_frame)
    {
      s.last_desync_frame = p.checksum_frame;
      DumpSnapshot(p.checksum_frame);
      ++s.desyncs;
      Log("DESYNC at frame {}: local {:016x} remote {:016x}", p.checksum_frame,
          it->second, p.checksum);
    }
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
      s.peer = event.peer;
      s.connected = true;
      Log("connected");
      break;
    case ENET_EVENT_TYPE_RECEIVE:
      HandlePacket(event.packet);
      enet_packet_destroy(event.packet);
      break;
    case ENET_EVENT_TYPE_DISCONNECT:
      s.connected = false;
      s.failed = true;
      Log("peer disconnected");
      break;
    default:
      break;
    }
  }
}

void SendLatest(u32 latest_frame)
{
  Session& s = s_session;
  if (!s.connected || !s.peer)
    return;
  Packet p{};
  p.magic = PACKET_MAGIC;
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
  ENetPacket* packet = enet_packet_create(&p, size, ENET_PACKET_FLAG_UNSEQUENCED);
  enet_peer_send(s.peer, 0, packet);
  enet_host_flush(s.host);
}

bool WaitForConnection()
{
  Session& s = s_session;
  const auto start = std::chrono::steady_clock::now();
  while (!s.connected && !s.failed)
  {
    Service(10);
    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(CONNECT_TIMEOUT_MS))
    {
      Log("no peer within {} ms; giving up (playing offline)", CONNECT_TIMEOUT_MS);
      s.failed = true;
    }
  }
  return s.connected;
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

void WritePad(const Core::CPUThreadGuard& guard, u32 buf, u32 port, const Pad& pad)
{
  for (u32 i = 0; i < PAD_SIZE; ++i)
    PowerPC::MMU::HostWrite<u8>(guard, pad[i], buf + port * PAD_SIZE + i);
}
}  // namespace

bool IsEnabled()
{
  static const bool enabled = ReadRole() != Role::Off;
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
  if (!s.initialized && !Setup())
    s.failed = true;
  if (s.failed)
    return;
  if (!s.connected && !WaitForConnection())
    return;

  // The pad poll can run more than once for the same retrace (the game also
  // polls once directly at init). Re-apply what was agreed for that frame.
  if (auto it = s.applied.find(frame); it != s.applied.end())
  {
    WritePad(guard, buf, 0, it->second.first);
    WritePad(guard, buf, 1, it->second.second);
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
  // The first `delay` frames (from the first hook call, identical on both
  // sides) use neutral pads. Every target frame from the last recorded one up
  // to frame + delay gets a pad, so a retrace that skips the poll (e.g. during
  // loading) can't leave a hole the peer would wait on forever.
  if (!s.started)
  {
    s.started = true;
    s.first_frame = frame;
    s.next_target = frame;
    for (u32 f = frame; f < frame + s.delay; ++f)
      s.remote_pads.try_emplace(f, NeutralPad());
  }
  for (u32 t = s.next_target; t <= frame + s.delay; ++t)
    s.local_pads.try_emplace(t, t < s.first_frame + s.delay ? NeutralPad() : local);
  s.next_target = std::max(s.next_target, frame + s.delay + 1);

  // 2. Send, then block (host-side, no emulated time) for the peer's pad.
  SendLatest(frame + s.delay);
  const auto start = std::chrono::steady_clock::now();
  auto last_send = start;
  while (!s.remote_pads.contains(frame))
  {
    Service(1);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_send > std::chrono::milliseconds(8))
    {
      SendLatest(frame + s.delay);  // cover packet loss
      last_send = now;
    }
    if (s.failed || now - start > std::chrono::milliseconds(PEER_TIMEOUT_MS))
    {
      Log("peer silent at frame {}; continuing offline", frame);
      s.failed = true;
      return;
    }
  }

  // 3. Agree on (host pad, client pad) and write them to ports 0 and 1.
  const Pad mine = s.local_pads.at(frame);
  const Pad theirs = s.remote_pads.at(frame);
  const Pad host_pad = s.role == Role::Host ? mine : theirs;
  const Pad client_pad = s.role == Role::Host ? theirs : mine;
  s.applied.emplace(frame, std::make_pair(host_pad, client_pad));
  WritePad(guard, buf, 0, host_pad);
  WritePad(guard, buf, 1, client_pad);

  // 4. Periodic desync check: hash MEM1 after the agreed pads are in the
  //    buffer (same emulated instant on both; before that, the buffer still
  //    holds each side's own raw local pad).
  if (frame % CHECKSUM_INTERVAL == 0 && frame > 0)
  {
    s.local_hashes[frame] = HashMem1(system);
    if (std::getenv("NSR_DUMP_PREFIX") && !s.dumped)
    {
      auto& memory = system.GetMemory();
      s.snapshots[frame].assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSizeReal());
      while (s.snapshots.size() > 4)
        s.snapshots.erase(s.snapshots.begin());
    }
    s.last_sent_hash_frame = frame;
    if (auto it = s.remote_hashes.find(frame);
        it != s.remote_hashes.end() && it->second != s.local_hashes[frame] &&
        frame != s.last_desync_frame)
    {
      s.last_desync_frame = frame;
      DumpSnapshot(frame);
      ++s.desyncs;
      Log("DESYNC at frame {}", frame);
    }
  }


  // Trim history.
  if (frame > 600)
  {
    const u32 cutoff = frame - 600;
    std::erase_if(s.local_pads, [&](const auto& kv) { return kv.first < cutoff; });
    std::erase_if(s.remote_pads, [&](const auto& kv) { return kv.first < cutoff; });
    std::erase_if(s.applied, [&](const auto& kv) { return kv.first < cutoff; });
  }
  if (frame % 600 == 0)
    Log("frame {} ok (desyncs so far: {})", frame, s.desyncs);
}

void Shutdown()
{
  Session& s = s_session;
  if (s.host)
  {
    if (s.peer)
      enet_peer_disconnect_now(s.peer, 0);
    enet_host_destroy(s.host);
  }
  if (s.log && s.log != stderr)
    std::fclose(s.log);
  s = Session{};
}
}  // namespace NSRNetplay
