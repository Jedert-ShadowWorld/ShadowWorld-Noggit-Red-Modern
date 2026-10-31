// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/TileRender.hpp>
#include <noggit/MapTile.h>
#include <noggit/MapChunk.h>
#include <noggit/Log.h>
#include <noggit/texture_set.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <ClientData.hpp>

#include <opengl/shader.hpp>

#include <external/tracy/Tracy.hpp>

#include <cstdint>
#include <vector>

using namespace Noggit::Rendering;

TileRender::TileRender(MapTile* map_tile)
: _map_tile(map_tile)
{

}

void TileRender::upload()
{
  uploadTextures();
  _buffers.upload();

  gl.bindBuffer(GL_UNIFORM_BUFFER, _chunk_instance_data_ubo);
  gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::CHUNK_INSTANCE_DATA,
                     _chunk_instance_data_ubo, 0, sizeof(OpenGL::ChunkInstanceDataUniformBlock) * 256);
  gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::ChunkInstanceDataUniformBlock) * 256, NULL, GL_DYNAMIC_DRAW);

  gl.bindBuffer(GL_UNIFORM_BUFFER, _chunk_layer_ext_ubo);
  gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::CHUNK_LAYER_EXT,
                     _chunk_layer_ext_ubo, 0, sizeof(_chunk_layer_ext_data));
  gl.bufferData(GL_UNIFORM_BUFFER, sizeof(_chunk_layer_ext_data), NULL, GL_DYNAMIC_DRAW);

  _draw_calls.clear();
  _split_drawcall = false;
  _requires_sampler_reset = true;
  MapTileDrawCall& draw_call = _draw_calls.emplace_back();
  draw_call.start_chunk = 0;
  draw_call.n_chunks = 256;
  std::fill(draw_call.samplers.begin(), draw_call.samplers.end(), -1);

  gl.genQueries(1, &_tile_occlusion_query);

  _uploaded = true;

}

void TileRender::unload()
{
  if (_uploaded)
  {
    _chunk_texture_arrays.unload();
    _buffers.unload();
    _uploaded = false;
    _alphamap_extended = false;
    gl.deleteQueries(1, &_tile_occlusion_query);
  }


  _draw_calls.clear();
  _split_drawcall = false;
  _requires_sampler_reset = true;
  _texture_not_loaded = true;
  _sampler_error = false;

  _map_tile->_chunk_update_flags = ChunkUpdateFlags::VERTEX | ChunkUpdateFlags::ALPHAMAP
                                  | ChunkUpdateFlags::SHADOW | ChunkUpdateFlags::MCCV
                                  | ChunkUpdateFlags::NORMALS| ChunkUpdateFlags::HOLES
                                  | ChunkUpdateFlags::AREA_ID| ChunkUpdateFlags::FLAGS
                                  | ChunkUpdateFlags::GROUND_EFFECT | ChunkUpdateFlags::DETAILDOODADS_EXCLUSION;
}


void TileRender::draw (OpenGL::Scoped::use_program& mcnk_shader
    , const glm::vec3& camera
    , bool show_unpaintable_chunks
    , bool draw_paintability_overlay
    , bool is_selected
    , bool skip_upload_alphamap
)
{
  ZoneScopedN(NOGGIT_CURRENT_FUNCTION);

  if (!_map_tile->finished.load())
  [[unlikely]]
  {
    return;
  }

  if (!_uploaded)
  [[unlikely]]
  {
    upload();
  }

  bool alphamap_bound = false;
  bool heightmap_bound = false;
  bool shadowmap_bound = false;
  bool mccv_bound = false;

  _uploaded_alphamap_last_frame = false;
  _num_uploaded_chunk_alphamaps = 0;


  // iterate all textures to check if there's one that's not loaded yet
  if (_texture_not_loaded)
  {
    _texture_not_loaded = !_map_tile->texturesFinishedLoading();
  }
  // _texture_not_loaded = false;

  // figure out if we need to update based on paintability
  bool need_paintability_update = false;
  if (_requires_paintability_recalc && draw_paintability_overlay && show_unpaintable_chunks)
  [[unlikely]]
  {
    auto cur_tex = Noggit::Ui::selected_texture::get();
    for (int j = 0; j < 16; ++j)
    {
      for (int i = 0; i < 16; ++i)
      {
        auto& chunk = _map_tile->mChunks[j][i];
        bool cant_paint = cur_tex && !chunk->canPaintTexture(*cur_tex);

        if (chunk->currently_paintable != !cant_paint)
        {
          chunk->currently_paintable = !cant_paint;
          _chunk_instance_data[i * 16 + j].ChunkHoles_DrawImpass_TexLayerCount_CantPaint[3] = cant_paint;
          need_paintability_update = true;
        }
      }
    }

    _requires_paintability_recalc = false;
  }

  // A removed/replaced layer can release the last texture reference. Drop its
  // cached GL name before uploads reclaim it, even when alpha uploads are skipped.
  if ((_map_tile->_chunk_update_flags & ChunkUpdateFlags::ALPHAMAP) || _texture_not_loaded)
    pruneUnusedSamplers(skip_upload_alphamap);

  // run chunk updates. running this when splitdraw call detected unused sampler configuration as well.
  if (!skip_upload_alphamap && (_map_tile->_chunk_update_flags || is_selected != _selected || need_paintability_update || _requires_sampler_reset || _texture_not_loaded
    || _requires_ground_effect_color_recalc || _require_geffect_active_texture_update))
  {

    gl.bindBuffer(GL_UNIFORM_BUFFER, _chunk_instance_data_ubo);

    if (_requires_sampler_reset)
    [[unlikely]]
    {
      _draw_calls.clear();
      MapTileDrawCall& draw_call = _draw_calls.emplace_back();
      std::fill(draw_call.samplers.begin(), draw_call.samplers.end(), -1);
      draw_call.start_chunk = 0;
      draw_call.n_chunks = 256;
    }

    _selected = is_selected;

    // grow the alphamap array with the extension slices for layers 5+ the
    // first time a chunk of this tile needs them. reallocating voids the
    // base slices too, so every chunk alphamap has to be uploaded again.
    if (!_alphamap_extended)
    {
      for (int i = 0; i < 256; ++i)
      {
        if (static_cast<int>(_map_tile->mChunks[i % 16][i / 16]->texture_set->num()) <= BASE_RENDER_TEXTURE_LAYERS)
          continue;

        constexpr int n_slices = 256 + 256 * EXT_ALPHAMAP_SLICES;

        // the new storage has to be allocated from zeroed data : the target is
        // GL 4.1, which has no glClearTexImage, and a slice whose chunk does
        // not make it through the re-upload below would be sampled as
        // uninitialized memory.
        std::vector<std::uint8_t> zeroed_slices(64 * 64 * 3 * n_slices, 0);

        gl.activeTexture(GL_TEXTURE0 + 3);
        gl.bindTexture(GL_TEXTURE_2D_ARRAY, _alphamap_tex);
        gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB, 64, 64,
                      n_slices, 0, GL_RGB, GL_UNSIGNED_BYTE, zeroed_slices.data());

        if (Noggit::Application::NoggitApplication::instance()->clientData()->version()
            == BlizzardArchive::ClientVersion::RETAIL)
        {
          for (int j = 0; j < 256; ++j)
          {
            _map_tile->mChunks[j % 16][j / 16]->requeueChunkUpdate(ChunkUpdateFlags::ALPHAMAP);
          }
        }
        else
        {
          _map_tile->registerChunkUpdate(ChunkUpdateFlags::ALPHAMAP);
          for (int j = 0; j < 256; ++j)
          {
            _map_tile->mChunks[j % 16][j / 16]->registerChunkUpdate(ChunkUpdateFlags::ALPHAMAP);
          }
        }

        _alphamap_extended = true;
        break;
      }
    }

    for (int i = 0; i < 256; ++i)
    {
      int chunk_x = i / 16;
      int chunk_y = i % 16;

      auto& chunk = _map_tile->mChunks[chunk_y][chunk_x];

      _chunk_instance_data[i].ChunkXZ_TileXZ[0] = chunk->px;
      _chunk_instance_data[i].ChunkXZ_TileXZ[1] = chunk->py;
      _chunk_instance_data[i].ChunkXZ_TileXZ[2] = static_cast<int>(_map_tile->index.x);
      _chunk_instance_data[i].ChunkXZ_TileXZ[3] = static_cast<int>(_map_tile->index.z);

      unsigned flags = chunk->getUpdateFlags();

      if (!skip_upload_alphamap && (flags & ChunkUpdateFlags::ALPHAMAP || _requires_sampler_reset || _texture_not_loaded))
      {
        gl.activeTexture(GL_TEXTURE0 + 3);
        gl.bindTexture(GL_TEXTURE_2D_ARRAY, _alphamap_tex);
        alphamap_bound = true;
        chunk->texture_set->uploadAlphamapData(_alphamap_extended);

        _uploaded_alphamap_last_frame = true;
        _num_uploaded_chunk_alphamaps++;

        if (!_split_drawcall && !fillSamplers(chunk.get(), i, static_cast<unsigned int>(_draw_calls.size() - 1)))
        {
          _split_drawcall = true;
        }
      }
      // ground effect active-layer overlay. loading is not an edit
      // (doodadMappingNeedsUpdate is only set by alpha edits), so initial
      // uploads keep the stored doodadMapping and only refresh the overlay
      // bits; the recompute waits until the temp edit values have been
      // applied (stroke end) so it reads the real alphamaps
      if (!_texture_not_loaded)
      {
        if (flags & ChunkUpdateFlags::ALPHAMAP)
        {
          if (chunk->doodadMappingNeedsUpdate() && !chunk->texture_set->getTempAlphamaps())
          {
            chunk->texture_set->updateDoodadMapping();
            chunk->clearDoodadMappingNeedsUpdate();
          }
          setChunkGroundEffectActiveData(chunk.get());
        }
        else if (_require_geffect_active_texture_update)
        {
          // active texture changed, just refresh the overlay bits
          setChunkGroundEffectActiveData(chunk.get());
        }
      }

      if (!flags)
        continue;

      if (flags & ChunkUpdateFlags::VERTEX || flags & ChunkUpdateFlags::NORMALS)
      {
        heightmap_bound = true;
        if (flags & ChunkUpdateFlags::VERTEX)
        {
          chunk->updateVerticesData();
        }

        gl.activeTexture(GL_TEXTURE0 + 0);
        gl.bindTexture(GL_TEXTURE_2D, _height_tex);
        gl.texSubImage2D(GL_TEXTURE_2D, 0, 0, i, mapbufsize, 1, GL_RGBA,
                         GL_FLOAT, _map_tile->_chunk_heightmap_buffer.data() + i * mapbufsize * 4);
      }

      if (flags & ChunkUpdateFlags::MCCV)
      {
        mccv_bound = true;
        gl.activeTexture(GL_TEXTURE0 + 1);
        gl.bindTexture(GL_TEXTURE_2D, _mccv_tex);
        chunk->update_vertex_colors();
      }

      if (flags & ChunkUpdateFlags::SHADOW)
      {
        shadowmap_bound = true;
        gl.activeTexture(GL_TEXTURE0 + 2);
        gl.bindTexture(GL_TEXTURE_2D_ARRAY, _shadowmap_tex);
        chunk->update_shadows();
      }

      if (flags & ChunkUpdateFlags::HOLES)
      {
        _chunk_instance_data[i].ChunkHoles_DrawImpass_TexLayerCount_CantPaint[0] = chunk->holes;
        auto const fine_holes = chunk->getHighResHoleMask();
        _chunk_instance_data[i].AreaIDColor_Pad2_DrawSelection[1] = static_cast<std::int32_t>(fine_holes);
        _chunk_instance_data[i].AreaIDColor_Pad2_DrawSelection[2] = static_cast<std::int32_t>(fine_holes >> 32);
      }

      if (flags & ChunkUpdateFlags::FLAGS)
      {
        _chunk_instance_data[i].ChunkHoles_DrawImpass_TexLayerCount_CantPaint[1] = chunk->header_flags.flags.impass;

        updateTextureAnimation(chunk.get(), i);
      }

      if (flags & ChunkUpdateFlags::AREA_ID)
      {
        
        _chunk_instance_data[i].AreaIDColor_Pad2_DrawSelection[0] = chunk->areaID;
      }

      if (flags & ChunkUpdateFlags::GROUND_EFFECT)
      {
          // TODO.
          // currently directly handled in functions
          // setChunkGroundEffectColor()
          // setChunkGroundEffectActiveData()
      }

      if (flags & ChunkUpdateFlags::DETAILDOODADS_EXCLUSION)
      {
          setChunkDetaildoodadsExclusionData(chunk.get());
      }
      

      _chunk_instance_data[i].AreaIDColor_Pad2_DrawSelection[3] = _selected;

      chunk->endChunkUpdates();

      if (_texture_not_loaded || skip_upload_alphamap)
        chunk->requeueChunkUpdate(ChunkUpdateFlags::ALPHAMAP);

    }

    _requires_sampler_reset = false;
    _requires_ground_effect_color_recalc = false;
    _require_geffect_active_texture_update = false;

    if (_split_drawcall)
    {
      rebuildDrawCalls();
    }

    _map_tile->endChunkUpdates();

    if (_texture_not_loaded)
      _map_tile->registerChunkUpdate(ChunkUpdateFlags::ALPHAMAP);

    gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::ChunkInstanceDataUniformBlock) * 256,
                     &_chunk_instance_data);

    // tiles without layers 5+ skip the upload, the shader never reads
    // the block for chunks with a layer count <= 4
    if (_alphamap_extended)
    {
      gl.bindBuffer(GL_UNIFORM_BUFFER, _chunk_layer_ext_ubo);
      gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(_chunk_layer_ext_data), &_chunk_layer_ext_data);
    }
  }

  _map_tile->recalcExtents();

  // Do not render or export a partially assigned texture set.
  if (!texturesReady())
  [[unlikely]]
  {
    gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::CHUNK_INSTANCE_DATA,
                       _chunk_instance_data_ubo, 0, sizeof(OpenGL::ChunkInstanceDataUniformBlock) * 256);
    return;
  }

  gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::CHUNK_INSTANCE_DATA,
                     _chunk_instance_data_ubo, 0, sizeof(OpenGL::ChunkInstanceDataUniformBlock) * 256);
  gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::CHUNK_LAYER_EXT,
                     _chunk_layer_ext_ubo, 0, sizeof(_chunk_layer_ext_data));


  for (auto& draw_call : _draw_calls)
  {

    if (!alphamap_bound)
    {
      gl.activeTexture(GL_TEXTURE0 + 3);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, _alphamap_tex);
    }

    if (!shadowmap_bound)
    {
      gl.activeTexture(GL_TEXTURE0 + 2);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, _shadowmap_tex);
    }

    if (!mccv_bound)
    {
      gl.activeTexture(GL_TEXTURE0 + 1);
      gl.bindTexture(GL_TEXTURE_2D, _mccv_tex);
    }

    if (!heightmap_bound)
    {
      gl.activeTexture(GL_TEXTURE0 + 0);
      gl.bindTexture(GL_TEXTURE_2D, _height_tex);
    }

    float tile_center_x = _map_tile->xbase + TILESIZE / 2.0f;
    float tile_center_z = _map_tile->zbase + TILESIZE / 2.0f;

    bool is_lod = misc::dist(tile_center_x, tile_center_z, camera.x, camera.z) > TILESIZE * 3;
    if (is_lod && Noggit::Application::NoggitApplication::instance()->clientData()->version()
        == BlizzardArchive::ClientVersion::RETAIL)
    {
      // The coarse mesh spans 2x2 cells per fan and cannot represent an 8x8
      // hole mask. Keep exact geometry for batches containing Retail holes.
      for (unsigned i = draw_call.start_chunk; i < draw_call.start_chunk + draw_call.n_chunks; ++i)
        if (_chunk_instance_data[i].AreaIDColor_Pad2_DrawSelection[1]
            || _chunk_instance_data[i].AreaIDColor_Pad2_DrawSelection[2])
        {
          is_lod = false;
          break;
        }
    }
    mcnk_shader.uniform("lod_level", int(is_lod));

    assert(draw_call.n_chunks <= 256);
    mcnk_shader.uniform("base_instance", static_cast<int>(draw_call.start_chunk));

    for (std::size_t i = 0; i < draw_call.samplers.size(); ++i)
    {
      gl.activeTexture(GL_TEXTURE0 + 5 + i);

      if (draw_call.samplers[i] < 0)
      {
        gl.bindTexture(GL_TEXTURE_2D_ARRAY, 0);
        continue;
      }

      gl.bindTexture(GL_TEXTURE_2D_ARRAY, draw_call.samplers[i]);
    }

    if (is_lod)
    {
      gl.drawElementsInstanced(GL_TRIANGLES, 192, GL_UNSIGNED_SHORT,
                               reinterpret_cast<void*>(768 * sizeof(std::uint16_t)), draw_call.n_chunks);
    }
    else
    {
      gl.drawElementsInstanced(GL_TRIANGLES, 768, GL_UNSIGNED_SHORT, nullptr,
                               draw_call.n_chunks);
    }

  }
}

void TileRender::uploadTextures()
{
  _chunk_texture_arrays.upload();
  gl.activeTexture(GL_TEXTURE0 + 0);
  gl.bindTexture(GL_TEXTURE_2D, _height_tex);
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, mapbufsize,
                256, 0, GL_RGBA, GL_FLOAT,nullptr);

  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  //const GLint swizzleMask[] = {GL_RED, GL_RED, GL_RED, GL_ONE};
  //gl.texParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzleMask);

  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  gl.bindTexture(GL_TEXTURE_2D, 0);

  gl.activeTexture(GL_TEXTURE0 + 2);
  gl.bindTexture(GL_TEXTURE_2D, _mccv_tex);
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, mapbufsize,
                256, 0, GL_RGB, GL_FLOAT, nullptr);

  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  gl.bindTexture(GL_TEXTURE_2D, 0);

  gl.activeTexture(GL_TEXTURE0 + 4);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, _alphamap_tex);
  gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB, 64, 64,
                256, 0, GL_RGB, GL_FLOAT,nullptr);

  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, 0);


  gl.activeTexture(GL_TEXTURE0 + 3);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, _shadowmap_tex);
  gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RED, 64, 64, 256,
                0, GL_RED, GL_UNSIGNED_BYTE,nullptr);

  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void TileRender::doTileOcclusionQuery(OpenGL::Scoped::use_program& occlusion_shader)
{
  if (_tile_occlusion_query_in_use || !_uploaded)
    return;

  _tile_occlusion_query_in_use = true;
  gl.beginQuery(GL_ANY_SAMPLES_PASSED, _tile_occlusion_query);
  occlusion_shader.uniform("aabb", _map_tile->getCombinedExtents().data(), _map_tile->getCombinedExtents().size());
  gl.drawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
  gl.endQuery(GL_ANY_SAMPLES_PASSED);
}

bool TileRender::getTileOcclusionQueryResult(glm::vec3 const& camera)
{
  // returns true if tile is not occluded by other tiles

  if (!_tile_occlusion_query_in_use)
  [[unlikely]]
  {
    return !_tile_occluded;
  }

  if (!_uploaded)
    return !_tile_occluded;

  if (misc::pointInside(camera, _map_tile->getCombinedExtents()))
  {
    _tile_occlusion_query_in_use = false;
    return true;
  }

  GLint result;
  gl.getQueryObjectiv(_tile_occlusion_query, GL_QUERY_RESULT_AVAILABLE, &result);

  if (result != GL_TRUE)
  {
    return _tile_occlusion_cull_override || !_tile_occluded;
  }

  if (!_tile_occlusion_cull_override)
  {
    gl.getQueryObjectiv(_tile_occlusion_query, GL_QUERY_RESULT, &result);
  }
  else
  {
    result = true;
  }

  _tile_occlusion_query_in_use = false;
  _tile_occlusion_cull_override = false;

  return static_cast<bool>(result);
}

void Noggit::Rendering::TileRender::discardTileOcclusionQuery()
{
  _tile_occlusion_query_in_use = false;
}

void Noggit::Rendering::TileRender::notifyTileRendererOnSelectedTextureChange()
{
  _requires_paintability_recalc = true;
}


void TileRender::pruneUnusedSamplers(bool skip_upload_alphamap)
{
  bool const modern_features = Noggit::Application::NoggitApplication::instance()->getConfiguration()->modern_features;
  bool const retail = Noggit::Application::NoggitApplication::instance()->clientData()->version()
    == BlizzardArchive::ClientVersion::RETAIL;
  for (auto& draw_call : _draw_calls)
  {
    std::array<bool, MapTileDrawCall::sampler_count> used{};
    auto retain = [&](GLuint texture_array)
    {
      for (std::size_t sampler = 0; sampler < used.size(); ++sampler)
        if (texture_array && draw_call.samplers[sampler] == texture_array)
          used[sampler] = true;
    };

    for (unsigned i = draw_call.start_chunk; i < draw_call.start_chunk + draw_call.n_chunks; ++i)
    {
      auto const& texture_set = _map_tile->mChunks[i % 16][i / 16]->texture_set;
      auto const& textures = *texture_set->getTextures();
      auto const& instance = _chunk_instance_data[i];
      if (skip_upload_alphamap && instance.ChunkHoles_DrawImpass_TexLayerCount_CantPaint[2] != texture_set->num())
        _requires_sampler_reset = true;
      auto matches = [&](int sampler, int index, auto* texture)
      {
        return texture->is_uploaded() && sampler >= 0 && sampler < draw_call.samplers.size()
          && draw_call.samplers[sampler] == texture->texture_array() && index == texture->array_index();
      };
      for (int layer = 0; layer < texture_set->num(); ++layer)
      {
        retain(textures[layer]->texture_array());
        auto* height = modern_features ? texture_set->heightTexture(layer) : nullptr;
        if (height)
          retain(height->texture_array());
        if (!skip_upload_alphamap)
          continue;

        // Shared pages keep their GLuint when a layer is freed or reused.
        // Validate the old mapping's slice too before a deferred frame draws it.
        bool const base = layer < BASE_RENDER_TEXTURE_LAYERS;
        auto const& ext = _chunk_layer_ext_data[i * EXT_RENDER_TEXTURE_LAYERS + (base ? 0 : layer - BASE_RENDER_TEXTURE_LAYERS)];
        int const sampler = base ? instance.ChunkTextureSamplers[layer] : ext.params_packed & 0xF;
        int const index = base ? instance.ChunkTextureArrayIDs[layer] : ext.array_index;
        int const decoded_index = index < 0 ? -(index + 1) : index;
        if (!matches(sampler, decoded_index, textures[layer].get()) || (index >= 0) != textures[layer]->is_specular())
          _requires_sampler_reset = true;
        if (!modern_features || (!base && !retail))
          continue;

        auto const data = texture_set->hasHeightMappingData(layer) ? texture_set->getHeightMappingData(layer)
          : _map_tile->GetTextureHeightMappingData(textures[layer]->file_key().filepath());
        int const uv_scale = base ? instance.ChunkTextureUVScale[layer] : (ext.params_packed >> 11) & 0xF;
        float const height_scale = base ? instance.ChunkTextureHeightScale[layer] : ext.height_scale;
        float const height_offset = base ? instance.ChunkTextureHeightOffset[layer] : ext.height_offset;
        if (uv_scale != data.uvScale || height_scale != (height ? data.heightScale : 0.0f)
          || height_offset != (height ? data.heightOffset : 1.0f))
          _requires_sampler_reset = true;
        if (height)
        {
          int const height_sampler = base ? instance.ChunkHeightTextureSamplers[layer] : (ext.params_packed >> 15) & 0xF;
          int const height_index = base ? instance.ChunkHeightTextureArrayIDs[layer] : (ext.params_packed >> 19) & 0xFF;
          if (!matches(height_sampler, height_index, height))
            _requires_sampler_reset = true;
        }
      }
    }

    // Preserve the indices of live bindings: alpha-only painting needs no
    // sampler rebuild or re-upload of the other chunks' instance data.
    for (std::size_t sampler = 0; sampler < used.size(); ++sampler)
      if (!used[sampler] && draw_call.samplers[sampler] >= 0)
      {
        draw_call.samplers[sampler] = -1;
        // A throttled frame retains the old per-chunk sampler mappings. Do not
        // draw those mappings after pruning one of the names they still use.
        if (skip_upload_alphamap)
          _requires_sampler_reset = true;
      }
  }
}

void TileRender::updateTextureAnimation(MapChunk* chunk, unsigned chunk_index)
{
  for (int layer = 0; layer < BASE_RENDER_TEXTURE_LAYERS + EXT_RENDER_TEXTURE_LAYERS; ++layer)
  {
    unsigned const flags = layer < chunk->texture_set->num() ? chunk->texture_set->flag(layer) : 0;
    unsigned const enabled = (flags >> 6) & 1;
    unsigned const speed = (flags >> 3) & 7;
    unsigned const direction = flags & 7;
    if (layer < BASE_RENDER_TEXTURE_LAYERS)
    {
      _chunk_instance_data[chunk_index].ChunkTexDoAnim[layer] = enabled;
      _chunk_instance_data[chunk_index].ChunkTexAnimSpeed[layer] = speed;
      _chunk_instance_data[chunk_index].ChunkTexAnimDir[layer] = direction;
    }
    else
    {
      auto& params = _chunk_layer_ext_data[chunk_index * EXT_RENDER_TEXTURE_LAYERS + layer - BASE_RENDER_TEXTURE_LAYERS];
      params.params_packed = (params.params_packed & ~(0x7F << 4)) | (enabled << 4) | (speed << 5) | (direction << 8);
    }
  }
}

void TileRender::rebuildDrawCalls()
{
  bool const previous_error = _sampler_error;
  _sampler_error = false;
  _requires_sampler_reset = false;
  _draw_calls.clear();
  auto append_call = [&](unsigned start)
  {
    auto& call = _draw_calls.emplace_back();
    call.samplers.fill(-1);
    call.start_chunk = start;
    call.n_chunks = 0;
  };
  append_call(0);

  for (unsigned i = 0; i < 256; ++i)
  {
    auto* chunk = _map_tile->mChunks[i % 16][i / 16].get();
    if (!fillSamplers(chunk, i, static_cast<unsigned>(_draw_calls.size() - 1)))
    {
      if (_draw_calls.back().n_chunks)
      {
        append_call(i);
        if (fillSamplers(chunk, i, static_cast<unsigned>(_draw_calls.size() - 1)))
        {
          ++_draw_calls.back().n_chunks;
          continue;
        }
      }

      // Splitting cannot help a single chunk which needs too many arrays.
      // Do not publish a partial mapping as ready or repeatedly reload its BLPs.
      _sampler_error = true;
      if (!previous_error)
        LogError << "Terrain tile " << _map_tile->index.x << '_' << _map_tile->index.z
                 << ", chunk " << chunk->px << '_' << chunk->py
                 << " exceeds the " << MapTileDrawCall::sampler_count
                 << " texture-array sampler limit; rendering and map texture export are unavailable.\n";
      return;
    }
    ++_draw_calls.back().n_chunks;
  }

  if (_draw_calls.size() == 1)
  {
    _split_drawcall = false;
  }
}

bool TileRender::fillSamplers(MapChunk* chunk, unsigned chunk_index,  unsigned draw_call_index)
{
  MapTileDrawCall& draw_call = _draw_calls[draw_call_index];
  auto samplers = draw_call.samplers;

  _chunk_instance_data[chunk_index].ChunkHoles_DrawImpass_TexLayerCount_CantPaint[2] = static_cast<int>(chunk->texture_set->num());

  auto sampler_for = [&](GLuint texture_array) -> int
  {
    // Pruning can leave holes before a live binding. Reuse that binding before
    // claiming a hole, otherwise alpha-only edits can exhaust the sampler budget.
    auto sampler = std::find(samplers.begin(), samplers.end(), texture_array);
    if (sampler == samplers.end())
    {
      sampler = std::find(samplers.begin(), samplers.end(), -1);
      if (sampler == samplers.end())
        return -1;
      *sampler = texture_array;
    }
    return static_cast<int>(sampler - samplers.begin());
  };

  for (int i = 0; i < BASE_RENDER_TEXTURE_LAYERS; i++)
  {
      _chunk_instance_data[chunk_index].ChunkTextureSamplers[i] = 0;
      _chunk_instance_data[chunk_index].ChunkTextureArrayIDs[i] = -1;

      // Mists Heightmapping
      _chunk_instance_data[chunk_index].ChunkHeightTextureSamplers[i] = 0;
      _chunk_instance_data[chunk_index].ChunkHeightTextureArrayIDs[i] = 0;
      _chunk_instance_data[chunk_index].ChunkTextureUVScale[i] = 0;
      _chunk_instance_data[chunk_index].ChunkTextureHeightScale[i] = 0;
      _chunk_instance_data[chunk_index].ChunkTextureHeightOffset[i] = 1.0f;
  }

  OpenGL::ChunkExtLayerParams* ext_layers = &_chunk_layer_ext_data[chunk_index * EXT_RENDER_TEXTURE_LAYERS];
  for (int i = 0; i < EXT_RENDER_TEXTURE_LAYERS; i++)
  {
      ext_layers[i] = OpenGL::ChunkExtLayerParams{};
  }

  // Layer insertion, removal and reordering only mark ALPHAMAP. Refresh their
  // animation metadata together with the sampler mapping, clearing unused slots.
  updateTextureAnimation(chunk, chunk_index);

  auto& chunk_textures = (*chunk->texture_set->getTextures());
  bool const modern_features = Noggit::Application::NoggitApplication::instance()->getConfiguration()->modern_features;
  bool const retail = Noggit::Application::NoggitApplication::instance()->clientData()->version()
    == BlizzardArchive::ClientVersion::RETAIL;

  for (int k = 0; k < chunk->texture_set->num(); ++k)
  {
    chunk_textures[k]->upload();

    if (!chunk_textures[k]->is_uploaded())
    {
      _texture_not_loaded = true;
      continue;
    }

    bool const base_layer = k < BASE_RENDER_TEXTURE_LAYERS;
    bool const height_mapping = modern_features && (base_layer || retail);

    auto* heightRef = height_mapping ? chunk->texture_set->heightTexture(k) : nullptr;
    if (heightRef)
    {
        heightRef->upload();

        if(!heightRef->is_uploaded())
        {
            _texture_not_loaded = true;
            continue;
        }
    }

    if (height_mapping) {
        // Mists uses four layers; Retail extends this metadata to every layer.
        auto const hData = chunk->texture_set->hasHeightMappingData(k)
          ? chunk->texture_set->getHeightMappingData(k)
          : chunk->mt->GetTextureHeightMappingData(chunk_textures[k]->file_key().filepath());
        if (base_layer)
        {
          _chunk_instance_data[chunk_index].ChunkTextureUVScale[k] = static_cast<int>(hData.uvScale);
          _chunk_instance_data[chunk_index].ChunkTextureHeightScale[k] = heightRef ? hData.heightScale : 0.0f;
          _chunk_instance_data[chunk_index].ChunkTextureHeightOffset[k] = heightRef ? hData.heightOffset : 1.0f;
        }
        else
        {
          auto& ext = ext_layers[k - BASE_RENDER_TEXTURE_LAYERS];
          ext.params_packed |= (hData.uvScale & 0xF) << 11;
          ext.height_scale = heightRef ? hData.heightScale : 0.0f;
          ext.height_offset = heightRef ? hData.heightOffset : 1.0f;
        }
    }

    GLuint tex_array = (*chunk->texture_set->getTextures())[k]->texture_array();
    int tex_index = (*chunk->texture_set->getTextures())[k]->array_index();

    int sampler_id = sampler_for(tex_array);

    // Retry this chunk in a separate draw call if the current one is full.
    if (sampler_id < 0)
    [[unlikely]]
    {
      return false;
    }

    if (!base_layer)
    {
      OpenGL::ChunkExtLayerParams& ext_params = ext_layers[k - BASE_RENDER_TEXTURE_LAYERS];
      ext_params.params_packed = (ext_params.params_packed & ~0xF) | sampler_id;
      ext_params.array_index = chunk_textures[k]->is_specular() ? tex_index : -(tex_index + 1);
    }
    else
    {
      _chunk_instance_data[chunk_index].ChunkTextureSamplers[k] = sampler_id;
      _chunk_instance_data[chunk_index].ChunkTextureArrayIDs[k] = chunk_textures[k]->is_specular() ? tex_index : -(tex_index + 1);
    }

    if(modern_features && heightRef)
    {
        GLuint hTex_array = heightRef->texture_array();

        sampler_id = sampler_for(hTex_array);

        if (sampler_id < 0)
            [[unlikely]]
        {
          return false;
        }

        if (base_layer)
        {
          _chunk_instance_data[chunk_index].ChunkHeightTextureSamplers[k] = sampler_id;
          _chunk_instance_data[chunk_index].ChunkHeightTextureArrayIDs[k] = heightRef->array_index();
        }
        else
        {
          // The bounded texture pages fit in the eight-bit height slice field.
          auto& ext = ext_layers[k - BASE_RENDER_TEXTURE_LAYERS];
          ext.params_packed |= (sampler_id << 15) | (heightRef->array_index() << 19);
        }
    }
  }

  // A failed append is retried in a new draw call. Keep the previous call's
  // bindings intact instead of leaving names from a partially assigned chunk.
  draw_call.samplers = samplers;
  return true;
}

void Noggit::Rendering::TileRender::setChunkGroundEffectColor(unsigned int chunkid, glm::vec3 color)
{
    if (chunkid > 255)
        return;

    // int chunk_x = chunkid / 16;
    // int chunk_y = chunkid % 16;
    // auto& chunk = _map_tile->mChunks[chunk_y][chunk_x];
    // chunk->registerChunkUpdate(ChunkUpdateFlags::GROUND_EFFECT);

    _requires_ground_effect_color_recalc = true;

    _chunk_instance_data[chunkid].ChunkGroundEffectColor[0] = color.r;
    _chunk_instance_data[chunkid].ChunkGroundEffectColor[1] = color.g;
    _chunk_instance_data[chunkid].ChunkGroundEffectColor[2] = color.b;
    _chunk_instance_data[chunkid].ChunkGroundEffectColor[3] = 0.0; // not used
}

void TileRender::initChunkData(MapChunk* chunk)
{
  auto& chunk_render_instance = _chunk_instance_data[chunk->px * 16 + chunk->py];

  chunk_render_instance.ChunkHoles_DrawImpass_TexLayerCount_CantPaint[0] = chunk->holes;
  chunk_render_instance.ChunkHoles_DrawImpass_TexLayerCount_CantPaint[1] = chunk->header_flags.flags.impass;
  chunk_render_instance.ChunkHoles_DrawImpass_TexLayerCount_CantPaint[2] = static_cast<int>(chunk->texture_set->num());
  chunk_render_instance.ChunkHoles_DrawImpass_TexLayerCount_CantPaint[3] = 0;
  chunk_render_instance.AreaIDColor_Pad2_DrawSelection[0] = chunk->areaID;
  auto const fine_holes = chunk->getHighResHoleMask();
  chunk_render_instance.AreaIDColor_Pad2_DrawSelection[1] = static_cast<std::int32_t>(fine_holes);
  chunk_render_instance.AreaIDColor_Pad2_DrawSelection[2] = static_cast<std::int32_t>(fine_holes >> 32);
  chunk_render_instance.AreaIDColor_Pad2_DrawSelection[3] = 0;

  chunk_render_instance.ChunkGroundEffectColor[0] = 0.0f;
  chunk_render_instance.ChunkGroundEffectColor[1] = 0.0f;
  chunk_render_instance.ChunkGroundEffectColor[2] = 0.0f;
  chunk_render_instance.ChunkGroundEffectColor[3] = 0.0f;

  // setChunkDetaildoodadsExclusionData(chunk);
  chunk_render_instance.ChunkDoodadsEnabled2_ChunksLayerEnabled2[0] = 0;
  chunk_render_instance.ChunkDoodadsEnabled2_ChunksLayerEnabled2[1] = 0;
  chunk_render_instance.ChunkDoodadsEnabled2_ChunksLayerEnabled2[2] = 0;
  chunk_render_instance.ChunkDoodadsEnabled2_ChunksLayerEnabled2[3] = 0;
}

void TileRender::setChunkDetaildoodadsExclusionData(MapChunk* chunk)
{
  auto doodadExclusionMap = chunk->texture_set->getDoodadStencilBase();

  // pack it to int32s
  int32_t exclusionmap1 = (int32_t)((uint32_t)(doodadExclusionMap[0] << 0) | (uint32_t)(doodadExclusionMap[1] << 8)
      | (uint32_t)(doodadExclusionMap[2] << 16) | (uint32_t)(doodadExclusionMap[3] << 24));

  int32_t exclusionmap2 = (int32_t)((uint32_t)(doodadExclusionMap[4] << 0) | (uint32_t)(doodadExclusionMap[5] << 8)
      | (uint32_t)(doodadExclusionMap[6] << 16) | (uint32_t)(doodadExclusionMap[7] << 24));

  auto& chunk_render_instance = _chunk_instance_data[chunk->px * 16 + chunk->py];
  chunk_render_instance.ChunkDoodadsEnabled2_ChunksLayerEnabled2[0] = exclusionmap1;
  chunk_render_instance.ChunkDoodadsEnabled2_ChunksLayerEnabled2[1] = exclusionmap2;
}

void Noggit::Rendering::TileRender::setChunkGroundEffectActiveData(MapChunk* chunk)
{
  // 1 : check if chunk has our texture AND set
  // if it does, then check if it's the active layer for each unit
  
  // get the layer id of our texture
  int layer_id = -1;
  for (int i = 0; i < chunk->getTextureSet()->num(); ++i)
  {
      if (chunk->getTextureSet()->filename(i) == _geffect_active_texture)
      {
          layer_id = i;
      }
  }

  // int layer_id = chunk->getTextureSet()->texture_id(active_texture); // -1 if not present

  auto& chunk_render_instance = _chunk_instance_data[chunk->px * 16 + chunk->py];

  if (layer_id == -1)
  {
      chunk_render_instance.ChunkDoodadsEnabled2_ChunksLayerEnabled2[2] = 0;
      chunk_render_instance.ChunkDoodadsEnabled2_ChunksLayerEnabled2[3] = 0;
      return;
  }

  int32_t active_map1 = 0;
  int32_t active_map2 = 0;

  // convert layer id to bool (Is Active). packed like the exclusion map and
  // the on-disk doodadMapping: bit = unit_z * 8 + unit_x (row = z, slot = x)
  for (unsigned int y = 0; y < 8; y++)
  {
      for (unsigned int x = 0; x < 8; x++)
      {
          uint8_t unit_layer_id = chunk->texture_set->getDoodadActiveLayerIdAt(x, y);

          if (layer_id != unit_layer_id)
              continue;

          int bit = y * 8 + x;

          if (bit < 32)
            active_map1 |= (1 << bit);
          else
            active_map2 |= (1 << (bit-32));
      }
  }

  chunk_render_instance.ChunkDoodadsEnabled2_ChunksLayerEnabled2[2] = active_map1;
  chunk_render_instance.ChunkDoodadsEnabled2_ChunksLayerEnabled2[3] = active_map2;
}

void Noggit::Rendering::TileRender::setActiveRenderGEffectTexture(std::string active_texture)
{
    if (active_texture == _geffect_active_texture)
        return;

    _geffect_active_texture = active_texture;

    _require_geffect_active_texture_update = true;

}

[[nodiscard]]
unsigned Noggit::Rendering::TileRender::objectsFrustumCullTest() const
{
  return _objects_frustum_cull_test;
}

void Noggit::Rendering::TileRender::setObjectsFrustumCullTest(unsigned state)
{
  _objects_frustum_cull_test = state;
}

[[nodiscard]]
bool Noggit::Rendering::TileRender::isOccluded() const
{
  return _tile_occluded;
}

void Noggit::Rendering::TileRender::setOccluded(bool state)
{
  _tile_occluded = state;
}

[[nodiscard]]
bool Noggit::Rendering::TileRender::isFrustumCulled() const
{
  return _tile_frustum_culled;
}

void Noggit::Rendering::TileRender::setFrustumCulled(bool state)
{
  _tile_frustum_culled = state;
}

[[nodiscard]]
bool Noggit::Rendering::TileRender::isOverridingOcclusionCulling() const
{
  return _tile_occlusion_cull_override;
}

void Noggit::Rendering::TileRender::setOverrideOcclusionCulling(bool state)
{
  _tile_frustum_culled = state;
}

[[nodiscard]]
bool Noggit::Rendering::TileRender::isUploaded() const
{
  return _uploaded;
}

[[nodiscard]]
bool Noggit::Rendering::TileRender::alphamapUploadedLastFrame() const
{
  return _uploaded_alphamap_last_frame;
}

int Noggit::Rendering::TileRender::numUploadedChunkAlphamaps() const
{
  return _num_uploaded_chunk_alphamaps;
}
