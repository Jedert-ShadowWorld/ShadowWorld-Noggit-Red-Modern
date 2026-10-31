// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace Noggit::Format
{
  inline void validateRetailWmoGroup(std::span<char const> group)
  {
    if (group.size() < 88 || std::memcmp(group.data(), "REVM", 4) != 0
        || std::memcmp(group.data() + 12, "PGOM", 4) != 0)
      throw std::runtime_error("Invalid Retail WMO group MVER/MOGP header.");
    std::uint32_t versionSize, version, groupSize;
    std::memcpy(&versionSize, group.data() + 4, 4);
    std::memcpy(&version, group.data() + 8, 4);
    std::memcpy(&groupSize, group.data() + 16, 4);
    if (versionSize != 4 || version != 17 || groupSize < 68 || groupSize > group.size() - 20)
      throw std::runtime_error("Invalid Retail WMO group version or MOGP size.");
  }

  // GFID is grouped by LOD: the first MOHD.groupCount IDs are the primary mesh.
  // Keep primary geometry for editing, picking, bounds and tile references.
  inline std::vector<std::uint32_t> primaryWmoGroupIds(std::span<char const> root)
  {
    auto read32 = [&](std::size_t at) {
      if (at > root.size() || root.size() - at < 4)
        throw std::runtime_error("Truncated Retail WMO header.");
      std::uint32_t value;
      std::memcpy(&value, root.data() + at, 4);
      return value;
    };
    if (root.size() < 84 || std::memcmp(root.data(), "REVM", 4) != 0
        || read32(4) != 4 || read32(8) != 17
        || std::memcmp(root.data() + 12, "DHOM", 4) != 0 || read32(16) < 64)
      throw std::runtime_error("Invalid Retail WMO MVER/MOHD header.");
    auto const count = read32(24);
    bool found = false;
    std::vector<std::uint32_t> ids;
    for (std::size_t at = 0; at < root.size();)
    {
      if (root.size() - at < 8)
        throw std::runtime_error("Truncated Retail WMO chunk header.");
      auto size = read32(at + 4);
      auto payload = at + 8;
      if (size > root.size() - payload)
        throw std::runtime_error("Truncated Retail WMO chunk payload.");
      if (std::memcmp(root.data() + at, "DIFG", 4) == 0)
      {
        if (found || size % 4 || (count == 0 ? size != 0 : size / 4 < count || (size / 4) % count != 0))
          throw std::runtime_error("Invalid Retail WMO GFID group count.");
        found = true;
        ids.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
          auto id = read32(payload + static_cast<std::size_t>(i) * 4);
          if (!id) throw std::runtime_error("Retail WMO GFID has a zero primary group FileDataID.");
          ids.push_back(id);
        }
      }
      at = payload + size;
    }
    return ids;
  }
}
