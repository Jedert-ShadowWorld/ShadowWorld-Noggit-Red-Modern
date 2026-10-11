// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Noggit::Formats::ADT
{
  // Indexed texture tables must follow the rebuilt MDID, not the old tile order.
  inline std::vector<std::uint8_t> remapTextureMetadata(
    std::span<std::uint32_t const> old_ids, std::span<std::uint32_t const> new_ids,
    std::span<std::uint8_t const> records, std::span<std::uint8_t const> default_record)
  {
    auto const stride = default_record.size();
    if (!stride || records.size() % stride || records.size() / stride > old_ids.size())
      throw std::runtime_error("Modern ADT texture metadata has an invalid record count.");

    std::unordered_map<std::uint32_t, std::size_t> indices;
    for (std::size_t i = 0; i < records.size() / stride; ++i)
    {
      auto const [it, inserted] = indices.emplace(old_ids[i], i);
      if (!inserted && !std::equal(records.begin() + i * stride,
                                  records.begin() + (i + 1) * stride,
                                  records.begin() + it->second * stride))
        throw std::runtime_error("Modern ADT has conflicting metadata for a duplicate texture ID.");
    }

    std::vector<std::uint8_t> result;
    result.reserve(new_ids.size() * stride);
    for (auto id : new_ids)
    {
      auto const it = indices.find(id);
      auto const record = it == indices.end()
        ? default_record : records.subspan(it->second * stride, stride);
      result.insert(result.end(), record.begin(), record.end());
    }
    return result;
  }
}
