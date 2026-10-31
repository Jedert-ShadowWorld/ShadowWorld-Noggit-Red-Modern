// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Noggit::Rendering
{
  struct RetailM2Shader
  {
    int pixel;
    std::array<int, 3> uv; // environment=0, UV1=1, UV2=2, unused=3
    unsigned textures;
    bool edgeFade = false;
  };

  // Validate the complete stage-to-resource chain before a batch can draw.
  // A failed bind must never reuse a texture left by an unrelated previous model.
  template<class HasReplacement>
  bool validRetailM2Textures(std::uint16_t combo, unsigned count,
      std::vector<std::uint16_t> const& lookup, std::vector<int> const& special,
      std::size_t textures, HasReplacement hasReplacement)
  {
    if (!count || count > 3 || std::uint32_t(combo) + count > 65536u
        || combo > lookup.size() || count > lookup.size() - combo)
      return false;
    for (unsigned stage = 0; stage < count; ++stage)
    {
      auto const texture = lookup[std::size_t(combo) + stage];
      if (texture >= special.size()) return false;
      if (special[texture] == -1)
      {
        if (texture >= textures) return false;
      }
      else if (special[texture] < 0 || !hasReplacement(static_cast<std::size_t>(special[texture])))
        return false;
    }
    return true;
  }

  // Authored high-bit SKIN shader table, matching wow.export ShaderMapper.
  // Attribution/license: docs/third-party/wow.export-MIT.txt.
  // IDs requiring unimplemented inputs are explicit, never a generic Mod fallback.
  inline std::optional<RetailM2Shader> retailM2Shader(std::uint16_t shader)
  {
    if (!(shader & 0x8000)) return std::nullopt;
    auto const id = shader & 0x7fff;
    RetailM2Shader result{100 + id, {1, 1, 3}, 2};
    switch (id)
    {
      case 0: case 1: case 2: case 4: case 7: case 8: case 11: case 12: case 13: case 16:
        result.uv = {1, 0, 3}; break;
      case 3: case 9: case 17:
        result.uv = {1, 0, 1}; result.textures = 3; break;
      case 5: case 6: case 10: case 14: case 23:
        break;
      case 15: case 27:
        result.uv = {1, 2, 3}; break;
      case 24:
        result.uv = {1, 0, 2}; result.textures = 3; break;
      case 21: case 33:
        result.uv = {1, 2, 3}; result.edgeFade = true; break;
      case 26: case 28:
        result.uv = {1, 2, 1}; result.textures = 3; break;
      case 30:
        result.uv = {1, 2, 2}; result.textures = 3; break;
      case 32:
        result.uv = {1, 3, 3}; result.textures = 1; break;
      case 34:
        result.uv = {1, 3, 3}; result.textures = 1; result.edgeFade = true; break;
      default: return std::nullopt;
    }
    return result;
  }
}
