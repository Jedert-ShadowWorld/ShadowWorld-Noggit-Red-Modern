// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "WMOGroupRender.hpp"
#include <noggit/WMO.h>
#include <noggit/formats/WmoMaterial.hpp>
#include <noggit/wmo_liquid.hpp>
#include <noggit/Log.h> // LogDebug
#include <noggit/TextureManager.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/project/CurrentProject.hpp>

#include <math/frustum.hpp>

#include <opengl/shader.hpp>

#include <QSettings>

using namespace Noggit::Rendering;

WMOGroupRender::WMOGroupRender(WMOGroup* wmo_group)
: _wmo_group(wmo_group)
{

}

void WMOGroupRender::upload()
{
  _upload_attempted = true;
  if (_reload_before_upload)
  {
    try
    {
      // Upload releases CPU-only streams. Read them again only if a surviving
      // cached group is drawn after its old OpenGL resources were released.
      _wmo_group->load();
      _reload_before_upload = false;
    }
    catch (std::exception const& error)
    {
      LogError << "Could not restore WMO group rendering data: " << error.what() << std::endl;
      return;
    }
  }
  auto const& materials = _wmo_group->wmo->materials;
  auto const& textures = _wmo_group->wmo->textures;
  if (materials.empty() || textures.empty())
  {
    LogError << "Skipping WMO group upload with no usable materials or textures." << std::endl;
    return;
  }

  bool const modern_wmo = _wmo_group->wmo->uses_file_data_ids();
  auto const* project = Noggit::Project::CurrentProject::get();
  bool const retail = modern_wmo && project && project->projectVersion == Noggit::Project::ProjectVersion::RETAIL;
  if (!retail)
    packLegacyBatches();
  else if (!packRetailBatches())
    return;

  // opengl resources
  _vertex_array.upload();
  _buffers.upload();
  gl.genTextures(1, &_render_batch_tex);

  gl.bufferData<GL_ARRAY_BUFFER> ( _vertices_buffer
      , _wmo_group->_vertices.size() * sizeof (*_wmo_group->_vertices.data())
      , _wmo_group->_vertices.data()
      , GL_STATIC_DRAW
  );

  gl.bufferData<GL_ARRAY_BUFFER> ( _normals_buffer
      , _wmo_group->_normals.size() * sizeof (*_wmo_group->_normals.data())
      , _wmo_group->_normals.data()
      , GL_STATIC_DRAW
  );

  gl.bufferData<GL_ARRAY_BUFFER> ( _texcoords_buffer
      , _wmo_group->_texcoords.size() * sizeof (*_wmo_group->_texcoords.data())
      , _wmo_group->_texcoords.data()
      , GL_STATIC_DRAW
  );

  gl.bufferData<GL_ARRAY_BUFFER> ( _render_batch_mapping_buffer
      , _render_batch_mapping.size() * sizeof(unsigned)
      , _render_batch_mapping.data()
      , GL_STATIC_DRAW
  );

  gl.bindBuffer(GL_TEXTURE_BUFFER, _render_batch_tex_buffer);
  gl.bufferData(GL_TEXTURE_BUFFER, _render_batches.size() * sizeof(WMORenderBatch),_render_batches.data(), GL_STATIC_DRAW);
  gl.bindTexture(GL_TEXTURE_BUFFER, _render_batch_tex);
  gl.texBuffer(GL_TEXTURE_BUFFER,  GL_RGBA32UI, _render_batch_tex_buffer);

  {
    // Element-buffer bindings belong to a VAO. A cold upload must not replace
    // the index buffer of another cached mesh that is currently bound.
    OpenGL::Scoped::vao_binder const upload_vao(_vao);
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_buffer, _wmo_group->_indices, GL_STATIC_DRAW);
  }

  if (_wmo_group->header.flags.has_two_motv)
  {
    gl.bufferData<GL_ARRAY_BUFFER, glm::vec2> ( _texcoords_buffer_2
        , _wmo_group->_texcoords_2
        , GL_STATIC_DRAW
    );
  }

  gl.bufferData<GL_ARRAY_BUFFER> ( _vertex_colors_buffer
      , _wmo_group->_vertex_colors.size() * sizeof (*_wmo_group->_vertex_colors.data())
      , _wmo_group->_vertex_colors.data()
      , GL_STATIC_DRAW
  );

  if (retail)
  {
    gl.bufferData<GL_ARRAY_BUFFER, glm::vec2>(_texcoords_buffer_3, _wmo_group->_texcoords_3, GL_STATIC_DRAW);
    gl.bufferData<GL_ARRAY_BUFFER, glm::vec2>(_texcoords_buffer_4, _wmo_group->_texcoords_4, GL_STATIC_DRAW);
    gl.bufferData<GL_ARRAY_BUFFER, glm::vec4>(_material_weights_buffer, _wmo_group->_material_weights, GL_STATIC_DRAW);
    gl.bufferData<GL_ARRAY_BUFFER, float>(_material_blend_alpha_buffer, _wmo_group->_material_blend_alpha, GL_STATIC_DRAW);
  }

  // free unused data
  _wmo_group->_normals.clear();
  _wmo_group->_texcoords.clear();
  _wmo_group->_texcoords_2.clear();
  _wmo_group->_texcoords_3.clear();
  _wmo_group->_texcoords_4.clear();
  _wmo_group->_material_weights.clear();
  _wmo_group->_material_blend_alpha.clear();
  _wmo_group->_vertex_colors.clear();
  _render_batches.clear();
  _render_batch_mapping.clear();

  _uploaded = true;
}

void WMOGroupRender::packLegacyBatches()
{
  auto const& materials = _wmo_group->wmo->materials;
  auto const& textures = _wmo_group->wmo->textures;

  // render batches

  bool const modern_wmo = _wmo_group->wmo->uses_file_data_ids();

  std::size_t batch_counter = 0;
  for (auto& batch : _wmo_group->_batches)
  {
    std::uint16_t material_to_use = batch.texture;
    if (modern_wmo && (batch.flags & 2))
        material_to_use = batch.unused[5];

    if (material_to_use >= materials.size())
    {
      LogError << "Invalid WMO material index " << material_to_use
               << "; using material 0." << std::endl;
      material_to_use = 0;
    }
    WMOMaterial const& mat(materials[material_to_use]);

    auto const texture1 = mat.texture1 < textures.size() ? mat.texture1 : 0;
    auto& tex1 = textures[texture1];

    tex1->wait_until_loaded();
    tex1->upload();

    std::uint32_t tex_array0 = tex1->texture_array();
    std::uint32_t array_index0 = tex1->array_index();

    std::uint32_t tex_array1 = 0;
    std::uint32_t array_index1 = 0;

    bool use_tex2 = mat.shader == 6 || mat.shader == 5 || mat.shader == 3 || mat.shader == 21 || mat.shader == 23;

    if (use_tex2)
    {
      auto const texture2 = mat.texture2 < textures.size() ? mat.texture2 : 0;
      auto& tex2 = textures[texture2];
      tex2->wait_until_loaded();
      tex2->upload();

      tex_array1 = tex2->texture_array();
      array_index1 = tex2->array_index();
    }

    _render_batches[batch_counter].tex_array0 = tex_array0;
    _render_batches[batch_counter].tex_array1 = tex_array1;
    _render_batches[batch_counter].tex0 = array_index0;
    _render_batches[batch_counter].tex1 = array_index1;

    batch_counter++;
  }

  _draw_calls.clear();
  WMOCombinedDrawCall* draw_call = nullptr;
  batch_counter = 0;
  for (auto& batch : _wmo_group->_batches)
  {
    std::uint16_t material_to_use = batch.texture;
    if (modern_wmo && (batch.flags & 2))
        material_to_use = batch.unused[5];

    if (material_to_use >= materials.size())
      material_to_use = 0;
    WMOMaterial const& mat(materials[material_to_use]);

    bool backface_cull = !mat.flags.unculled;
    bool use_tex2 = mat.shader == 6 || mat.shader == 5 || mat.shader == 3 || mat.shader == 21 || mat.shader == 23;

    bool create_draw_call = false;
    if (draw_call && draw_call->backface_cull == backface_cull && batch.index_start == draw_call->index_start + draw_call->index_count)
    {
      // identify if we can fit this batch into current draw_call
      unsigned n_avaliable_slots = static_cast<unsigned>(draw_call->samplers.size()) - draw_call->n_used_samplers;
      unsigned n_slots_to_be_occupied = 0;

      decltype(draw_call->samplers)::iterator it2;
      auto it = std::find(draw_call->samplers.begin(), draw_call->samplers.end(), _render_batches[batch_counter].tex_array0);

      if (it == draw_call->samplers.end())
      {
        if (n_avaliable_slots)
          n_slots_to_be_occupied++;
        else
          create_draw_call = true;
      }


      if (!create_draw_call && use_tex2)
      {
        it2 = std::find(draw_call->samplers.begin(), draw_call->samplers.end(), _render_batches[batch_counter].tex_array1);

        if (it2 == draw_call->samplers.end())
        {
          if (n_slots_to_be_occupied < n_avaliable_slots)
            n_slots_to_be_occupied++;
          else
            create_draw_call = true;
        }

      }

      if (!create_draw_call)
      {
        if (it != draw_call->samplers.end())
        {
          _render_batches[batch_counter].tex_array0 = it - draw_call->samplers.begin();
        }
        else
        {
          draw_call->samplers[draw_call->n_used_samplers] = _render_batches[batch_counter].tex_array0;
          _render_batches[batch_counter].tex_array0 = draw_call->n_used_samplers;
          draw_call->n_used_samplers++;
        }

        if (use_tex2)
        {
          if (it2 != draw_call->samplers.end())
          {
            _render_batches[batch_counter].tex_array1 = it2 - draw_call->samplers.begin();
          }
          else
          {
            draw_call->samplers[draw_call->n_used_samplers] = _render_batches[batch_counter].tex_array1;
            _render_batches[batch_counter].tex_array1 = draw_call->n_used_samplers;
            draw_call->n_used_samplers++;
          }
        }
      }

    }
    else
    {
      create_draw_call = true;
    }

    if (create_draw_call)
    {
      // create new combined draw call
      draw_call = &_draw_calls.emplace_back();
      draw_call->samplers.fill(-1);
      draw_call->index_start = batch.index_start;
      draw_call->index_count = 0;
      draw_call->n_used_samplers = use_tex2 ? 2 : 1;
      draw_call->backface_cull = backface_cull;

      draw_call->samplers[0] = _render_batches[batch_counter].tex_array0;
      _render_batches[batch_counter].tex_array0 = 0;

      if (use_tex2)
          [[unlikely]]
      {
        draw_call->samplers[1] = _render_batches[batch_counter].tex_array1;
        _render_batches[batch_counter].tex_array1 = 1;
      }

    }

    draw_call->index_count += batch.index_count;

    batch_counter++;
  }
}

bool WMOGroupRender::packRetailBatches()
{
  auto const& materials = _wmo_group->wmo->materials;
  auto const& textures = _wmo_group->wmo->textures;

  // BLP wait_until_loaded() does not block. Every texture of the group must be
  // resident before array/layer handles are cached and CPU streams are released.
  for (auto const& batch : _wmo_group->_batches)
  {
    auto const material_index = Noggit::Format::wmo_material_index(batch, true, materials.size());
    unsigned const texture_count = Noggit::Format::wmo_texture_count(materials[material_index].shader, true);
    for (unsigned slot = 0; slot < texture_count; ++slot)
    {
      auto const index = _wmo_group->wmo->material_textures[material_index][slot];
      auto const& texture = textures[index < textures.size() ? index : 0];
      if (!texture->finishedLoading())
      {
        _upload_attempted = false;
        return false;
      }
      if (texture->loading_failed())
      {
        LogError << "Skipping WMO group upload: texture load failed for material "
                 << material_index << ", slot " << slot << std::endl;
        return false;
      }
    }
  }
  _draw_calls.clear();
  WMOCombinedDrawCall* draw_call = nullptr;
  std::size_t batch_counter = 0;
  for (auto const& batch : _wmo_group->_batches)
  {
    auto const material_index = Noggit::Format::wmo_material_index(batch, true, materials.size());
    auto const& mat = materials[material_index];
    unsigned const texture_count = Noggit::Format::wmo_texture_count(mat.shader, true);
    auto& render_batch = _render_batches[batch_counter++];
    std::array<std::uint32_t, 9> arrays{};
    std::array<std::uint32_t, 9> needed{};
    unsigned needed_count = 0;
    for (unsigned slot = 0; slot < texture_count; ++slot)
    {
      auto index = _wmo_group->wmo->material_textures[material_index][slot];
      auto& texture = textures[index < textures.size() ? index : 0];
      texture->upload();
      arrays[slot] = texture->texture_array();
      render_batch.layer(slot) = texture->array_index();
      if (std::find(needed.begin(), needed.begin() + needed_count, arrays[slot]) == needed.begin() + needed_count)
        needed[needed_count++] = arrays[slot];
    }
    bool const backface_cull = !mat.flags.unculled;
    int const blend_mode = static_cast<int>(mat.blend_mode);
    unsigned additional = 0;
    if (draw_call)
      for (unsigned i = 0; i < needed_count; ++i)
        if (std::find(draw_call->samplers.begin(), draw_call->samplers.end(), needed[i]) == draw_call->samplers.end()) ++additional;
    if (!draw_call || draw_call->backface_cull != backface_cull || draw_call->blend_mode != blend_mode ||
        batch.index_start != draw_call->index_start + draw_call->index_count ||
        draw_call->n_used_samplers + additional > 15)
    {
      draw_call = &_draw_calls.emplace_back();
      draw_call->samplers.fill(-1);
      draw_call->index_start = batch.index_start;
      draw_call->backface_cull = backface_cull;
      draw_call->blend_mode = blend_mode;
    }
    for (unsigned slot = 0; slot < texture_count; ++slot)
    {
      auto it = std::find(draw_call->samplers.begin(), draw_call->samplers.end(), arrays[slot]);
      if (it == draw_call->samplers.end())
      {
        auto const index = draw_call->n_used_samplers++;
        draw_call->samplers[index] = arrays[slot];
        render_batch.sampler(slot) = index;
      }
      else render_batch.sampler(slot) = static_cast<std::uint32_t>(it - draw_call->samplers.begin());
    }
    draw_call->index_count += batch.index_count;
  }
  return true;
}

void WMOGroupRender::unload()
{
  if (!_uploaded)
  {
    _upload_attempted = false;
    return;
  }

  _vertex_array.unload();
  _buffers.unload();

  gl.deleteTextures(1, &_render_batch_tex);
  _render_batch_tex = 0;

  _uploaded = false;
  _reload_before_upload = true;
  _upload_attempted = false;
  _vao_is_setup = false;
}

void WMOGroupRender::setupVao(OpenGL::Scoped::use_program& wmo_shader)
{
  auto const* project = Noggit::Project::CurrentProject::get();
  bool const retail = _wmo_group->wmo->uses_file_data_ids() && project
      && project->projectVersion == Noggit::Project::ProjectVersion::RETAIL;
  {
    OpenGL::Scoped::vao_binder const _ (_vao);

    wmo_shader.attrib("position", _vertices_buffer, 3, GL_FLOAT, GL_FALSE, 0, 0);
    wmo_shader.attrib("normal", _normals_buffer, 3, GL_FLOAT, GL_FALSE, 0, 0);
    wmo_shader.attrib("texcoord", _texcoords_buffer, 2, GL_FLOAT, GL_FALSE, 0, 0);
    wmo_shader.attribi("batch_mapping", _render_batch_mapping_buffer, 1, GL_UNSIGNED_INT, 0, 0);

    if (_wmo_group->header.flags.has_two_motv)
    {
      wmo_shader.attrib("texcoord_2", _texcoords_buffer_2, 2, GL_FLOAT, GL_FALSE, 0, 0);
    }

    if (retail)
    {
      wmo_shader.attrib("texcoord_3", _texcoords_buffer_3, 2, GL_FLOAT, GL_FALSE, 0, 0);
      wmo_shader.attrib("texcoord_4", _texcoords_buffer_4, 2, GL_FLOAT, GL_FALSE, 0, 0);
      wmo_shader.attrib("material_weights", _material_weights_buffer, 4, GL_FLOAT, GL_FALSE, 0, 0);
      wmo_shader.attrib("material_blend_alpha", _material_blend_alpha_buffer, 1, GL_FLOAT, GL_FALSE, 0, 0);
    }

    // even if the 2 flags are set there's only one vertex color vector, the 2nd chunk is used for alpha only
    if (_wmo_group->header.flags.has_vertex_color || (!retail && _wmo_group->header.flags.use_mocv2_for_texture_blending))
    {
      wmo_shader.attrib("vertex_color", _vertex_colors_buffer, 4, GL_FLOAT, GL_FALSE, 0, 0);
    }

    // The element buffer binding is VAO state and must not be restored after the VAO scope.
    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indices_buffer);
  }

  _vao_is_setup = true;
}

void WMOGroupRender::draw(OpenGL::Scoped::use_program& wmo_shader
    , math::frustum const& // frustum
    , const float& //cull_distance
    , const glm::vec3& //camera
    , bool // draw_fog
    , bool // world_has_skies
)
{
  if (!_uploaded)
  [[unlikely]]
  {
    if (_upload_attempted)
      return;

    upload();

    if (!_uploaded)
    [[unlikely]]
    {
      return;
    }
  }

  if (!_vao_is_setup)
  [[unlikely]]
  {
    setupVao(wmo_shader);
  }

  OpenGL::Scoped::vao_binder const _ (_vao);

  gl.activeTexture(GL_TEXTURE0);
  gl.bindTexture(GL_TEXTURE_BUFFER, _render_batch_tex);

  bool backface_cull = true;
  gl.enable(GL_CULL_FACE);

  for (auto& draw_call : _draw_calls)
  {
    if (backface_cull != draw_call.backface_cull)
    {
      if (draw_call.backface_cull)
      {
        gl.enable(GL_CULL_FACE);
      }
      else
      {
        gl.disable(GL_CULL_FACE);
      }

      backface_cull = draw_call.backface_cull;
    }

    if (draw_call.blend_mode >= 0)
    {
      auto const mode = draw_call.blend_mode;
      if (mode == 0 || mode == 1 || mode > 13) gl.disable(GL_BLEND);
      else gl.enable(GL_BLEND);
      gl.depthMask(mode == 0 || mode == 1 || mode == 8 || mode == 9 || mode > 13 ? GL_TRUE : GL_FALSE);
      switch (mode)
      {
      case 2: gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
      case 3: gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE); break;
      case 4: gl.blendFunc(GL_DST_COLOR, GL_ZERO); break;
      case 5: gl.blendFunc(GL_DST_COLOR, GL_SRC_COLOR); break;
      case 6: gl.blendFuncSeparate(GL_DST_COLOR, GL_ONE, GL_DST_ALPHA, GL_ONE); break;
      case 7: gl.blendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_ONE); break;
      case 8: gl.blendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_ZERO); break;
      case 9: gl.blendFunc(GL_SRC_ALPHA, GL_ZERO); break;
      case 10: gl.blendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE); break;
      // No per-instance constant alpha is available: use the neutral full-opacity preview.
      case 11: gl.blendColor(0.f, 0.f, 0.f, 1.f); gl.blendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA); break;
      case 12: gl.blendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
      case 13: gl.blendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
      }
    }

    for(std::size_t i = 0; i < draw_call.samplers.size(); ++i)
    {
      if (draw_call.samplers[i] < 0)
        break;

      gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 1 + i));
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, draw_call.samplers[i]);
    }

    gl.drawElements (GL_TRIANGLES, draw_call.index_count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(sizeof(std::uint16_t)*draw_call.index_start));

  }

  if (!_draw_calls.empty() && _draw_calls.front().blend_mode >= 0)
  {
    gl.disable(GL_BLEND);
    gl.depthMask(GL_TRUE);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
}

void WMOGroupRender::initRenderBatches()
{
  auto const* project = Noggit::Project::CurrentProject::get();
  bool const retail = _wmo_group->wmo->uses_file_data_ids() && project
      && project->projectVersion == Noggit::Project::ProjectVersion::RETAIL;
  _render_batch_mapping.resize(_wmo_group->_vertices.size());
  std::fill(_render_batch_mapping.begin(), _render_batch_mapping.end(), 0);

  _render_batches.resize(_wmo_group->_batches.size());
  if (_wmo_group->wmo->materials.empty())
  {
    if (!_wmo_group->_batches.empty())
      LogError << "Skipping WMO render-batch initialization with no materials." << std::endl;
    return;
  }

  bool const modern_wmo = _wmo_group->wmo->uses_file_data_ids();

  std::size_t batch_counter = 0;
  for (auto& batch : _wmo_group->_batches)
  {
    // some custom models have bugged batch.vertex_end as 0, avoid crash
    if (batch.vertex_end >= batch.vertex_start && batch.vertex_start < _render_batch_mapping.size())
    {
        auto const vertex_end = std::min<std::size_t>(batch.vertex_end, _render_batch_mapping.size() - 1);
        for (std::size_t i = 0; i < (vertex_end - batch.vertex_start + 1); ++i)
        {
          _render_batch_mapping[batch.vertex_start + i] = static_cast<unsigned>(batch_counter + 1);
        }
    }
    else
        LogError << "WMO has incorrect render batch data. batch.vertex_end < batch.vertex_start" << std::endl;

    std::uint32_t flags = retail ? WMORenderBatchFlags::eWMOBatch_RetailMaterials : 0;

    if (_wmo_group->header.flags.exterior_lit || _wmo_group->header.flags.exterior)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_ExteriorLit;
    }
    if (_wmo_group->header.flags.has_vertex_color || (!retail && _wmo_group->header.flags.use_mocv2_for_texture_blending))
    {
      flags |= WMORenderBatchFlags::eWMOBatch_HasMOCV;
    }

    std::uint16_t const requested_material = modern_wmo && (batch.flags & 2) ? batch.unused[5] : batch.texture;
    if (!retail && requested_material >= _wmo_group->wmo->materials.size())
      LogError << "Invalid WMO material index " << requested_material
               << " during batch initialization; using material 0." << std::endl;
    auto const material_to_use = Noggit::Format::wmo_material_index(
        batch, modern_wmo, _wmo_group->wmo->materials.size());
    WMOMaterial const& mat (_wmo_group->wmo->materials[material_to_use]);
    if (retail)
      flags |= std::uint32_t(_wmo_group->wmo->material_missing_textures[material_to_use]) << 8;

    if (retail && (((mat.shader == 3 || mat.shader == 5 || mat.shader == 12) && mat.texture_offset_2 == 0) ||
        (mat.shader == 23 && mat.texture_offset_1 == 0) ||
        (mat.shader == 7 && mat.texture_offset_3 == 0)))
      flags |= WMORenderBatchFlags::eWMOBatch_NoEnvironment;

    if (mat.flags.unlit)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_Unlit;
    }

    if (mat.flags.unfogged)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_Unfogged;
    }

    if (mat.flags.sidn)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_SIDN;
    }

    std::uint32_t alpha_test;

    switch (mat.blend_mode)
    {
      case 1:
        alpha_test = 1; // 224/255
        break;
      case 2:
      case 3:
      case 4:
      case 5:
      case 6:
        alpha_test = 2;
        break;
      case 0:
      default:
        alpha_test = 0;
        break;
    }

    if (retail && mat.blend_mode >= 2 && mat.blend_mode <= 13) alpha_test = 2;

    auto sidn_color =
      (std::uint32_t(mat.sidn_color.a) << 24) |
      (std::uint32_t(mat.sidn_color.r) << 16) |
      (std::uint32_t(mat.sidn_color.g) << 8) |
      std::uint32_t(mat.sidn_color.b);

    _render_batches[batch_counter] = WMORenderBatch{flags, mat.shader, 0, 0, 0, 0, alpha_test, sidn_color};

    batch_counter++;
  }
}
