// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Noggit::Formats::ADT
{
  enum class TerrainLodLayout : std::uint8_t
  {
    YOuterZeroCorner,
    YOuterMaxCorner,
    XOuterZeroCorner,
    XOuterMaxCorner
  };

  struct TerrainLodReferenceInspection
  {
    // Layout confidence is not proof of template provenance or mesh validity.
    // A flat reference can be usable without identifying a unique orientation.
    bool usable = false;
    bool equivalent_layouts = false;
    std::optional<TerrainLodLayout> unique_layout;
    std::uint8_t candidate_layouts = 0;
    std::array<std::size_t, 4> matches{};
    std::size_t sample_count = 0;
    std::string diagnostics;
  };

  // Updates Retail LOD terrain heights, cuts current ROOT holes and fills LOD
  // openings that are solid in ROOT. The reference root determines orientation and
  // liquid baseline. Liquid meshes, depth, visibility and bounds are rebuilt from
  // MH2O. Repair patches retain unaffected triangles and use whole-cell borders;
  // MLND/MLLL ranges and skirts follow the mesh on all four LOD levels.
  // Height changes refine existing triangles within bounded per-level error and
  // triangle budgets; no new terrain grid vertices or active hole fills are used.
  class TerrainLodUpdater
  {
  public:
    // Malformed data throws. Low confidence or distinguishable tied layouts
    // return usable=false, allowing callers to inspect another reference pair.
    [[nodiscard]] static TerrainLodReferenceInspection inspectReference(
      std::vector<std::uint8_t> const& reference_root,
      std::vector<std::uint8_t> const& original_lod);

    // Reconstruct reference heights in a temporary current template when its
    // terrain indexing exactly matches a verified reference pair. Preserves all
    // current non-MLVH bytes; the result is input to update, never a saved asset.
    [[nodiscard]] static std::vector<std::uint8_t> calibrateReference(
      std::vector<std::uint8_t> const& reference_root,
      std::vector<std::uint8_t> const& reference_lod,
      std::vector<std::uint8_t> const& current_lod);

    // Throws std::runtime_error for invalid data, uncertain orientation, or
    // unsupported mesh boundaries or legacy/PBR liquid representations.
    // diagnostics receives all four orientation scores, including on rejection.
    // Explicit repair may also refine already height-updated, coarse meshes;
    // ordinary saves preserve unchanged authored topology.
    [[nodiscard]] static std::vector<std::uint8_t> update(
      std::vector<std::uint8_t> const& reference_root,
      std::vector<std::uint8_t> const& original_lod,
      std::vector<std::uint8_t> const& updated_root,
      std::string* diagnostics = nullptr,
      bool force_refinement = false);
  };
}
