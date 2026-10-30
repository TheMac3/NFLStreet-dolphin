// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Socket-free, atomic edit planning over native EA DB structures. Reuse the
// audited results reader's bounds/schema checks; never embed roster records.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Core/NSR/NSRGameStatsRead.h"

namespace NSRContent
{
struct Edit
{
  std::string first, last, field;
  std::uint32_t value;
  std::vector<std::uint32_t> expected;
  std::string string_value{};
  std::vector<std::string> string_expected{};
  bool strict_names = false;
};

// Strict UTF-8 to the game's Windows-1252 byte encoding. Reject undefined
// characters and controls instead of silently substituting a different name.
inline std::optional<std::string> GuestText(const std::string& input)
{
  constexpr std::array<std::uint32_t, 32> special{{
      0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
      0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
      0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
      0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178}};
  std::string result;
  for (std::size_t i = 0; i < input.size();)
  {
    const auto first = static_cast<unsigned char>(input[i++]);
    std::uint32_t cp = first;
    unsigned remaining = 0;
    if (first >= 0xc2 && first <= 0xdf) { cp = first & 31; remaining = 1; }
    else if (first >= 0xe0 && first <= 0xef) { cp = first & 15; remaining = 2; }
    else if (first >= 0xf0 && first <= 0xf4) { cp = first & 7; remaining = 3; }
    else if (first >= 0x80) return std::nullopt;
    if (i + remaining > input.size()) return std::nullopt;
    const auto length = remaining;
    while (remaining--)
    {
      const auto next = static_cast<unsigned char>(input[i++]);
      if ((next & 0xc0) != 0x80) return std::nullopt;
      cp = (cp << 6) | (next & 63);
    }
    if ((length == 2 && cp < 0x800) || (length == 3 && cp < 0x10000) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 ||
        (cp >= 0x7f && cp < 0xa0)) return std::nullopt;
    if (cp <= 0xff) result.push_back(static_cast<char>(cp));
    else
    {
      const auto it = std::find(special.begin(), special.end(), cp);
      if (it == special.end()) return std::nullopt;
      result.push_back(static_cast<char>(0x80 + (it - special.begin())));
    }
  }
  return result;
}

// Match roster.name(): valid CP1252, no controls or pipe, and no Python
// strip() whitespace at the edges. Internal nonbreaking spaces are allowed.
inline bool NativeNameValid(const std::string& text, std::size_t maximum)
{
  if (text.size() > maximum) return false;
  const auto edge_space = [](unsigned char c) { return c == 0x20 || c == 0xa0; };
  if (!text.empty() && (edge_space(static_cast<unsigned char>(text.front())) ||
                        edge_space(static_cast<unsigned char>(text.back())))) return false;
  for (unsigned char c : text)
    if (c < 0x20 || c == 0x7f || c == '|' || c == 0x81 || c == 0x8d ||
        c == 0x8f || c == 0x90 || c == 0x9d) return false;
  return true;
}

// Casefold the representable repertoire, including the expanding sharp-s.
// Input is native CP1252, not UTF-8; this also matches split metadata.
inline std::string Name(std::string value)
{
  std::string folded;
  for (unsigned char c : value)
  {
    if (c == 0xdf) { folded += "ss"; continue; }
    if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xd6) ||
        (c >= 0xd8 && c <= 0xde)) c += 32;
    else if (c == 0x8a || c == 0x8c || c == 0x8e) c += 16;
    else if (c == 0x9f) c = 0xff;
    folded.push_back(static_cast<char>(c));
  }
  const auto begin = folded.find_first_not_of(' ');
  if (begin == std::string::npos) return {};
  return folded.substr(begin, folded.find_last_not_of(' ') - begin + 1);
}

// The same allowlist/ranges as nsr_content.py. Storage bounds are checked too.
inline std::pair<std::uint32_t, std::uint32_t> Limits(const std::string& field)
{
  if (field == "PPVL" || field == "PCEL")
    return {0, 99};
  if (field == "PBPO")
    return {0, 11};
  if (field == "PHGT")
    return {48, 96};
  if (field == "PWGT")
    return {0, 255};
  static const std::vector<std::string> ratings{
      "POVR", "PSPD", "PCEL", "PAGI", "PJUM", "PCTH", "PPSS", "PCOV", "PTAK", "PBLK", "PBTK"};
  if (std::find(ratings.begin(), ratings.end(), field) == ratings.end())
    return {1, 0};
  return {0, 127};
}

// Validate every requested copy and every field before returning any writes.
// The returned map merges adjacent packed fields while preserving all other bits.
inline std::optional<std::map<std::uint32_t, std::uint8_t>>
Plan(const NSRGameStats::Memory& memory, const std::vector<Edit>& edits, std::string* error = nullptr)
{
  const auto reject = [&](const std::string& message) -> std::nullopt_t {
    if (error)
      *error = message;
    return std::nullopt;
  };
  using namespace NSRGameStats;
  const auto rows = ReadTable(memory, "TATS", "YALP");
  const auto table = FindTable(memory, "TATS", "YALP");
  if (!rows || !table)
    return reject("missing or malformed STAT.PLAY");
  const auto stride = *memory.UInt(*table + 0x20);
  const auto count = *memory.UInt(*table + 0x44, 2);
  const auto deleted = *memory.UInt(*table + 0x24);
  const auto buffer = *memory.UInt(*table + 0x14);
  if (buffer < 0x7e000000 || buffer - 0x7e000000 > memory.vmem.size() ||
      count * stride > memory.vmem.size() - (buffer - 0x7e000000))
    return reject("roster record buffer is outside fake VMEM");
  std::vector<std::uint32_t> physical;
  for (std::uint32_t i = 0; i < count; ++i)
  {
    const auto bytes = *memory.Bytes(buffer + i * stride, stride);
    if (!((bytes[deleted / 8] >> (7 - deleted % 8)) & 1))
      physical.push_back(i);
  }
  std::map<std::uint32_t, std::uint8_t> writes;
  std::set<std::tuple<std::string, std::string, std::string>> seen;
  for (const auto& edit : edits)
  {
    const auto guest_first = GuestText(edit.first), guest_last = GuestText(edit.last);
    if (!guest_first || !guest_last ||
        (edit.strict_names && (!NativeNameValid(*guest_first, 32) ||
                               !NativeNameValid(*guest_last, 32))))
      return reject("invalid player identity encoding");
    const auto first = Name(*guest_first), last = Name(*guest_last);
    const bool is_name = edit.field == "PFNA" || edit.field == "PLNA";
    const auto [low, high] = Limits(edit.field);
    const std::set<std::uint32_t> roles{0, 1, 2, 4, 7, 9, 11};
    if ((first.empty() && last.empty()) ||
        (!is_name && (low > high || edit.value < low || edit.value > high ||
         (edit.field == "PBPO" && !roles.count(edit.value)) || edit.expected.empty() ||
         !std::is_sorted(edit.expected.begin(), edit.expected.end()) ||
         std::adjacent_find(edit.expected.begin(), edit.expected.end()) != edit.expected.end())) ||
        !seen.emplace(first, last, edit.field).second)
      return reject("invalid or duplicate content edit");
    const auto replacement = GuestText(edit.string_value);
    std::set<std::string> expected_names;
    if (is_name)
    {
      const auto max = edit.field == "PFNA" ? 10u : 13u;
      if (!replacement || !NativeNameValid(*replacement, max) || edit.string_expected.empty())
        return reject("invalid name replacement");
      for (const auto& original : edit.string_expected)
      {
        const auto text = GuestText(original);
        if (!text || !NativeNameValid(*text, max) || !expected_names.insert(*text).second)
          return reject("invalid original name");
      }
    }
    std::uint32_t offset = 0, width = 0;
    auto tag = edit.field;
    std::reverse(tag.begin(), tag.end());
    for (std::uint32_t i = 0; i < *memory.UInt(*table + 0x4a, 1); ++i)
    {
      const auto field = *table + 0x4c + i * 16;
      if (memory.Tag(field + 8) == tag && memory.UInt(field) == (is_name ? 0u : 3u))
      {
        offset = *memory.UInt(field + 4);
        width = *memory.UInt(field + 12);
      }
    }
    if ((is_name && (offset != (edit.field == "PFNA" ? 0u : 88u) ||
                     width != (edit.field == "PFNA" ? 88u : 112u))) ||
        (edit.field == "PBPO" && (offset != 638 || width != 4)))
      return reject("unsupported name or role descriptor");
    // PPVL is virtual: accept only the audited GN7E69-r0 packed padding.
    // Native descriptors, record size, and anchors must still match exactly.
    if (edit.field == "PPVL")
    {
      bool pgid = false, ppss = false, pcov = false, prty = false, disjoint = true;
      for (std::uint32_t i = 0; i < *memory.UInt(*table + 0x4a, 1); ++i)
      {
        const auto f = *table + 0x4c + i * 16;
        const auto off = *memory.UInt(f + 4), bits = *memory.UInt(f + 12);
        const auto name = memory.Tag(f + 8);
        pgid |= name == "DIGP" && off == 461 && bits == 15;
        ppss |= name == "SSPP" && off == 708 && bits == 7;
        pcov |= name == "VOCP" && off == 763 && bits == 7;
        prty |= name == "YTRP" && off == 788 && bits == 2;
        disjoint &= off <= 790 && bits <= 790 - off;
      }
      if (stride != 100 || !pgid || !ppss || !pcov || !prty || !disjoint)
        return reject("unsupported PPVL padding layout");
      offset = 790;
      width = 7;
    }
    if (!is_name && (!width || width > 16 || edit.value >= (std::uint32_t{1} << width)))
      return reject("missing or unsupported rating descriptor");
    std::set<std::uint32_t> originals;
    std::set<std::string> original_names;
    for (std::size_t i = 0; i < rows->size(); ++i)
    {
      const auto& row = (*rows)[i];
      const auto row_first = String(row, "ANFP"), row_last = String(row, "ANLP");
      if (!row_first || !row_last)
        return reject("missing roster identity fields");
      // Blank unused identities cannot match a requested player.
      if (Name(*row_first) != first || Name(*row_last) != last)
        continue;
      if (is_name)
      {
        original_names.insert(edit.field == "PFNA" ? *row_first : *row_last);
        for (std::uint32_t byte = 0; byte < width / 8; ++byte)
        {
          const auto address = buffer + physical[i] * stride + offset / 8 + byte;
          writes[address] = byte < replacement->size() ?
              static_cast<std::uint8_t>((*replacement)[byte]) : 0;
        }
        continue;
      }
      auto value = Integer(row, tag);
      if (edit.field == "PPVL")
      {
        const auto bytes = *memory.Bytes(buffer + physical[i] * stride, stride);
        value = 0;
        for (std::uint32_t bit = 0; bit < width; ++bit)
          *value = (*value << 1) | ((bytes[(offset + bit) / 8] >> (7 - (offset + bit) % 8)) & 1);
      }
      if (!value || *value < 0)
        return reject("invalid original rating");
      originals.insert(static_cast<std::uint32_t>(*value));
      for (std::uint32_t bit = 0; bit < width; ++bit)
      {
        const auto address = buffer + physical[i] * stride + (offset + bit) / 8;
        auto [it, inserted] = writes.emplace(address, (*memory.Bytes(address, 1))[0]);
        (void)inserted;
        const auto mask = static_cast<std::uint8_t>(1u << (7 - (offset + bit) % 8));
        it->second = static_cast<std::uint8_t>((it->second & ~mask) |
            (((edit.value >> (width - 1 - bit)) & 1) ? mask : 0));
      }
    }
    if (is_name ? original_names != expected_names :
        originals != std::set<std::uint32_t>(edit.expected.begin(), edit.expected.end()))
      return reject("player absent or original ratings differ: " + edit.first + " " + edit.last);
  }
  // Distinct original identities must not converge after any combination of
  // first/last changes. Match all edits against the unmodified snapshot above.
  std::map<std::pair<std::string, std::string>, std::pair<std::string, std::string>> final_names;
  for (const auto& row : *rows)
  {
    const auto a = String(row, "ANFP"), b = String(row, "ANLP");
    if (!a || !b) return reject("missing roster identity fields");
    const auto original = std::make_pair(Name(*a), Name(*b));
    if (original.first.empty() && original.second.empty()) continue;
    auto result = original;
    for (const auto& edit : edits)
    {
      const auto first = GuestText(edit.first), last = GuestText(edit.last);
      if (first && last && std::make_pair(Name(*first), Name(*last)) == original &&
          (edit.field == "PFNA" || edit.field == "PLNA"))
      {
        const auto value = GuestText(edit.string_value);
        if (!value) return reject("invalid replacement encoding");
        (edit.field == "PFNA" ? result.first : result.second) = Name(*value);
      }
    }
    if (result.first.empty() && result.second.empty()) return reject("empty final identity");
    const auto [it, inserted] = final_names.emplace(result, original);
    if (!inserted && it->second != original) return reject("renamed player identity collision");
  }
  return writes;
}
// Immutable, agreed boot overrides; host lookup never depends on simulation
// history. An absent/zero split inherits the live native cache (and modifiers).
using SplitRatings = std::map<std::pair<std::string, std::string>, std::array<std::uint32_t, 2>>;

// Collect only requested split fields; validation remains atomic in Plan.
inline SplitRatings Splits(const std::vector<Edit>& edits)
{
  SplitRatings result;
  for (const auto& edit : edits)
  {
    if (edit.field != "PPVL" && edit.field != "PCEL") continue;
    const auto first = GuestText(edit.first), last = GuestText(edit.last);
    if (!first || !last) continue;  // Plan rejects malformed identities.
    const auto original = std::make_pair(Name(*first), Name(*last));
    auto final = original;
    for (const auto& rename : edits)
    {
      const auto a = GuestText(rename.first), b = GuestText(rename.last);
      const auto value = GuestText(rename.string_value);
      if (a && b && value && std::make_pair(Name(*a), Name(*b)) == original)
      {
        if (rename.field == "PFNA") final.first = Name(*value);
        if (rename.field == "PLNA") final.second = Name(*value);
      }
    }
    result[final][edit.field == "PPVL" ? 0 : 1] = edit.value;
  }
  return result;
}

// GN7E69-r0 player names are fixed-size NUL-terminated metadata. Bounded reads
// make bad pointers harmless; exact normalized names avoid hash collisions.
inline std::uint32_t SplitValue(const NSRGameStats::Memory& view, std::uint32_t player,
                                bool velocity, const SplitRatings& splits,
                                std::uint32_t fallback)
{
  if (!view.Bytes(player, 0xbcc))
    return fallback;
  const auto text = [&](std::uint32_t offset, std::uint32_t size) {
    const auto bytes = *view.Bytes(player + offset, size);
    const auto end = std::find(bytes.begin(), bytes.end(), 0);
    return end == bytes.end() ? std::string{} : Name(std::string(bytes.begin(), end));
  };
  const auto found = splits.find({text(0xb6f, 21), text(0xb84, 25)});
  if (found == splits.end() || found->second[velocity ? 0 : 1] == 0)
    return fallback;
  // PCEL is read from the live native metadata cache, not from PSPD. The
  // immutable map only opts this player in; unedited stock PCEL stays inert.
  const auto rating = velocity ? found->second[0] : view.UInt(player + 0xb66, 1).value_or(0);
  return std::min(rating, 99u) * 255 / 99;
}

}
