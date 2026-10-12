// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace EMM
{
// A client may claim a write-protection fault before Dolphin treats it as an
// invalid fastmem access and permanently backpatches the JIT store. The
// callback runs in the platform exception context and therefore must not
// allocate, lock, or log.
using MemoryFaultHandler = bool (*)(std::uintptr_t fault_address);

void InstallExceptionHandler();
void UninstallExceptionHandler();
bool IsExceptionHandlerSupported();
void SetMemoryFaultHandler(MemoryFaultHandler handler);
}  // namespace EMM
