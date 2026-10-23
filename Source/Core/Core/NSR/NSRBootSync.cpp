// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// NSR's boot contract: a high-priority, nonpersistent config layer and fresh
// empty GCI cards. Mirrors ConfigLoaders/NetPlayConfigLoader.cpp, including its
// strict graphics settings. Revisit this list whenever upstream adds a synced
// setting. Host audio, rendering backend, textures and controller mapping stay local.
#include "Core/NSR/NSRBootSync.h"
#include "Core/NSR/NSRQuickstart.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Version.h"
#include "Core/NSR/NSRBootAgreement.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/Sram.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/NSR/NSRRollbackNetplay.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace NSRBootSync
{
namespace
{
std::string s_card_root;

// Hash every emulated byte of a newly formatted empty directory card, using the
// same Header/Directory/BAT constructors as GCMemcardDirectory. Unallocated data
// blocks read as 0xff. This is a contents digest, not a path or save-name hash.
static u64 EmptyCardDigest(const Memcard::HeaderData& header)
{
  const Memcard::Header block(header);
  Memcard::Directory directory;
  directory.FixChecksums();
  const Memcard::BlockAlloc bat(Memcard::MBIT_SIZE_MEMORY_CARD_1019);
  std::array<u8, Memcard::BLOCK_SIZE> erased;
  erased.fill(0xff);
  XXH3_state_t state{};
  XXH3_64bits_reset(&state);
  XXH3_64bits_update(&state, &block, sizeof(block));
  for (int i = 0; i < 2; ++i)
    XXH3_64bits_update(&state, &directory, sizeof(directory));
  for (int i = 0; i < 2; ++i)
    XXH3_64bits_update(&state, &bat, sizeof(bat));
  constexpr int blocks = Memcard::MBIT_SIZE_MEMORY_CARD_1019 * Memcard::MBIT_TO_BLOCKS;
  for (int i = Memcard::MC_FST_BLOCKS; i < blocks; ++i)
    XXH3_64bits_update(&state, erased.data(), erased.size());
  return XXH3_64bits_digest(&state);
}

// The same layer owns application and enumeration so the handshake cannot omit
// a canonicalized setting. Paths are deliberately excluded from the wire contract.
class BootLayer final : public Config::ConfigLayerLoader
{
public:
  BootLayer() : ConfigLayerLoader(Config::LayerType::Netplay) {}

  // Apply fixed values rather than copying any participant's local preferences.
  void Load(Config::Layer* layer) override
  {
    layer->Set(Config::MAIN_CPU_THREAD, Config::MAIN_CPU_THREAD.GetDefaultValue());
    layer->Set(Config::MAIN_CPU_CORE, Config::MAIN_CPU_CORE.GetDefaultValue());
    layer->Set(Config::MAIN_ENABLE_CHEATS, Config::MAIN_ENABLE_CHEATS.GetDefaultValue());
    layer->Set(Config::MAIN_GC_LANGUAGE, Config::MAIN_GC_LANGUAGE.GetDefaultValue());
    layer->Set(Config::MAIN_OVERRIDE_REGION_SETTINGS,
               Config::MAIN_OVERRIDE_REGION_SETTINGS.GetDefaultValue());
    layer->Set(Config::MAIN_DSP_HLE, Config::MAIN_DSP_HLE.GetDefaultValue());
    layer->Set(Config::MAIN_OVERCLOCK_ENABLE, Config::MAIN_OVERCLOCK_ENABLE.GetDefaultValue());
    layer->Set(Config::MAIN_OVERCLOCK, Config::MAIN_OVERCLOCK.GetDefaultValue());
    layer->Set(Config::MAIN_VI_OVERCLOCK_ENABLE,
               Config::MAIN_VI_OVERCLOCK_ENABLE.GetDefaultValue());
    layer->Set(Config::MAIN_VI_OVERCLOCK, Config::MAIN_VI_OVERCLOCK.GetDefaultValue());
    layer->Set(Config::MAIN_MEMORY_CARD_SIZE, Config::MAIN_MEMORY_CARD_SIZE.GetDefaultValue());
    layer->Set(Config::SESSION_SAVE_DATA_WRITABLE,
               Config::SESSION_SAVE_DATA_WRITABLE.GetDefaultValue());
    layer->Set(Config::MAIN_RAM_OVERRIDE_ENABLE,
               Config::MAIN_RAM_OVERRIDE_ENABLE.GetDefaultValue());
    layer->Set(Config::MAIN_MEM1_SIZE, Config::MAIN_MEM1_SIZE.GetDefaultValue());
    layer->Set(Config::MAIN_MEM2_SIZE, Config::MAIN_MEM2_SIZE.GetDefaultValue());
    layer->Set(Config::MAIN_FALLBACK_REGION, Config::MAIN_FALLBACK_REGION.GetDefaultValue());
    layer->Set(Config::MAIN_ALLOW_SD_WRITES, Config::MAIN_ALLOW_SD_WRITES.GetDefaultValue());
    layer->Set(Config::MAIN_DSP_JIT, Config::MAIN_DSP_JIT.GetDefaultValue());
    layer->Set(Config::GFX_HACK_EFB_ACCESS_ENABLE,
               Config::GFX_HACK_EFB_ACCESS_ENABLE.GetDefaultValue());
    layer->Set(Config::GFX_HACK_BBOX_ENABLE, Config::GFX_HACK_BBOX_ENABLE.GetDefaultValue());
    layer->Set(Config::GFX_HACK_FORCE_PROGRESSIVE,
               Config::GFX_HACK_FORCE_PROGRESSIVE.GetDefaultValue());
    layer->Set(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM,
               Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM.GetDefaultValue());
    layer->Set(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM,
               Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM.GetDefaultValue());
    layer->Set(Config::GFX_HACK_DISABLE_COPY_TO_VRAM,
               Config::GFX_HACK_DISABLE_COPY_TO_VRAM.GetDefaultValue());
    layer->Set(Config::GFX_HACK_IMMEDIATE_XFB, Config::GFX_HACK_IMMEDIATE_XFB.GetDefaultValue());
    layer->Set(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES,
               Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES.GetDefaultValue());
    layer->Set(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES,
               Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES.GetDefaultValue());
    layer->Set(Config::GFX_PERF_QUERIES_ENABLE, Config::GFX_PERF_QUERIES_ENABLE.GetDefaultValue());
    layer->Set(Config::MAIN_FLOAT_EXCEPTIONS, Config::MAIN_FLOAT_EXCEPTIONS.GetDefaultValue());
    layer->Set(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS,
               Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS.GetDefaultValue());
    layer->Set(Config::MAIN_FPRF, Config::MAIN_FPRF.GetDefaultValue());
    layer->Set(Config::MAIN_ACCURATE_NANS, Config::MAIN_ACCURATE_NANS.GetDefaultValue());
    layer->Set(Config::MAIN_DISABLE_ICACHE, Config::MAIN_DISABLE_ICACHE.GetDefaultValue());
    layer->Set(Config::MAIN_SYNC_ON_SKIP_IDLE, Config::MAIN_SYNC_ON_SKIP_IDLE.GetDefaultValue());
    layer->Set(Config::MAIN_SYNC_GPU, Config::MAIN_SYNC_GPU.GetDefaultValue());
    layer->Set(Config::MAIN_SYNC_GPU_MAX_DISTANCE,
               Config::MAIN_SYNC_GPU_MAX_DISTANCE.GetDefaultValue());
    layer->Set(Config::MAIN_SYNC_GPU_MIN_DISTANCE,
               Config::MAIN_SYNC_GPU_MIN_DISTANCE.GetDefaultValue());
    layer->Set(Config::MAIN_SYNC_GPU_OVERCLOCK, Config::MAIN_SYNC_GPU_OVERCLOCK.GetDefaultValue());
    layer->Set(Config::MAIN_JIT_FOLLOW_BRANCH, Config::MAIN_JIT_FOLLOW_BRANCH.GetDefaultValue());
    layer->Set(Config::MAIN_FAST_DISC_SPEED, Config::MAIN_FAST_DISC_SPEED.GetDefaultValue());
    layer->Set(Config::MAIN_MMU, Config::MAIN_MMU.GetDefaultValue());
    layer->Set(Config::MAIN_FASTMEM, Config::MAIN_FASTMEM.GetDefaultValue());
    layer->Set(Config::MAIN_SKIP_IPL, Config::MAIN_SKIP_IPL.GetDefaultValue());
    layer->Set(Config::SESSION_LOAD_IPL_DUMP, Config::SESSION_LOAD_IPL_DUMP.GetDefaultValue());
    layer->Set(Config::GFX_HACK_DEFER_EFB_COPIES,
               Config::GFX_HACK_DEFER_EFB_COPIES.GetDefaultValue());
    layer->Set(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE,
               Config::GFX_HACK_EFB_ACCESS_TILE_SIZE.GetDefaultValue());
    layer->Set(Config::GFX_HACK_EFB_DEFER_INVALIDATION,
               Config::GFX_HACK_EFB_DEFER_INVALIDATION.GetDefaultValue());
    layer->Set(Config::SESSION_USE_FMA, Config::SESSION_USE_FMA.GetDefaultValue());
    layer->Set(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED,
               Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED.GetDefaultValue());
    layer->Set(Config::GFX_HACK_VERTEX_ROUNDING,
               Config::GFX_HACK_VERTEX_ROUNDING.GetDefaultValue());
    layer->Set(Config::GFX_EFB_SCALE, Config::GFX_EFB_SCALE.GetDefaultValue());
    layer->Set(Config::GFX_HACK_COPY_EFB_SCALED,
               Config::GFX_HACK_COPY_EFB_SCALED.GetDefaultValue());
    layer->Set(Config::GFX_FAST_DEPTH_CALC, Config::GFX_FAST_DEPTH_CALC.GetDefaultValue());
    layer->Set(Config::GFX_ENABLE_PIXEL_LIGHTING,
               Config::GFX_ENABLE_PIXEL_LIGHTING.GetDefaultValue());
    layer->Set(Config::GFX_WIDESCREEN_HACK, Config::GFX_WIDESCREEN_HACK.GetDefaultValue());
    layer->Set(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
               Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING.GetDefaultValue());
    layer->Set(Config::GFX_ENHANCE_MAX_ANISOTROPY,
               Config::GFX_ENHANCE_MAX_ANISOTROPY.GetDefaultValue());
    layer->Set(Config::GFX_ENHANCE_FORCE_TRUE_COLOR,
               Config::GFX_ENHANCE_FORCE_TRUE_COLOR.GetDefaultValue());
    layer->Set(Config::GFX_ENHANCE_DISABLE_COPY_FILTER,
               Config::GFX_ENHANCE_DISABLE_COPY_FILTER.GetDefaultValue());
    layer->Set(Config::GFX_DISABLE_FOG, Config::GFX_DISABLE_FOG.GetDefaultValue());
    layer->Set(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION,
               Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION.GetDefaultValue());
    layer->Set(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD,
               Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD.GetDefaultValue());
    layer->Set(Config::GFX_ENABLE_GPU_TEXTURE_DECODING,
               Config::GFX_ENABLE_GPU_TEXTURE_DECODING.GetDefaultValue());
    layer->Set(Config::GFX_MSAA, Config::GFX_MSAA.GetDefaultValue());
    layer->Set(Config::GFX_SSAA, Config::GFX_SSAA.GetDefaultValue());
    layer->Set(Config::SESSION_GCI_FOLDER_CURRENT_GAME_ONLY,
               Config::SESSION_GCI_FOLDER_CURRENT_GAME_ONLY.GetDefaultValue());
    layer->Set(Config::SESSION_CODE_SYNC_OVERRIDE,
               Config::SESSION_CODE_SYNC_OVERRIDE.GetDefaultValue());
    layer->Set(Config::MAIN_CUSTOM_RTC_ENABLE, Config::MAIN_CUSTOM_RTC_ENABLE.GetDefaultValue());
    layer->Set(Config::MAIN_CUSTOM_RTC_VALUE, Config::MAIN_CUSTOM_RTC_VALUE.GetDefaultValue());
    layer->Set(Config::MAIN_FASTMEM_ARENA, Config::MAIN_FASTMEM_ARENA.GetDefaultValue());
    layer->Set(Config::MAIN_ENABLE_DEBUGGING, Config::MAIN_ENABLE_DEBUGGING.GetDefaultValue());
    layer->Set(Config::SYSCONF_PROGRESSIVE_SCAN,
               Config::SYSCONF_PROGRESSIVE_SCAN.GetDefaultValue());
    layer->Set(Config::MAIN_SERIAL_PORT_1, Config::MAIN_SERIAL_PORT_1.GetDefaultValue());
    layer->Set(Config::MAIN_SERIAL_PORT_2, Config::MAIN_SERIAL_PORT_2.GetDefaultValue());

    layer->Set(Config::MAIN_CPU_THREAD, false);
    layer->Set(Config::MAIN_DSP_HLE, true);
    layer->Set(Config::MAIN_ENABLE_CHEATS, false);
    layer->Set(Config::MAIN_CUSTOM_RTC_ENABLE, true);
    layer->Set(Config::MAIN_CUSTOM_RTC_VALUE, u32{0x30000000});
    layer->Set(Config::MAIN_GC_LANGUAGE, 0);
    layer->Set(Config::MAIN_OVERRIDE_REGION_SETTINGS, true);
    layer->Set(Config::MAIN_FAST_DISC_SPEED, true);
    layer->Set(Config::MAIN_OVERCLOCK_ENABLE, false);
    layer->Set(Config::MAIN_VI_OVERCLOCK_ENABLE, false);
    layer->Set(Config::MAIN_RAM_OVERRIDE_ENABLE, false);
    layer->Set(Config::MAIN_MEMORY_CARD_SIZE, 4);
    layer->Set(Config::MAIN_SKIP_IPL, true);
    layer->Set(Config::SESSION_LOAD_IPL_DUMP, false);
    layer->Set(Config::SESSION_USE_FMA, false);
    layer->Set(Config::SESSION_SAVE_DATA_WRITABLE, false);
    layer->Set(Config::SESSION_GCI_FOLDER_CURRENT_GAME_ONLY, true);
    layer->Set(Config::SESSION_CODE_SYNC_OVERRIDE, false);
    layer->Set(Config::SYSCONF_PROGRESSIVE_SCAN, false);
    // Disable GPU-dependent readback enhancements as in strict stock netplay.
    layer->Set(Config::GFX_MSAA, 1);
    layer->Set(Config::GFX_SSAA, false);
    layer->Set(Config::GFX_EFB_SCALE, 1);
    layer->Set(Config::GFX_HACK_COPY_EFB_SCALED, false);
    for (const auto slot : ExpansionInterface::SLOTS)
      layer->Set(Config::GetInfoForEXIDevice(slot),
                 slot == ExpansionInterface::Slot::A || slot == ExpansionInterface::Slot::B ?
                     ExpansionInterface::EXIDeviceType::MemoryCardFolder :
                     ExpansionInterface::EXIDeviceType::None);

    const char* count = std::getenv("NSR_PLAYERS");
    const char* mode = std::getenv("NSR_NETPLAY");
    const bool shortcut = mode && (std::string(mode) == "host" || std::string(mode) == "client");
    const int players = !shortcut && count ? std::clamp(std::atoi(count), 2, 4) : 2;
    for (int i = 0; i < 4; ++i)
      layer->Set(Config::GetInfoForSIDevice(i), i < players ?
                     SerialInterface::SIDEVICE_GC_CONTROLLER : SerialInterface::SIDEVICE_NONE);
  }

  // Session policy is never saved into the player's configuration.
  void Save(Config::Layer*) override {}
};
}  // namespace

// Environment detection intentionally matches the transport's nonempty mode rule.
bool Enabled()
{
  const char* value = std::getenv("NSR_NETPLAY");
  const char* quickstart = std::getenv("NSR_QUICKSTART");
  return (value && *value) ||
         (quickstart && NSRQuickstart::Parse(quickstart) != NSRQuickstart::Mode::Manual);
}

// Allocate empty cards before connecting. No save or SRAM from User/ is read.
std::optional<std::map<std::string, std::string>> Prepare()
{
  Cleanup();
  s_card_root = File::CreateTempDir();
  if (s_card_root.empty())
    return std::nullopt;
  Config::AddLayer(std::make_unique<BootLayer>());
  auto layer = Config::GetLayer(Config::LayerType::Netplay);
  // CurrentRun outranks Netplay; remove inherited overrides only for our keys.
  for (const auto& [location, value] : layer->GetLayerMap())
    Config::GetLayer(Config::LayerType::CurrentRun)->DeleteKey(location);
  Config::OnConfigChanged();
  std::map<std::string, std::string> manifest;
  for (const auto& [location, value] : layer->GetLayerMap())
  {
    manifest[fmt::format("{}.{}.{}", Config::GetSystemName(location.system), location.section,
                         location.key)] = Config::GetAsString(location).value_or("");
  }
  // The CPU backend is architecture-specific; both native JITs implement the
  // same PPC contract. Refuse interpreter overrides via the canonical layer.
  manifest["Dolphin.Core.CPUCore"] = "native-jit";
  for (const auto slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    const std::string suffix = slot == ExpansionInterface::Slot::A ? "A" : "B";
    const auto directory = s_card_root + "/Card " + suffix;
    if (!File::CreateDir(directory))
      return std::nullopt;
    Config::Set(Config::LayerType::Netplay, Config::GetInfoForGCIPathOverride(slot), directory);
    Config::GetLayer(Config::LayerType::CurrentRun)->DeleteKey(
        Config::GetInfoForGCIPathOverride(slot).GetLocation());
    Memcard::HeaderData header{};
    Memcard::InitializeHeaderData(&header, GetDefaultSRAM().settings_ex.flash_id[Memcard::SLOT_A],
                                  Memcard::MBIT_SIZE_MEMORY_CARD_1019, false, 0, 0,
                                  CARD_FORMAT_TIME + (slot == ExpansionInterface::Slot::A ? 0 : 1));
    manifest["MemoryCard." + suffix + ".contents"] =
        fmt::format("empty-gci-v1-{:016x}", EmptyCardDigest(header));
  }
  Config::OnConfigChanged();
  manifest["SRAM"] = fmt::format("{:016x}", XXH3_64bits(&GetDefaultSRAM(), sizeof(Sram)));
  manifest["BootProtocol"] = "1";
  manifest["ForkBuild"] = Common::GetScmRevGitStr();
  manifest["RollbackProtocol"] = fmt::format("{}:{}", NSRNetplay::RollbackNet::VERSION,
                                            NSRNetplay::RollbackNet::HANDOFF_VERSION);
  manifest["Game"] = fmt::format("{}-r{}", SConfig::GetInstance().GetGameID(),
                                 SConfig::GetInstance().GetRevision());
  // Also agree the simulation environment before menus. Path-bearing diagnostics,
  // local cost/dirty-tracking choices and per-peer delay/prediction are excluded.
  for (const char* key : {
      "NSR_FINGERPRINT", "NSR_SEED", "NSR_DELAY", "NSR_RB_NET", "NSR_RB_VTIME_FROM",
      "NSR_RB_VTIME_BASE", "NSR_RB_VMEM_BYTES", "NSR_RB_RESIM_SPEED",
      "NSR_RB_RESIM_GPU_SPEED", "NSR_RB_RESIM_ARAM_SPEED", "NSR_RB_RESIM_AUDIO_SPEED",
      "NSR_RB_RESIM_DRAW", "NSR_RB_INSTANT_DISC", "NSR_RB_IO_IDLE", "NSR_RB_AUDIO_ROLLBACK",
      "NSR_RB_AUDIO_GATE", "NSR_RB_EFFECT_GATE", "NSR_RB_SKIP_RESIM", "NSR_RB_RESIM_VSYNC",
      "NSR_RB_ASYNC_PRESENT", "NSR_RB_PEEKZ", "NSR_RB_SESSION_END", "NSR_RB_AUTO_REENTER",
      "NSR_RB_DIGEST", "NSR_RB_DIGEST_EVERY", "NSR_AC", "NSR_AC_HASH", "NSR_AC_SPEECH_FRAMES",
      "NSR_AC_SPEECH_LEAD", "NSR_AC_SFX_DEFAULT", "NSR_AC_SFX_TAIL", "NSR_AC_BANK_FRAMES",
      "NSR_AC_POLLS_PER_TICK", "NSR_AC_SONG_FRAMES", "NSR_AC_SONG_GAP"})
  {
    const char* value = std::getenv(key);
    const std::string text = value ? value : "";
    manifest[key] = fmt::format("{:016x}", XXH3_64bits(text.data(), text.size()));
  }
  const auto bytes = EncodeBoot({BootKind::Settings, 0, 2, manifest});
  manifest["ManifestDigest"] = fmt::format("{:016x}",
      XXH3_64bits(bytes.data() + 8, bytes.size() - 8));
  return manifest;
}

// Called only after EXI's flushing threads stop, including failed-start cleanup.
void Cleanup()
{
  if (!s_card_root.empty())
  {
    File::DeleteDirRecursively(s_card_root);
    s_card_root.clear();
  }
}
}  // namespace NSRBootSync
