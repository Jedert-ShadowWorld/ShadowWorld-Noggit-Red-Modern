// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Noggit::DetailDoodads
{
  inline std::optional<std::uint32_t> fileDataId(std::string_view value, bool retail)
  {
    if (!retail || value.substr(0, 11) != "filedataid:") return std::nullopt;
    value.remove_prefix(11);
    std::uint32_t id = 0;
    auto const result = std::from_chars(value.data(), value.data() + value.size(), id);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !id)
      throw std::runtime_error("Invalid ground effect FileDataID.");
    return id;
  }

  inline std::string modelReference(std::string name, bool retail)
  {
    if (fileDataId(name, retail)) return name;
    std::string path = "world/nodxt/detail/" + name;
    if (path.size() > 4)
    {
      auto ext = path.substr(path.size() - 4);
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
      if (ext == ".mdx" || ext == ".mdl") path.replace(path.size() - 4, 4, ".m2");
    }
    return path;
  }
}
