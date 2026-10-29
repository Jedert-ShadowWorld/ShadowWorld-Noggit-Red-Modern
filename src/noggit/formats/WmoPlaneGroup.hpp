// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace Noggit::Format
{
  // Retail can store a non-rendered group as MOPL planes without a triangle mesh.
  // Accept only this layout; missing mesh chunks in other groups remain errors.
  inline bool isPlaneOnlyGroup(bool retail, std::uint32_t flags, unsigned batches,
                               std::span<char const> chunks)
  {
    constexpr std::uint32_t planeGroup = 0x08000080; // plane group and unreachable
    if (!retail || (flags & planeGroup) != planeGroup || batches || chunks.size() < 24)
      return false;
    if (std::memcmp(chunks.data(), "LPOM", 4) != 0) return false;
    std::uint32_t size = 0;
    std::memcpy(&size, chunks.data() + 4, 4);
    if (!size || size % 16 || size != chunks.size() - 8) return false;
    for (std::size_t offset = 8; offset < chunks.size(); offset += 16)
    {
      float plane[4];
      std::memcpy(plane, chunks.data() + offset, sizeof(plane));
      for (float component : plane) if (!std::isfinite(component)) return false;
      if (plane[0] == 0.f && plane[1] == 0.f && plane[2] == 0.f) return false;
    }
    return true;
  }
}
