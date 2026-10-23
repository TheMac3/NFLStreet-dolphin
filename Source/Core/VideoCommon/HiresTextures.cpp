// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/HiresTextures.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xxhash.h>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "Core/System.h"
#include "VideoCommon/Assets/CustomAsset.h"
#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"

constexpr std::string_view s_format_prefix{"tex1_"};

static std::unordered_map<std::string, std::shared_ptr<HiresTexture>> s_hires_texture_cache;
static std::unordered_map<std::string, bool> s_hires_texture_id_to_arbmipmap;

static auto s_file_library = std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();

namespace
{
std::pair<std::string, bool> GetNameArbPair(const TextureInfo& texture_info)
{
  if (s_hires_texture_id_to_arbmipmap.empty())
    return {"", false};

  const auto texture_name_details = texture_info.CalculateTextureName();
  // look for an exact match first
  const std::string full_name = texture_name_details.GetFullName();
  if (auto iter = s_hires_texture_id_to_arbmipmap.find(full_name);
      iter != s_hires_texture_id_to_arbmipmap.end())
  {
    return {full_name, iter->second};
  }

  // Single wildcard ignoring the tlut hash
  const std::string texture_name_single_wildcard_tlut =
      fmt::format("{}_{}_$_{}", texture_name_details.base_name, texture_name_details.texture_name,
                  texture_name_details.format_name);
  if (auto iter = s_hires_texture_id_to_arbmipmap.find(texture_name_single_wildcard_tlut);
      iter != s_hires_texture_id_to_arbmipmap.end())
  {
    return {texture_name_single_wildcard_tlut, iter->second};
  }

  // Single wildcard ignoring the texture hash
  const std::string texture_name_single_wildcard_tex =
      fmt::format("{}_${}_{}", texture_name_details.base_name, texture_name_details.tlut_name,
                  texture_name_details.format_name);
  if (auto iter = s_hires_texture_id_to_arbmipmap.find(texture_name_single_wildcard_tex);
      iter != s_hires_texture_id_to_arbmipmap.end())
  {
    return {texture_name_single_wildcard_tex, iter->second};
  }

  return {"", false};
}

// --- NSR: ordered texture folders (feat/textures, 2026-10-23) ---------------
// NFLStreetRollback's launcher composes the player's texture set from their NFL
// Street Reborn install without copying or modifying it: Reborn's pristine HD
// library, the mod packages the player turned on in Reborn, then the button-icon
// package matching their controller. It lists those folders, lowest precedence
// first, in <User>/Load/NSRTextures/<game id>.txt (UTF-8, one absolute folder per
// line; lines starting with '#' are comments). A later folder's texture replaces
// an earlier folder's file of the same name, and every listed folder outranks the
// ordinary Load/Textures/<game id> folder. Read-only overlay: no file is linked,
// copied or written, so a multi-GB library costs nothing and Reborn's folders stay
// pristine.
// Textures are presentation only; with NSR netplay's canonical EFB peeks
// (EFBInterfaceBase::SetCanonicalPeeks) they cannot reach guest RAM, so peers
// may use different sets. Deferred: per-texture masking (hiding a lower folder's
// file without a replacement) is not supported; the launcher never needs it
// because it lists Reborn's pristine library rather than its composed tree.
// Revisit if a package must remove a texture rather than replace it.
std::vector<std::string> NSRTextureFolders(const std::string& game_id)
{
  std::vector<std::string> folders;
  if (game_id.empty())
    return folders;
  const std::string list_path =
      File::GetUserPath(D_LOAD_IDX) + "NSRTextures" DIR_SEP + game_id + ".txt";
  std::string text;
  if (!File::ReadFileToString(list_path, text))
    return folders;
  for (std::string line : SplitString(text, '\n'))
  {
    line = std::string(StripWhitespace(line));
    if (line.empty() || line[0] == '#')  // whole-line comments only: paths may hold '#'
      continue;
    if (!File::IsDirectory(line))
    {
      WARN_LOG_FMT(VIDEO, "NSR textures: listed folder '{}' is missing; skipped", line);
      continue;
    }
    folders.push_back(line);
  }
  // Highest precedence first: the scan below keeps the first file of each name.
  std::reverse(folders.begin(), folders.end());
  NOTICE_LOG_FMT(VIDEO, "NSR textures: {} folder(s) from {}", folders.size(), list_path);
  return folders;
}
}  // namespace

void HiresTexture::Shutdown()
{
  Clear();
}

void HiresTexture::Update()
{
  if (!g_ActiveConfig.bHiresTextures)
  {
    Clear();
    return;
  }

  const std::string& game_id = SConfig::GetInstance().GetGameID();
  const std::set<std::string> texture_directories =
      GetTextureDirectoriesWithGameId(File::GetUserPath(D_HIRESTEXTURES_IDX), game_id);
  const std::vector<std::string> extensions{".png", ".dds"};

  // NSR: listed folders first (highest precedence), then the stock ones.
  std::vector<std::string> ordered_directories = NSRTextureFolders(game_id);
  const bool nsr_overlay = !ordered_directories.empty();
  ordered_directories.insert(ordered_directories.end(), texture_directories.begin(),
                             texture_directories.end());

  for (const auto& texture_directory : ordered_directories)
  {
    // Watch this directory for any texture reloads
    s_file_library->Watch(texture_directory);

    const auto texture_paths =
        Common::DoFileSearch({texture_directory}, extensions, /*recursive*/ true);

    bool failed_insert = false;
    size_t nsr_shadowed = 0;  // files a higher-precedence NSR folder already supplies
    for (auto& path : texture_paths)
    {
      std::string filename;
      SplitPath(path, nullptr, &filename, nullptr);

      if (filename.substr(0, s_format_prefix.length()) == s_format_prefix)
      {
        const size_t arb_index = filename.rfind("_arb");
        const bool has_arbitrary_mipmaps = arb_index != std::string::npos;
        if (has_arbitrary_mipmaps)
          filename.erase(arb_index, 4);

        const auto [it, inserted] =
            s_hires_texture_id_to_arbmipmap.try_emplace(filename, has_arbitrary_mipmaps);
        if (!inserted)
        {
          // With NSR folders a same-named file is the intended override.
          if (nsr_overlay)
            ++nsr_shadowed;
          else
            failed_insert = true;
        }
        else
        {
          // Since this is just a texture (single file) the mapper doesn't really matter
          // just provide a string
          s_file_library->SetAssetIDMapData(filename, std::map<std::string, std::filesystem::path>{
                                                          {"texture", StringToPath(path)}});

          if (g_ActiveConfig.bCacheHiresTextures)
          {
            auto hires_texture =
                std::make_shared<HiresTexture>(has_arbitrary_mipmaps, std::move(filename));
            static_cast<void>(hires_texture->LoadTexture());
            s_hires_texture_cache.try_emplace(hires_texture->GetId(), hires_texture);
          }
        }
      }
    }

    if (failed_insert)
    {
      ERROR_LOG_FMT(VIDEO, "One or more textures at path '{}' were already inserted",
                    texture_directory);
    }
    if (nsr_overlay)
    {
      NOTICE_LOG_FMT(VIDEO, "NSR textures: '{}': {} file(s), {} replaced by a later folder",
                     texture_directory, texture_paths.size(), nsr_shadowed);
    }
  }

  if (g_ActiveConfig.bCacheHiresTextures)
  {
    OSD::AddMessage(fmt::format("Loading '{}' custom textures", s_hires_texture_cache.size()),
                    10000);
  }
  else
  {
    OSD::AddMessage(
        fmt::format("Found '{}' custom textures", s_hires_texture_id_to_arbmipmap.size()), 10000);
  }
}

void HiresTexture::Clear()
{
  s_hires_texture_cache.clear();
  s_hires_texture_id_to_arbmipmap.clear();
  s_file_library = std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();
}

std::shared_ptr<HiresTexture> HiresTexture::Search(const TextureInfo& texture_info)
{
  auto [base_filename, has_arb_mipmaps] = GetNameArbPair(texture_info);
  if (base_filename == "")
    return nullptr;

  if (auto iter = s_hires_texture_cache.find(base_filename); iter != s_hires_texture_cache.end())
  {
    return iter->second;
  }
  else
  {
    auto hires_texture = std::make_shared<HiresTexture>(has_arb_mipmaps, std::move(base_filename));
    if (g_ActiveConfig.bCacheHiresTextures)
    {
      s_hires_texture_cache.try_emplace(hires_texture->GetId(), hires_texture);
    }
    return hires_texture;
  }
}

HiresTexture::HiresTexture(bool has_arbitrary_mipmaps, std::string id)
    : m_has_arbitrary_mipmaps(has_arbitrary_mipmaps), m_id(std::move(id))
{
}

VideoCommon::CustomResourceManager::TextureTimePair HiresTexture::LoadTexture() const
{
  auto& system = Core::System::GetInstance();
  auto& custom_resource_manager = system.GetCustomResourceManager();
  return custom_resource_manager.GetTextureDataFromAsset(m_id, s_file_library);
}

std::set<std::string> GetTextureDirectoriesWithGameId(const std::string& root_directory,
                                                      const std::string& game_id)
{
  std::set<std::string> result;
  const std::string texture_directory = root_directory + game_id;

  if (File::Exists(texture_directory))
  {
    result.insert(texture_directory);
  }
  else
  {
    // If there's no directory with the region-specific ID, look for a 3-character region-free one
    const std::string region_free_directory = root_directory + game_id.substr(0, 3);

    if (File::Exists(region_free_directory))
    {
      result.insert(region_free_directory);
    }
  }

  const auto match_gameid_or_all = [game_id](const std::string& filename) {
    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    return basename == game_id || basename == game_id.substr(0, 3) || basename == "all";
  };

  // Look for any other directories that might be specific to the given gameid
  const auto files = Common::DoFileSearch({root_directory}, {".txt"}, true);
  for (const auto& file : files)
  {
    if (match_gameid_or_all(file))
    {
      // The following code is used to calculate the top directory
      // of a found gameid.txt file
      // ex:  <root directory>/My folder/gameids/<gameid>.txt
      // would insert "<root directory>/My folder"
      const auto directory_path = file.substr(root_directory.size());
      const std::size_t first_path_separator_position = directory_path.find_first_of(DIR_SEP_CHR);
      result.insert(root_directory + directory_path.substr(0, first_path_separator_position));
    }
  }

  return result;
}
