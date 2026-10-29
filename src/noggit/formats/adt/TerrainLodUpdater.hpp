// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Noggit::Formats::ADT
{
  // Updates Retail LOD terrain heights and fills cell-aligned LOD openings that
  // are solid in ROOT. The reference root determines the height orientation and
  // liquid baseline. Liquid meshes, depth, visibility and bounds are rebuilt from
  // MH2O. Existing terrain and active holes remain; MLND/MLLL ranges and border
  // skirts are adjusted when triangles are added to any of the four LOD levels.
  class TerrainLodUpdater
  {
  public:
    // Throws std::runtime_error for invalid data, uncertain orientation, or
    // newly cut holes or unsupported legacy/PBR liquid representations.
    // diagnostics receives all four orientation scores, including on rejection.
    [[nodiscard]] static std::vector<std::uint8_t> update(
      std::vector<std::uint8_t> const& reference_root,
      std::vector<std::uint8_t> const& original_lod,
      std::vector<std::uint8_t> const& updated_root,
      std::string* diagnostics = nullptr);
  };
}
