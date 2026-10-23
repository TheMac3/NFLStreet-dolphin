// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

enum class EFBReinterpretType;

class EFBInterfaceBase
{
public:
  virtual ~EFBInterfaceBase();

  virtual void ReinterpretPixelData(EFBReinterpretType convtype) = 0;

  virtual void PokeColor(u16 x, u16 y, u32 color) = 0;
  virtual void PokeDepth(u16 x, u16 y, u32 depth) = 0;

  u32 PeekColor(u16 x, u16 y);
  u32 PeekDepth(u16 x, u16 y);

  // NSR netplay (Core/NSR/NSRBootSync.cpp): while enabled, CPU peeks of the
  // EFB read every pixel as 0, exactly what the Null backend answers, instead
  // of asking the host GPU. Peeked values land in guest RAM, and what the GPU
  // drew depends on per-PC presentation (internal resolution, MSAA/SSAA, the
  // player's HD texture pack and button-icon set, GPU vendor), so a real peek
  // could make two peers' games diverge. With peeks canonical (and EFB/XFB
  // copies kept out of RAM by the canonical config layer) presentation is
  // RAM-neutral. All determinism gates run on the Null backend, so they
  // already ran with these answers.
  static void SetCanonicalPeeks(bool enabled);

protected:
  bool ShouldSkipAccess(u16 x, u16 y) const;

  virtual u32 PeekColorInternal(u16 x, u16 y) = 0;
  virtual u32 PeekDepthInternal(u16 x, u16 y) = 0;
};

class HardwareEFBInterface final : public EFBInterfaceBase
{
  void ReinterpretPixelData(EFBReinterpretType convtype) override;

  void PokeColor(u16 x, u16 y, u32 color) override;
  void PokeDepth(u16 x, u16 y, u32 depth) override;

  u32 PeekColorInternal(u16 x, u16 y) override;
  u32 PeekDepthInternal(u16 x, u16 y) override;
};

extern std::unique_ptr<EFBInterfaceBase> g_efb_interface;
