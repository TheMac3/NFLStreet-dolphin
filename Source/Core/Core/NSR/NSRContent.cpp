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
#include "Core/HLE/HLE.h"
#include "Core/NSR/NSRContentRead.h"
#include "Core/NSR/NSRDraft.h"
#include "Core/NSR/NSRPlaybooks.h"
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
  bool draft = false, split_policy = false;
  SplitRatings splits;
  std::vector<Playbook> playbooks;
  bool measure_splits = false;
  std::array<std::uint64_t, 2> split_calls{}, split_ns{};
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
  const bool books = Keys(root, {"format", "version", "packs", "draft", "playbooks"});
  const bool draft = books || Keys(root, {"format", "version", "packs", "draft"});
  if (!draft && !Keys(root, {"format", "version", "packs"}))
    return "invalid content object shape";
  bool valid = true;
  const auto format = Text(root.get("format"), 64, valid);
  s.split_policy = format == "nsrb.runtime-content.v4";
  if ((!s.split_policy && format !=
          (books ? "nsrb.runtime-content.v3" : draft ? "nsrb.runtime-content.v2" : "nsrb.runtime-content.v1")) ||
      !root.get("packs").is<picojson::array>())
    return "unsupported content format";
  s.fingerprint[4] = Number(root.get("version"), 0x7fffffff, valid);
  s.fingerprint[5] = s.split_policy ? 4 : books ? 3 : draft ? 2 : 1;  // exact hook/application policy is part of M2
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
  if (books)
  {
    const auto& settings = root.get("playbooks");
    if (!Keys(settings, {"policy", "captain", "users"}) ||
        Number(settings.get("policy"), 1, valid) != 1 ||
        Text(settings.get("captain"), 32, valid) != "lowest-pad" ||
        !settings.get("users").is<picojson::array>())
      return "unsupported playbook binding policy";
    const auto& users = settings.get("users").get<picojson::array>();
    if (users.empty() || users.size() > 4)
      return "invalid playbook participant count";
    for (const auto& user : users)
    {
      if (!Keys(user, {"pad", "pack"}) || Number(user.get("pad"), 3, valid) != s.playbooks.size())
        return "playbooks must be in contiguous pad order";
      s.playbooks.emplace_back();
      const auto& pack = user.get("pack");
      if (pack.is<picojson::null>())
        continue;
      if (!Keys(pack, {"format", "owner", "revision", "name", "base_sha256", "igdata_sha256", "groups"}) ||
          Text(pack.get("format"), 64, valid) != "nsrb.playbook.v1" ||
          !Number(pack.get("revision"), 0x7fffffff, valid) ||
          !pack.get("groups").is<picojson::array>())
        return "invalid user playbook";
      for (const char* key : {"owner", "base_sha256", "igdata_sha256"})
      {
        const auto hash = Text(pack.get(key), 64, valid);
        if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
          return "invalid playbook provenance hash";
      }
      // Python limits names to 64 Unicode characters; UTF-8 needs up to 256 bytes.
      if (Text(pack.get("name"), 256, valid).empty())
        return "empty playbook name";
      const auto& groups = pack.get("groups").get<picojson::array>();
      if (groups.empty() || groups.size() > 16)
        return "invalid playbook category count";
      std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
      for (const auto& group : groups)
      {
        if (!Keys(group, {"entry", "pbst", "library", "slots"}) ||
            !group.get("library").is<picojson::array>() || !group.get("slots").is<picojson::array>())
          return "invalid playbook category";
        PlaybookGroup parsed;
        parsed.entry = Number(group.get("entry"), 2, valid);
        parsed.category = Number(group.get("pbst"), 65535, valid);
        if (!parsed.entry || !seen.emplace(parsed.entry, parsed.category).second)
          return "invalid/duplicate playbook category";
        for (const char* key : {"library", "slots"})
        {
          const auto& ids = group.get(key).get<picojson::array>();
          if (ids.empty() || ids.size() > 127)
            return "invalid playbook slot count";
          auto& output = std::string_view(key) == "library" ? parsed.library : parsed.slots;
          std::set<std::uint32_t> unique;
          for (const auto& id : ids)
          {
            const auto value = Number(id, 1023, valid);
            if (!unique.insert(value).second)
              return "duplicate playbook occurrence";
            output.push_back(value);
          }
        }
        for (const auto id : parsed.slots)
          if (std::find(parsed.library.begin(), parsed.library.end(), id) == parsed.library.end())
            return "playbook slot outside library";
        s.playbooks.back().push_back(std::move(parsed));
      }
    }
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
      // An explicit nonzero PCEL entry activates a new consumer even when
      // its packed bits already equal that value. Only v4 grants that meaning.
      const bool activation = s.split_policy && edit.field == "PCEL" && edit.value != 0;
      if (edit.expected == std::vector<std::uint32_t>{edit.value} && !activation)
        return "no-op content edit";
      if (edit.field == "PPVL" && !s.split_policy)
        return "PPVL requires split policy v4";
      s.edits.push_back(std::move(edit));
    }
  }
  if (s.split_policy)
    s.splits = Splits(s.edits);
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
  s.measure_splits = !Env("NSR_RB_COST").empty();
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
  const bool velocity = std::any_of(s.splits.begin(), s.splits.end(),
      [](const auto& entry) { return entry.second[0] != 0; });
  const bool acceleration = std::any_of(s.splits.begin(), s.splits.end(),
      [](const auto& entry) { return entry.second[1] != 0; });
  std::string error;
  std::optional<std::map<std::uint32_t, std::uint8_t>> writes;
  std::optional<std::map<std::uint32_t, std::uint32_t>> draft_words;
  if (s.failed || ppc.gpr[3] != 0)
    error = "invalid content or failed native database load";
  else
  {
    if (!s.playbooks.empty() && view.UInt(0x80057d8c) != 0x7f23cb78u)
      error = "playbook native instruction guard mismatch";
    // Install no gameplay hooks for an unchanged roster. Validate all sites
    // before committing ratings/draft writes or publishing any HLE mapping.
    if (!s.splits.empty())
    {
      if ((velocity && view.UInt(0x80103544) != 0xa81f0bc4u) ||
          (acceleration && (view.UInt(0x8016d1b8) != 0xa81e0bc0u ||
                            view.UInt(0x8016d26c) != 0xa81e0bc0u)))
        error = "split consumer instruction mismatch";
    }
    if (s.draft && error.empty())
      draft_words = PlanDraft(view, s.draft_order, s.draft_first, &error);
    if (error.empty())
      writes = Plan(view, s.edits, &error);
  }
  if (error.empty() && writes && (velocity || acceleration))
  {
    // Patch silently ignores unknown names. Fail before any guest write if a
    // stale/mismatched registration table cannot install the compiled recipe.
    const auto install = [&](std::uint32_t address, std::string_view name) {
      HLE::Patch(system, address, name);
      const auto index = HLE::GetHookByAddress(address);
      return index != 0 && std::string_view{HLE::GetHookNameByIndex(index)} == name;
    };
    if ((velocity && !install(0x80103544, "NSR_SplitVelocity")) ||
        (acceleration && (!install(0x8016d1b8, "NSR_SplitAccelFirst") ||
                          !install(0x8016d26c, "NSR_SplitAccelSecond"))))
    {
      HLE::UnPatch(system, "NSR_SplitVelocity");
      HLE::UnPatch(system, "NSR_SplitAccelFirst");
      HLE::UnPatch(system, "NSR_SplitAccelSecond");
      error = "missing split HLE registration";
    }
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
  if (!s.splits.empty())
  {
    std::fprintf(stderr, "[NSR] splits applied: velocity %u acceleration %u names %zu policy 4\n",
                 velocity ? 1u : 0u, acceleration ? 1u : 0u, s.splits.size());
  }
  s.applied = true;
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started).count();
  std::fprintf(stderr, "[NSR] content applied: version %llu sha256 %s frame %u hook 8020d334 "
                       "table %08x bytes %zu host_us %lld\n",
               static_cast<unsigned long long>(s.fingerprint[4]), s.sha.c_str(), frame,
               NSRGameStats::FindTable(view, "TATS", "YALP").value_or(0), writes->size(), static_cast<long long>(elapsed));
}

// GN7E69 rev 0, fn_80057C58 at 0x80057D8C: the game has allocated an
// order array (r26, r28 bytes), and retains team r25, book tag r27, category
// PBST at sp+0xF1C. Custom indices replace saved-profile resolution, then the
// native tail publishes counts/fills unused slots. Other sides run the original.
void PlaybookMenu(const Core::CPUThreadGuard& guard, bool resim, std::uint32_t frame)
{
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  ppc.gpr[3] = ppc.gpr[25];  // displaced mr r3,r25 for the stock path
  ppc.npc = 0x80057d90;
  Configure();
  if (s.playbooks.empty())
    return;
  auto& memory = system.GetMemory();
  const NSRGameStats::Memory view{{memory.GetRAM(), memory.GetRamSizeReal()},
      {memory.GetFakeVMEM(), memory.GetFakeVMEM() ? memory.GetFakeVMemSize() : 0}};
  std::string error;
  const auto side = ppc.gpr[25];
  const auto players = static_cast<std::uint32_t>(s.playbooks.size());
  if (side > 1 || !PlaybookTeams(view, players))
  {
    s.failed = true;
    std::fprintf(stderr, "[NSR] playbook failed: invalid native controller/team registry\n");
    Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
    return;
  }
  const auto captain = PlaybookCaptain(view, side, players);
  if (!captain || s.playbooks[*captain].empty())
    return;
  const auto category = view.Bytes(ppc.gpr[1], 0xf20) ?
      view.UInt(ppc.gpr[1] + 0xf1c) : std::nullopt;
  const std::uint32_t entry = ppc.gpr[27] == 0x31544250 ? 2 : ppc.gpr[27] == 0x31444250 ? 1 : 0;
  const auto& book = s.playbooks[*captain];
  const auto group = std::find_if(book.begin(), book.end(), [&](const PlaybookGroup& candidate) {
    return category && candidate.entry == entry && candidate.category == *category;
  });
  std::optional<std::vector<std::uint8_t>> indices;
  if (s.failed || resim || group == book.end() || !view.Bytes(ppc.gpr[26], ppc.gpr[28]) ||
      ppc.gpr[26] < 0x80000000 || !view.Bytes(ppc.gpr[24], 1) || !view.Bytes(ppc.gpr[23], 1))
    error = "invalid native playbook boundary or unsupported reload in rollback";
  else
    indices = PlanPlaybook(view, *group, ppc.gpr[28], &error);
  if (!indices)
  {
    s.failed = true;
    std::fprintf(stderr, "[NSR] playbook failed: %s\n", error.c_str());
    Core::QueueHostJob([](Core::System& core) { Core::Stop(core); });
    return;
  }
  for (std::size_t i = 0; i < indices->size(); ++i)
    memory.GetRAM()[ppc.gpr[26] - 0x80000000 + i] = (*indices)[i];
  ppc.gpr[29] = static_cast<std::uint32_t>(indices->size());
  ppc.npc = 0x80057ee4;
  std::fprintf(stderr, "[NSR] playbook applied: pad %u team %u entry %u category %u slots %zu first_index %u frame %u\n",
               *captain, side, entry, *category, indices->size(), unsigned(indices->front()), frame);
}

// Replace exactly one signed halfword load. Other registers/CR/FPRs are
// untouched; the same immutable lookup runs for live and restored frames.
namespace
{
// Select r0 from restored guest metadata and the immutable agreed boot policy.
void SelectRating(const Core::CPUThreadGuard& guard, bool velocity, std::uint32_t continuation)
{
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  auto& memory = system.GetMemory();
  const NSRGameStats::Memory view{{memory.GetRAM(), memory.GetRamSizeReal()}, {}};
  const auto player = ppc.gpr[velocity ? 31 : 30];
  const auto native = view.UInt(player + (velocity ? 0xbc4u : 0xbc0u), 2).value_or(0);
  const auto fallback = static_cast<std::uint32_t>(static_cast<std::int32_t>(
      static_cast<std::int16_t>(native)));
  // Optional host-only attribution shares NSR_RB_COST's diagnostic switch.
  // Counts/timings never choose simulation data and are intentionally not rolled back.
  const auto started = s.measure_splits ? std::chrono::steady_clock::now() :
                                        std::chrono::steady_clock::time_point{};
  ppc.gpr[0] = SplitValue(view, player, velocity, s.splits, fallback);
  if (s.measure_splits)
  {
    const auto lane = velocity ? 0u : 1u;
    ++s.split_calls[lane];
    s.split_ns[lane] += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
  }
  ppc.npc = continuation;
}
}

// JIT may call an HLE in the middle of a block without updating ppc.pc.
// Separate fixed continuations avoid using stale block-start CPU state.
void VelocityHook(const Core::CPUThreadGuard& guard)
{
  SelectRating(guard, true, 0x80103548);
}

// The two native acceleration branches have distinct return instructions.
void AccelerationFirstHook(const Core::CPUThreadGuard& guard)
{
  SelectRating(guard, false, 0x8016d1bc);
}

// Continue the ordinary movement branch after its displaced PSPD load.
void AccelerationSecondHook(const Core::CPUThreadGuard& guard)
{
  SelectRating(guard, false, 0x8016d270);
}

void Reset()
{
  if (s.measure_splits)
    std::fprintf(stderr, "[NSR] split cost: velocity calls %llu ns %llu acceleration calls %llu ns %llu\n",
        static_cast<unsigned long long>(s.split_calls[0]), static_cast<unsigned long long>(s.split_ns[0]),
        static_cast<unsigned long long>(s.split_calls[1]), static_cast<unsigned long long>(s.split_ns[1]));
  s = {};
}
}
