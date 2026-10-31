// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ModelRender.hpp"
#include <noggit/rendering/RetailM2Shader.hpp>
#include <noggit/rendering/SurfaceDecalPreview.hpp>
#include <noggit/formats/RetailSkin.hpp>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/Misc.h>
#include <noggit/Particle.h>
#include <noggit/TextureManager.h>
#include <noggit/project/CurrentProject.hpp>

#include <math/bounding_box.hpp>
#include <math/frustum.hpp>

#include <opengl/shader.hpp>

#include <Listfile.hpp>
#include <ClientFile.hpp>
#include <noggit/application/NoggitApplication.hpp>

#include <external/tracy/Tracy.hpp>
#include <map>


using namespace Noggit::Rendering;

namespace
{
  bool isRetailProject()
  {
    auto const* project = Noggit::Project::CurrentProject::get();
    return project && project->projectVersion == Noggit::Project::ProjectVersion::RETAIL;
  }

  bool isShadowlandsProject()
  {
    auto* project = Noggit::Project::CurrentProject::get();
    return project
      && (project->projectVersion == Noggit::Project::ProjectVersion::SL
          || project->projectVersion == Noggit::Project::ProjectVersion::RETAIL
          || project->projectVersion == Noggit::Project::ProjectVersion::FOREVER);
  }
}

ModelRender::ModelRender(Model* model)
: _model(model)
{

}

void ModelRender::upload()
{
  _vertex_box_points = math::box_points(
      misc::transform_model_box_coords(_model->bounding_box_min)
      , misc::transform_model_box_coords(_model->bounding_box_max));

  for (std::size_t i = 0; i < _model->_textureFilenames.size(); ++i)
  {
    if (i < _model->_textureFileDataIds.size() && _model->_textureFileDataIds[i] != 0)
    {
      _model->_textures.emplace_back(
          BlizzardArchive::Listfile::FileKey(
              _model->_textureFilenames[i], _model->_textureFileDataIds[i]),
          _model->_context);
    }
    else
    {
      _model->_textures.emplace_back(_model->_textureFilenames[i], _model->_context);
    }
  }

  _buffers.upload();
  _vertex_arrays.upload();
  _bone_matrices_buf_tex = 0;

  if (_model->animBones)
  {
    gl.genTextures(1, &_bone_matrices_buf_tex);

    gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
    OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> const binder(_bone_matrices_buffer);
    std::vector<glm::mat4x4> const initialPose(_model->bone_matrices.size(), glm::mat4x4(1.f));
    gl.bufferData(GL_TEXTURE_BUFFER, initialPose.size() * sizeof(glm::mat4x4), initialPose.data(), GL_STREAM_DRAW);
    gl.texBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, _bone_matrices_buffer);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_vertices_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, _model->_vertices.size() * sizeof(ModelVertex), _model->_vertices.data(), GL_STATIC_DRAW);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (_box_vbo);
    gl.bufferData (GL_ARRAY_BUFFER, _vertex_box_points.size() * sizeof (glm::vec3), _vertex_box_points.data(), GL_STATIC_DRAW);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);
  gl.bufferData (GL_ELEMENT_ARRAY_BUFFER, _model->_indices.size() * sizeof(uint16_t), _model->_indices.data(), GL_STATIC_DRAW);

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> box_indices_binder(_box_indices_buffer);
  gl.bufferData (GL_ELEMENT_ARRAY_BUFFER, _box_indices.size() * sizeof(uint16_t), _box_indices.data(), GL_STATIC_DRAW);

  if (isRetailProject()) classifyPreviewDecals(_render_passes);
  // Texture keys are kept for the next context upload; unload releases the handles.

  _model->anim_calculated = false;
  _bone_matrices_dirty = false;
  _last_preview_animtime = -1;
  _uploaded = true;
}

void ModelRender::unload()
{
  _model->_textures.clear();
  _buffers.unload();
  _preview_index_buffers.unload();
  for (auto& skin : _preview_skins)
    skin = PreviewSkin{};
  _last_preview_animtime = -1;
  _vertex_arrays.unload();

  if (_bone_matrices_buf_tex)
    gl.deleteTextures(1, &_bone_matrices_buf_tex);

  for (auto& particle : _model->_particles)
  {
    particle.unload();
  }

  for (auto& ribbon : _model->_ribbons)
  {
    ribbon.unload();
  }

  _uploaded = false;
  _vao_setup = false;
}

void ModelRender::draw(glm::mat4x4 const& model_view
    , ModelInstance& instance
    , OpenGL::Scoped::use_program& m2_shader
    , OpenGL::M2RenderState& model_render_state
    , math::frustum const& frustum
    , const float& cull_distance
    , const glm::vec3& camera
    , int animtime
    , display_mode display
    , bool no_cull
    , bool animate
)
{
  if (!_model->finishedLoading() || _model->loading_failed())
  {
    return;
  }

  if (!no_cull && !instance.isInFrustum(frustum) && !instance.isInRenderDist(cull_distance, camera, display))
  {
    return;
  }

  if (!_uploaded)
  {
    upload();

    OpenGL::Scoped::vao_binder const _(_vao);

    {
        OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_vertices_buffer);
        m2_shader.attrib("pos", 3, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), 0);
        m2_shader.attrib("normal", 3, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*> (sizeof(::glm::vec3) + 8));
        m2_shader.attrib("texcoord1", 2, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*> (sizeof(::glm::vec3) * 2 + 8));
        m2_shader.attrib("texcoord2", 2, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*> (sizeof(::glm::vec3) * 2 + 8 + sizeof(glm::vec2)));
    }

    {
        OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(_transform_buffer);
        m2_shader.uniform("transform", instance.transformMatrix());
    }
  }

  if (_model->animated && animate && (!_model->anim_calculated || _model->_per_instance_animation))
  {
    _model->animate(model_view, 0, animtime);
    _model->anim_calculated = true;
  }

  updateBoneMatrices();
  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);

  for (ModelRenderPass& p : _render_passes)
  {
    if (p.prepareDraw(m2_shader, _model, model_render_state))
    {
      gl.drawElements(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(p.index_start * sizeof(GLushort)));
      p.afterDraw();
    }
  }

  gl.disable(GL_BLEND);
  gl.enable(GL_CULL_FACE);
  gl.depthMask(GL_TRUE);
}

void ModelRender::draw(glm::mat4x4 const& model_view
    , std::vector<glm::mat4x4> const& instances
    , OpenGL::Scoped::use_program& m2_shader
    , OpenGL::M2RenderState& model_render_state
    , math::frustum const& frustum
    , const float cull_distance
    , glm::vec3 const& camera
    , int animtime
    , bool all_boxes
    , std::unordered_map<Model*, std::size_t>& model_boxes_to_draw
    , display_mode display
    , bool no_cull
    , bool animate
    , bool draw_fake_geometry_box
    , bool draw_animation_box
    , int preview_lod
    , bool preview_animation
    , int preview_decals
)
{
  ZoneScopedN(NOGGIT_CURRENT_FUNCTION);

  {
    ZoneScopedN("Model::draw() : uploads")

    if (!_model->finishedLoading() || _model->loading_failed())
    {
      return;
    }

    if (!_uploaded)
    {
      upload();
    }

    if (!_vao_setup)
    {
      setupVAO(m2_shader);
    }
  }

  if (instances.empty())
  {
    return;
  }

  {
    ZoneScopedN("Model::draw() : drawing")

    if (_model->animated && animate
        && (preview_animation ? (_last_preview_animtime != animtime || _model->_global_animtime != animtime || _model->_per_instance_animation)
                              : (!_model->anim_calculated || _model->_per_instance_animation)))
    {
      _model->animate(model_view, 0, animtime);
      _model->anim_calculated = true;
      _last_preview_animtime = preview_animation ? animtime : -1;
    }
    else if (_model->animated && animate && preview_animation)
    {
      // ModelManager::resetAnim clears this for every renderer; a held preview pose stays valid.
      _model->anim_calculated = true;
    }

    updateBoneMatrices();

    if (all_boxes || _model->_hidden )
    {
      model_boxes_to_draw.emplace(_model, instances.size());

      if (draw_animation_box)
      {
      }

    }
    else if (draw_fake_geometry_box && _model->use_fake_geometry())
    {
      model_boxes_to_draw.emplace(_model, instances.size());
    }

    OpenGL::Scoped::vao_binder const _ (_vao);

    {
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
      gl.bufferData(GL_ARRAY_BUFFER, instances.size() * sizeof(::glm::mat4x4), instances.data(), GL_DYNAMIC_DRAW);
    }

    if (_model->animBones)
    {
      gl.activeTexture(GL_TEXTURE0);
      gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
      m2_shader.uniform("anim_bones", true);
    }
    else
    {
      m2_shader.uniform("anim_bones", false);
    }

    int const lod = preview_lod >= 1 && preview_lod <= 2 ? preview_lod - 1 : -1;
    if (lod >= 0)
      loadPreviewSkin(lod);
    bool useLod = lod >= 0 && _preview_skins[lod].valid;
    // Authored LODs can merge equal-ID primary bone partitions. A custom
    // geoset selection that separates those partitions needs the primary mesh.
    if (useLod)
      for (auto const& group : _shared_geosets)
        for (auto index : group)
          if (_model->showGeosets[index] != _model->showGeosets[group.front()])
            useLod = false;
    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(useLod ? _preview_index_buffers[lod] : _indices_buffer);
    auto& passes = useLod ? _preview_skins[lod].passes : _render_passes;

    for (ModelRenderPass& p : passes)
    {
      if (p.prepareDraw(m2_shader, _model, model_render_state, preview_decals))
      {
        gl.drawElementsInstanced(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(p.index_start * sizeof(GLushort)), static_cast<GLsizei>(instances.size()));
      }
    }
  }
}

void ModelRender::drawParticles(glm::mat4x4 const& model_view
    , OpenGL::Scoped::use_program& particles_shader
    , std::vector<ModelInstance*> const& instances
)
{
  std::vector<ParticleEmitterInstance const*> states;
  states.reserve(instances.size());

  for (std::size_t i = 0; i < _model->_particles.size(); ++i)
  {
    states.clear();

    for (ModelInstance* instance : instances)
    {
      if (i < instance->emitter_states.particles.size())
      {
        states.push_back(&instance->emitter_states.particles[i]);
      }
    }

    if (!states.empty())
    {
      _model->_particles[i].draw(model_view, particles_shader, states);
    }
  }
}

void ModelRender::drawRibbons( OpenGL::Scoped::use_program& ribbons_shader
    , std::vector<ModelInstance*> const& instances
)
{
  std::vector<RibbonEmitterInstance const*> states;
  states.reserve(instances.size());

  for (std::size_t i = 0; i < _model->_ribbons.size(); ++i)
  {
    states.clear();

    for (ModelInstance* instance : instances)
    {
      if (i < instance->emitter_states.ribbons.size())
      {
        states.push_back(&instance->emitter_states.ribbons[i]);
      }
    }

    if (!states.empty())
    {
      _model->_ribbons[i].draw(ribbons_shader, states);
    }
  }
}

void ModelRender::drawBox(OpenGL::Scoped::use_program& m2_box_shader, std::size_t box_count)
{
  OpenGL::Scoped::vao_binder const _ (_box_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
    m2_box_shader.attrib("transform", 0, 1);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (_box_vbo);
    m2_box_shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_box_indices_buffer);

  gl.drawElementsInstanced (GL_LINE_STRIP, static_cast<GLsizei>(_box_indices.size()), GL_UNSIGNED_SHORT, nullptr, static_cast<GLsizei>(box_count));
}

[[nodiscard]]
std::vector<ModelRenderPass> const& Noggit::Rendering::ModelRender::renderPasses() const
{
  return _render_passes;
}

void ModelRender::setupVAO(OpenGL::Scoped::use_program& m2_shader)
{
  OpenGL::Scoped::vao_binder const _(_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (_vertices_buffer);
    m2_shader.attrib("pos",           3, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), 0);
    m2_shader.attribi("bones_weight", 4, GL_UNSIGNED_BYTE,   sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3)));
    m2_shader.attribi("bones_indices",4, GL_UNSIGNED_BYTE,  sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) + 4));
    m2_shader.attrib("normal",        3, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) + 8));
    m2_shader.attrib("texcoord1",     2, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) * 2 + 8));
    m2_shader.attrib("texcoord2",     2, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) * 2 + 8 + sizeof(glm::vec2)));
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, 10 * sizeof(::glm::mat4x4), nullptr, GL_DYNAMIC_DRAW);
    m2_shader.attrib("transform", 0, 1);
  }

  _vao_setup = true;
}

void ModelRender::fixShaderIdBlendOverride()
{
  for (auto& pass : _render_passes)
  {
    bool const authoredRetail = isRetailProject()
        && (pass.shader_id & 0x8000);
    // Retail authored materials keep all stages; unsupported counts are rejected in computePixelShaderIDs.
    if (!authoredRetail && pass.texture_count > 2)
    {
      LogDebug << "modern M2: limiting texture stages to 2 for "
               << _model->_file_key.stringRepr() << std::endl;
      pass.texture_count = 2;
    }

    if (pass.shader_id & 0x8000)
    {
      continue;
    }

    int shader = 0;
    bool blend_mode_override = (_model->Flags & m2_flag_use_texture_combiner_combos);

    auto const lookup_size = _model->_texture_unit_lookup.size();
    bool const invalid_texture_coord_combo =
        pass.texture_count == 0
        || pass.texture_coord_combo_index >= lookup_size
        || pass.texture_count > lookup_size - pass.texture_coord_combo_index;

    if (invalid_texture_coord_combo)
    {
      if (isShadowlandsProject())
      {
        LogDebug << "modern M2: using shader UV semantics for " << _model->_file_key.stringRepr() << std::endl;
        if (pass.texture_count == 0)
          pass.texture_count = 1;
        if (pass.texture_count > 2)
          pass.texture_count = 2;
        continue;
      }

      LogDebug << "wrong texture coord combo index on modern/fuckported model: " << _model->_file_key.stringRepr() << std::endl;
      pass.shader_id = 0;
      pass.texture_count = 1;
      continue;
    }

    if (!blend_mode_override)
    {
      uint16_t texture_unit_lookup = _model->_texture_unit_lookup[pass.texture_coord_combo_index];

      if (_model->_render_flags[pass.renderflag_index].blend)
      {
        shader = 1;

        if (texture_unit_lookup == 0xFFFF)
        {
          shader |= 0x8;
        }
      }

      shader <<= 4;

      if (texture_unit_lookup == 1)
      {
        shader |= 0x4000;
      }
    }
    else
    {
      uint16_t runtime_shader_val[2] = { 0, 0 };

      if (pass.shader_id >= _model->blend_override.size()
          || pass.texture_count > _model->blend_override.size() - pass.shader_id)
      {
        LogDebug << "modern M2: invalid blend override range for "
                 << _model->_file_key.stringRepr() << std::endl;
        pass.shader_id = 0;
        pass.texture_count = 1;
        continue;
      }

      for (std::size_t i = 0; i < pass.texture_count; ++i)
      {
        uint16_t override_blend = _model->blend_override[pass.shader_id + i];
        uint16_t texture_unit_lookup = _model->_texture_unit_lookup[pass.texture_coord_combo_index + i];

        if (i == 0 && _model->_render_flags[pass.renderflag_index].blend == 0)
        {
          override_blend = 0;
        }

        runtime_shader_val[i] = override_blend;

        if (texture_unit_lookup == 0xFFFF)
        {
          runtime_shader_val[i] |= 0x8;
        }

        if (texture_unit_lookup == 1 && i + 1 == pass.texture_count)
        {
          shader |= 0x4000;
        }
      }

      shader |= (runtime_shader_val[1] & 0xFFFF) | ((runtime_shader_val[0] << 4) & 0xFFFF);
    }

    pass.shader_id = shader;
  }
}

void ModelRender::fixShaderIDLayer()
{
  if (isRetailProject())
  {
    // Retail SKIN batches carry final material/layer IDs; the legacy reducer does not apply.
    for (auto& pass : _render_passes)
      for (unsigned stage = 0; stage < 3; ++stage)
      {
        pass.textures[stage] = pass.texture_combo_index + stage;
        auto const animation = std::uint32_t(pass.animation_combo_index) + stage;
        pass.uv_animations[stage] = animation < 0xffff ? static_cast<uint16_t>(animation) : 0xffff;
      }
    return;
  }
  int non_layered_count = 0;

  for (auto const& pass : _render_passes)
  {
    if (pass.material_layer <= 0)
    {
      non_layered_count++;
    }
  }

  if (non_layered_count < _render_passes.size())
  {
    std::vector<ModelRenderPass> passes;

    ModelRenderPass* first_pass = nullptr;
    bool need_reducing = false;
    uint16_t previous_render_flag = -1, some_flags = 0;
    auto same_transparency = [this](ModelRenderPass const& lhs, ModelRenderPass const& rhs)
    {
      if (lhs.transparency_combo_index >= _model->_transparency_lookup.size()
          || rhs.transparency_combo_index >= _model->_transparency_lookup.size())
        return lhs.transparency_combo_index == 0xFFFF
            && rhs.transparency_combo_index == 0xFFFF;

      return _model->_transparency_lookup[lhs.transparency_combo_index]
          == _model->_transparency_lookup[rhs.transparency_combo_index];
    };

    for (auto& pass : _render_passes)
    {
      if (pass.renderflag_index == previous_render_flag)
      {
        need_reducing = true;
        continue;
      }

      previous_render_flag = pass.renderflag_index;

      uint8_t lower_bits = pass.shader_id & 0x7;

      if (pass.material_layer == 0)
      {
        if (pass.texture_count >= 1 && _model->_render_flags[pass.renderflag_index].blend == 0)
        {
          pass.shader_id &= 0xFF8F;
        }

        first_pass = &pass;
      }
      assert(first_pass);
      if (first_pass == nullptr)
        return;

      bool xor_unlit = ((_model->_render_flags[pass.renderflag_index].flags.unlit ^ _model->_render_flags[first_pass->renderflag_index].flags.unlit) & 1) == 0;

      if ((some_flags & 0xFF) == 1)
      {
        if ((_model->_render_flags[pass.renderflag_index].blend == 1 || _model->_render_flags[pass.renderflag_index].blend == 2)
            && pass.texture_count == 1
            && xor_unlit
            && pass.texture_combo_index == first_pass->texture_combo_index)
        {
          if (same_transparency(pass, *first_pass))
          {
            pass.shader_id = 0x8000;
            first_pass->shader_id = 0x8001;
            some_flags = (some_flags & 0xFF00) | 3;
            continue;
          }
        }

        some_flags = (some_flags & 0xFF00);
      }

      auto const lookup_size = _model->_texture_unit_lookup.size();
      bool const invalid_texture_coord_combo =
          pass.texture_count == 0
          || pass.texture_coord_combo_index >= lookup_size
          || pass.texture_count > lookup_size - pass.texture_coord_combo_index;

      if (invalid_texture_coord_combo)
      {
        if (pass.texture_count == 0)
          pass.texture_count = 1;
        if (pass.texture_count > 2)
          pass.texture_count = 2;
        if (!isShadowlandsProject())
          pass.texture_count = 1;

        pass.textures[0] = pass.texture_combo_index;
        pass.textures[1] = pass.texture_count > 1 ? pass.texture_combo_index + 1 : 0;
        pass.uv_animations[0] = pass.animation_combo_index;
        pass.uv_animations[1] = pass.texture_count > 1 ? pass.animation_combo_index + 1 : 0;
        passes.push_back(pass);
        continue;
      }

      int16_t texture_unit_lookup = _model->_texture_unit_lookup[pass.texture_coord_combo_index];

      if ((some_flags & 0xFF) < 2)
      {
        if ((_model->_render_flags[pass.renderflag_index].blend == 0) && (pass.texture_count == 2) && ((lower_bits == 4) || (lower_bits == 6)))
        {
          if (texture_unit_lookup == 0 && (_model->_texture_unit_lookup[pass.texture_coord_combo_index + 1] == -1))
          {
            some_flags = (some_flags & 0xFF00) | 1;
          }
        }
      }

      if ((some_flags >> 8) != 0)
      {
        if ((some_flags >> 8) == 1)
        {
          if (((_model->_render_flags[pass.renderflag_index].blend != 4) && (_model->_render_flags[pass.renderflag_index].blend != 6)) || (pass.texture_count != 1) || (texture_unit_lookup >= 0))
          {
            some_flags &= 0xFF00;
          }
          else if (same_transparency(pass, *first_pass))
          {
            pass.shader_id = 0x8000;
            first_pass->shader_id = _model->_render_flags[pass.renderflag_index].blend != 4 ? 0xE : 0x8002;
            some_flags = (some_flags & 0xFF) | (2 << 8);
            first_pass->texture_count = 2;
            first_pass->textures[1] = pass.texture_combo_index;
            first_pass->uv_animations[1] = pass.animation_combo_index;
            continue;
          }
        }
        else
        {
          if ((some_flags >> 8) != 2)
          {
            continue;
          }

          if (((_model->_render_flags[pass.renderflag_index].blend != 2) && (_model->_render_flags[pass.renderflag_index].blend != 1))
               || (pass.texture_count != 1)
               || xor_unlit
               || ((pass.texture_combo_index & 0xff) != (first_pass->texture_combo_index & 0xff)))
          {
            some_flags &= 0xFF00;
          }
          else if (same_transparency(pass, *first_pass))
          {
            pass.shader_id = 0x8000;
            first_pass->shader_id = ((first_pass->shader_id == 0x8002 ? 2 : 0) - 0x7FFF) & 0xFFFF;
            some_flags = (some_flags & 0xFF) | (3 << 8);
            continue;
          }
        }
        some_flags = (some_flags & 0xFF);
      }

      if ((_model->_render_flags[pass.renderflag_index].blend == 0) && (pass.texture_count == 1) && (texture_unit_lookup == 0))
      {
        some_flags = (some_flags & 0xFF) | (1 << 8);
      }

      pass.textures[0] = pass.texture_combo_index;
      pass.textures[1] = pass.texture_count > 1 ? pass.texture_combo_index + 1 : 0;
      pass.uv_animations[0] = pass.animation_combo_index;
      pass.uv_animations[1] = pass.texture_count > 1 ? pass.animation_combo_index + 1 : 0;

      passes.push_back(pass);
    }

    if (need_reducing)
    {
      previous_render_flag = -1;
      for (int i = 0; i < passes.size(); ++i)
      {
        auto& pass = _render_passes[i];
        uint16_t renderflag_index = pass.renderflag_index;

        if (renderflag_index == previous_render_flag)
        {
          pass.shader_id = _render_passes[i - 1].shader_id;
          pass.texture_count = _render_passes[i - 1].texture_count;
          pass.texture_combo_index = _render_passes[i - 1].texture_combo_index;
          pass.texture_coord_combo_index = _render_passes[i - 1].texture_coord_combo_index;
        }
        else
        {
          previous_render_flag = renderflag_index;
        }
      }
    }

    _render_passes = passes;
  }
  else
  {
    for (auto& pass : _render_passes)
    {
      pass.textures[0] = pass.texture_combo_index;
      pass.textures[1] = pass.texture_count > 1 ? pass.texture_combo_index + 1 : 0;
      pass.uv_animations[0] = pass.animation_combo_index;
      pass.uv_animations[1] = pass.texture_count > 1 ? pass.animation_combo_index + 1 : 0;
    }
  }
}

namespace
{
  std::optional<ModelPixelShader> GetPixelShader(uint16_t texture_count, uint16_t shader_id)
  {
    uint16_t texture1_fragment_mode = (shader_id >> 4) & 7;
    uint16_t texture2_fragment_mode = shader_id & 7;

    std::optional<ModelPixelShader> pixel_shader;

    if (texture_count == 1)
    {
      switch (texture1_fragment_mode)
      {
        case 0: pixel_shader = ModelPixelShader::Combiners_Opaque; break;
        case 2: pixel_shader = ModelPixelShader::Combiners_Decal; break;
        case 3: pixel_shader = ModelPixelShader::Combiners_Add; break;
        case 4: pixel_shader = ModelPixelShader::Combiners_Mod2x; break;
        case 5: pixel_shader = ModelPixelShader::Combiners_Fade; break;
        default: pixel_shader = ModelPixelShader::Combiners_Mod; break;
      }
    }
    else
    {
      if (!texture1_fragment_mode)
      {
        switch (texture2_fragment_mode)
        {
          case 0: pixel_shader = ModelPixelShader::Combiners_Opaque_Opaque; break;
          case 3: pixel_shader = ModelPixelShader::Combiners_Opaque_Add; break;
          case 4: pixel_shader = ModelPixelShader::Combiners_Opaque_Mod2x; break;
          case 6: pixel_shader = ModelPixelShader::Combiners_Opaque_Mod2xNA; break;
          case 7: pixel_shader = ModelPixelShader::Combiners_Opaque_AddNA; break;
          default: pixel_shader = ModelPixelShader::Combiners_Opaque_Mod; break;
        }
      }
      else if (texture1_fragment_mode == 1)
      {
        switch (texture2_fragment_mode)
        {
          case 0: pixel_shader = ModelPixelShader::Combiners_Mod_Opaque; break;
          case 3: pixel_shader = ModelPixelShader::Combiners_Mod_Add; break;
          case 4: pixel_shader = ModelPixelShader::Combiners_Mod_Mod2x; break;
          case 6: pixel_shader = ModelPixelShader::Combiners_Mod_Mod2xNA; break;
          case 7: pixel_shader = ModelPixelShader::Combiners_Mod_AddNA; break;
          default: pixel_shader = ModelPixelShader::Combiners_Mod_Mod; break;
        }
      }
      else if (texture1_fragment_mode == 3)
      {
        if (texture2_fragment_mode == 1)
          pixel_shader = ModelPixelShader::Combiners_Add_Mod;
      }
      else if (texture1_fragment_mode == 4 && texture2_fragment_mode == 4)
      {
        pixel_shader = ModelPixelShader::Combiners_Mod2x_Mod2x;
      }
      else if (texture2_fragment_mode == 1)
      {
        pixel_shader = ModelPixelShader::Combiners_Mod_Mod2x;
      }
    }

    return pixel_shader;
  }

  std::optional<ModelPixelShader> M2GetPixelShaderID(uint16_t texture_count, uint16_t shader_id)
  {
    std::optional<ModelPixelShader> pixel_shader;

    if (!(shader_id & 0x8000))
    {
      pixel_shader = GetPixelShader(texture_count, shader_id);
      if (!pixel_shader)
        pixel_shader = GetPixelShader(texture_count, 0x11);
    }
    else
    {
      switch (shader_id & 0x7FFF)
      {
        case 1: pixel_shader = ModelPixelShader::Combiners_Opaque_Mod2xNA_Alpha; break;
        case 2: pixel_shader = ModelPixelShader::Combiners_Opaque_AddAlpha; break;
        case 3: pixel_shader = ModelPixelShader::Combiners_Opaque_AddAlpha_Alpha; break;
      }
    }

    return pixel_shader;
  }

  std::optional<ModelPixelShader> M2GetShadowlandsPixelShaderID(uint16_t texture_count, uint16_t shader_id)
  {
    if (!(shader_id & 0x8000))
      return M2GetPixelShaderID(texture_count, shader_id);

    switch (shader_id & 0xFF)
    {
      case 0:
      case 3:
      case 9:
      case 17:
      case 24:
        return ModelPixelShader::Combiners_Mod_Mod2x;
      case 2:
        return ModelPixelShader::Combiners_Mod_Add;
      case 21:
        return ModelPixelShader::Combiners_Mod_Mod;
      case 1:
      case 5:
      case 8:
      case 10:
      case 12:
      case 15:
      case 16:
      case 23:
      default:
        return ModelPixelShader::Combiners_Mod;
    }
  }
}

void ModelRender::computePixelShaderIDs()
{
  for (auto& pass : _render_passes)
  {
    if (isRetailProject() && (pass.shader_id & 0x8000))
    {
      auto const material = retailM2Shader(pass.shader_id);
      if (material && material->textures <= pass.texture_count && pass.texture_count <= 3
          && std::uint32_t(pass.texture_combo_index) + pass.texture_count <= 65536u
          && pass.texture_combo_index <= _model->_texture_lookup.size()
          && pass.texture_count <= _model->_texture_lookup.size() - pass.texture_combo_index)
        pass.pixel_shader = static_cast<ModelPixelShader>(material->pixel);
      else
      {
        pass.pixel_shader.reset();
        LogError << "Unsupported Retail M2 material: " << _model->file_key().stringRepr()
                 << " shader=" << pass.shader_id << " stages=" << pass.texture_count << std::endl;
      }
    }
    else
      pass.pixel_shader = isShadowlandsProject()
          ? M2GetShadowlandsPixelShaderID(pass.texture_count, pass.shader_id)
          : M2GetPixelShaderID(pass.texture_count, pass.shader_id);
  }
}

void ModelRender::initRenderPasses(ModelView const* view, ModelTexUnit const* tex_unit, ModelGeoset const* model_geosets, std::size_t indexCount)
{
  bool const retail = isRetailProject();
  if (indexCount == 0)
  {
    _primary_geoset_ids.clear();
    for (std::size_t i = 0; i < view->n_submesh; ++i)
      _primary_geoset_ids.push_back(model_geosets[i].id);
    _shared_geosets.clear();
    if (retail)
    {
      std::map<unsigned, std::vector<std::size_t>> groups;
      for (std::size_t i = 0; i < _primary_geoset_ids.size(); ++i)
        groups[_primary_geoset_ids[i]].push_back(i);
      for (auto& [id, group] : groups)
        if (group.size() > 1)
          _shared_geosets.push_back(std::move(group));
    }
    indexCount = _model->_indices.size();
  }
  std::vector<bool> layeredGeosets(view->n_submesh, false);
  for (std::size_t i = 0; i < view->n_texture_unit; ++i)
    if (tex_unit[i].submesh < view->n_submesh && tex_unit[i].material_layer != 0)
      layeredGeosets[tex_unit[i].submesh] = true;
  _render_passes.reserve(view->n_texture_unit);
  for (size_t j = 0; j<view->n_texture_unit; j++)
  {
    size_t geoset = tex_unit[j].submesh;

    if (geoset >= view->n_submesh
        || tex_unit[j].renderflag_index >= _model->_render_flags.size())
    {
      LogDebug << "modern M2: skipping invalid render pass for "
               << _model->_file_key.stringRepr() << std::endl;
      continue;
    }

    ModelRenderPass pass(tex_unit[j], _model);
    pass.ordering_thingy = model_geosets[geoset].BoundingBox[0].x;
    pass.layered_geometry = layeredGeosets[geoset];

    pass.index_start = Noggit::Format::skinIndexStart(model_geosets[geoset].istart,
                                                     model_geosets[geoset].d2, retail);
    pass.index_count = model_geosets[geoset].icount;
    pass.vertex_start = model_geosets[geoset].vstart;
    pass.vertex_end = pass.vertex_start + model_geosets[geoset].vcount;

    if (retail)
    {
      Noggit::Format::validateSkinRange(pass.index_start, pass.index_count, indexCount);
      Noggit::Format::validateSkinRange(pass.vertex_start, model_geosets[geoset].vcount, view->n_index);
    }

    _render_passes.push_back(std::move(pass));
  }

  fixShaderIdBlendOverride();
  fixShaderIDLayer();
  computePixelShaderIDs();

  for (auto& pass : _render_passes)
  {
    pass.initUVTypes(_model);
  }

  if (retail)
    std::stable_sort(_render_passes.begin(), _render_passes.end());
  else
    std::sort(_render_passes.begin(), _render_passes.end());
}


void ModelRender::classifyPreviewDecals(std::vector<ModelRenderPass>& passes)
{
  auto* client = Noggit::Application::NoggitApplication::instance()->clientData();
  auto pathFor = [client](BlizzardArchive::Listfile::FileKey const& key)
  {
    if (key.hasFilepath())
      return key.filepath();
    return key.hasFileDataID() ? client->listfile()->getPath(key.fileDataID()) : std::string{};
  };
  auto const modelPath = pathFor(_model->file_key());
  for (auto& pass : passes)
  {
    std::string texturePath;
    if (pass.texture_count == 1 && pass.textures[0] < _model->_texture_lookup.size())
    {
      auto const index = _model->_texture_lookup[pass.textures[0]];
      if (index < _model->_textures.size())
        texturePath = pathFor(_model->_textures[index]->file_key());
    }
    pass.surface_decal = isSurfaceDecal(pass.flags, modelPath, pass.texture_count, texturePath, pass.layered_geometry);
  }
}

void ModelRender::configurePreviewSkins(std::vector<std::uint32_t> const& fileIds, std::size_t viewCount)
{
  // The first nViews skins are bone partitions, not geometric LODs.
  _preview_skin_ids = Noggit::Format::previewSkinFileIds(fileIds, viewCount);
}

void ModelRender::loadPreviewSkin(int choice)
{
  auto& target = _preview_skins[choice];
  if (target.attempted)
    return;
  auto const id = _preview_skin_ids[choice];
  if (!id)
  {
    target.attempted = true;
    return;
  }
  // Optional SKIN reads are limited per frame; the primary mesh is drawn until then.
  if (_preview_skin_load_budget <= 0)
    return;
  --_preview_skin_load_budget;
  target.attempted = true;

  // Optional SKINs get separate indices and passes; picking and saving use the primary mesh.
  auto primaryPasses = std::move(_render_passes);
  _render_passes.clear();
  try
  {
    auto* client = Noggit::Application::NoggitApplication::instance()->clientData();
    BlizzardArchive::ClientFile file(BlizzardArchive::Listfile::FileKey(id), client);
    if (file.isEof() || file.getSize() < sizeof(ModelView))
      throw std::runtime_error("missing or truncated optional SKIN");
    ModelView view;
    std::memcpy(&view, file.getBuffer(), sizeof(view));
    if (std::memcmp(view.id, "SKIN", 4) || view.n_triangle == 0 || view.n_triangle % 3)
      throw std::runtime_error("invalid optional SKIN header");
    auto valid = [&](std::size_t offset, std::size_t count, std::size_t stride)
    {
      return offset <= file.getSize() && count <= (file.getSize() - offset) / stride;
    };
    if (!valid(view.ofs_index, view.n_index, sizeof(uint16_t))
        || !valid(view.ofs_triangle, view.n_triangle, sizeof(uint16_t))
        || !valid(view.ofs_submesh, view.n_submesh, sizeof(ModelGeoset))
        || !valid(view.ofs_texture_unit, view.n_texture_unit, sizeof(ModelTexUnit)))
      throw std::runtime_error("optional SKIN array is out of range");
    auto const* lookup = reinterpret_cast<uint16_t const*>(file.getBuffer() + view.ofs_index);
    auto const* triangles = reinterpret_cast<uint16_t const*>(file.getBuffer() + view.ofs_triangle);
    target.indices.resize(view.n_triangle);
    for (std::size_t i = 0; i < target.indices.size(); ++i)
    {
      if (triangles[i] >= view.n_index || lookup[triangles[i]] >= _model->_vertices.size())
        throw std::runtime_error("optional SKIN triangle is out of range");
      target.indices[i] = lookup[triangles[i]];
    }
    auto const* geosets = reinterpret_cast<ModelGeoset const*>(file.getBuffer() + view.ofs_submesh);
    auto const* units = reinterpret_cast<ModelTexUnit const*>(file.getBuffer() + view.ofs_texture_unit);
    // An optional SKIN is used whole or not at all.
    for (std::size_t i = 0; i < view.n_texture_unit; ++i)
      if (units[i].submesh >= view.n_submesh || units[i].renderflag_index >= _model->_render_flags.size())
        throw std::runtime_error("optional SKIN material is out of range");
    initRenderPasses(&view, units, geosets, target.indices.size());
    for (auto& pass : _render_passes)
    {
      auto const found = std::find(_primary_geoset_ids.begin(), _primary_geoset_ids.end(), geosets[pass.submesh].id);
      if (found == _primary_geoset_ids.end() || !pass.pixel_shader)
        throw std::runtime_error("optional SKIN geoset or shader is unavailable");
      pass.submesh = static_cast<uint16_t>(found - _primary_geoset_ids.begin());
    }
    if (_render_passes.empty())
      throw std::runtime_error("optional SKIN contains no drawable material");
    if (isRetailProject()) classifyPreviewDecals(_render_passes);
    target.passes = std::move(_render_passes);
    _preview_index_buffers.upload();
    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> binder(_preview_index_buffers[choice]);
    gl.bufferData(GL_ELEMENT_ARRAY_BUFFER, target.indices.size() * sizeof(uint16_t), target.indices.data(), GL_STATIC_DRAW);
    std::vector<uint16_t>().swap(target.indices);
    target.valid = true;
  }
  catch (std::exception const& error)
  {
    target.indices.clear();
    target.passes.clear();
    LogDebug << "[Retail preview] Using primary mesh for " << _model->file_key().stringRepr()
             << ", optional SKIN " << id << ": " << error.what() << std::endl;
  }
  _render_passes = std::move(primaryPasses);
}

void ModelRender::updateBoneMatrices()
{
  // Requires this renderer's context to be current; picking only evaluates CPU poses.
  if (!_uploaded || !_model->animBones || !_bone_matrices_dirty)
    return;
  OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> const binder (_bone_matrices_buffer);
  gl.bufferSubData(GL_TEXTURE_BUFFER, 0, _model->bone_matrices.size() * sizeof(glm::mat4x4), _model->bone_matrices.data());
  _bone_matrices_dirty = false;
}

ModelRenderPass::ModelRenderPass(ModelTexUnit const& tex_unit, Model* m)
    : ModelTexUnit(tex_unit)
    , blend_mode(m->_render_flags[renderflag_index].blend)
{
}

bool ModelRenderPass::prepareDraw(OpenGL::Scoped::use_program& m2_shader, Model *m, OpenGL::M2RenderState& model_render_state, int preview_decals)
{
  bool const retail = isRetailProject();
  m2_shader.uniform("retail_alpha_key", retail ? 1 : 0);
  m2_shader.uniform("retail_materials", retail ? 1 : 0);
  m2_shader.uniform("preview_decal_lod_bias", retail && surface_decal && preview_decals == 1 ? 2.f : 0.f);
  if (retail && surface_decal && preview_decals == 2)
    return false;
  if (submesh >= m->showGeosets.size()
      || renderflag_index >= m->_render_flags.size()
      || !m->showGeosets[submesh]
      || !pixel_shader)
  {
    return false;
  }

  // Texture resources exist only after upload; validate before changing any binding state.
  if (retail
      && (shader_id & 0x8000)
      && !validRetailM2Textures(texture_combo_index, texture_count, m->_texture_lookup,
          m->_specialTextures, m->_textures.size(),
          [m](std::size_t type) { return m->_replaceTextures.find(type) != m->_replaceTextures.end(); }))
  {
    LogError << "Invalid Retail M2 texture stages: " << m->file_key().stringRepr()
             << " shader=" << shader_id << std::endl;
    pixel_shader.reset();
    return false;
  }

  glm::vec4 mesh_color = glm::vec4(1.0f, 1.0f, 1.0f, m->trans);
  glm::vec4 emissive_color = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);

  auto const& renderflag(m->_render_flags[renderflag_index]);

  if (color_index >= 0
      && static_cast<std::size_t>(color_index) < m->_colors.size()
      && m->_colors[color_index].color.uses(0))
  {
    ::glm::vec3 c (m->_colors[color_index].color.getValue (0, m->_anim_time, m->_global_animtime));
    if (m->_colors[color_index].opacity.uses (m->_current_anim_seq))
    {
      mesh_color.w = m->_colors[color_index].opacity.getValue (m->_current_anim_seq, m->_anim_time, m->_global_animtime);
    }

    mesh_color.x = c.x; mesh_color.y = c.y; mesh_color.z = c.z;
    emissive_color = glm::vec4(c.x,c.y,c.z, mesh_color.w);
  }

  if (transparency_combo_index != 0xFFFF && transparency_combo_index < m->_transparency_lookup.size())
  {
    auto const transparency_index = m->_transparency_lookup[transparency_combo_index];
    if (transparency_index >= 0
        && static_cast<std::size_t>(transparency_index) < m->_transparency.size())
    {
      auto& transparency (m->_transparency[transparency_index].trans);
      if (transparency.uses (0))
      {
        mesh_color.w = mesh_color.w * transparency.getValue(0, m->_anim_time, m->_global_animtime);
      }
    }
  }

  if (!((mesh_color.w > 0) && (color_index == -1 || emissive_color.w > 0)))
  {
    return false;
  }

  if (model_render_state.blend != renderflag.blend)
  {
    switch (static_cast<M2Blend>(renderflag.blend))
    {
      default:
      case M2Blend::Opaque:
      case M2Blend::Alpha_Key:
        gl.disable(GL_BLEND);
        break;
      case M2Blend::Alpha:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
      case M2Blend::No_Add_Alpha:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_ONE, GL_ONE);
        break;
      case M2Blend::Add:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
      case M2Blend::Mod:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_DST_COLOR, GL_ZERO);
        break;
      case M2Blend::Mod2x:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_DST_COLOR, GL_SRC_COLOR);
        break;
      case M2Blend::InvSrcAlphaAdd:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_ONE);
        break;
    }

    m2_shader.uniform("blend_mode", static_cast<int>(renderflag.blend));
    model_render_state.blend = renderflag.blend;
  }

  if (model_render_state.backface_cull != !renderflag.flags.two_sided)
  {
    if (renderflag.flags.two_sided)
      gl.disable(GL_CULL_FACE);
    else
      gl.enable(GL_CULL_FACE);

    model_render_state.backface_cull = !renderflag.flags.two_sided;
  }

  if (model_render_state.z_buffered != renderflag.flags.z_buffered)
  {
    if (renderflag.flags.z_buffered)
      gl.depthMask(GL_FALSE);
    else
      gl.depthMask(GL_TRUE);

    model_render_state.z_buffered = renderflag.flags.z_buffered;
  }

  if (model_render_state.unfogged != renderflag.flags.unfogged)
  {
    m2_shader.uniform("unfogged", (int)renderflag.flags.unfogged);
    model_render_state.unfogged = renderflag.flags.unfogged;
  }

  if (model_render_state.unlit != renderflag.flags.unlit)
  {
    m2_shader.uniform("unlit", (int)renderflag.flags.unlit);
    model_render_state.unlit = renderflag.flags.unlit;
  }

  m2_shader.uniform("tex3", 3);
  if (retail && texture_count > 2)
    bindTexture(2, m, model_render_state, m2_shader);
  if (texture_count > 1)
    bindTexture(1, m, model_render_state, m2_shader);

  bindTexture(0, m, model_render_state, m2_shader);

  GLint tu1 = static_cast<GLint>(tu_lookups[0]), tu2 = static_cast<GLint>(tu_lookups[1]);

  if (model_render_state.tex_unit_lookups[0] != tu1)
  {
    m2_shader.uniform("tex_unit_lookup_1", tu1);
    model_render_state.tex_unit_lookups[0] = tu1;
  }

  if (model_render_state.tex_unit_lookups[1] != tu2)
  {
    m2_shader.uniform("tex_unit_lookup_2", tu2);
    model_render_state.tex_unit_lookups[1] = tu2;
  }

  m2_shader.uniform("tex_unit_lookup_3", static_cast<int>(tu_lookups[2]));
  auto const retailShader = retailM2Shader(shader_id);
  m2_shader.uniform("retail_edge_fade", retail
      && retailShader && retailShader->edgeFade ? 1 : 0);

  static const glm::mat4x4 unit(glm::mat4x4(1));
  auto texture_animation = [m](uint16_t lookup_index) -> int16_t
  {
    if (lookup_index >= m->_texture_animation_lookups.size())
      return -1;

    auto const animation_index = m->_texture_animation_lookups[lookup_index];
    if (animation_index < 0
        || static_cast<std::size_t>(animation_index) >= m->_texture_animations.size())
      return -1;

    return animation_index;
  };

  int16_t const first_tex_anim = texture_animation(uv_animations[0]);
  int16_t const second_tex_anim = texture_count > 1
    ? texture_animation(uv_animations[1]) : -1;
  m2_shader.uniform("tex_matrix_1", first_tex_anim != -1
    ? m->_texture_animations[first_tex_anim].mat : unit);
  m2_shader.uniform("tex_matrix_2", second_tex_anim != -1
    ? m->_texture_animations[second_tex_anim].mat : unit);

  int16_t const third_tex_anim = texture_count > 2 ? texture_animation(uv_animations[2]) : -1;
  m2_shader.uniform("tex_matrix_3", third_tex_anim != -1 ? m->_texture_animations[third_tex_anim].mat : unit);

  GLint ps = static_cast<GLint>(pixel_shader.value());
  if (model_render_state.pixel_shader != ps)
  {
    m2_shader.uniform("pixel_shader", ps);
    model_render_state.pixel_shader = ps;
  }

  m2_shader.uniform("mesh_color", mesh_color);

  return true;
}

void ModelRenderPass::afterDraw()
{
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void ModelRenderPass::bindTexture(size_t index, Model* m, OpenGL::M2RenderState& model_render_state, OpenGL::Scoped::use_program& m2_shader)
{
  if (index >= std::size(textures) || textures[index] >= m->_texture_lookup.size())
  {
    LogError << "model: texture lookup index out of range " << m->file_key().stringRepr() << std::endl;
    return;
  }

  uint16_t tex = m->_texture_lookup[textures[index]];
  if (tex >= m->_specialTextures.size())
  {
    LogError << "model: texture index out of range " << m->file_key().stringRepr() << std::endl;
    return;
  }

  scoped_blp_texture_reference* reference = nullptr;
  if (m->_specialTextures[tex] == -1)
  {
    if (tex >= m->_textures.size())
    {
      LogError << "model: ordinary texture index out of range " << m->file_key().stringRepr() << std::endl;
      return;
    }
    reference = &m->_textures[tex];
  }
  else
  {
    auto const replacement = m->_replaceTextures.find(static_cast<std::size_t>(m->_specialTextures[tex]));
    if (replacement == m->_replaceTextures.end())
    {
      LogError << "model: special texture index out of range " << m->file_key().stringRepr() << std::endl;
      return;
    }
    reference = &replacement->second;
  }
  auto& texture = *reference;
  texture->upload();
  auto const tex_index = texture->array_index();
  gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + index + 1));
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, texture->texture_array());
  m2_shader.uniform(index == 2 ? "tex3_index" : index == 1 ? "tex2_index" : "tex1_index", tex_index);
  model_render_state.tex_indices[index] = tex_index;
}
void ModelRenderPass::initUVTypes(Model* m)
{
  std::fill(std::begin(tu_lookups), std::end(tu_lookups), texture_unit_lookup::none);
  if (isRetailProject() && (shader_id & 0x8000))
  {
    if (auto const material = retailM2Shader(shader_id))
      for (unsigned stage = 0; stage < 3; ++stage)
        tu_lookups[stage] = static_cast<texture_unit_lookup>(material->uv[stage]);
    return;
  }

  auto const lookup_size = m->_texture_unit_lookup.size();
  bool const invalid_texture_coord_combo =
      texture_count == 0
      || texture_coord_combo_index >= lookup_size
      || texture_count > lookup_size - texture_coord_combo_index;

  if (invalid_texture_coord_combo)
  {
    if (isShadowlandsProject())
    {
      LogDebug << "modern M2: texture_coord_combo_index resolved from shader " << m->file_key().stringRepr() << std::endl;

      if (texture_count == 0)
        texture_count = 1;
      if (texture_count > 2)
        texture_count = 2;

      if (shader_id & 0x8000)
      {
        switch (shader_id & 0xFF)
        {
          case 0:
          case 2:
          case 3:
          case 9:
          case 17:
          case 24:
            tu_lookups[0] = texture_unit_lookup::t1;
            if (texture_count > 1)
              tu_lookups[1] = texture_unit_lookup::environment;
            break;
          case 21:
            tu_lookups[0] = texture_unit_lookup::t1;
            if (texture_count > 1)
              tu_lookups[1] = texture_unit_lookup::t2;
            break;
          default:
            tu_lookups[0] = texture_unit_lookup::t1;
            if (texture_count > 1)
              tu_lookups[1] = texture_unit_lookup::t1;
            break;
        }
      }
      else
      {
        tu_lookups[0] = (shader_id & 0x80)
            ? texture_unit_lookup::environment
            : texture_unit_lookup::t1;

        if (texture_count > 1)
        {
          if (shader_id & 0x8)
            tu_lookups[1] = texture_unit_lookup::environment;
          else if (shader_id & 0x4000)
            tu_lookups[1] = texture_unit_lookup::t2;
          else
            tu_lookups[1] = texture_unit_lookup::t1;
        }
      }

      return;
    }

    LogDebug << "model: texture_coord_combo_index fallback " << m->file_key().stringRepr() << std::endl;
    if (texture_count > 0)
      tu_lookups[0] = texture_unit_lookup::t1;
    if (texture_count > 1)
      tu_lookups[1] = texture_unit_lookup::t2;
    return;
  }

  for (int i = 0; i < texture_count; ++i)
  {
    switch (m->_texture_unit_lookup[texture_coord_combo_index + i])
    {
      case (int16_t)(-1): tu_lookups[i] = texture_unit_lookup::environment; break;
      case 0: tu_lookups[i] = texture_unit_lookup::t1; break;
      case 1: tu_lookups[i] = texture_unit_lookup::t2; break;
    }
  }
}

bool ModelRenderPass::operator< (const ModelRenderPass& m) const
{
  if (priority_plane < m.priority_plane)
  {
    return true;
  }
  else if (priority_plane > m.priority_plane)
  {
    return false;
  }
  else
  {
    return blend_mode == m.blend_mode ? (ordering_thingy < m.ordering_thingy) : blend_mode < m.blend_mode;
  }
}
