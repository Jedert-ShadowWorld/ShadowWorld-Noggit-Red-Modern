// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapHeaders.h>
#include <noggit/TextureManager.h>

#include <ClientFile.hpp>
#include <Listfile.hpp>

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

  struct ModernTex0Data
  {
    std::vector<std::uint32_t> diffuse_ids;
    std::vector<std::uint32_t> height_ids;
    std::vector<std::uint32_t> base_diffuse_fdids;
  };

  // Decode TEX0 far enough to bind the first/base terrain texture of each MCNK.
  // Extra layers and MCAL are still only diagnosed; they will be wired next.
  ModernTex0Data inspect_modern_tex0(std::string const& root_path, int tile_x, int tile_z)
  {
    ModernTex0Data result;
    auto const tex_path = split_adt_path(root_path, "_tex0");
    BlizzardArchive::Listfile::FileKey key(tex_path);
    BlizzardArchive::ClientFile tex_file(
      key, Noggit::Application::NoggitApplication::instance()->clientData());

    auto const* data = tex_file.getBuffer();
    auto const file_size = tex_file.getSize();
    std::size_t mcnk_count = 0;
    std::size_t pos = 0;
    result.base_diffuse_fdids.reserve(256);

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
        std::uint32_t base_fdid = 0;
        std::size_t sub = payload; // TEX0 MCNK is headerless.
        while (sub + 8 <= payload + size)
        {
          std::uint32_t sub_magic = 0, sub_size = 0;
          std::memcpy(&sub_magic, data + sub, 4);
          std::memcpy(&sub_size, data + sub + 4, 4);
          auto const sub_data = sub + 8;
          if (sub_size > payload + size - sub_data)
            break;

          if (sub_magic == on_disk_fourcc('M','C','L','Y'))
          {
            if (sub_size < 16 || sub_size % 16 != 0)
              throw std::runtime_error("Shadowlands TEX0 MCLY has an invalid size.");

            std::uint32_t base_texture_id = 0;
            std::memcpy(&base_texture_id, data + sub_data, 4);
            if (base_texture_id >= result.diffuse_ids.size())
              throw std::runtime_error("Shadowlands TEX0 MCLY references a diffuse texture outside MDID.");
            base_fdid = result.diffuse_ids[base_texture_id];

            if (tile_x == 34 && tile_z == 49 && mcnk_count < 4)
            {
              std::ostringstream layers;
              auto const entry_count = sub_size / 16;
              auto const show_count = std::min<std::size_t>(entry_count, 8);
              for (std::size_t i = 0; i < show_count; ++i)
              {
                std::uint32_t texture_id = 0, flags = 0, alpha_offset = 0, effect_id = 0;
                auto const entry = sub_data + i * 16;
                std::memcpy(&texture_id, data + entry, 4);
                std::memcpy(&flags, data + entry + 4, 4);
                std::memcpy(&alpha_offset, data + entry + 8, 4);
                std::memcpy(&effect_id, data + entry + 12, 4);
                if (i) layers << ' ';
                layers << '[' << i << ":tex=" << texture_id
                       << ",fdid=" << (texture_id < result.diffuse_ids.size() ? result.diffuse_ids[texture_id] : 0)
                       << ",flags=0x" << std::hex << flags << std::dec
                       << ",alpha=" << alpha_offset << ",effect=" << effect_id << ']';
              }
              LogDebug << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
                       << " chunk=" << mcnk_count << " MCLY entries=" << entry_count
                       << " :: " << layers.str() << std::endl;
            }
          }
          else if (tile_x == 34 && tile_z == 49 && mcnk_count < 4
                   && sub_magic == on_disk_fourcc('M','C','A','L'))
          {
            LogDebug << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
                     << " chunk=" << mcnk_count << " MCAL bytes=" << sub_size << std::endl;
          }
          else if (tile_x == 34 && tile_z == 49 && mcnk_count < 4
                   && sub_magic == on_disk_fourcc('M','C','S','H'))
          {
            LogDebug << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
                     << " chunk=" << mcnk_count << " MCSH bytes=" << sub_size << std::endl;
          }

          sub = sub_data + sub_size;
        }

        result.base_diffuse_fdids.push_back(base_fdid);
        ++mcnk_count;
      }

      pos = payload + size;
    }

    if (result.base_diffuse_fdids.size() != 256)
      throw std::runtime_error("Shadowlands TEX0 base-texture decoder expected exactly 256 MCNK chunks.");

    LogDebug << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
             << " TEX0 diffuseIDs=" << result.diffuse_ids.size()
             << " heightIDs=" << result.height_ids.size()
             << " MCNK=" << mcnk_count;
    if (!result.diffuse_ids.empty())
    {
      LogDebug << " firstDiffuseFDIDs={";
      for (std::size_t i = 0; i < std::min<std::size_t>(result.diffuse_ids.size(), 8); ++i)
        LogDebug << (i ? "," : "") << result.diffuse_ids[i];
      LogDebug << '}';
    }
    LogDebug << std::endl;

    return result;
  }

  bool bind_modern_base_texture(MapChunk* chunk,
                                std::uint32_t file_data_id,
                                Noggit::NoggitRenderContext context)
  {
    if (!file_data_id)
      return false;

    auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
    auto const texture_path = client_data->listfile()->getPath(file_data_id);
    if (texture_path.empty())
    {
      LogError << "[ModernADT] Unable to resolve terrain texture FileDataID "
               << file_data_id << " through the project listfile." << std::endl;
      return false;
    }

    int const layer = chunk->addTexture(scoped_blp_texture_reference(texture_path, context));
    if (layer != 0)
    {
      LogError << "[ModernADT] Expected modern base texture to become layer 0, got "
               << layer << " for FileDataID " << file_data_id << "." << std::endl;
      return false;
    }

    return true;
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

  LogDebug << "[ModernADT] Building base-textured MapTile " << index.x << ',' << index.z
           << " from 256 ROOT MCNK chunks. Only TEX0 base layer is enabled; MCAL blending remains disabled."
           << std::endl;

  std::size_t base_textures_bound = 0;
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

    bool const emit_height_diag = (index.x == 34 && index.z == 49 && next_chunk < 4);
    if (emit_height_diag)
      log_modern_height_diagnostic(this, chunk, next_chunk, raw_height);

    if (next_chunk < tex0_data.base_diffuse_fdids.size()
        && bind_modern_base_texture(chunk, tex0_data.base_diffuse_fdids[next_chunk], _context))
    {
      ++base_textures_bound;
    }

    _renderer.initChunkData(chunk);
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
           << ". Direct MCVT terrain + TEX0 base textures bound on "
           << base_textures_bound << "/256 chunks."
           << std::endl;
}
