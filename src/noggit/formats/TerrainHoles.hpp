// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <cstdint>

namespace Noggit::TerrainHoles
{
  constexpr std::uint64_t cell_block(int x, int y)
  {
    return std::uint64_t{0x303} << (y * 16 + x * 2);
  }

  constexpr std::uint16_t coarse_mask(std::uint64_t fine)
  {
    std::uint16_t coarse = 0;
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        if (fine & cell_block(x, y))
          coarse |= std::uint16_t{1} << (y * 4 + x);
    return coarse;
  }

  // The editor/undo stack edits 4x4 holes. Match the Retail save codec's delta
  // rule: untouched blocks retain their exact 8x8 cells; changed blocks fill
  // or clear all four cells. The baseline advances after successful saves.
  // Starting from zero also expands legacy masks.
  constexpr std::uint64_t apply_coarse_edits(std::uint64_t original, std::uint16_t edited)
  {
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
      {
        auto const block = cell_block(x, y);
        bool const was_hole = (original & block) != 0;
        bool const is_hole = (edited & (std::uint16_t{1} << (y * 4 + x))) != 0;
        if (was_hole != is_hole)
          original = is_hole ? original | block : original & ~block;
      }
    return original;
  }
}
