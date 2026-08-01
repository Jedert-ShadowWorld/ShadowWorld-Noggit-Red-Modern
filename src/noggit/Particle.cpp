// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Misc.h>
#include <noggit/Model.h>
#include <noggit/Particle.h>
#include <noggit/TextureManager.h>
#include <opengl/context.hpp>
#include <opengl/context.inl>
#include <opengl/shader.hpp>
#include <ClientFile.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <list>

static const unsigned int MAX_PARTICLES = 10000;

ParticleSystem::ParticleSystem(Model* model_
                               , const BlizzardArchive::ClientFile& f
                               , const ModelParticleEmitterDef &mta
                               , int *globals
                               , Noggit::NoggitRenderContext context)
  : model (model_)
  , emitter_type(mta.EmitterType)
  , emitter ( mta.EmitterType == 1 ? std::unique_ptr<ParticleEmitter> (std::make_unique<PlaneParticleEmitter>())
            : mta.EmitterType == 2 ? std::unique_ptr<ParticleEmitter> (std::make_unique<SphereParticleEmitter>())
            : std::unique_ptr<ParticleEmitter> (std::make_unique<PlaneParticleEmitter>())
            )
  , speed (mta.EmissionSpeed, f, globals)
  , variation (mta.SpeedVariation, f, globals)
  , spread (mta.VerticalRange, f, globals)
  , lat (mta.HorizontalRange, f, globals)
  , gravity (mta.Gravity, f, globals)
  , lifespan (mta.Lifespan, f, globals)
  , rate (mta.EmissionRate, f, globals)
  , areal (mta.EmissionAreaLength, f, globals)
  , areaw (mta.EmissionAreaWidth, f, globals)
  , deacceleration (mta.Gravity2, f, globals)
  , enabled (mta.en, f, globals)
  , tail_length (mta.p.unk[0])
  , render_head ((mta.flags & 0x20000) != 0)
  , render_tail ((mta.flags & 0x40000) != 0)
  , slowdown (mta.p.slowdown)
  , lifespan_vary (mta.lifespanVary)
  , rate_vary (mta.emissionRateVary)
  , base_spin (mta.p.baseSpin)
  , base_spin_vary (mta.p.baseSpinVary)
  , spin_speed (mta.p.rotation)
  , spin_vary (mta.p.spinVary)
  , scale_vary (mta.p.scaleVary[0], mta.p.scaleVary[1])
  , tumble ((mta.flags & 0x1000) != 0)
  , pos (fixCoordSystem(mta.pos))
  , _texture_id (mta.texture)
  , blend (mta.blend)
  , order (mta.ParticleType > 0 ? -1 : 0)
  , type (mta.ParticleType)
  , manim (0)
  , mtime (0)
  , manimtime (0)
  , rows (std::max<int>(1, mta.rows))
  , cols (std::max<int>(1, mta.cols))
  , rem(0)
  , parent (&model->bones[mta.bone])
  , flags(mta.flags)
  , tofs (misc::frand())
  , _context(context)
{
  color_track.times = Model::M2Array<uint16_t>(f, mta.p.colors.ofsTimes, mta.p.colors.nTimes);
  for (auto const& c : Model::M2Array<glm::vec3>(f, mta.p.colors.ofsKeys, mta.p.colors.nKeys))
  {
    color_track.keys.push_back(c / 255.0f);
  }

  alpha_track.times = Model::M2Array<uint16_t>(f, mta.p.opacity.ofsTimes, mta.p.opacity.nTimes);
  for (int16_t a : Model::M2Array<int16_t>(f, mta.p.opacity.ofsKeys, mta.p.opacity.nKeys))
  {
    alpha_track.keys.push_back(a / 32768.0f);
  }

  scale_track.times = Model::M2Array<uint16_t>(f, mta.p.sizes.ofsTimes, mta.p.sizes.nTimes);
  scale_track.keys = Model::M2Array<glm::vec2>(f, mta.p.sizes.ofsKeys, mta.p.sizes.nKeys);

  cell_track.times = Model::M2Array<uint16_t>(f, mta.p.Intensity.ofsTimes, mta.p.Intensity.nTimes);
  cell_track.keys = Model::M2Array<uint16_t>(f, mta.p.Intensity.ofsKeys, mta.p.Intensity.nKeys);

  for (int i = 0; i<rows*cols; ++i) {
    TexCoordSet tc;
    initTile(tc.tc, i);
    tiles.push_back(tc);
  }
}

ParticleSystem::ParticleSystem(ParticleSystem const& other)
  : model(other.model)
  , emitter_type(other.emitter_type)
  , emitter( emitter_type == 1 ? std::unique_ptr<ParticleEmitter>(std::make_unique<PlaneParticleEmitter>())
           : emitter_type == 2 ? std::unique_ptr<ParticleEmitter>(std::make_unique<SphereParticleEmitter>())
           : std::unique_ptr<ParticleEmitter>(std::make_unique<PlaneParticleEmitter>())
           )
  , speed(other.speed)
  , variation(other.variation)
  , spread(other.spread)
  , lat(other.lat)
  , gravity(other.gravity)
  , lifespan(other.lifespan)
  , rate(other.rate)
  , areal(other.areal)
  , areaw(other.areaw)
  , deacceleration(other.deacceleration)
  , enabled(other.enabled)
  , color_track(other.color_track)
  , alpha_track(other.alpha_track)
  , scale_track(other.scale_track)
  , cell_track(other.cell_track)
  , tail_length(other.tail_length)
  , render_head(other.render_head)
  , render_tail(other.render_tail)
  , slowdown(other.slowdown)
  , lifespan_vary(other.lifespan_vary)
  , rate_vary(other.rate_vary)
  , base_spin(other.base_spin)
  , base_spin_vary(other.base_spin_vary)
  , spin_speed(other.spin_speed)
  , spin_vary(other.spin_vary)
  , scale_vary(other.scale_vary)
  , tumble(other.tumble)
  , pos(other.pos)
  , _texture_id(other._texture_id)
  , particles(other.particles)
  , blend(other.blend)
  , order(other.order)
  , type(other.type)
  , manim(other.manim)
  , mtime(other.mtime)
  , manimtime(other.manimtime)
  , rows(other.rows)
  , cols(other.cols)
  , tiles(other.tiles)
  , rem(other.rem)
  , parent(other.parent)
  , flags(other.flags)
  , tofs(other.tofs)
  , _context(other._context)
{

}

ParticleSystem::ParticleSystem(ParticleSystem&& other)
  : model(other.model)
  , emitter_type(other.emitter_type)
  , emitter(std::move(other.emitter))
  , speed(other.speed)
  , variation(other.variation)
  , spread(other.spread)
  , lat(other.lat)
  , gravity(other.gravity)
  , lifespan(other.lifespan)
  , rate(other.rate)
  , areal(other.areal)
  , areaw(other.areaw)
  , deacceleration(other.deacceleration)
  , enabled(other.enabled)
  , color_track(other.color_track)
  , alpha_track(other.alpha_track)
  , scale_track(other.scale_track)
  , cell_track(other.cell_track)
  , tail_length(other.tail_length)
  , render_head(other.render_head)
  , render_tail(other.render_tail)
  , slowdown(other.slowdown)
  , lifespan_vary(other.lifespan_vary)
  , rate_vary(other.rate_vary)
  , base_spin(other.base_spin)
  , base_spin_vary(other.base_spin_vary)
  , spin_speed(other.spin_speed)
  , spin_vary(other.spin_vary)
  , scale_vary(other.scale_vary)
  , tumble(other.tumble)
  , pos(other.pos)
  , _texture_id(other._texture_id)
  , particles(other.particles)
  , blend(other.blend)
  , order(other.order)
  , type(other.type)
  , manim(other.manim)
  , mtime(other.mtime)
  , manimtime(other.manimtime)
  , rows(other.rows)
  , cols(other.cols)
  , tiles(other.tiles)
  , rem(other.rem)
  , parent(other.parent)
  , flags(other.flags)
  , tofs(other.tofs)
  , _context(other._context)
{

}

void ParticleSystem::initTile(glm::vec2 *tc, int num)
{
  glm::vec2 otc[4];
  glm::vec2 a, b;
  int x = num % cols;
  int y = num / cols;
  a.x = x * (1.0f / cols);
  b.x = (x + 1) * (1.0f / cols);
  a.y = y * (1.0f / rows);
  b.y = (y + 1) * (1.0f / rows);

  otc[0] = a;
  otc[2] = b;
  otc[1].x = b.x;
  otc[1].y = a.y;
  otc[3].x = a.x;
  otc[3].y = b.y;

  for (int i = 0; i<4; ++i) {
    tc[(i + 4 - order) & 3] = otc[i];
  }
}


void ParticleSystem::update(float dt)
{
  float grav = gravity.getValue(manim, mtime, manimtime);

  if (emitter)
  {
    bool en = true;
    if (enabled.uses(manim))
      en = enabled.getValue(manim, mtime, manimtime) != 0;

    if (en)
    {
      // per-tick rate jitter (CParticleEmitter2::Update: rate + vary * U[-1,1])
      float frate = std::max(0.0f, rate.getValue(manim, mtime, manimtime)
                                   + rate_vary * misc::randfloat(-1.0f, 1.0f));
      float ftospawn = frate * dt + rem;
      int tospawn = static_cast<int>(ftospawn + 0.5f);
      rem = ftospawn - static_cast<float>(tospawn);

      if (particles.size() + static_cast<std::size_t>(tospawn) > MAX_PARTICLES)
        tospawn = static_cast<int>(MAX_PARTICLES - particles.size());

      if (tospawn > 0)
      {
        float w = areaw.getValue(manim, mtime, manimtime);
        float l = areal.getValue(manim, mtime, manimtime);
        float spd = speed.getValue(manim, mtime, manimtime);
        float var = variation.getValue(manim, mtime, manimtime);
        float spr = spread.getValue(manim, mtime, manimtime);
        float spr2 = lat.getValue(manim, mtime, manimtime);

        // InheritBoneScale (disk flag 0x20 -> runtime 0x400): sprite size scales
        // with the emission frame's world scale (CParticleEmitter2::BuildVertex)
        float bone_scale = (flags & 0x20) ? glm::length(glm::vec3(parent->mat[0])) : 1.0f;

        for (int i = 0; i<tospawn; ++i) {
          Particle p = emitter->newParticle(this, manim, mtime, manimtime, w, l, spd, var, spr, spr2);
          p.life = misc::frand() * dt;

          p.spin_angle = base_spin + base_spin_vary * misc::randfloat(-1.0f, 1.0f);
          p.spin_rate = spin_speed + spin_vary * misc::randfloat(-1.0f, 1.0f);

          // scale variance: x always rolled; y independent only with flag 0x80000
          p.scale_mul.x = std::max(0.0f, 1.0f + scale_vary.x * misc::randfloat(-1.0f, 1.0f));
          p.scale_mul.y = (flags & 0x80000)
                        ? std::max(0.0f, 1.0f + scale_vary.y * misc::randfloat(-1.0f, 1.0f))
                        : p.scale_mul.x;
          p.scale_mul *= bone_scale;

          particles.push_back(p);
        }
      }
    }
  }

  for (ParticleList::iterator it = particles.begin(); it != particles.end();) {
    Particle &p = *it;

    if (slowdown > 0)
      p.speed *= expf(-slowdown * dt);

    p.speed += p.down * grav * dt;
    p.pos += p.speed * dt;

    p.spin_angle += p.spin_rate * dt;
    p.life += dt;
    float rlife = p.life / std::max(p.maxlife, 0.001f);

    if (rlife >= 1.0f)
    {
      it = particles.erase (it);
      continue;
    }

    p.size = scale_track.sample(rlife, glm::vec2(1.0f, 1.0f));
    p.color = glm::vec4(color_track.sample(rlife, glm::vec3(1.0f)), alpha_track.sample(rlife, 1.0f));

    if (!cell_track.keys.empty())
      p.tile = cell_track.sampleStep(rlife) % (rows * cols);

    ++it;
  }
}

void ParticleSystem::setup(int anim, int time, int animtime)
{
  manim = anim;
  mtime = time;
  manimtime = animtime;
}

void ParticleSystem::draw( glm::mat4x4 const& model_view
                         , OpenGL::Scoped::use_program& shader
                         , GLuint const& transform_vbo
                         , int instances_count
)
{
  if (particles.empty() || (!render_head && !render_tail) || _texture_id >= model->_textures.size())
  {
    return;
  }

  if (!_uploaded)
  {
    upload();
  }

  // client particle blend table (CGxDeviceD3d::s_srcBlend / s_dstBlend)
  float alpha_test = -1.f;

  switch (blend)
  {
  case 0:
    gl.disable(GL_BLEND);
    break;
  case 1:
    gl.disable(GL_BLEND);
    alpha_test = 224.0f / 255.0f;
    break;
  case 2:
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    alpha_test = 1.0f / 255.0f;
    break;
  case 3:
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_ONE, GL_ONE);
    alpha_test = 1.0f / 255.0f;
    break;
  case 4:
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE);
    alpha_test = 1.0f / 255.0f;
    break;
  case 5:
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_DST_COLOR, GL_ZERO);
    alpha_test = 1.0f / 255.0f;
    break;
  case 6:
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_DST_COLOR, GL_SRC_COLOR);
    alpha_test = 1.0f / 255.0f;
    break;
  default:
    gl.disable(GL_BLEND);
    break;
  }

  gl.depthMask(blend <= 1 ? GL_TRUE : GL_FALSE);

  auto& texture = model->_textures[_texture_id];
  texture->upload();
  gl.activeTexture(GL_TEXTURE0);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, texture->texture_array());
  shader.uniform("tex_index", texture->array_index());

  // camera basis in world space (rows of the view rotation)
  glm::vec3 vRight(model_view[0][0], model_view[1][0], model_view[2][0]);
  glm::vec3 vUp(model_view[0][1], model_view[1][1], model_view[2][1]);
  glm::vec3 vDir(model_view[0][2], model_view[1][2], model_view[2][2]);

  std::vector<std::uint16_t> indices;
  std::vector<glm::vec3> vertices;
  std::vector<glm::vec3> offsets;
  std::vector<glm::vec4> colors_data;
  std::vector<glm::vec2> texcoords;

  std::uint16_t indice = 0;

  auto add_quad_indices([] (std::vector<std::uint16_t>& indices, std::uint16_t& start)
  {
    indices.push_back(start + 0);
    indices.push_back(start + 1);
    indices.push_back(start + 2);

    indices.push_back(start + 2);
    indices.push_back(start + 3);
    indices.push_back(start + 0);

    start += 4;
  });

  std::size_t pi = 0;
  for (ParticleList::iterator it = particles.begin(); it != particles.end(); ++it, ++pi)
  {
    if (it->tile >= tiles.size() || vertices.size() + 8 > 65535)
    {
      break;
    }

    TexCoordSet const& tc = tiles[it->tile];
    float const sx = it->size.x * it->scale_mul.x;
    float const sy = it->size.y * it->scale_mul.y;

    if (render_head)
    {
      glm::vec3 right = vRight;
      glm::vec3 up = vUp;

      // VelocityOrient (flag 0x4): head's long axis follows screen-space
      // velocity, falling back to the plain billboard when the projection is
      // too short (CParticleEmitter2::BuildVertex HEAD-1, threshold 1/1296)
      bool velocity_basis = false;
      if (flags & 0x4)
      {
        float vx = glm::dot(vRight, -it->speed);
        float vy = glm::dot(vUp, -it->speed);
        float xy_len_sq = vx * vx + vy * vy;
        if (xy_len_sq > 1.0f / 1296.0f)
        {
          float inv = 1.0f / std::sqrt(xy_len_sq);
          vx *= inv;
          vy *= inv;
          right = vx * vRight + vy * vUp;
          up = -vy * vRight + vx * vUp;
          velocity_basis = true;
        }
      }

      if (!velocity_basis)
      {
        // tumble (flag 0x1000) recomputes the angle from age with the base
        // values instead of the integrated per-particle spin; flag 0x200 flips
        // the sign for every other particle (BuildVertex parity flip)
        float angle = tumble ? it->life * spin_speed + base_spin : it->spin_angle;
        if ((flags & 0x200) && (pi & 1))
        {
          angle = -angle;
        }
        if (angle != 0.0f)
        {
          float c = std::cos(angle);
          float s = std::sin(angle);
          right = vRight * c + vUp * s;
          up = vUp * c - vRight * s;
        }
      }

      vertices.insert(vertices.end(), 4, it->pos);
      offsets.push_back(-(right * sx + up * sy));
      offsets.push_back(right * sx - up * sy);
      offsets.push_back(right * sx + up * sy);
      offsets.push_back(-(right * sx - up * sy));

      for (int i = 0; i < 4; ++i)
      {
        texcoords.push_back(tc.tc[i]);
        colors_data.push_back(it->color);
      }

      add_quad_indices(indices, indice);
    }

    if (render_tail)
    {
      float vlen = glm::length(it->speed);

      if (vlen > 1e-4f)
      {
        glm::vec3 axis = it->speed / vlen;
        glm::vec3 tail_pos = it->pos - axis * (tail_length * vlen);
        glm::vec3 perp = glm::cross(axis, vDir);
        float plen = glm::length(perp);
        perp = plen > 1e-4f ? perp / plen : vRight;

        vertices.push_back(it->pos + perp * sx);
        vertices.push_back(it->pos - perp * sx);
        vertices.push_back(tail_pos - perp * sx);
        vertices.push_back(tail_pos + perp * sx);
        offsets.insert(offsets.end(), 4, glm::vec3(0.f));
      }
      else if (!render_head)
      {
        vertices.insert(vertices.end(), 4, it->pos);
        offsets.push_back(-(vRight * sx + vUp * sy));
        offsets.push_back(vRight * sx - vUp * sy);
        offsets.push_back(vRight * sx + vUp * sy);
        offsets.push_back(-(vRight * sx - vUp * sy));
      }
      else
      {
        continue;
      }

      for (int i = 0; i < 4; ++i)
      {
        texcoords.push_back(tc.tc[i]);
        colors_data.push_back(it->color);
      }

      add_quad_indices(indices, indice);
    }
  }

  if (indices.empty())
  {
    return;
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_vertices_vbo, vertices, GL_STREAM_DRAW);
  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_offsets_vbo, offsets, GL_STREAM_DRAW);
  gl.bufferData<GL_ARRAY_BUFFER, glm::vec4>(_colors_vbo, colors_data, GL_STREAM_DRAW);
  gl.bufferData<GL_ARRAY_BUFFER, glm::vec2>(_texcoord_vbo, texcoords, GL_STREAM_DRAW);
  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_vbo, indices, GL_STREAM_DRAW);

  shader.uniform("alpha_test", alpha_test);

  OpenGL::Scoped::vao_binder const _ (_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder (_vertices_vbo);
    shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const offset_binder (_offsets_vbo);
    shader.attrib("offset", 3, GL_FLOAT, GL_FALSE, 0, 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const texcoord_binder (_texcoord_vbo);
    shader.attrib("uv", 2, GL_FLOAT, GL_FALSE, 0, 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const colors_binder (_colors_vbo);
    shader.attrib("color", 4, GL_FLOAT, GL_FALSE, 0, 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (transform_vbo);
    shader.attrib("transform", 0, 1);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const indices_binder (_indices_vbo);
  gl.drawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_SHORT, nullptr, instances_count);
}

void ParticleSystem::upload()
{
  _vertex_array.upload();
  _buffers.upload();
  _uploaded = true;
}

void ParticleSystem::unload()
{
  _vertex_array.unload();
  _buffers.unload();
  _uploaded = false;
}

Particle PlaneParticleEmitter::newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2)
{
  Particle p;

  p.pos = sys->pos + glm::vec3(misc::randfloat(-0.5f, 0.5f) * w, 0, misc::randfloat(-0.5f, 0.5f) * l);
  p.pos = sys->parent->mat * glm::vec4(p.pos, 1);

  // velocity in spherical coords off the emitter up axis, polar/azimuth signed
  // (CPlaneParticleEmitter::CreateParticle @ 0x9815C0)
  float polar = misc::randfloat(-spr, spr);
  float azim = misc::randfloat(-spr2, spr2);
  glm::vec3 dir(sinf(polar) * cosf(azim), cosf(polar), sinf(polar) * sinf(azim));
  dir = sys->parent->mrot * glm::vec4(dir, 0);

  p.dir = glm::normalize(dir);
  p.down = glm::vec3(0, -1.0f, 0);
  p.speed = p.dir * spd * (1.0f - var * misc::frand());

  p.life = 0;
  p.maxlife = sys->lifespan.getValue(anim, time, animtime)
            + sys->lifespan_vary * misc::randfloat(-1.0f, 1.0f);
  p.size = glm::vec2(1.0f, 1.0f);
  p.color = glm::vec4(1.0f);

  p.origin = p.pos;

  p.tile = misc::randint(0, sys->rows*sys->cols - 1);
  return p;
}

Particle SphereParticleEmitter::newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2)
{
  Particle p;

  // area width = outer radius, length = inner radius; elevation/azimuth signed
  // (CSphereParticleEmitter::CreateParticle @ 0x981950)
  float radius_outer = w;
  float radius_inner = std::min(l, radius_outer);
  float radius = radius_inner + (radius_outer - radius_inner) * misc::frand();

  float elev = misc::randfloat(-spr, spr);
  float azim = misc::randfloat(-spr2, spr2);
  glm::vec3 normal(cosf(elev) * cosf(azim), sinf(elev), cosf(elev) * sinf(azim));

  p.pos = sys->parent->mat * glm::vec4(sys->pos + normal * radius, 1);

  glm::vec3 dir;
  if (sys->flags & 0x100)
    dir = sys->parent->mrot * glm::vec4(0, 1, 0, 0);
  else
    dir = sys->parent->mrot * glm::vec4(normal, 0);

  float dlen = glm::length(dir);
  p.dir = dlen > 1e-6f ? dir / dlen : glm::vec3(0, 1, 0);
  p.down = glm::vec3(0, -1.0f, 0);
  p.speed = p.dir * spd * (1.0f - var * misc::frand());

  p.life = 0;
  p.maxlife = sys->lifespan.getValue(anim, time, animtime)
            + sys->lifespan_vary * misc::randfloat(-1.0f, 1.0f);
  p.size = glm::vec2(1.0f, 1.0f);
  p.color = glm::vec4(1.0f);

  p.origin = p.pos;

  p.tile = misc::randint(0, sys->rows*sys->cols - 1);
  return p;
}

RibbonEmitter::RibbonEmitter(Model* model_
                             , const BlizzardArchive::ClientFile &f
                             , ModelRibbonEmitterDef const& mta
                             , int *globals
                             , Noggit::NoggitRenderContext context)
  : model (model_)
  , color (mta.color, f, globals)
  , opacity (mta.opacity, f, globals)
  , above (mta.above, f, globals)
  , below (mta.below, f, globals)
  , parent (&model->bones[mta.bone])
  , pos (fixCoordSystem(mta.pos))
  , seglen (mta.length)
  , length (mta.res * seglen)
   // just use the first texture for now; most models I've checked only had one
  , tpos (fixCoordSystem(mta.pos))
   //! \todo  figure out actual correct way to calculate length
   // in BFD, res is 60 and len is 0.6, the trails are very short (too long here)
   // in CoT, res and len are like 10 but the trails are supposed to be much longer (too short here)
  , _context(context)
{
  _texture_ids = Model::M2Array<uint16_t>(f, mta.ofsTextures, mta.nTextures);
  _material_ids = Model::M2Array<uint16_t>(f, mta.ofsMaterials, mta.nMaterials);

   // create first segment
  segs.emplace_back(tpos, 0);

}

RibbonEmitter::RibbonEmitter(RibbonEmitter const& other)
  : model(other.model)
  , color(other.color)
  , opacity(other.opacity)
  , above(other.above)
  , below(other.below)
  , parent(other.parent)
  , pos(other.pos)
  , manim(other.manim)
  , mtime(other.mtime)
  , seglen(other.seglen)
  , length(other.length)
  , tpos(other.tpos)
  , tcolor(other.tcolor)
  , tabove(other.tabove)
  , tbelow(other.tbelow)
  , _texture_ids(other._texture_ids)
  , _material_ids(other._material_ids)
  , segs(other.segs)
  , _context(other._context)
{

}

RibbonEmitter::RibbonEmitter(RibbonEmitter&& other)
  : model(other.model)
  , color(other.color)
  , opacity(other.opacity)
  , above(other.above)
  , below(other.below)
  , parent(other.parent)
  , pos(other.pos)
  , manim(other.manim)
  , mtime(other.mtime)
  , seglen(other.seglen)
  , length(other.length)
  , tpos(other.tpos)
  , tcolor(other.tcolor)
  , tabove(other.tabove)
  , tbelow(other.tbelow)
  , _texture_ids(other._texture_ids)
  , _material_ids(other._material_ids)
  , segs(other.segs)
  , _context(other._context)
{

}

void RibbonEmitter::setup(int anim, int time, int animtime)
{
  glm::vec3 ntpos = parent->mat * glm::vec4(pos,0);
  glm::vec3 ntup = parent->mat * (glm::vec4(pos, 0) + glm::vec4(0, 0, 1,0));
  ntup -= ntpos;
  ntup = glm::normalize(ntup);
  float dlen = glm::distance(ntpos, tpos);

  manim = anim;
  mtime = time;

  // move first segment
  RibbonSegment &first = *segs.begin();
  if (first.len > seglen) {
    // add new segment
    first.back = glm::normalize((tpos - ntpos));
    first.len0 = first.len;
    RibbonSegment newseg (ntpos, dlen);
    newseg.up = ntup;
    segs.push_front(newseg);
  }
  else {
    first.up = ntup;
    first.pos = ntpos;
    first.len += dlen;
  }

  // kill stuff from the end TODO: occasional crashes here
  float l = 0;
  bool erasemode = false;
  for (std::list<RibbonSegment>::iterator it = segs.begin(); it != segs.end();)
  {
    if (!erasemode)
    {
      l += it->len;
      if (l > length)
      {
          it->len = l - length;
          erasemode = true;
      }

      ++it;
    }
    else
    {
      it = segs.erase(it);
    }
  }

  tpos = ntpos;
  auto col = color.getValue(anim, time, animtime);
  tcolor = glm::vec4(col.x,col.y,col.z, opacity.getValue(anim, time, animtime));

  tabove = above.getValue(anim, time, animtime);
  tbelow = below.getValue(anim, time, animtime);
}

void RibbonEmitter::draw( OpenGL::Scoped::use_program& shader
                        , GLuint const& transform_vbo
                        , int instances_count
                        )
{
  if (segs.size() < 2 || _texture_ids.empty() || _texture_ids[0] >= model->_textures.size())
  {
    return;
  }

  if (!_uploaded)
  {
    upload();
  }

  std::vector<std::uint16_t> indices;
  std::vector<glm::vec3> vertices;
  std::vector<glm::vec2> texcoords;

  auto& texture = model->_textures[_texture_ids[0]];
  texture->upload();
  gl.activeTexture(GL_TEXTURE0);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, texture->texture_array());
  shader.uniform("tex_index", texture->array_index());

  gl.enable(GL_BLEND);

  shader.uniform("color", tcolor);

  std::uint16_t indice = 0;
  auto add_quad_indices([] (std::vector<std::uint16_t>& indices, std::uint16_t& start)
  {
    indices.push_back(start + 0);
    indices.push_back(start + 1);
    indices.push_back(start + 2);

    indices.push_back(start + 2);
    indices.push_back(start + 1);
    indices.push_back(start + 3);

    start += 2;
  });

  std::list<RibbonSegment>::iterator it = segs.begin();
  float l = 0;
  for (; it != segs.end(); ++it) 
  {
    float u = l / length;

    texcoords.emplace_back(u, 0);
    vertices.push_back(it->pos + tabove * it->up);
    texcoords.emplace_back(u, 1);
    vertices.push_back(it->pos - tbelow * it->up);

    l += it->len;

    add_quad_indices(indices, indice);
  }

  if (segs.size() > 1)
  {
    // last segment...?
    --it;
    float t = it->len0 > 0.f ? it->len / it->len0 : 0.f;
    texcoords.emplace_back(1, 0);
    vertices.push_back(it->pos + tabove * it->up + t * it->back);
    texcoords.emplace_back(1, 1);
    vertices.push_back(it->pos - tbelow * it->up + t * it->back);
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_vertices_vbo, vertices, GL_STREAM_DRAW);
  gl.bufferData<GL_ARRAY_BUFFER, glm::vec2>(_texcoord_vbo, texcoords, GL_STREAM_DRAW);
  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_vbo, indices, GL_STREAM_DRAW);

  OpenGL::Scoped::vao_binder const _(_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder(_vertices_vbo);
    shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const texcoord_binder(_texcoord_vbo);
    shader.attrib("uv", 2, GL_FLOAT, GL_FALSE, 0, 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(transform_vbo);
    shader.attrib("transform", 0, 1);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const indices_binder(_indices_vbo);
  gl.drawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_SHORT, nullptr, instances_count);
}

void RibbonEmitter::upload()
{
  _vertex_array.upload();
  _buffers.upload();
  _uploaded = true;
}

void RibbonEmitter::unload()
{
  _vertex_array.unload();
  _buffers.unload();
  _uploaded = false;
}
