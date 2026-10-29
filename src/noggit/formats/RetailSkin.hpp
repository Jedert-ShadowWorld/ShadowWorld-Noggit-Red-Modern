// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <array>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace Noggit::Format
{
  // SFID starts with nViews bone-partition skins. Only trailing entries are
  // authored geometry LODs; missing entries request the primary mesh fallback.
  inline std::array<std::uint32_t, 2> previewSkinFileIds(std::vector<std::uint32_t> const& ids, std::size_t views)
  {
    if (views == 0 || views >= ids.size())
      return {0, 0};
    return {ids[views], ids.back()};
  }

  // Retail SKIN stores the high 16 bits of triangleStart in the section level.
  // Layout reference: wow.export Skin.js (see docs/third-party/wow.export-MIT.txt).
  inline std::uint32_t skinIndexStart(std::uint16_t low, std::uint16_t level, bool retail)
  {
    return low + (retail ? (static_cast<std::uint32_t>(level) << 16) : 0u);
  }

  inline void validateSkinRange(std::uint32_t start, std::uint32_t count, std::size_t total)
  {
    if (start > total || count > total - start)
      throw std::runtime_error("Retail SKIN section references data outside its array.");
  }
}
