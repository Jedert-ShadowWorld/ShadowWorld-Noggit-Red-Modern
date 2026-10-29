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
#include <map>
#include "RetailObjectTransfer.hpp"

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
    // Rebuilds OBJ1 from MDDF/MODF records with per-UID world bounds (Retail UID repair).
    static std::vector<std::uint8_t> repairObjectLod(
      std::vector<std::uint8_t> const& original,
      std::vector<std::uint8_t> const& mddf, std::vector<std::uint8_t> const& modf,
      std::map<std::uint32_t, std::array<float, 6>> const& worldBounds);
    [[nodiscard]] static ShadowlandsADTSource makeSource(std::string const& root_path);

    // Loads the physical split files through Noggit's ClientData/CASC gateway.
    // ROOT, TEX0 and OBJ0 are required for the current Shadowlands terrain
    // pipeline; OBJ1 and LOD are retained when present.
    [[nodiscard]] static ShadowlandsADTBackingStore loadBackingStore(std::string const& root_path);

    // Retail only. Rebuilds LOD and OBJ1 liquid data from the saved ROOT/OBJ1;
    // ROOT/TEX0/OBJ0 are not written.
    static void rebuildDerived(std::string const& root_path);

    // Builds the required split parts for an explicitly new tile, without CASC reads.
    [[nodiscard]] static ShadowlandsADTBackingStore createBackingStore(
      std::string const& root_path, std::vector<std::uint8_t> const& legacy_adt);

    // Converts Noggit's canonical legacy serialization back into the modern
    // split layout while retaining chunks that the editor does not own.
    // Retail: also updates terrain LOD and returns whether it was updated; on
    // failure the existing LOD is kept and the primary parts are still saved.
    // Other clients keep their LOD bytes and return false.
    static bool saveFromLegacy(ShadowlandsADTBackingStore const& backing,
                               std::vector<std::uint8_t> const& legacy_adt,
                               RetailObjectTransfer::WmoSources const* wmo_sources = nullptr);
  };
}
