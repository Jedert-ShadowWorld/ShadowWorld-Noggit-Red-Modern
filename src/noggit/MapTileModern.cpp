// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapHeaders.h>

#include <ClientFile.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
  constexpr std::uint32_t reversed_fourcc(char a, char b, char c, char d)
  {
    return (static_cast<std::uint32_t>(d) << 24)
         | (static_cast<std::uint32_t>(c) << 16)
         | (static_cast<std::uint32_t>(b) << 8)
         | static_cast<std::uint32_t>(a);
  }

  struct ModernHeightDiagnostic
  {
    bool found_mcvt = false;
    float raw_header_zpos = 0.0f;
    float raw_header_xpos = 0.0f;
    float raw_header_ypos = 0.0f;
    float mcvt_min = 0.0f;
    float mcvt_max = 0.0f;
    std::vector<float> first_heights;
  };

  ModernHeightDiagnostic inspect_modern_height_data(
    char const* file_data,
    std::size_t file_size,
    std::size_t mcnk_offset)
  {
    ModernHeightDiagnostic result;

    if (mcnk_offset + 8 + sizeof(MapChunkHeader) > file_size)
      return result;

    MapChunkHeader raw_header{};
    std::memcpy(&raw_header, file_data + mcnk_offset + 8, sizeof(raw_header));
    result.raw_header_zpos = raw_header.zpos;
    result.raw_header_xpos = raw_header.xpos;
    result.raw_header_ypos = raw_header.ypos;

    std::uint32_t declared_size = 0;
    std::memcpy(&declared_size, file_data + mcnk_offset + 4, sizeof(declared_size));

    auto const payload_begin = mcnk_offset + 8;
    auto const payload_end = payload_begin + declared_size;
    if (payload_end > file_size || declared_size < 128)
      return result;

    // Shadowlands ROOT MCNK keeps a 128-byte header followed by normal
    // fourcc+size terrain subchunks. Read MCVT directly instead of trusting
    // the legacy header offsets so we can compare raw heights to MapChunk's
    // decoded vertex Y values.
    std::size_t sub = payload_begin + 128;
    while (sub + 8 <= payload_end)
    {
      std::uint32_t magic = 0;
      std::uint32_t size = 0;
      std::memcpy(&magic, file_data + sub, sizeof(magic));
      std::memcpy(&size, file_data + sub + 4, sizeof(size));

      auto const data_begin = sub + 8;
      if (size > payload_end - data_begin)
        break;

      if (magic == reversed_fourcc('M', 'C', 'V', 'T') && size >= 145 * sizeof(float))
      {
        result.found_mcvt = true;
        result.mcvt_min = std::numeric_limits<float>::max();
        result.mcvt_max = std::numeric_limits<float>::lowest();
        result.first_heights.reserve(10);

        for (std::size_t i = 0; i < 145; ++i)
        {
          float height = 0.0f;
          std::memcpy(&height, file_data + data_begin + i * sizeof(float), sizeof(height));
          result.mcvt_min = std::min(result.mcvt_min, height);
          result.mcvt_max = std::max(result.mcvt_max, height);
          if (i < 10)
            result.first_heights.push_back(height);
        }
        break;
      }

      sub = data_begin + size;
    }

    return result;
  }

  void log_modern_height_diagnostic(
    MapTile const* tile,
    MapChunk const* chunk,
    std::size_t chunk_index,
    ModernHeightDiagnostic const& raw)
  {
    float final_min_y = std::numeric_limits<float>::max();
    float final_max_y = std::numeric_limits<float>::lowest();
    for (auto const& vertex : chunk->mVertices)
    {
      final_min_y = std::min(final_min_y, vertex.y);
      final_max_y = std::max(final_max_y, vertex.y);
    }

    std::ostringstream heights;
    heights << std::fixed << std::setprecision(3);
    for (std::size_t i = 0; i < raw.first_heights.size(); ++i)
    {
      if (i)
        heights << ',';
      heights << raw.first_heights[i];
    }

    LogDebug << "[ModernADT][HeightDiag] tile=" << tile->index.x << ',' << tile->index.z
             << " chunk=" << chunk_index
             << " grid(parsed)=" << chunk->px << ',' << chunk->py
             << " rawHeaderPos(z,x,y)=" << raw.raw_header_zpos << ','
             << raw.raw_header_xpos << ',' << raw.raw_header_ypos
             << " MapChunkBase(x,y,z)=" << chunk->xbase << ',' << chunk->ybase << ',' << chunk->zbase
             << " directMCVT=" << (raw.found_mcvt ? "yes" : "no")
             << " MCVT[min,max]=" << raw.mcvt_min << ',' << raw.mcvt_max
             << " finalY[min,max]=" << final_min_y << ',' << final_max_y
             << " first10={" << heights.str() << "}"
             << std::endl;
  }

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
    // Capture the raw modern header and MCVT before MapChunk touches the file.
    // Limit verbose diagnostics to the tile being actively tested and its first
    // four chunks so the async loader does not flood the log for nearby tiles.
    ModernHeightDiagnostic raw_diag;
    bool const emit_height_diag = (index.x == 34 && index.z == 49 && next_chunk < 4);
    if (emit_height_diag)
      raw_diag = inspect_modern_height_data(data, file_size, mcnk_offsets[next_chunk]);

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

    if (emit_height_diag)
      log_modern_height_diagnostic(this, chunk, next_chunk, raw_diag);

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