// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/DetailDoodadRender.hpp>

#include <noggit/MapChunk.h>
#include <noggit/Model.h>
#include <noggit/rendering/ModelRender.hpp>
#include <noggit/TextureManager.h>

#include <opengl/context.hpp>
#include <opengl/context.inl>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace Noggit::Rendering
{
  namespace
  {
    struct DDVertex
    {
      glm::vec3 pos;
      glm::vec3 normal;
      std::uint32_t color;
      glm::vec2 uv;
    };
  }

  void DetailDoodadRender::deleteBuffers(ChunkGL& gl_data)
  {
    if (gl_data.vao)
    {
      gl.deleteVertexArray(1, &gl_data.vao);
      gl.deleteBuffers(1, &gl_data.vbo);
      gl.deleteBuffers(1, &gl_data.ibo);
      gl_data.vao = gl_data.vbo = gl_data.ibo = 0;
    }
    gl_data.batches.clear();
    gl_data.ready = false;
  }

  bool DetailDoodadRender::build(ChunkGL& gl_data, OpenGL::Scoped::use_program& shader, ChunkDetailDoodads* cache)
  {
    // wait until every referenced model settled; failed ones drop out
    for (auto const& model : cache->models)
    {
      if (!model->finishedLoading() && !model->loading_failed())
      {
        return false;
      }
    }

    struct ModelGeo
    {
      bool ok = false;
      Model* model = nullptr;
      unsigned int texture_id = 0;
      std::uint16_t vertex_start = 0;
      std::uint16_t vertex_count = 0;
      std::uint16_t index_start = 0;
      std::uint16_t index_count = 0;
    };

    std::vector<ModelGeo> geo(cache->models.size());
    for (std::size_t i = 0; i < cache->models.size(); ++i)
    {
      Model* model = cache->models[i].get();
      if (model->loading_failed() || model->skin_load_failed())
      {
        continue;
      }

      auto const& passes = model->renderer()->renderPasses();
      if (passes.empty() || model->vertexData().empty())
      {
        continue;
      }

      // the client only draws submesh 0 with the first batch's texture
      auto const pass = std::min_element(passes.begin(), passes.end()
        , [](auto const& a, auto const& b) { return a.index_start < b.index_start; });

      if (pass->textures[0] >= model->textureRefs().size() || !pass->index_count)
      {
        continue;
      }

      ModelGeo& g = geo[i];
      g.model = model;
      g.texture_id = pass->textures[0];
      g.vertex_start = pass->vertex_start;
      g.vertex_count = pass->vertex_end - pass->vertex_start;
      g.index_start = pass->index_start;
      g.index_count = pass->index_count;
      g.ok = g.vertex_count && g.index_count;
    }

    std::vector<DDVertex> vertices;
    std::vector<std::uint32_t> indices;
    gl_data.batches.clear();

    // batches stay contiguous per model so one draw covers each texture
    for (std::size_t mi = 0; mi < geo.size(); ++mi)
    {
      ModelGeo const& g = geo[mi];
      if (!g.ok)
      {
        continue;
      }

      std::size_t const batch_index_offset = indices.size();
      auto const& model_vertices = g.model->vertexData();
      auto const& model_indices = g.model->indexData();

      for (auto const& p : cache->placements)
      {
        if (p.model_index != mi)
        {
          continue;
        }

        glm::mat3 rot3;
        float const cs = std::cos(p.rot);
        float const sn = std::sin(p.rot);
        // rotation about the up axis; noggit's axes flip both horizontal
        // directions relative to the client, so the angle negates
        glm::mat3 const spin{ { cs, 0.f, sn }, { 0.f, 1.f, 0.f }, { -sn, 0.f, cs } };

        if (p.terrain_align)
        {
          // align model up to the facet normal, then spin about it
          glm::vec3 const up = p.normal;
          glm::vec3 const ref = std::fabs(up.y) < 0.99f ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
          glm::vec3 const t0 = glm::normalize(glm::cross(ref, up));
          glm::vec3 const t1 = glm::cross(up, t0);
          rot3 = glm::mat3(t0, up, t1) * spin;
        }
        else
        {
          rot3 = spin;
        }

        std::uint32_t const base_vertex = static_cast<std::uint32_t>(vertices.size());

        for (std::uint16_t v = 0; v < g.vertex_count; ++v)
        {
          auto const& mv = model_vertices[g.vertex_start + v];
          DDVertex out;
          out.pos = rot3 * mv.position * p.scale + p.pos;
          out.normal = p.normal;
          out.color = p.color;
          out.uv = mv.texcoords[0];
          vertices.push_back(out);
        }

        for (std::uint16_t idx = 0; idx < g.index_count; ++idx)
        {
          indices.push_back(base_vertex + (model_indices[g.index_start + idx] - g.vertex_start));
        }
      }

      std::size_t const count = indices.size() - batch_index_offset;
      if (count)
      {
        gl_data.batches.push_back({ g.model, g.texture_id
                                  , static_cast<int>(count)
                                  , batch_index_offset * sizeof(std::uint32_t) });
      }
    }

    if (!gl_data.vao)
    {
      gl.genVertexArrays(1, &gl_data.vao);
      gl.genBuffers(1, &gl_data.vbo);
      gl.genBuffers(1, &gl_data.ibo);
    }

    if (!indices.empty())
    {
      gl.bufferData<GL_ARRAY_BUFFER, DDVertex>(gl_data.vbo, vertices, GL_STATIC_DRAW);
      gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint32_t>(gl_data.ibo, indices, GL_STATIC_DRAW);
    }

    gl_data.revision = cache->revision;
    gl_data.ready = true;
    return true;
  }

  void DetailDoodadRender::drawChunk(OpenGL::Scoped::use_program& shader, MapChunk* chunk, ChunkDetailDoodads* cache, int frame)
  {
    ChunkGL& gl_data = _chunks[chunk];
    gl_data.last_frame = frame;

    if (gl_data.revision != cache->revision || !gl_data.ready)
    {
      if (!build(gl_data, shader, cache))
      {
        return; // models still loading, retry next frame
      }
    }

    if (gl_data.batches.empty())
    {
      return;
    }

    OpenGL::Scoped::vao_binder const _ (gl_data.vao);

    {
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (gl_data.vbo);
      shader.attrib("position", 3, GL_FLOAT, GL_FALSE, sizeof(DDVertex), reinterpret_cast<void*>(offsetof(DDVertex, pos)));
      shader.attrib("normal", 3, GL_FLOAT, GL_FALSE, sizeof(DDVertex), reinterpret_cast<void*>(offsetof(DDVertex, normal)));
      shader.attrib("color", 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(DDVertex), reinterpret_cast<void*>(offsetof(DDVertex, color)));
      shader.attrib("uv", 2, GL_FLOAT, GL_FALSE, sizeof(DDVertex), reinterpret_cast<void*>(offsetof(DDVertex, uv)));
    }

    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const indices_binder (gl_data.ibo);

    for (auto const& batch : gl_data.batches)
    {
      auto const& texture = batch.model->textureRefs()[batch.texture_id];
      texture->upload();
      gl.activeTexture(GL_TEXTURE0);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, texture->texture_array());
      shader.uniform("tex_index", texture->array_index());

      gl.drawElements(GL_TRIANGLES, batch.index_count, GL_UNSIGNED_INT
        , reinterpret_cast<void*>(batch.index_offset_bytes));
    }
  }

  void DetailDoodadRender::endFrame(int frame)
  {
    for (auto it = _chunks.begin(); it != _chunks.end();)
    {
      // dropped out of range or unloaded a while ago: free the buffers.
      // only GL handles are touched, the chunk pointer is never dereferenced
      if (frame - it->second.last_frame > 120 || frame < it->second.last_frame)
      {
        deleteBuffers(it->second);
        it = _chunks.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  void DetailDoodadRender::unload()
  {
    for (auto& pair : _chunks)
    {
      deleteBuffers(pair.second);
    }
    _chunks.clear();
  }
}
