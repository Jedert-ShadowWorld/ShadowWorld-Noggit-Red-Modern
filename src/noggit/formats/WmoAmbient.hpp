// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>
#include <glm/vec3.hpp>

namespace Noggit::Format
{
  struct WmoAmbientVolume
  {
    std::array<glm::vec3, 3> colors; // sky, horizon, ground
    std::uint16_t doodad_set;
  };

  // MAVG/MAVD's 48-byte record contains BGRA ambient colors at offsets 20..31,
  // flags at 32, and a doodad-set selector at 36. The renderer selects MAVG by
  // active set, with the first MAVD record as the fallback when MAVG is absent.
  inline std::vector<WmoAmbientVolume> read_wmo_ambient_volumes(std::span<std::uint8_t const> bytes)
  {
    constexpr std::size_t record_size = 48;
    if (bytes.size() % record_size != 0)
      throw std::runtime_error("Invalid WMO ambient volume record size.");

    std::vector<WmoAmbientVolume> records;
    records.reserve(bytes.size() / record_size);
    for (std::size_t offset = 0; offset < bytes.size(); offset += record_size)
    {
      auto const record = bytes.subspan(offset, record_size);
      auto color = [&](unsigned at)
      {
        return glm::vec3(record[at + 2], record[at + 1], record[at]) / 255.f;
      };
      auto const sky = color(20);
      bool const directional = (record[32] & 1) != 0;
      records.push_back({{sky, directional ? color(24) : sky, directional ? color(28) : sky},
          std::uint16_t(record[36] | (std::uint16_t(record[37]) << 8))});
    }
    return records;
  }

  inline std::array<glm::vec3, 3> wmo_ambient_colors(std::span<WmoAmbientVolume const> records,
      std::span<std::uint16_t const> selected_sets, glm::vec3 const& fallback)
  {
    if (records.empty()) return {fallback, fallback, fallback};

    // DefaultGlobal is always active. Keep authored record order when several
    // selected sets have ambient records, as with the visible doodad sets.
    for (auto const& record : records)
      if (record.doodad_set == 0 || std::find(selected_sets.begin(), selected_sets.end(), record.doodad_set) != selected_sets.end())
        return record.colors;
    return records.front().colors;
  }
}
