// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/formats/TerrainHoles.hpp>

#include <cstdint>
#include <iostream>

int main()
{
  using namespace Noggit::TerrainHoles;
  // Exercise every fine cell, including both signed GPU words and their seam.
  for (int cell = 0; cell < 64; ++cell)
  {
    auto const original = std::uint64_t{1} << cell;
    int const coarse_cell = (cell / 16) * 4 + (cell % 8) / 2;
    auto const coarse = std::uint16_t{1} << coarse_cell;
    if (coarse_mask(original) != coarse
        || apply_coarse_edits(original, coarse) != original
        || apply_coarse_edits(original, 0) != 0)
    {
      std::cerr << "FAIL: fine hole load, no-edit preview or close at cell " << cell << '\n';
      return 1;
    }
    for (int edit = 0; edit < 16; ++edit)
    {
      auto const preview = apply_coarse_edits(original, coarse | (1u << edit));
      for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
          int const neighbor = y * 8 + x;
          bool const expected = neighbor == cell
            || (edit != coarse_cell && (y / 2) * 4 + x / 2 == edit);
          if (bool(preview & (std::uint64_t{1} << neighbor)) != expected)
          {
            std::cerr << "FAIL: coarse edit expands an untouched partial block.\n";
            return 1;
          }
        }
      // Undo changes the editor mask, not the retained source mask.
      if (apply_coarse_edits(original, coarse) != original)
        return 1;
    }
  }

  // Saving advances the mask used by the next codec rewrite. After clearing
  // and saving a partial block, undo/repaint of its coarse bit opens all four
  // cells, matching the fresh ROOT rather than restoring the load-time mask.
  auto baseline = std::uint64_t{1};
  baseline = apply_coarse_edits(baseline, 1); // No-edit save retains the detail.
  if (baseline != 1)
    return 1;
  baseline = apply_coarse_edits(baseline, 0); // Successful clear/save.
  if (baseline != 0 || apply_coarse_edits(baseline, 1) != 0x303)
  {
    std::cerr << "FAIL: saved baseline for undo/repaint.\n";
    return 1;
  }
  baseline = apply_coarse_edits(baseline, 1); // Save the reopened block.
  if (baseline != 0x303 || apply_coarse_edits(baseline, 0) != 0
      || apply_coarse_edits(baseline, 1) != 0x303)
    return 1;

  // Legacy 4x4 masks on Retail must still remove exactly four fine cells each.
  for (std::uint32_t coarse = 0; coarse <= 0xffff; ++coarse)
  {
    auto const expanded = apply_coarse_edits(0, static_cast<std::uint16_t>(coarse));
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x)
        if (bool(expanded & (std::uint64_t{1} << (y * 8 + x)))
            != bool(coarse & (1u << ((y / 2) * 4 + x / 2))))
        {
          std::cerr << "FAIL: legacy hole expansion.\n";
          return 1;
        }
  }
  std::cout << "PASS: exact 8x8 masks, coarse edits/undo and all legacy masks.\n";
  return 0;
}
