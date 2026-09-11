// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ModelRender.hpp"
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/Misc.h>
#include <noggit/Particle.h>
#include <noggit/TextureManager.h>
#include <noggit/project/CurrentProject.hpp>

#include <math/bounding_box.hpp>
#include <math/frustum.hpp>

#include <opengl/shader.hpp>

#include <external/tracy/Tracy.hpp>


using namespace Noggit::Rendering;

namespace
{
  bool isShadowlandsProject()
  {
    auto* project = Noggit::Project::CurrentProject::get();
    return project
      && (project->projectVersion == Noggit::Project::ProjectVersion::SL
          || project->projectVersion == Noggit::Project::ProjectVersion::RETAIL);
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

  for (std::string const& texture : _model->_textureFilenames)
    _model->_textures.emplace_back(texture, _model->_context);

  _buffers.upload();
  _vertex_arrays.upload();
  _bone_matrices_buf_tex = 0;

  if (_model->animBones)
  {
    gl.genTextures(1, &_bone_matrices_buf_tex);

    gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
    OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> const binder(_bone_matrices_buffer);
    gl.bufferData(GL_TEXTURE_BUFFER, _model->bone_matrices.size() * sizeof(glm::mat4x4), nullptr, GL_STREAM_DRAW);
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

  _model->_textureFilenames.clear();

  _uploaded = true;
}

void ModelRender::unload()
{
  _model->_textures.clear();
  _buffers.unload();
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

    if (_model->animated && animate && (!_model->anim_calculated || _model->_per_instance_animation))
    {
      _model->animate(model_view, 0, animtime);
      _model->anim_calculated = true;
    }

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

    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);

    for (ModelRenderPass& p : _render_passes)
    {
      if (p.prepareDraw(m2_shader, _model, model_render_state))
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
    // Noggit's M2 shader path and ModelRenderPass storage support two texture
    // stages. Newer clients may advertise more; never write past the arrays.
    if (pass.texture_count > 2)
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
    pass.pixel_shader = isShadowlandsProject()
        ? M2GetShadowlandsPixelShaderID(pass.texture_count, pass.shader_id)
        : M2GetPixelShaderID(pass.texture_count, pass.shader_id);
  }
}

void ModelRender::initRenderPasses(ModelView const* view, ModelTexUnit const* tex_unit, ModelGeoset const* model_geosets)
{
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

    pass.index_start = model_geosets[geoset].istart;
    pass.index_count = model_geosets[geoset].icount;
    pass.vertex_start = model_geosets[geoset].vstart;
    pass.vertex_end = pass.vertex_start + model_geosets[geoset].vcount;

    _render_passes.push_back(std::move(pass));
  }

  fixShaderIdBlendOverride();
  fixShaderIDLayer();
  computePixelShaderIDs();

  for (auto& pass : _render_passes)
  {
    pass.initUVTypes(_model);
  }

  std::sort(_render_passes.begin(), _render_passes.end());
}

void ModelRender::updateBoneMatrices()
{
  OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> const binder (_bone_matrices_buffer);
  gl.bufferSubData(GL_TEXTURE_BUFFER, 0, _model->bone_matrices.size() * sizeof(glm::mat4x4), _model->bone_matrices.data());
}

ModelRenderPass::ModelRenderPass(ModelTexUnit const& tex_unit, Model* m)
    : ModelTexUnit(tex_unit)
    , blend_mode(m->_render_flags[renderflag_index].blend)
{
}

bool ModelRenderPass::prepareDraw(OpenGL::Scoped::use_program& m2_shader, Model *m, OpenGL::M2RenderState& model_render_state)
{
  if (submesh >= m->showGeosets.size()
      || renderflag_index >= m->_render_flags.size()
      || !m->showGeosets[submesh]
      || !pixel_shader)
  {
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

  int16_t tex_anim_lookup = texture_animation(uv_animations[0]);

  if (tex_anim_lookup != -1)
  {
    m2_shader.uniform("tex_matrix_1", m->_texture_animations[tex_anim_lookup].mat);
    if (texture_count > 1)
    {
      tex_anim_lookup = texture_animation(uv_animations[1]);
      if (tex_anim_lookup != -1)
        m2_shader.uniform("tex_matrix_2", m->_texture_animations[tex_anim_lookup].mat);
      else
        m2_shader.uniform("tex_matrix_2", unit);
    }
  }
  else
  {
    m2_shader.uniform("tex_matrix_1", unit);
    m2_shader.uniform("tex_matrix_2", unit);
  }

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

  if (m->_specialTextures[tex] == -1)
  {
    if (tex >= m->_textures.size())
    {
      LogError << "model: ordinary texture index out of range " << m->file_key().stringRepr() << std::endl;
      return;
    }
    auto& texture = m->_textures[tex];
    texture->upload();
    GLuint tex_array = texture->texture_array();
    int tex_index = texture->array_index();

    gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + index + 1));
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex_array);
    m2_shader.uniform(index ? "tex2_index" : "tex1_index", tex_index);
    model_render_state.tex_indices[index] = tex_index;
  }
  else
  {
    auto const replacement = m->_replaceTextures.find(static_cast<std::size_t>(m->_specialTextures[tex]));
    if (replacement == m->_replaceTextures.end())
    {
      LogError << "model: special texture index out of range " << m->file_key().stringRepr() << std::endl;
      return;
    }

    auto& texture = replacement->second;
    texture->upload();
    GLuint tex_array = texture->texture_array();
    int tex_index = texture->array_index();

    gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + index + 1));
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex_array);
    m2_shader.uniform(index ? "tex2_index" : "tex1_index", tex_index);
    model_render_state.tex_indices[index] = tex_index;
  }
}

void ModelRenderPass::initUVTypes(Model* m)
{
  tu_lookups[0] = texture_unit_lookup::none;
  tu_lookups[1] = texture_unit_lookup::none;

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
