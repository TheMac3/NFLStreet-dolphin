// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE.h"

#include <algorithm>
#include <cstdlib>
#include <array>
#include <map>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HLE/HLE_Misc.h"
#include "Core/HLE/HLE_OS.h"
#include "Core/NSR/NSRNetplay.h"
#include "Core/NSR/NSRContent.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/IOS/ES/ES.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace HLE
{
// Map addresses to the HLE hook index
static std::map<u32, u32> s_hooked_addresses;

// clang-format off
constexpr std::array<Hook, 60> os_patches{{
    // Placeholder, os_patches[0] is the "non-existent function" index
    {"FAKE_TO_SKIP_0",               HLE_Misc::UnimplementedFunction,       HookType::Replace, HookFlag::Generic},

    // Name doesn't matter, installed in CBoot::BootUp()
    {"HBReload",                     HLE_Misc::HBReload,                    HookType::Replace, HookFlag::Fixed},

    // Debug/OS Support
    {"OSPanic",                      HLE_OS::HLE_OSPanic,                   HookType::Replace, HookFlag::Debug},

    // This needs to be put before vprintf (because vprintf is called indirectly by this)
    {"JUTWarningConsole_f",          HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},

    {"OSReport",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"DEBUGPrint",                   HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"WUD_DEBUGPrint",               HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"__DSP_debug_printf",           HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"vprintf",                      HLE_OS::HLE_GeneralDebugVPrint,        HookType::Start,   HookFlag::Debug},
    {"printf",                       HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"vdprintf",                     HLE_OS::HLE_LogVDPrint,                HookType::Start,   HookFlag::Debug},
    {"dprintf",                      HLE_OS::HLE_LogDPrint,                 HookType::Start,   HookFlag::Debug},
    {"vfprintf",                     HLE_OS::HLE_LogVFPrint,                HookType::Start,   HookFlag::Debug},
    {"fprintf",                      HLE_OS::HLE_LogFPrint,                 HookType::Start,   HookFlag::Debug},
    {"nlPrintf",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"DWC_Printf",                   HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"RANK_Printf",                  HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"puts",                         HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug}, // gcc-optimized printf?
    {"___blank",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug}, // used for early init things (normally)
    {"__write_console",              HLE_OS::HLE_write_console,             HookType::Start,   HookFlag::Debug}, // used by sysmenu (+more?)

    {"GeckoCodehandler",             HLE_Misc::GeckoCodeHandlerICacheFlush, HookType::Start,   HookFlag::Fixed},
    {"GeckoHandlerReturnTrampoline", HLE_Misc::GeckoReturnTrampoline,       HookType::Replace, HookFlag::Fixed},
    {"AppLoaderReport",              HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Fixed}, // apploader needs OSReport-like function
    {"NSR_Netplay",                  NSRNetplay::Hook,                      HookType::Replace, HookFlag::Fixed}, // NFLStreet POC lockstep input exchange
    {"NSR_RollbackPoll",             NSRNetplay::RollbackPollHook,          HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: pad poll
    {"NSR_RollbackTick",             NSRNetplay::RollbackTickHook,          HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: sim tick
    {"NSR_RollbackTickEnd",          NSRNetplay::RollbackTickEndHook,       HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: after sim tick
    {"NSR_RollbackTime",             NSRNetplay::RollbackTimeHook,          HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: game mftb
    {"NSR_OSGetTime",                NSRNetplay::OSGetTimeHook,             HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: tick time
    {"NSR_OSGetTick",                NSRNetplay::OSGetTickHook,             HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: tick time
    {"NSR_Probe",                    NSRNetplay::ProbeHook,                 HookType::Start,   HookFlag::Fixed}, // NFLStreet POC diagnostics
    {"NSR_RollbackFrame",            NSRNetplay::RollbackFrameHook,         HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: frame start
    {"NSR_RollbackFrameEnd",         NSRNetplay::RollbackFrameEndHook,      HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: frame end
    {"NSR_InGame",                   NSRNetplay::InGameHook,                 HookType::Start,   HookFlag::Fixed}, // NFLStreet POC rollback: match lifecycle
    {"NSR_PlaybookMenu",             NSRNetplay::PlaybookMenuHook,         HookType::Replace, HookFlag::Fixed}, // existing-play side/pad ordering
    {"NSR_SplitVelocity",            NSRContent::VelocityHook,             HookType::Replace, HookFlag::Fixed}, // opt-in normal-pass velocity
    {"NSR_SplitAccelFirst",          NSRContent::AccelerationFirstHook,    HookType::Replace, HookFlag::Fixed}, // opt-in acceleration consumer
    {"NSR_SplitAccelSecond",         NSRContent::AccelerationSecondHook,   HookType::Replace, HookFlag::Fixed}, // opt-in acceleration consumer
    {"NSR_ContentDatabase",          NSRNetplay::ContentDatabaseHook,      HookType::Start,   HookFlag::Fixed}, // native database registration complete
    {"NSR_GameStatsPlayResult",       NSRNetplay::GameStatsPlayResultHook,    HookType::Start,   HookFlag::Fixed}, // passive native play-result observation
    {"NSR_GameStatsEvent",            NSRNetplay::GameStatsEventHook,          HookType::Start,   HookFlag::Fixed}, // passive native play-event observation
    {"NSR_GameStatsMvp",             NSRNetplay::GameStatsMvpHook,           HookType::Start,   HookFlag::Fixed}, // passive native MVP observation
    {"NSR_GameStatsTotals",          NSRNetplay::GameStatsTotalsHook,        HookType::Start,   HookFlag::Fixed}, // passive native totals observation
    {"NSR_TickCount",                NSRNetplay::TickCountHook,             HookType::Gate,    HookFlag::Fixed}, // NFLStreet POC rollback: one tick per frame
    {"NSR_Present",                  NSRNetplay::PresentHook,               HookType::Gate,    HookFlag::Fixed}, // NFLStreet POC rollback: no vsync wait in re-sim
    {"NSR_PeekZ",                    NSRNetplay::PeekZHook,                 HookType::Gate,    HookFlag::Fixed}, // NFLStreet POC rollback: GPU-independent depth peek
    {"NSR_ViCallback",               NSRNetplay::ViCallbackHook,            HookType::Gate,    HookFlag::Fixed}, // NFLStreet POC rollback: retrace counters per frame
    {"NSR_LoadBarrier",              NSRNetplay::LoadBarrierHook,           HookType::Start,   HookFlag::Fixed}, // NFLStreet POC rollback: disc load barrier
    {"NSR_AudioGate",                NSRNetplay::AudioGateHook,             HookType::Gate,    HookFlag::Fixed}, // NFLStreet POC rollback: audio side effects
    {"NSR_AudioGateReturn",          NSRNetplay::AudioGateReturnHook,       HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: record audio results
    {"NSR_AudioCallLog",             NSRNetplay::AudioCallLogHook,          HookType::Start,   HookFlag::Fixed}, // NFLStreet POC rollback: audio clock diagnostics
    {"NSR_AudioCallReturn",          NSRNetplay::AudioCallReturnHook,       HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: audio clock diagnostics
    {"NSR_AudioClockQuery",          NSRNetplay::AudioClockQueryHook,       HookType::Gate,    HookFlag::Fixed}, // NFLStreet POC rollback: frame-based audio answers
    {"NSR_AudioClockEffect",         NSRNetplay::AudioClockEffectHook,      HookType::Start,   HookFlag::Fixed}, // NFLStreet POC rollback: frame-based audio bookkeeping
    {"NSR_MusicDispatch",            NSRNetplay::MusicDispatchHook,         HookType::Gate,    HookFlag::Fixed}, // NFLStreet POC rollback: music events on frames
    {"NSR_MusicPick",                NSRNetplay::MusicPickHook,             HookType::Start,   HookFlag::Fixed}, // NFLStreet POC rollback: music events on frames
    {"NSR_MusicTramp",               NSRNetplay::MusicTrampHook,            HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: music events on frames
    {"NSR_SimWaitTramp",             NSRNetplay::SimWaitTrampHook,          HookType::Replace, HookFlag::Fixed}, // NFLStreet POC rollback: retrace waits inside the sim
    {"NSR_Alloc",                    NSRNetplay::AllocHook,                 HookType::Start,   HookFlag::Fixed}, // NFLStreet POC rollback: per-machine dump sidecar
    {"NSR_Free",                     NSRNetplay::FreeHook,                  HookType::Start,   HookFlag::Fixed}, // NFLStreet POC rollback: per-machine dump sidecar
}};
// clang-format on

void Patch(Core::System& system, u32 addr, std::string_view func_name)
{
  auto& ppc_state = system.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();
  for (u32 i = 1; i < os_patches.size(); ++i)
  {
    if (os_patches[i].name == func_name)
    {
      s_hooked_addresses[addr] = i;
      ppc_state.iCache.Invalidate(memory, jit_interface, addr);
      Host_JitCacheInvalidation();
      return;
    }
  }
}

void PatchFixedFunctions(Core::System& system)
{
  // MIOS puts patch data in low MEM1 (0x1800-0x3000) for its own use.
  // Overwriting data in this range can cause the IPL to crash when launching games
  // that get patched by MIOS. See https://bugs.dolphin-emu.org/issues/11952 for more info.
  // Not applying the Gecko HLE patches means that Gecko codes will not work under MIOS,
  // but this is better than the alternative of having specific games crash.
  if (system.IsMIOS())
    return;

  // HLE jump to loader (homebrew).  Disabled when Gecko is active as it interferes with the code
  // handler
  if (!Config::AreCheatsEnabled())
  {
    Patch(system, 0x80001800, "HBReload");
    auto& memory = system.GetMemory();
    memory.CopyToEmu(0x00001804, "STUBHAXX", 8);
  }

  // Not part of the binary itself, but either we or Gecko OS might insert
  // this, and it doesn't clear the icache properly.
  Patch(system, Gecko::ENTRY_POINT, "GeckoCodehandler");
  // This has to always be installed even if cheats are not enabled because of the possiblity of
  // loading a savestate where PC is inside the code handler while cheats are disabled.
  Patch(system, Gecko::HLE_TRAMPOLINE_ADDRESS, "GeckoHandlerReturnTrampoline");

  // NFLStreet POC: the netplay stub in the patched NFL Street 2 image. Only
  // installed when NSR_NETPLAY is set; otherwise the stub's `blr` runs.
  if (NSRNetplay::IsEnabled())
    Patch(system, NSRNetplay::STUB_ADDRESS, "NSR_Netplay");
  // Rollback images (isopatch `rollback`) route every pad poll and sim tick
  // through these stubs, so they're always installed for NFL Street 2. In
  // other images that RAM is unused data that never runs.
  if (SConfig::GetInstance().GetGameID() == "GN7E69" &&
      SConfig::GetInstance().GetRevision() == 0 && NSRNetplay::ValidateRollbackImage(system))
  {
    if (system.GetMemory().Read_U32(0x80057d8c) == 0x7f23cb78)
      Patch(system, 0x80057d8c, "NSR_PlaybookMenu");
    // GN7E69 rev 0: immediately after fn_801F9134 registers a loaded DB.
    if (system.GetMemory().Read_U32(0x8020d334) == 0x7c7e1b78)
      Patch(system, 0x8020d334, "NSR_ContentDatabase");
    Patch(system, NSRNetplay::RB_POLL_STUB, "NSR_RollbackPoll");
    Patch(system, NSRNetplay::RB_TICK_STUB, "NSR_RollbackTick");
    Patch(system, NSRNetplay::RB_TICK_END_STUB, "NSR_RollbackTickEnd");
    Patch(system, NSRNetplay::RB_TIME_STUB, "NSR_RollbackTime");
    Patch(system, NSRNetplay::RB_FRAME_STUB, "NSR_RollbackFrame");
    Patch(system, NSRNetplay::RB_FRAME_END_STUB, "NSR_RollbackFrameEnd");
    // GN7E69 rev 0 only: passive native event/result readers. Start preserves
    // each original instruction and the CPU state consumed by native code.
    if (system.GetMemory().Read_U32(0x800b1540) == 0x7c7e1b78)
      Patch(system, 0x800b1540, "NSR_GameStatsEvent");
    if (system.GetMemory().Read_U32(0x8017bd70) == 0x9421ffb0)
      Patch(system, 0x8017bd70, "NSR_GameStatsPlayResult");
    // After MVP selection/stat calculation, before closing its DB cursor.
    if (system.GetMemory().Read_U32(0x8005ae44) == 0x4bfffe85)
      Patch(system, 0x8005ae44, "NSR_GameStatsMvp");
    if (system.GetMemory().Read_U32(0x80055f4c) == 0x9421ff80)
      Patch(system, 0x80055f4c, "NSR_GameStatsTotals");
    Patch(system, NSRNetplay::IN_GAME_FN, "NSR_InGame");
    Patch(system, NSRNetplay::TICK_COUNT_FN, "NSR_TickCount");
    Patch(system, NSRNetplay::VI_WAIT_FOR_RETRACE, "NSR_Present");
    // Present paces with a GX FIFO breakpoint released by the next VI post
    // callback; its counter is game state (see PresentHook).
    Patch(system, NSRNetplay::GX_ENABLE_BREAKPT, "NSR_Present");
    Patch(system, NSRNetplay::VI_POST_COUNTER_FN, "NSR_Present");
    Patch(system, NSRNetplay::PEEK_Z_FN, "NSR_PeekZ");
    if (system.GetMemory().Read_U32(NSRNetplay::STREAM_START_WAIT_FN) ==
        NSRNetplay::STREAM_START_WAIT_FIRST)
      Patch(system, NSRNetplay::STREAM_START_WAIT_FN, "NSR_Present");
    Patch(system, NSRNetplay::VI_PRE_CB, "NSR_ViCallback");
    // Functions skipped in re-simulated frames: NSR_RB_SKIP_RESIM=addr,...
    // (default: none, re-simulated frames draw fully); NSR_RB_RENDER_RESIM forces none.
    if (!std::getenv("NSR_RB_RENDER_RESIM"))
    {
      for (const u32 f : NSRNetplay::ResimSkipList())
        Patch(system, f, "NSR_Present");
    }
    // Side-effect gate for other game->subsystem calls (e.g. disc load
    // requests): NSR_RB_EFFECT_GATE=addr,... (same record/replay as audio).
    for (const u32 entry : NSRNetplay::EffectGateList())
      Patch(system, entry, "NSR_AudioGate");
    Patch(system, NSRNetplay::LOAD_START_FN, "NSR_LoadBarrier");
    Patch(system, NSRNetplay::LOAD_DONE_FN, "NSR_LoadBarrier");
    // Gated calls (audio or effect) return through this stub: always install.
    Patch(system, NSRNetplay::RB_GATE_RETURN, "NSR_AudioGateReturn");
    for (const u32 entry : NSRNetplay::AudioGateList())
      Patch(system, entry, "NSR_AudioGate");
    // Audio clock: the sim's view of audio follows frames (always installed;
    // inactive outside the rollback session; NSR_AC=0 turns it off).
    for (const u32 a : NSRNetplay::AudioClockQueryHooks())
      Patch(system, a, "NSR_AudioClockQuery");
    for (const u32 a : NSRNetplay::AudioClockEffectHooks())
      Patch(system, a, "NSR_AudioClockEffect");
    for (const u32 a : NSRNetplay::MusicDispatchers())
      Patch(system, a, "NSR_MusicDispatch");
    Patch(system, NSRNetplay::MUSIC_PICK_SONG_FN, "NSR_MusicPick");
    Patch(system, NSRNetplay::MUSIC_TRAMP_ADDR, "NSR_MusicTramp");
    Patch(system, NSRNetplay::SIM_WAIT_TRAMP_ADDR, "NSR_SimWaitTramp");
    // Allocator watch for the per-machine dump sidecar (always on: audio
    // blocks are allocated at load time, long before any dump).
    for (const u32 a : NSRNetplay::AllocHookAddresses())
      Patch(system, a, "NSR_Alloc");
    Patch(system, NSRNetplay::FREE_FN_ADDR, "NSR_Free");
    if (std::getenv("NSR_AC_CALLS") || std::getenv("NSR_AC_TRACE"))  // audio clock diagnostics
    {
      if (std::getenv("NSR_AC_CALLS"))  // log game->audio calls
      {
        for (const u32 entry : NSRNetplay::AUDIO_GATE_ENTRIES)
          Patch(system, entry, "NSR_AudioCallLog");
      }
      for (const u32 entry : NSRNetplay::AudioTraceList())
        Patch(system, entry, "NSR_AudioCallLog");
      Patch(system, NSRNetplay::AC_RETURN_STUB, "NSR_AudioCallReturn");
      system.GetMemory().Write_U32(0x4E800020, NSRNetplay::AC_RETURN_STUB);  // blr
    }
    for (const u32 probe : NSRNetplay::ProbeAddresses())  // (diagnostics)
      Patch(system, probe, "NSR_Probe");
    if (!std::getenv("NSR_RB_NO_TIME_HOOKS"))  // (diagnostics)
    {
      Patch(system, NSRNetplay::OS_GET_TIME, "NSR_OSGetTime");
      Patch(system, NSRNetplay::OS_GET_TICK, "NSR_OSGetTick");
    }
  }
}

void PatchFunctions(Core::System& system)
{
  auto& power_pc = system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();
  auto& ppc_symbol_db = power_pc.GetSymbolDB();

  // Remove all hooks that aren't fixed address hooks
  for (auto i = s_hooked_addresses.begin(); i != s_hooked_addresses.end();)
  {
    if (os_patches[i->second].flags != HookFlag::Fixed)
    {
      ppc_state.iCache.Invalidate(memory, jit_interface, i->first);
      i = s_hooked_addresses.erase(i);
    }
    else
    {
      ++i;
    }
  }

  for (u32 i = 1; i < os_patches.size(); ++i)
  {
    // Fixed hooks don't map to symbols
    if (os_patches[i].flags == HookFlag::Fixed)
      continue;

    for (const auto& symbol : ppc_symbol_db.GetSymbolsFromName(os_patches[i].name))
    {
      for (u32 addr = symbol->address; addr < symbol->address + symbol->size; addr += 4)
      {
        s_hooked_addresses[addr] = i;
        ppc_state.iCache.Invalidate(memory, jit_interface, addr);
      }
      INFO_LOG_FMT(OSHLE, "Patching {} {:08x}", os_patches[i].name, symbol->address);
    }
  }

  Host_JitCacheInvalidation();
}

void Clear()
{
  s_hooked_addresses.clear();
}

void Reload(Core::System& system)
{
  Clear();
  PatchFixedFunctions(system);
  PatchFunctions(system);
}

void Execute(const Core::CPUThreadGuard& guard, u32 current_pc, u32 hook_index)
{
  hook_index &= 0xFFFFF;
  if (hook_index > 0 && hook_index < os_patches.size())
  {
    NSRNetplay::SetHookPc(current_pc);  // NFLStreet probes: which address fired
    os_patches[hook_index].function(guard);
  }
  else
  {
    PanicAlertFmt("HLE system tried to call an undefined HLE function {}.", hook_index);
  }
}

void ExecuteFromJIT(u32 current_pc, u32 hook_index, Core::System& system)
{
  ASSERT(Core::IsCPUThread());
  Core::CPUThreadGuard guard(system);
  Execute(guard, current_pc, hook_index);
}

u32 GetHookByAddress(u32 address)
{
  auto iter = s_hooked_addresses.find(address);
  return (iter != s_hooked_addresses.end()) ? iter->second : 0;
}

u32 GetHookByFunctionAddress(PPCSymbolDB& ppc_symbol_db, u32 address)
{
  const u32 index = GetHookByAddress(address);
  // Fixed hooks use a fixed address and don't patch the whole function
  if (index == 0 || os_patches[index].flags == HookFlag::Fixed)
    return index;

  const Common::Symbol* const symbol = ppc_symbol_db.GetSymbolFromAddr(address);
  return (symbol && symbol->address == address) ? index : 0;
}

const char* GetHookNameByIndex(u32 index)
{
  return os_patches[index].name;
}

HookType GetHookTypeByIndex(u32 index)
{
  return os_patches[index].type;
}

HookFlag GetHookFlagsByIndex(u32 index)
{
  return os_patches[index].flags;
}

TryReplaceFunctionResult TryReplaceFunction(PPCSymbolDB& ppc_symbol_db, u32 address,
                                            PowerPC::CoreMode mode)
{
  const u32 hook_index = GetHookByFunctionAddress(ppc_symbol_db, address);
  if (hook_index == 0)
    return {};

  const HookType type = GetHookTypeByIndex(hook_index);
  if (type != HookType::Start && type != HookType::Replace && type != HookType::Gate)
    return {};

  const HookFlag flags = GetHookFlagsByIndex(hook_index);
  if (!IsEnabled(flags, mode))
    return {};

  return {type, hook_index};
}

bool IsEnabled(HookFlag flag, PowerPC::CoreMode mode)
{
  return flag != HLE::HookFlag::Debug || Config::IsDebuggingEnabled() ||
         mode == PowerPC::CoreMode::Interpreter;
}

u32 UnPatch(Core::System& system, std::string_view patch_name)
{
  const auto patch = std::ranges::find(os_patches, patch_name, &Hook::name);
  if (patch == std::end(os_patches))
    return 0;

  auto& power_pc = system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();

  if (patch->flags == HookFlag::Fixed)
  {
    const u32 patch_idx = static_cast<u32>(std::distance(os_patches.begin(), patch));
    u32 addr = 0;
    // Reverse search by OSPatch key instead of address
    for (auto i = s_hooked_addresses.begin(); i != s_hooked_addresses.end();)
    {
      if (i->second == patch_idx)
      {
        addr = i->first;
        ppc_state.iCache.Invalidate(memory, jit_interface, i->first);
        i = s_hooked_addresses.erase(i);
      }
      else
      {
        ++i;
      }
    }
    Host_JitCacheInvalidation();
    return addr;
  }

  const auto symbols = power_pc.GetSymbolDB().GetSymbolsFromName(patch_name);
  if (!symbols.empty())
  {
    const Common::Symbol* const symbol = symbols.front();
    for (u32 addr = symbol->address; addr < symbol->address + symbol->size; addr += 4)
    {
      s_hooked_addresses.erase(addr);
      ppc_state.iCache.Invalidate(memory, jit_interface, addr);
    }
    Host_JitCacheInvalidation();
    return symbol->address;
  }

  return 0;
}

u32 UnpatchRange(Core::System& system, u32 start_addr, u32 end_addr)
{
  auto& ppc_state = system.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();

  u32 count = 0;

  auto i = s_hooked_addresses.lower_bound(start_addr);
  while (i != s_hooked_addresses.end() && i->first < end_addr)
  {
    INFO_LOG_FMT(OSHLE, "Unpatch HLE hooks [{:08x};{:08x}): {} at {:08x}", start_addr, end_addr,
                 os_patches[i->second].name, i->first);
    ppc_state.iCache.Invalidate(memory, jit_interface, i->first);
    i = s_hooked_addresses.erase(i);
    count += 1;
  }
  Host_JitCacheInvalidation();

  return count;
}
}  // namespace HLE
