// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <cstddef>
#include <optional>

namespace Noggit::Format
{
  struct WmoColorStreams
  {
    std::optional<std::size_t> lighting;
    std::optional<std::size_t> blending;
  };

  // MOCV occurrence order follows MOGP flags: CVERTS2 can be the sole stream.
  inline WmoColorStreams wmo_color_streams(bool lighting, bool blending, std::size_t count)
  {
    WmoColorStreams result;
    if (lighting && count > 0) result.lighting = 0;
    std::size_t const blend_index = lighting ? 1 : 0;
    if (blending && count > blend_index) result.blending = blend_index;
    return result;
  }
}
