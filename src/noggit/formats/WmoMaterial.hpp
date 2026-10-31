// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <cstdint>
#include <cstddef>
namespace Noggit::Format
{
  // Extended MOBA material selectors are independent of the client target.
  // Keep loading, readiness checks and batch packing on the same fallback.
  template<typename Batch>
  constexpr std::size_t wmo_material_index(Batch const& batch, bool modern, std::size_t count)
  {
    auto const index = modern && (batch.flags & 2) ? batch.unused[5] : batch.texture;
    return index < count ? index : 0;
  }

  // MOMT texture references, before the last two on-disk fields become runtime aliases.
  constexpr unsigned wmo_texture_count(std::uint32_t shader, bool retail)
  {
    if (retail)
    {
      switch (shader)
      {
      case 23: return 9;
      case 7: case 11: case 12: case 17: case 18: case 20: return 3;
      case 3: case 5: case 6: case 8: case 9: case 13: case 15: case 19: return 2;
      default: return 1; // 10/14 unsupported; 22 retains diffuse preview, not parallax.
      }
    }
    return shader == 3 || shader == 5 || shader == 6 || shader == 21 || shader == 23 ? 2 : 1;
  }
}
