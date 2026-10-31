// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace Noggit::Rendering
{
  inline bool hasDecalBasename(std::string_view path)
  {
    auto const slash = path.find_last_of("/\\");
    std::string name(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
    for (char& c : name)
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    return name.find("decal") != std::string::npos && name.find("nodecal") == std::string::npos;
  }

  // Batch projection flags are distinct from M2RenderFlags and alpha blending.
  // Layer-combined materials are kept intact unless the whole asset is a decal.
  inline bool isSurfaceDecal(std::uint8_t batchFlags, std::string_view modelPath,
                             std::size_t textureCount, std::string_view texturePath,
                             bool layeredGeometry = false)
  {
    if (hasDecalBasename(modelPath))
      return true;
    if (layeredGeometry)
      return false;
    return (batchFlags & 0x24u) != 0 || (textureCount == 1 && hasDecalBasename(texturePath));
  }

  inline int surfaceDecalMode(bool retail, bool exporting, int setting)
  {
    return retail && !exporting && setting >= 0 && setting <= 2 ? setting : 0;
  }
}
