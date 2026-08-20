// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Shadowlands split-ADT codec boundary.
// The design follows the verified split ADT/semantic round-trip model documented
// by skarndev/wowlib, while remaining C++20/MSVC compatible with Noggit.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Noggit::Formats::ADT
{
  enum class ShadowlandsADTPart : std::uint8_t
  {
    Root = 0,
    Tex0,
    Obj0,
    Obj1,
    Lod,
    Count
  };

  struct ShadowlandsADTSource
  {
    std::string root_path;
    std::array<std::string, static_cast<std::size_t>(ShadowlandsADTPart::Count)> paths;
  };

  struct ShadowlandsADTRawPart
  {
    bool present = false;
    std::vector<std::uint8_t> bytes;
  };

  // Raw backing store for a single logical Shadowlands ADT tile.  Keeping all
  // split parts here is deliberate: until a field has a native writer, its
  // original bytes can be preserved instead of being silently dropped by the
  // legacy WotLK serializer.
  struct ShadowlandsADTBackingStore
  {
    ShadowlandsADTSource source;
    std::array<ShadowlandsADTRawPart, static_cast<std::size_t>(ShadowlandsADTPart::Count)> parts;

    [[nodiscard]] bool has(ShadowlandsADTPart part) const noexcept;
    [[nodiscard]] ShadowlandsADTRawPart const& part(ShadowlandsADTPart part) const noexcept;
    [[nodiscard]] ShadowlandsADTRawPart& part(ShadowlandsADTPart part) noexcept;
  };

  class ShadowlandsADTCodec
  {
  public:
    [[nodiscard]] static ShadowlandsADTSource makeSource(std::string const& root_path);

    // Loads the physical split files through Noggit's ClientData/CASC gateway.
    // ROOT, TEX0 and OBJ0 are required for the current Shadowlands terrain
    // pipeline; OBJ1 and LOD are retained when present.
    [[nodiscard]] static ShadowlandsADTBackingStore loadBackingStore(std::string const& root_path);
  };
}
