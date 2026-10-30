// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <noggit/TileIndex.hpp>

#include <string>
#include <functional>
#include <vector>

namespace Noggit
{
  // Per-tile derived data pending after an ADT save, stored in
  // <project>/.noggit/derived-maps/<map>.json. Independent of in-memory dirty flags.
  class DerivedMapState
  {
  public:
    // Wdl is tracked separately from Lod: the WDL is written once per save.
    // ObjectDependency is provenance, not pending work: the tile's only project
    // files are OBJ0/OBJ1 written by a project-scope UID repair of a neighbour.
    enum Flag : unsigned
    {
      Diffuse = 1, Normal = 2, Lod = 4, Wdl = 8, All = Diffuse | Normal | Lod | Wdl,
      ObjectDependency = 16, Known = All | ObjectDependency
    };

    // Every operation rereads the file. Writes are locked and atomic and throw on failure.
    static std::vector<TileIndex> pending(std::string const& projectPath,
                                         std::string const& mapName, unsigned flags = All);
    // Tiles with ROOT/TEX0/OBJ0/OBJ1 files in the project. LOD/map texture files
    // and OBJ files recorded as ObjectDependency do not select a tile.
    static std::vector<TileIndex> projectTiles(std::string const& projectPath,
                                              std::string const& mapName);
    static void mark(std::string const& projectPath, std::string const& mapName,
                     TileIndex const& tile, unsigned flags = All);
    static void mark(std::string const& projectPath, std::string const& mapName,
                     std::vector<TileIndex> const& tiles, unsigned flags = All);
    static void clear(std::string const& projectPath, std::string const& mapName,
                      TileIndex const& tile, unsigned flags);
    static void clear(std::string const& projectPath, std::string const& mapName,
                      std::vector<TileIndex> const& tiles, unsigned flags);
    // Writers receive temporary output paths. Both files are prepared before
    // publication; a failed commit restores files already replaced. This is
    // recoverable on reported errors, not crash-atomic across the pair.
    static void writeTexturePair(std::string const& projectPath, std::string const& mapName,
                                 TileIndex const& tile,
                                 std::function<void(std::string const&)> const& writeDiffuse,
                                 std::function<void(std::string const&)> const& writeNormal);
  };
}
