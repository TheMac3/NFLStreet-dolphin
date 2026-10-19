// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Read-only GN7E69 rev-0 match database decoder. This deliberately has no emulator,
// filesystem, network or game-call dependency: malformed pointers/packed fields
// fail closed, and synthetic RAM can exercise the same reader as a live capture.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace NSRGameStats
{
using Value = std::variant<std::int64_t, std::string>;
using Row = std::map<std::string, Value>;

// Only the two audited direct memory mappings are readable. Range subtraction
// avoids guest address overflow, wrapping MEM1 masks and accidental MMIO reads.
struct Memory
{
  std::span<const std::uint8_t> ram;
  std::span<const std::uint8_t> vmem;

  std::optional<std::span<const std::uint8_t>> Bytes(std::uint32_t address,
                                                   std::uint32_t size) const
  {
    const auto region = address >= 0x80000000 ? ram : vmem;
    const std::uint32_t base = address >= 0x80000000 ? 0x80000000 : 0x7e000000;
    if (address < base || address - base > region.size() ||
        size > region.size() - (address - base))
      return std::nullopt;
    return region.subspan(address - base, size);
  }

  // All target structures and bit-packed records are big endian.
  std::optional<std::uint32_t> UInt(std::uint32_t address, std::uint32_t size = 4) const
  {
    if (size == 0 || size > 4)
      return std::nullopt;
    const auto bytes = Bytes(address, size);
    if (!bytes)
      return std::nullopt;
    std::uint32_t value = 0;
    for (const auto byte : *bytes)
      value = (value << 8) | byte;
    return value;
  }

  // Identifiers in the runtime database retain their on-disc reversed spelling.
  std::optional<std::string> Tag(std::uint32_t address) const
  {
    const auto bytes = Bytes(address, 4);
    if (!bytes)
      return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
  }
};

// Decode exactly the named integer; callers decide whether an absent event row
// means zero. An absent column or a wrong type must never silently become zero.
inline std::optional<std::int64_t> Integer(const Row& row, std::string_view name)
{
  const auto it = row.find(std::string(name));
  if (it == row.end())
    return std::nullopt;
  if (const auto* value = std::get_if<std::int64_t>(&it->second))
    return *value;
  return std::nullopt;
}

// Strings are byte aligned in EA DB records. JSON encoding handles the game's
// Latin-1 separately; this reader preserves those source bytes without locale IO.
inline std::optional<std::string> String(const Row& row, std::string_view name)
{
  const auto it = row.find(std::string(name));
  if (it == row.end())
    return std::nullopt;
  if (const auto* value = std::get_if<std::string>(&it->second))
    return *value;
  return std::nullopt;
}

// Sparse event tables may omit a player, but duplicate IDs are malformed, not
// an absent event. Validate once before resolving absent rows as zero counters.
inline bool UniqueIntegerKeys(const std::vector<Row>& rows, std::string_view key)
{
  std::set<std::int64_t> seen;
  for (const auto& row : rows)
  {
    const auto value = Integer(row, key);
    if (!value || !seen.insert(*value).second)
      return false;
  }
  return true;
}

// Locate a table by database and table tags, never by the allocation address.
// GN7E69 rev 0: fn_801F88F0 walks root *(r13-0x6418), database next/+4/+8,
// then table next +4 and tag +0x1C. Limits also reject cycles/stale teardown data.
inline std::optional<std::uint32_t> FindTable(const Memory& memory, std::string_view database,
                                             std::string_view table)
{
  auto db = memory.UInt(0x803ebe88);
  for (unsigned i = 0; db && *db && i < 64; ++i)
  {
    if (!memory.Bytes(*db, 0x14))
      return std::nullopt;
    if (memory.Tag(*db + 8) == database)
    {
      auto current = memory.UInt(*db + 4);
      for (unsigned j = 0; current && *current && j < 256; ++j)
      {
        if (!memory.Bytes(*current, 0x4c))
          return std::nullopt;
        if (memory.Tag(*current + 0x1c) == table)
          return current;
        current = memory.UInt(*current + 4);
      }
      return std::nullopt;
    }
    db = memory.UInt(*db);
  }
  return std::nullopt;
}

// Runtime tables own a separate record buffer (+0x14), unlike on-disc EA DB.
// fn_8020C034 proves stride +0x20, high-water +0x44, and the deleted-record bit
// at +0x24. Field descriptors at +0x4C are (type, bit offset, tag, bit width).
// Only integer/string columns are needed for stats. Floating columns are skipped
// rather than misrepresented as integers (GAME.GINF contains float settings).
inline std::optional<std::vector<Row>> ReadTable(const Memory& memory, std::string_view database,
                                                std::string_view name,
                                                std::span<const std::string_view> integers = {})
{
  const auto table = FindTable(memory, database, name);
  if (!table)
    return std::nullopt;
  const auto stride = memory.UInt(*table + 0x20);
  const auto deleted_bit = memory.UInt(*table + 0x24);
  const auto capacity = memory.UInt(*table + 0x40, 2);
  const auto allocated = memory.UInt(*table + 0x42, 2);
  const auto count = memory.UInt(*table + 0x44, 2);
  const auto field_count = memory.UInt(*table + 0x4a, 1);
  const auto buffer = memory.UInt(*table + 0x14);
  if (!stride || !deleted_bit || !capacity || !allocated || !count || !field_count || !buffer ||
      *stride == 0 || *stride > 4096 || *capacity > 4096 || *allocated > *capacity ||
      *count > *allocated || *field_count == 0 || *field_count > 128 ||
      *deleted_bit >= *stride * 8 || !memory.Bytes(*table, 0x4c + *field_count * 16) ||
      (*count && !memory.Bytes(*buffer, *count * *stride)))
    return std::nullopt;

  struct Field
  {
    std::string tag;
    std::uint32_t type, offset, width;
  };
  std::vector<Field> fields;
  for (std::uint32_t i = 0; i < *field_count; ++i)
  {
    const auto address = *table + 0x4c + i * 16;
    Field field{*memory.Tag(address + 8), *memory.UInt(address), *memory.UInt(address + 4),
                *memory.UInt(address + 12)};
    if (field.type > 4 || field.width == 0 || field.offset > *stride * 8 ||
        field.width > *stride * 8 - field.offset ||
        (field.type == 0 && (field.offset % 8 || field.width % 8)) ||
        ((field.type == 2 || field.type == 3) && field.width > 32))
      return std::nullopt;
    for (const auto& previous : fields)
    {
      if (field.tag == previous.tag ||
          (field.offset < previous.offset + previous.width &&
           previous.offset < field.offset + field.width))
        return std::nullopt;
    }
    fields.emplace_back(std::move(field));
  }

  // Even an empty event table must have its expected integer columns. Otherwise
  // an unsupported schema could masquerade as a legitimate zero-event game.
  for (const auto required : integers)
  {
    bool found = false;
    for (const auto& field : fields)
      found |= field.tag == required && (field.type == 2 || field.type == 3);
    if (!found)
      return std::nullopt;
  }

  std::vector<Row> rows;
  for (std::uint32_t i = 0; i < *count; ++i)
  {
    const auto bytes = *memory.Bytes(*buffer + i * *stride, *stride);
    if ((bytes[*deleted_bit / 8] >> (7 - *deleted_bit % 8)) & 1)
      continue;
    Row row;
    for (const auto& field : fields)
    {
      if (field.type == 0)
      {
        std::string value;
        for (std::uint32_t j = 0; j < field.width / 8; ++j)
        {
          const auto byte = bytes[field.offset / 8 + j];
          if (!byte)
            break;
          value.push_back(static_cast<char>(byte));
        }
        row.emplace(field.tag, std::move(value));
      }
      else if (field.type == 2 || field.type == 3)
      {
        std::uint32_t value = 0;
        for (std::uint32_t j = 0; j < field.width; ++j)
        {
          const auto bit = field.offset + j;
          value = (value << 1) | ((bytes[bit / 8] >> (7 - bit % 8)) & 1);
        }
        std::int64_t signed_value = value;
        if (field.type == 2 && (value & (std::uint32_t{1} << (field.width - 1))))
          signed_value -= std::int64_t{1} << field.width;
        row.emplace(field.tag, signed_value);
      }
    }
    rows.emplace_back(std::move(row));
  }
  return rows;
}
}  // namespace NSRGameStats
