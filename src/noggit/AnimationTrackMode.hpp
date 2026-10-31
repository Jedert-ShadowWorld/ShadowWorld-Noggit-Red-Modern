// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <cstdint>
#include <map>
#include <vector>
namespace Animation
{
  struct SequenceMode { std::int16_t interpolation; std::int16_t globalSequence; };
  using TrackModes = std::map<std::uint32_t, std::vector<SequenceMode>>;
  inline std::vector<SequenceMode> const& modesFor(TrackModes const& modes, std::uint32_t offset)
  {
    static std::vector<SequenceMode> const empty;
    auto it = modes.find(offset); return it == modes.end() ? empty : it->second;
  }
}
