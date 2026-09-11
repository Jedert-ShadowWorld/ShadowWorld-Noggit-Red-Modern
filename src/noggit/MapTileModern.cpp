// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapHeaders.h>
#include <noggit/TextureManager.h>
#include <noggit/texture_set.hpp>

#include <ClientFile.hpp>
#include <Listfile.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
  constexpr std::uint32_t on_disk_fourcc(char a, char b, char c, char d)
  {
    return (static_cast<std::uint32_t>(a) << 24)
         | (static_cast<std::uint32_t>(b) << 16)
         | (static_cast<std::uint32_t>(c) << 8)
         | static_cast<std::uint32_t>(d);
  }

  std::string split_adt_path(std::string const& root_path, std::string const& suffix)
  {
    auto const extension = root_path.rfind(".adt");
    if (extension == std::string::npos)
      return root_path + suffix;
    return root_path.substr(0, extension) + suffix + ".adt";
  }

  struct ModernHeightDiagnostic
  {
    bool found_mcvt = false;
    float raw_header_zpos = 0.0f;
    float raw_header_xpos = 0.0f;
    float raw_header_ypos = 0.0f;
    float mcvt_min = 0.0f;
    float mcvt_max = 0.0f;
    std::vector<float> heights;
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

      if (magic == on_disk_fourcc('M', 'C', 'V', 'T') && size >= 145 * sizeof(float))
      {
        result.found_mcvt = true;
        result.mcvt_min = std::numeric_limits<float>::max();
        result.mcvt_max = std::numeric_limits<float>::lowest();
        result.heights.reserve(145);
        result.first_heights.reserve(10);

        for (std::size_t i = 0; i < 145; ++i)
        {
          float height = 0.0f;
          std::memcpy(&height, file_data + data_begin + i * sizeof(float), sizeof(height));
          result.heights.push_back(height);
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

  void rebuild_modern_chunk_geometry(
    MapChunk* chunk,
    MapTile const* tile,
    ModernHeightDiagnostic const& raw)
  {
    if (chunk->px < 0 || chunk->px >= 16 || chunk->py < 0 || chunk->py >= 16)
      throw std::runtime_error("Shadowlands ROOT MCNK contains an invalid chunk grid coordinate.");

    if (!raw.found_mcvt || raw.heights.size() != 145)
      throw std::runtime_error("Shadowlands ROOT MCNK is missing a valid 145-float MCVT subchunk.");

    chunk->xbase = tile->xbase + static_cast<float>(chunk->px) * CHUNKSIZE;
    chunk->zbase = tile->zbase + static_cast<float>(chunk->py) * CHUNKSIZE;
    chunk->ybase = 0.0f;

    auto* vertex = chunk->mVertices;
    std::size_t height_index = 0;
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

        float const y = raw.raw_header_ypos + raw.heights[height_index++];
        vertex->x = chunk->xbase + local_x;
        vertex->y = y;
        vertex->z = chunk->zbase + local_z;
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
        ++vertex;
      }
    }

    chunk->vmin = glm::vec3(chunk->xbase, min_y, chunk->zbase);
    chunk->vmax = glm::vec3(chunk->xbase + 8.0f * UNITSIZE,
                            max_y,
                            chunk->zbase + 8.0f * UNITSIZE);
    chunk->vcenter = (chunk->vmin + chunk->vmax) * 0.5f;
  }

  glm::vec3 get_modern_tile_local_neighbor(MapTile* tile, MapChunk* chunk, int vertex_index, unsigned dir)
  {
    constexpr float half_unit = UNITSIZE / 2.f;
    static constexpr std::array<float, 4> xdiff{-half_unit, half_unit, half_unit, -half_unit};
    static constexpr std::array<float, 4> zdiff{-half_unit, -half_unit, half_unit, half_unit};

    float const vertex_x = chunk->mVertices[vertex_index].x + xdiff[dir];
    float const vertex_z = chunk->mVertices[vertex_index].z + zdiff[dir];
    TileIndex const neighbor_tile({vertex_x, 0.0f, vertex_z});

    if (neighbor_tile.x == tile->index.x && neighbor_tile.z == tile->index.z)
    {
      glm::vec3 result{};
      tile->getVertexInternal(vertex_x, vertex_z, &result);
      return result;
    }

    // Loading modern tiles happens in parallel (normally a ~5x5 neighborhood).
    // Never query another tile from an AsyncLoader worker here: another tile may
    // still be waiting on this one. Mirror Noggit's unloaded-neighbor fallback
    // instead and keep the edge height flat until a later seam refresh exists.
    return {vertex_x, chunk->mVertices[vertex_index].y, vertex_z};
  }

  void recalc_modern_tile_local_normals(MapTile* tile, MapChunk* chunk)
  {
    auto& tile_buffer = tile->getChunkHeightmapBuffer();
    int const chunk_start = (chunk->px * 16 + chunk->py) * mapbufsize * 4;

    for (int i = 0; i < mapbufsize; ++i)
    {
      glm::vec3 const P1 = get_modern_tile_local_neighbor(tile, chunk, i, 0);
      glm::vec3 const P2 = get_modern_tile_local_neighbor(tile, chunk, i, 1);
      glm::vec3 const P3 = get_modern_tile_local_neighbor(tile, chunk, i, 2);
      glm::vec3 const P4 = get_modern_tile_local_neighbor(tile, chunk, i, 3);

      glm::vec3 const N1 = glm::cross(P2 - chunk->mVertices[i], P1 - chunk->mVertices[i]);
      glm::vec3 const N2 = glm::cross(P3 - chunk->mVertices[i], P2 - chunk->mVertices[i]);
      glm::vec3 const N3 = glm::cross(P4 - chunk->mVertices[i], P3 - chunk->mVertices[i]);
      glm::vec3 const N4 = glm::cross(P1 - chunk->mVertices[i], P4 - chunk->mVertices[i]);

      glm::vec3 norm = glm::normalize(N1 + N2 + N3 + N4);
      norm.x = std::floor(norm.x * 127.0f) / 127.0f;
      norm.y = std::floor(norm.y * 127.0f) / 127.0f;
      norm.z = std::floor(norm.z * 127.0f) / 127.0f;

      int const pixel_start = chunk_start + i * 4;
      tile_buffer[pixel_start] = -norm.z;
      tile_buffer[pixel_start + 1] = norm.y;
      tile_buffer[pixel_start + 2] = -norm.x;
    }

    chunk->requeueChunkUpdate(ChunkUpdateFlags::NORMALS);
  }

  struct ModernTextureLayer
  {
    std::uint32_t texture_id = 0;
    std::uint32_t file_data_id = 0;
    std::uint32_t flags = 0;
    std::uint32_t alpha_offset = 0;
    std::uint32_t effect_id = 0xFFFFFFFF;
  };

  struct ModernTexChunk
  {
    std::vector<ModernTextureLayer> layers;
    std::vector<std::uint8_t> mcal;
  };

  struct ModernTex0Data
  {
    std::vector<std::uint32_t> diffuse_ids;
    std::vector<std::uint32_t> height_ids;
    std::vector<ModernTexChunk> chunks;
  };

  bool decode_modern_alpha(ModernTexChunk const& tex_chunk,
                           ModernTextureLayer const& layer,
                           std::vector<std::uint8_t>& output)
  {
    output.clear();
    output.reserve(64 * 64);

    if (!(layer.flags & FLAG_USE_ALPHA))
      return false;
    if (layer.alpha_offset >= tex_chunk.mcal.size())
      return false;

    auto const* input = tex_chunk.mcal.data() + layer.alpha_offset;
    auto const* end = tex_chunk.mcal.data() + tex_chunk.mcal.size();

    if (layer.flags & FLAG_ALPHA_COMPRESSED)
    {
      while (output.size() < 4096 && input < end)
      {
        std::uint8_t const control = *input++;
        std::size_t const count = control & 0x7F;
        bool const fill = (control & 0x80) != 0;
        if (!count)
          continue;

        auto const remaining = 4096 - output.size();
        auto const copy_count = std::min<std::size_t>(count, remaining);

        if (fill)
        {
          if (input >= end)
            return false;
          std::uint8_t const value = *input++;
          output.insert(output.end(), copy_count, value);
        }
        else
        {
          if (static_cast<std::size_t>(end - input) < count)
            return false;
          output.insert(output.end(), input, input + copy_count);
          input += count;
        }
      }
      return output.size() == 4096;
    }

    if (static_cast<std::size_t>(end - input) < 4096)
      return false;

    output.assign(input, input + 4096);
    return true;
  }

  ModernTex0Data inspect_modern_tex0(std::string const& root_path, int tile_x, int tile_z)
  {
    ModernTex0Data result;
    auto const tex_path = split_adt_path(root_path, "_tex0");
    BlizzardArchive::Listfile::FileKey key(tex_path);
    BlizzardArchive::ClientFile tex_file(
      key, Noggit::Application::NoggitApplication::instance()->clientData());

    auto const* data = tex_file.getBuffer();
    auto const file_size = tex_file.getSize();
    std::size_t pos = 0;
    result.chunks.reserve(256);

    while (pos + 8 <= file_size)
    {
      std::uint32_t magic = 0;
      std::uint32_t size = 0;
      std::memcpy(&magic, data + pos, 4);
      std::memcpy(&size, data + pos + 4, 4);
      auto const payload = pos + 8;
      if (size > file_size - payload)
        throw std::runtime_error("Shadowlands TEX0 contains a truncated top-level chunk.");

      if (magic == on_disk_fourcc('M','D','I','D') || magic == on_disk_fourcc('M','H','I','D'))
      {
        auto& out = magic == on_disk_fourcc('M','D','I','D') ? result.diffuse_ids : result.height_ids;
        if (size % 4 != 0)
          throw std::runtime_error("Shadowlands TEX0 texture-ID chunk is not uint32 aligned.");
        out.resize(size / 4);
        if (size)
          std::memcpy(out.data(), data + payload, size);
      }
      else if (magic == on_disk_fourcc('M','C','N','K'))
      {
        ModernTexChunk tex_chunk;
        std::size_t sub = payload;
        while (sub + 8 <= payload + size)
        {
          std::uint32_t sub_magic = 0, sub_size = 0;
          std::memcpy(&sub_magic, data + sub, 4);
          std::memcpy(&sub_size, data + sub + 4, 4);
          auto const sub_data = sub + 8;
          if (sub_size > payload + size - sub_data)
            throw std::runtime_error("Shadowlands TEX0 MCNK contains a truncated subchunk.");

          if (sub_magic == on_disk_fourcc('M','C','L','Y'))
          {
            // WMO-only Shadowlands tiles (including Torghast) legitimately
            // contain an empty MCLY in every TEX0 MCNK.
            if (sub_size % 16 != 0)
              throw std::runtime_error("Shadowlands TEX0 MCLY has an invalid size.");

            auto const entry_count = sub_size / 16;
            auto const load_count = std::min<std::size_t>(entry_count, MAX_TEXTURE_LAYERS);
            tex_chunk.layers.reserve(load_count);

            for (std::size_t i = 0; i < load_count; ++i)
            {
              ModernTextureLayer layer;
              auto const entry = sub_data + i * 16;
              std::memcpy(&layer.texture_id, data + entry, 4);
              std::memcpy(&layer.flags, data + entry + 4, 4);
              std::memcpy(&layer.alpha_offset, data + entry + 8, 4);
              std::memcpy(&layer.effect_id, data + entry + 12, 4);
              tex_chunk.layers.push_back(layer);
            }
          }
          else if (sub_magic == on_disk_fourcc('M','C','A','L'))
          {
            tex_chunk.mcal.assign(
              reinterpret_cast<std::uint8_t const*>(data + sub_data),
              reinterpret_cast<std::uint8_t const*>(data + sub_data + sub_size));
          }

          sub = sub_data + sub_size;
        }

        result.chunks.push_back(std::move(tex_chunk));
      }

      pos = payload + size;
    }

    if (result.chunks.size() != 256)
      throw std::runtime_error("Shadowlands TEX0 texture decoder expected exactly 256 MCNK chunks.");

    for (auto& tex_chunk : result.chunks)
    {
      for (auto& layer : tex_chunk.layers)
      {
        if (layer.texture_id >= result.diffuse_ids.size())
          throw std::runtime_error("Shadowlands TEX0 MCLY references a diffuse texture outside MDID.");
        layer.file_data_id = result.diffuse_ids[layer.texture_id];
      }
    }

    LogDebug << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
             << " TEX0 diffuseIDs=" << result.diffuse_ids.size()
             << " heightIDs=" << result.height_ids.size()
             << " MCNK=" << result.chunks.size() << std::endl;

    if (tile_x == 34 && tile_z == 49)
    {
      for (std::size_t chunk_index = 0; chunk_index < std::min<std::size_t>(4, result.chunks.size()); ++chunk_index)
      {
        auto const& tex_chunk = result.chunks[chunk_index];
        std::ostringstream layers;
        for (std::size_t i = 0; i < tex_chunk.layers.size(); ++i)
        {
          auto const& layer = tex_chunk.layers[i];
          if (i) layers << ' ';
          layers << '[' << i << ":tex=" << layer.texture_id
                 << ",fdid=" << layer.file_data_id
                 << ",flags=0x" << std::hex << layer.flags << std::dec
                 << ",alpha=" << layer.alpha_offset
                 << ",effect=" << layer.effect_id << ']';
        }
        LogDebug << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
                 << " chunk=" << chunk_index
                 << " MCLY entries=" << tex_chunk.layers.size()
                 << " MCAL bytes=" << tex_chunk.mcal.size()
                 << " :: " << layers.str() << std::endl;
      }
    }

    return result;
  }

  std::size_t bind_modern_texture_layers(MapChunk* chunk,
                                         ModernTexChunk const& tex_chunk,
                                         Noggit::NoggitRenderContext context,
                                         bool emit_diag,
                                         std::size_t chunk_index)
  {
    auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
    std::size_t bound = 0;

    for (std::size_t layer_index = 0; layer_index < tex_chunk.layers.size(); ++layer_index)
    {
      auto const& source_layer = tex_chunk.layers[layer_index];
      auto const texture_path = client_data->listfile()->getPath(source_layer.file_data_id);
      if (texture_path.empty())
      {
        LogError << "[ModernADT] Unable to resolve terrain texture FileDataID "
                 << source_layer.file_data_id << " through the project listfile." << std::endl;
        break;
      }

      int const added_layer = chunk->addTexture(scoped_blp_texture_reference(texture_path, context));
      if (added_layer != static_cast<int>(layer_index))
      {
        LogError << "[ModernADT] Modern terrain texture layer order mismatch: expected "
                 << layer_index << ", got " << added_layer << "." << std::endl;
        break;
      }

      auto* info = chunk->texture_set->getMCLYEntries();
      info[layer_index].flags = source_layer.flags;
      info[layer_index].effectID = source_layer.effect_id;
      ++bound;

      if (layer_index > 0 && (source_layer.flags & FLAG_USE_ALPHA))
      {
        std::vector<std::uint8_t> alpha;
        if (!decode_modern_alpha(tex_chunk, source_layer, alpha))
        {
          LogError << "[ModernADT] Failed to decode MCAL for chunk " << chunk_index
                   << " layer " << layer_index
                   << " offset " << source_layer.alpha_offset
                   << " flags=0x" << std::hex << source_layer.flags << std::dec << std::endl;
          continue;
        }

        auto& alphamaps = *chunk->texture_set->getAlphamaps();
        if (!alphamaps[layer_index - 1])
          alphamaps[layer_index - 1] = std::make_unique<Alphamap>();
        alphamaps[layer_index - 1]->setAlpha(alpha.data());

        if (emit_diag)
        {
          auto const minmax = std::minmax_element(alpha.begin(), alpha.end());
          auto const nonzero = std::count_if(alpha.begin(), alpha.end(), [](std::uint8_t v) { return v != 0; });
          LogDebug << "[ModernADT][AlphaDiag] chunk=" << chunk_index
                   << " layer=" << layer_index
                   << " min=" << static_cast<int>(*minmax.first)
                   << " max=" << static_cast<int>(*minmax.second)
                   << " nonzero=" << nonzero << "/4096"
                   << (source_layer.flags & FLAG_ALPHA_COMPRESSED ? " compressed" : " raw")
                   << std::endl;
        }
      }
    }

    chunk->requeueChunkUpdate(ChunkUpdateFlags::ALPHAMAP);
    return bound;
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
      throw std::runtime_error("Shadowlands terrain loader encountered a truncated top-level ADT chunk.");

    if (magic == 0x4D434E4Bu)
      mcnk_offsets.push_back(pos);

    pos = payload_pos + declared_size;
  }

  if (pos != file_size)
    throw std::runtime_error("Shadowlands terrain loader did not end on the root ADT file boundary.");

  if (mcnk_offsets.size() != 256)
  {
    LogError << "[ModernADT] Terrain-only loader expected 256 ROOT MCNK chunks but found "
             << mcnk_offsets.size() << " in '" << _file_key.stringRepr() << "'." << std::endl;
    throw std::runtime_error("Shadowlands terrain loader requires exactly 256 ROOT MCNK chunks.");
  }

  ModernTex0Data tex0_data;
  if (_file_key.hasFilepath())
    tex0_data = inspect_modern_tex0(_file_key.filepath(), index.x, index.z);

  LogDebug << "[ModernADT] Building fully textured MapTile " << index.x << ',' << index.z
           << " from 256 ROOT MCNK chunks. TEX0 MCLY layers and MCAL blending enabled."
           << std::endl;

  std::size_t textured_chunks = 0;
  std::size_t texture_layers_bound = 0;
  for (std::size_t next_chunk = 0; next_chunk < mcnk_offsets.size(); ++next_chunk)
  {
    auto const raw_height = inspect_modern_height_data(data, file_size, mcnk_offsets[next_chunk]);
    root_file.seek(mcnk_offsets[next_chunk]);

    unsigned const x = static_cast<unsigned>(next_chunk / 16);
    unsigned const z = static_cast<unsigned>(next_chunk % 16);

    mChunks[x][z] = std::make_unique<MapChunk>(
      this, &root_file, mBigAlpha, _mode, _context, false, 0, false);

    auto* chunk = mChunks[x][z].get();
    rebuild_modern_chunk_geometry(chunk, this, raw_height);

    bool const emit_diag = (index.x == 34 && index.z == 49 && next_chunk < 4);
    if (emit_diag)
      log_modern_height_diagnostic(this, chunk, next_chunk, raw_height);

    if (next_chunk < tex0_data.chunks.size())
    {
      auto const bound = bind_modern_texture_layers(
        chunk, tex0_data.chunks[next_chunk], _context, emit_diag, next_chunk);
      texture_layers_bound += bound;
      if (bound)
        ++textured_chunks;
    }
  }

  LogDebug << "[ModernADT] Recalculating tile-local terrain normals for tile "
           << index.x << ',' << index.z
           << " without cross-tile streaming dependencies." << std::endl;

  for (unsigned x = 0; x < 16; ++x)
  {
    for (unsigned z = 0; z < 16; ++z)
      recalc_modern_tile_local_normals(this, mChunks[x][z].get());
  }

  for (unsigned x = 0; x < 16; ++x)
  {
    for (unsigned z = 0; z < 16; ++z)
      _renderer.initChunkData(mChunks[x][z].get());
  }

  mTextureFilenames.clear();
  _mtxf_entries.clear();
  _textures_finished_loading = true;
  _objects_finished_loading = true;

  root_file.close();
  recalcExtents();

  finished = true;
  _tile_is_being_reloaded = false;
  _state_changed.notify_all();

  LogDebug << "[ModernADT] Shadowlands tile loaded: " << index.x << ',' << index.z
           << ". Direct MCVT terrain + TEX0 MCLY/MCAL textures on "
           << textured_chunks << "/256 chunks, " << texture_layers_bound
           << " layers bound; normals recalculated tile-locally for streaming safety."
           << std::endl;
}
