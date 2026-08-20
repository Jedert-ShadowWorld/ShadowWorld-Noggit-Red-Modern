// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/formats/adt/ShadowlandsADTCodec.hpp>

#include <noggit/application/NoggitApplication.hpp>

#include <ClientFile.hpp>
#include <Listfile.hpp>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace Noggit::Formats::ADT
{
  namespace
  {
    constexpr std::size_t part_index(ShadowlandsADTPart part) noexcept
    {
      return static_cast<std::size_t>(part);
    }

    std::string split_path(std::string const& root_path, char const* suffix)
    {
      auto const extension = root_path.rfind(".adt");
      if (extension == std::string::npos)
        return root_path + suffix;

      return root_path.substr(0, extension) + suffix + ".adt";
    }

    ShadowlandsADTRawPart read_part(std::string const& path, bool required)
    {
      try
      {
        BlizzardArchive::Listfile::FileKey key(path);
        BlizzardArchive::ClientFile file(
          key,
          Noggit::Application::NoggitApplication::instance()->clientData());

        ShadowlandsADTRawPart result;
        result.present = true;
        auto const size = file.getSize();
        auto const* buffer = reinterpret_cast<std::uint8_t const*>(file.getBuffer());
        result.bytes.assign(buffer, buffer + size);
        return result;
      }
      catch (...)
      {
        if (required)
          throw;
        return {};
      }
    }
  }

  bool ShadowlandsADTBackingStore::has(ShadowlandsADTPart part) const noexcept
  {
    return parts[part_index(part)].present;
  }

  ShadowlandsADTRawPart const& ShadowlandsADTBackingStore::part(ShadowlandsADTPart part) const noexcept
  {
    return parts[part_index(part)];
  }

  ShadowlandsADTRawPart& ShadowlandsADTBackingStore::part(ShadowlandsADTPart part) noexcept
  {
    return parts[part_index(part)];
  }

  ShadowlandsADTSource ShadowlandsADTCodec::makeSource(std::string const& root_path)
  {
    ShadowlandsADTSource source;
    source.root_path = root_path;
    source.paths[part_index(ShadowlandsADTPart::Root)] = root_path;
    source.paths[part_index(ShadowlandsADTPart::Tex0)] = split_path(root_path, "_tex0");
    source.paths[part_index(ShadowlandsADTPart::Obj0)] = split_path(root_path, "_obj0");
    source.paths[part_index(ShadowlandsADTPart::Obj1)] = split_path(root_path, "_obj1");
    source.paths[part_index(ShadowlandsADTPart::Lod)] = split_path(root_path, "_lod");
    return source;
  }

  ShadowlandsADTBackingStore ShadowlandsADTCodec::loadBackingStore(std::string const& root_path)
  {
    ShadowlandsADTBackingStore backing;
    backing.source = makeSource(root_path);

    backing.parts[part_index(ShadowlandsADTPart::Root)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Root)], true);
    backing.parts[part_index(ShadowlandsADTPart::Tex0)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Tex0)], true);
    backing.parts[part_index(ShadowlandsADTPart::Obj0)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Obj0)], true);

    // These are not required by every map/tile.  They are nevertheless loaded
    // when available so future native writers can preserve their contents.
    backing.parts[part_index(ShadowlandsADTPart::Obj1)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Obj1)], false);
    backing.parts[part_index(ShadowlandsADTPart::Lod)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Lod)], false);

    return backing;
  }
}
