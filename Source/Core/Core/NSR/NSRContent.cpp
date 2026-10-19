// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Validated runtime ratings at native DB-load completion. The launcher checks
// release signatures; this adapter verifies the exact boot bundle again, applies
// edits atomically and exposes its full identity to rollback configuration M2.
#include "Core/NSR/NSRContent.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <fmt/format.h>
#include <mbedtls/sha256.h>
#include <picojson.h>

#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/NSR/NSRContentRead.h"
#include "Core/NSR/NSRDraft.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace NSRContent
{
namespace
{
struct State
{
  bool configured = false, enabled = false, failed = false, applied = false;
  std::string sha;
  std::array<std::uint64_t, 6> fingerprint{};
  std::vector<Edit> edits;
  std::uint32_t draft_order = 0, draft_first = 0;
  bool draft = false;
};
State s;

// Read boot settings once; the launcher strips inherited NSR_* variables.
std::string Env(const char* name)
{
#ifdef _WIN32
  // Python/CreateProcess supplies Unicode paths. Read the wide environment and
  // use Dolphin's UTF-8 file API, rather than losing a non-ASCII Windows user.
  const wchar_t* value = _wgetenv(UTF8ToWString(name).c_str());
  return value ? WStringToUTF8(value) : "";
#else
  const char* value = std::getenv(name);
  return value ? value : "";
#endif
}

// Parsing helpers propagate a sticky validation failure, without exceptions
// (Dolphin builds with exceptions disabled). No writes occur during parsing.
std::uint32_t Number(const picojson::value& value, std::uint32_t maximum, bool& valid)
{
  if (!value.is<double>())
  {
    valid = false;
    return 0;
  }
  const double number = value.get<double>();
  if (!std::isfinite(number) || number < 0 || number > maximum || std::floor(number) != number)
  {
    valid = false;
    return 0;
  }
  return static_cast<std::uint32_t>(number);
}

// Require exact shapes even for a hash-verified caller-supplied bundle.
bool Keys(const picojson::value& value, std::initializer_list<const char*> keys)
{
  if (!value.is<picojson::object>() || value.get<picojson::object>().size() != keys.size())
    return false;
  for (const char* key : keys)
    if (!value.contains(key))
      return false;
  return true;
}

// Text has explicit size and type limits before identity/field processing.
std::string Text(const picojson::value& value, std::size_t maximum, bool& valid)
{
  if (!value.is<std::string>() || value.get<std::string>().size() > maximum)
  {
    valid = false;
    return {};
  }
  return value.get<std::string>();
}

// Authenticate the immutable input before parsing. An explicit expected SHA is
// required for direct launches; signed-release trust belongs to the launcher.
std::string Load(const std::string& path, const std::string& expected)
{
  if (path.empty() || expected.size() != 64)
    return "content path and SHA-256 are both required";
  File::IOFile file(path, "rb");
  if (!file)
    return "cannot read content bundle";
  constexpr std::size_t MAX_BYTES = 4 * 1024 * 1024;
  const auto size = file.GetSize();
  if (!size || size > MAX_BYTES)
    return "content bundle size limit";
  std::string raw(static_cast<std::size_t>(size), '\0');
  if (!file.ReadBytes(raw.data(), raw.size()) || std::fgetc(file.GetHandle()) != EOF)
    return "content file changed or could not be read";
  std::array<unsigned char, 32> digest{};
  if (mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(raw.data()), raw.size(),
                         digest.data(), 0) != 0)
    return "content hash failed";
  for (std::size_t i = 0; i < digest.size(); ++i)
  {
    s.sha += fmt::format("{:02x}", digest[i]);
    s.fingerprint[i / 8] = (s.fingerprint[i / 8] << 8) | digest[i];
  }
  if (s.sha != expected)
    return "content SHA-256 mismatch";
  picojson::value root;
  if (!picojson::parse(root, raw).empty())
    return "invalid content JSON";
  const bool draft = Keys(root, {"format", "version", "packs", "draft"});
  if (!draft && !Keys(root, {"format", "version", "packs"}))
    return "invalid content object shape";
  bool valid = true;
  if (Text(root.get("format"), 64, valid) !=
          (draft ? "nsrb.runtime-content.v2" : "nsrb.runtime-content.v1") ||
      !root.get("packs").is<picojson::array>())
    return "unsupported content format";
  s.fingerprint[4] = Number(root.get("version"), 0x7fffffff, valid);
  s.fingerprint[5] = draft ? 2 : 1;  // exact hook/application policy is part of M2
  if (draft)
  {
    const auto& settings = root.get("draft");
    if (!Keys(settings, {"order", "first", "policy"}) ||
        Number(settings.get("policy"), 1, valid) != 1)
      return "unsupported draft policy";
    constexpr std::array<const char*, 3> ORDERS{{"alternating", "snake", "cocky"}};
    constexpr std::array<const char*, 3> FIRSTS{{"stock", "team-one", "team-two"}};
    const auto order = Text(settings.get("order"), 16, valid);
    const auto first = Text(settings.get("first"), 16, valid);
    const auto o = std::find(ORDERS.begin(), ORDERS.end(), order);
    const auto f = std::find(FIRSTS.begin(), FIRSTS.end(), first);
    if (o == ORDERS.end() || f == FIRSTS.end())
      return "unsupported draft setting";
    s.draft_order = static_cast<std::uint32_t>(o - ORDERS.begin());
    s.draft_first = static_cast<std::uint32_t>(f - FIRSTS.begin());
    s.draft = true;
  }
  const auto& packs = root.get("packs").get<picojson::array>();
  if (packs.size() > 8)
    return "too many content packs";
  for (const auto& pack : packs)
  {
    if (!Keys(pack, {"format", "name", "edits"}) ||
        Text(pack.get("format"), 64, valid) != "nsrb.ratings-pack.v1" ||
        !pack.get("edits").is<picojson::array>())
      return "invalid ratings pack";
    (void)Text(pack.get("name"), 80, valid);
    const auto& items = pack.get("edits").get<picojson::array>();
    if (items.empty() || items.size() > 5000)
      return "invalid edit count";
    for (const auto& item : items)
    {
      if (!Keys(item, {"first_name", "last_name", "field", "value", "expected"}))
        return "invalid rating edit";
      Edit edit{Text(item.get("first_name"), 64, valid), Text(item.get("last_name"), 64, valid),
                Text(item.get("field"), 4, valid), Number(item.get("value"), 255, valid), {}};
      if (!item.get("expected").is<picojson::array>())
        return "original ratings array required";
      const auto& originals = item.get("expected").get<picojson::array>();
      if (originals.empty() || originals.size() > 8 || s.edits.size() >= 5000)
        return "rating edit limit";
      for (const auto& original : originals)
        edit.expected.push_back(Number(original, 255, valid));
      if (edit.expected == std::vector<std::uint32_t>{edit.value})
        return "no-op content edit";
      s.edits.push_back(std::move(edit));
    }
  }
  return valid ? "" : "invalid content value";
}

// Configure once, preserving failures until the emulator shuts down.
void Configure()
{
  if (s.configured)
    return;
  s.configured = true;
  const auto path = Env("NSR_CONTENT_PACK"), expected = Env("NSR_CONTENT_SHA256");
  if (path.empty() && expected.empty())
    return;
  s.enabled = true;
  const auto error = Load(path, expected);
  s.failed = !error.empty();
  if (s.failed)
    std::fprintf(stderr, "[NSR] content rejected: %s\n", error.c_str());
  else
    std::fprintf(stderr, "[NSR] content verified: version %llu sha256 %s edits %zu\n",
                 static_cast<unsigned long long>(s.fingerprint[4]), s.sha.c_str(), s.edits.size());
}
}

std::array<std::uint64_t, 6> Fingerprint()
{
  Configure();
  return s.fingerprint;
}

bool Ready()
{
  Configure();
  return !s.failed && (!s.enabled || s.applied);
}

void DatabaseLoaded(const Core::CPUThreadGuard& guard, bool resim, std::uint32_t frame)
{
  auto& system = guard.GetSystem();
  const auto& ppc = system.GetPPCState();
  auto& memory = system.GetMemory();
  const NSRGameStats::Memory view{{memory.GetRAM(), memory.GetRamSizeReal()},
      {memory.GetFakeVMEM(), memory.GetFakeVMEM() ? memory.GetFakeVMemSize() : 0}};
  // fn_8020D2D0 preserves database tag, filename and TERF entry in r28..30.
  // Registration is complete at 0x8020D334; no player consumer has run yet.
  if (ppc.gpr[28] != 0x54415453 || ppc.gpr[29] != 2)
    return;
  const auto filename = view.Bytes(ppc.gpr[30], 13);
  constexpr std::string_view TEMPLATE = "template.dat";
  if (!filename || !std::equal(TEMPLATE.begin(), TEMPLATE.end(), filename->begin()) ||
      (*filename)[12] != 0)
    return;
  // No host file read, parse or write is permitted on the re-simulation path.
  // A disc DB reload within an epoch is unsupported and must stop explicitly.
  if (resim)
  {
    if (!Env("NSR_CONTENT_PACK").empty())
    {
      s.failed = true;
      std::fprintf(stderr, "[NSR] content injection refused in re-simulation\n");
      Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
    }
    return;
  }
  const auto started = std::chrono::steady_clock::now();
  Configure();
  if (!s.enabled)
    return;
  std::string error;
  std::optional<std::map<std::uint32_t, std::uint8_t>> writes;
  std::optional<std::map<std::uint32_t, std::uint32_t>> draft_words;
  if (s.failed || ppc.gpr[3] != 0)
    error = "invalid content or failed native database load";
  else
  {
    if (s.draft)
      draft_words = PlanDraft(view, s.draft_order, s.draft_first, &error);
    if (error.empty())
      writes = Plan(view, s.edits, &error);
  }
  if (!error.empty() || !writes)
  {
    s.failed = true;
    std::fprintf(stderr, "[NSR] content injection failed: %s\n", error.c_str());
    Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
    return;
  }
  for (const auto& [address, value] : *writes)
    memory.GetFakeVMEM()[address - 0x7e000000] = value;
  if (draft_words && !draft_words->empty())
  {
    // All words were checked before either plan committed. This hook runs
    // before draft consumers, outside rollback. Clear both instruction caches
    // once, so precompiled native blocks cannot execute displaced code.
    for (const auto& [address, value] : *draft_words)
      for (std::uint32_t byte = 0; byte < 4; ++byte)
        memory.GetRAM()[address - 0x80000000 + byte] =
            static_cast<std::uint8_t>(value >> (24 - byte * 8));
    system.GetPPCState().iCache.Reset(system.GetJitInterface());
    std::fprintf(stderr, "[NSR] draft applied: order %u first %u policy 1 words %zu frame %u\n",
                 s.draft_order, s.draft_first, draft_words->size(), frame);
  }
  s.applied = true;
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started).count();
  std::fprintf(stderr, "[NSR] content applied: version %llu sha256 %s frame %u hook 8020d334 "
                       "table %08x bytes %zu host_us %lld\n",
               static_cast<unsigned long long>(s.fingerprint[4]), s.sha.c_str(), frame,
               NSRGameStats::FindTable(view, "TATS", "YALP").value_or(0), writes->size(), static_cast<long long>(elapsed));
}

void Reset()
{
  s = {};
}
}
