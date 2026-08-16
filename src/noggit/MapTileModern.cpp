// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapHeaders.h>

#include <ClientFile.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
  void rebase_modern_chunk_to_tile_grid(MapChunk* chunk, MapTile const* tile)
  {
    // Shadowlands ROOT MCNK still provides usable MCVT/MCNR terrain data, but
    // its stored world-position fields must not be fed through the legacy
    // WotLK coordinate conversion.  The logical chunk position is already
    // known from the tile and the MCNK's 16x16 grid coordinates, so rebuild
    // X/Z deterministically and preserve only the parsed vertex heights.
    if (chunk->px < 0 || chunk->px >= 16 || chunk->py < 0 || chunk->py >= 16)
      throw std::runtime_error("Shadowlands ROOT MCNK contains an invalid chunk grid coordinate.");

    chunk->xbase = tile->xbase + static_cast<float>(chunk->px) * CHUNKSIZE;
    chunk->zbase = tile->zbase + static_cast<float>(chunk->py) * CHUNKSIZE;

    auto* vertex = chunk->mVertices;
    float min_y = std::numeric_limits<float>::max();
    float max_y = std::numeric_limits<float>::lowest();

    for (int row = 0; row < 17; ++row)
    {
      for (int column = 0; column < ((row % 2) ? 8 : 9); ++column)
      {
        float local_x = static_cast<float>(column) * UNITSIZE;
        float const local_z = static_cast<float>(row) * 0.5f * UNITSIZE;
        if (row % 2)
          local_x += UNITSIZE * 0.5f;

        // Keep Y exactly as decoded by the existing MCVT path.  Only X/Z are
        // corrected here; this lets us validate modern height parsing without
        // trusting legacy MCNK world-position semantics.
        vertex->x = chunk->xbase + local_x;
        vertex->z = chunk->zbase + local_z;
        min_y = std::min(min_y, vertex->y);
        max_y = std::max(max_y, vertex->y);
        ++vertex;
      }
    }

    chunk->vmin = glm::vec3(chunk->xbase, min_y, chunk->zbase);
    chunk->vmax = glm::vec3(chunk->xbase + 8.0f * UNITSIZE,
                            max_y,
                            chunk->zbase + 8.0f * UNITSIZE);
    chunk->vcenter = (chunk->vmin + chunk->vmax) * 0.5f;
  }
}

void MapTile::finishLoadingShadowlandsTerrainOnly()
{
  if (finished)
    return;

  BlizzardArchive::ClientFile root_file(
    _file_key,
    Noggit::Application::NoggitApplication::instance()->clientData());

  auto const* data = root_file.getBuffer();
  auto const file_size = root_file.getSize();

  std::vector<std::size_t> mcnk_offsets;
  mcnk_offsets.reserve(256);

  std::size_t pos = 0;
  while (pos + 8 <= file_size)
  {
    std::uint32_t magic = 0;
    std::uint32_t declared_size = 0;
    std::memcpy(&magic, data + pos, sizeof(magic));
    std::memcpy(&declared_size, data + pos + 4, sizeof(declared_size));

    auto const payload_pos = pos + 8;
    auto const remaining = file_size - payload_pos;
    if (declared_size > remaining)
    {
      throw std::runtime_error(
        "Shadowlands terrain loader encountered a truncated top-level ADT chunk.");
    }

    // WoW ADT FourCC bytes are reversed on disk: canonical MCNK is stored as KNCM.
    if (magic == 0x4D434E4Bu)
      mcnk_offsets.push_back(pos);

    pos = payload_pos + declared_size;
  }

  if (pos != file_size)
    throw std::runtime_error(
      "Shadowlands terrain loader did not end on the root ADT file boundary.");

  if (mcnk_offsets.size() != 256)
  {
    LogError << "[ModernADT] Terrain-only loader expected 256 ROOT MCNK chunks but found "
             << mcnk_offsets.size() << " in '" << _file_key.stringRepr() << "'."
             << std::endl;
    throw std::runtime_error(
      "Shadowlands terrain loader requires exactly 256 ROOT MCNK chunks.");
  }

  LogDebug << "[ModernADT] Building terrain-only MapTile " << index.x << ',' << index.z
           << " from 256 ROOT MCNK chunks. Textures, objects and liquids are intentionally disabled."
           << std::endl;

  // The ROOT MCNK still exposes terrain subchunks in a form the current
  // MapChunk reader can decode.  We use it for MCVT/MCNR, then explicitly
  // rebase every chunk to the known Noggit tile grid instead of trusting the
  // legacy WotLK world-position conversion for modern MCNK headers.
  for (std::size_t next_chunk = 0; next_chunk < mcnk_offsets.size(); ++next_chunk)
  {
    root_file.seek(mcnk_offsets[next_chunk]);

    unsigned const x = static_cast<unsigned>(next_chunk / 16);
    unsigned const z = static_cast<unsigned>(next_chunk % 16);

    mChunks[x][z] = std::make_unique<MapChunk>(
      this,
      &root_file,
      mBigAlpha,
      _mode,
      _context,
      false,
      0,
      false);

    auto* chunk = mChunks[x][z].get();
    rebase_modern_chunk_to_tile_grid(chunk, this);
    _renderer.initChunkData(chunk);
  }

  // There are deliberately no modern textures/objects wired into this first
  // milestone, so downstream readiness checks should not wait for them.
  mTextureFilenames.clear();
  _mtxf_entries.clear();
  _textures_finished_loading = true;
  _objects_finished_loading = true;

  root_file.close();

  recalcExtents();

  finished = true;
  _tile_is_being_reloaded = false;
  _state_changed.notify_all();

  LogDebug << "[ModernADT] Terrain-only Shadowlands tile loaded successfully: "
           << index.x << ',' << index.z << ". Chunk X/Z rebased to tile grid."
           << std::endl;
}
