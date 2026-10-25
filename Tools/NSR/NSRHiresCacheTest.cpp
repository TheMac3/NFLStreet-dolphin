// Copyright 2026 NFLStreet POC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Synthetic host-cache tests: warm GPU metadata must not schedule decoded
// pixels again, completed file reloads must remain visible, and LRU touches
// must retain their node. No renderer, sockets, game data or worker threads.
#include <cassert>
#include <atomic>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <memory>

#include "VideoCommon/Assets/CustomResourceManager.h"

namespace
{
std::atomic<size_t> s_allocations{0};
}

// Count actual allocations so a deallocate/reallocate touch cannot pass merely
// because the system allocator happens to return the same node address.
void* operator new(std::size_t size)
{
  s_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size ? size : 1))
    return memory;
  std::abort();
}

// Match both ordinary and sized deletes used by the standard library.
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace VideoCommon
{
// A tiny asset records every decode request without loading a file.
class SyntheticAsset final : public CustomAsset
{
public:
  // Construct metadata only; decoding is simulated by LoadImpl.
  explicit SyntheticAsset(u64 handle) : CustomAsset(nullptr, "synthetic", handle) {}
  u32 loads = 0;
private:
  // Record a synthetic completed decode without filesystem access.
  CustomAssetLibrary::LoadInfo LoadImpl(const CustomAssetLibrary::AssetID&) override
  {
    ++loads;
    return {16};
  }
  // No pixel storage belongs to the synthetic asset.
  void UnloadImpl() override {}
};

struct NSRHiresCacheTestAccess
{
  // Exercise actual public metadata/budget methods using synthetic ownership.
  static void Run()
  {
    CustomResourceManager manager;
    auto asset = std::make_unique<SyntheticAsset>(0);
    auto* raw = asset.get();
    assert(raw->Load() == 16);
    const auto first = raw->GetLastLoadedTime();
    CustomResourceManager::AssetData data;
    data.asset = std::move(asset);
    data.type = CustomResourceManager::AssetData::AssetType::TextureData;
    using Status = CustomResourceManager::AssetData::LoadStatus;
    data.load_status = Status::ResourceDataAvailable;
    manager.m_asset_handle_to_data.emplace(0, std::move(data));
    manager.m_asset_id_to_handle.emplace("synthetic", 0);
    auto& state = manager.m_asset_handle_to_data.at(0);

    assert(manager.GetTextureLoadTime("absent") == CustomAsset::TimeType{});
    assert(manager.m_texture_data_asset_cache.empty());
    assert(manager.m_pending_assets.IsEmpty());
    assert(manager.GetTextureLoadTime("synthetic") == first);
    const auto* node = &*manager.m_active_assets.Elements().begin();
    assert(manager.GetTextureLoadTime("synthetic") == first);
    assert(node == &*manager.m_active_assets.Elements().begin());
    const size_t before = s_allocations.load(std::memory_order_relaxed);
    for (u32 i = 0; i < 1000; ++i)
      assert(manager.GetTextureLoadTime("synthetic") == first);
    assert(s_allocations.load(std::memory_order_relaxed) == before);
    assert(raw->loads == 1);

    // Eviction preserves the GPU entry's old pixels without requesting CPU
    // data. A completed watched-file reload is subsequently observable.
    state.load_status = Status::Unloaded;
    manager.m_active_assets.RemoveAsset(0);
    assert(manager.GetTextureLoadTime("synthetic") == CustomAsset::TimeType{});
    assert(manager.m_pending_assets.IsEmpty() && raw->loads == 1);
    state.load_status = Status::PendingReload;
    assert(raw->Load() == 16);
    assert(manager.GetTextureLoadTime("synthetic") == CustomAsset::TimeType{});
    state.load_status = Status::LoadFinished;
    assert(manager.GetTextureLoadTime("synthetic") == raw->GetLastLoadedTime());
    assert(raw->GetLastLoadedTime() >= first && raw->loads == 2);
    state.has_load_error = true;
    assert(manager.GetTextureLoadTime("synthetic") == CustomAsset::TimeType{});

    // Cache policy may lower the normal budget, never expand it on a later call.
    manager.m_max_ram_available = 8ULL * 1024 * 1024 * 1024;
    manager.LimitTextureMemory(512ULL * 1024 * 1024);
    manager.LimitTextureMemory(1024ULL * 1024 * 1024);
    assert(manager.m_max_ram_available == 512ULL * 1024 * 1024);
  }
};
}  // namespace VideoCommon

// Standalone entry point for the production-metadata test; link with gc-sections
// so renderer-dependent resource-manager methods remain outside this fixture.
int main()
{
  VideoCommon::NSRHiresCacheTestAccess::Run();
  std::puts("NSR HD cache metadata tests passed");
}
