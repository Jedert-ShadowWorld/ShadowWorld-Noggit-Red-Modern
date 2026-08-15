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
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cmath>
#include <list>

static const unsigned int MAX_PARTICLES = 10000;

// twinkle modulation table, mirroring the client's g_particleFrameAnimTable.
// The static table in the binary is zero-filled and populated at init time
// with uniform [0,1) noise; these are the values captured from a running
// 3.3.5a client. The exact numbers aren't load-bearing (any uniform noise
// works statistically) but they are the client's actual values.
static const float TWINKLE_TABLE[128] = {
  0.6324f, 0.2921f, 0.2739f, 0.8879f, 0.0769f, 0.5087f, 0.6119f, 0.3590f,
  0.5281f, 0.8746f, 0.0271f, 0.2968f, 0.6658f, 0.1647f, 0.9740f, 0.6928f,
  0.9600f, 0.1155f, 0.8879f, 0.8070f, 0.9494f, 0.1874f, 0.0925f, 0.8640f,
  0.2901f, 0.3098f, 0.7672f, 0.8580f, 0.6798f, 0.2966f, 0.4896f, 0.1679f,
  0.7342f, 0.2610f, 0.7615f, 0.5902f, 0.0046f, 0.2726f, 0.3144f, 0.7713f,
  0.7489f, 0.8233f, 0.4711f, 0.1688f, 0.9746f, 0.1670f, 0.8768f, 0.7621f,
  0.0573f, 0.8980f, 0.7437f, 0.7491f, 0.7515f, 0.6776f, 0.5856f, 0.3498f,
  0.4228f, 0.9316f, 0.8946f, 0.6188f, 0.0078f, 0.5946f, 0.9378f, 0.8736f,
  0.4377f, 0.5119f, 0.4996f, 0.5348f, 0.2569f, 0.7771f, 0.5516f, 0.2216f,
  0.6822f, 0.7567f, 0.3617f, 0.5889f, 0.2660f, 0.8430f, 0.8257f, 0.0790f,
  0.0749f, 0.5859f, 0.8261f, 0.8788f, 0.5725f, 0.7957f, 0.1466f, 0.4167f,
  0.2580f, 0.5750f, 0.2026f, 0.4126f, 0.7093f, 0.6829f, 0.0086f, 0.8584f,
  0.5336f, 0.0751f, 0.4958f, 0.4362f, 0.5056f, 0.2468f, 0.9488f, 0.8274f,
  0.1818f, 0.5778f, 0.9489f, 0.9423f, 0.0291f, 0.2180f, 0.1417f, 0.8749f,
  0.1535f, 0.5698f, 0.2298f, 0.7220f, 0.5182f, 0.7896f, 0.4684f, 0.7986f,
  0.7308f, 0.5425f, 0.3844f, 0.1695f, 0.9620f, 0.0162f, 0.4795f, 0.8759f,
};

// client particle blend table (CGxDeviceD3d::s_srcBlend / s_dstBlend): sets the
// GL blend state for an M2 blend mode and returns the matching alpha-test
// threshold (-1 = no test). Out-of-range modes fall back to additive when
// additive_fallback is set (ribbons), plain opaque otherwise (particles).
static float apply_m2_blend_state(int blend, bool additive_fallback)
{
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
    if (additive_fallback)
    {
      gl.enable(GL_BLEND);
      gl.blendFunc(GL_SRC_ALPHA, GL_ONE);
    }
    else
    {
      gl.disable(GL_BLEND);
    }
    break;
  }

  return alpha_test;
}

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
  , z_source (mta.zSource, f, globals)
  , enabled (mta.en, f, globals)
  , tail_length (mta.p.tailLength)
  , render_head ((mta.flags & ParticleFlag_RenderHead) != 0)
  , render_tail ((mta.flags & ParticleFlag_RenderTail) != 0)
  , slowdown (mta.p.slowdown)
  , lifespan_vary (mta.lifespanVary)
  , rate_vary (mta.emissionRateVary)
  , twinkle_speed (mta.p.twinkleSpeed)
  , twinkle_percent (mta.p.twinklePercent)
  , twinkle_base (mta.p.twinkleScaleMin)
  , twinkle_range (mta.p.twinkleScaleMax - mta.p.twinkleScaleMin)
  , base_spin (mta.p.baseSpin)
  , base_spin_vary (mta.p.baseSpinVary)
  , spin_speed (mta.p.rotation)
  , spin_vary (mta.p.spinVary)
  , scale_vary (mta.p.scaleVary[0], mta.p.scaleVary[1])
  , tumble ((mta.flags & ParticleFlag_Tumble) != 0)
  , wind (fixCoordSystem(glm::vec3(mta.p.Rot2[2], mta.p.Trans[0], mta.p.Trans[1])))
  , wind_time (mta.p.Trans[2])
  , follow ((mta.flags & ParticleFlag_FollowPosition) != 0)
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
  , parent (&model->bones[mta.bone])
  , flags(mta.flags)
  , tofs (misc::frand())
  , _context(context)
{
  // FollowPosition 2-point fit (CParticleEmitter2::SetFollowParams): factor =
  // clamp(emitter_speed * slope + intercept, 0, 1); equal speeds disable it
  float follow_span = mta.p.followSpeed2 - mta.p.followSpeed1;
  if (std::fabs(follow_span) < 2.38e-7f) // ~2^-22, the client's span epsilon
  {
    follow_slope = 0.0f;
    follow_intercept = 0.0f;
  }
  else
  {
    follow_slope = (mta.p.followScale2 - mta.p.followScale1) / follow_span;
    follow_intercept = mta.p.followScale1 - mta.p.followSpeed1 * follow_slope;
  }

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
  , z_source(other.z_source)
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
  , twinkle_speed(other.twinkle_speed)
  , twinkle_percent(other.twinkle_percent)
  , twinkle_base(other.twinkle_base)
  , twinkle_range(other.twinkle_range)
  , base_spin(other.base_spin)
  , base_spin_vary(other.base_spin_vary)
  , spin_speed(other.spin_speed)
  , spin_vary(other.spin_vary)
  , scale_vary(other.scale_vary)
  , tumble(other.tumble)
  , wind(other.wind)
  , wind_time(other.wind_time)
  , follow(other.follow)
  , follow_slope(other.follow_slope)
  , follow_intercept(other.follow_intercept)
  , pos(other.pos)
  , _texture_id(other._texture_id)
  , blend(other.blend)
  , order(other.order)
  , type(other.type)
  , manim(other.manim)
  , mtime(other.mtime)
  , manimtime(other.manimtime)
  , rows(other.rows)
  , cols(other.cols)
  , tiles(other.tiles)
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
  , speed(std::move(other.speed))
  , variation(std::move(other.variation))
  , spread(std::move(other.spread))
  , lat(std::move(other.lat))
  , gravity(std::move(other.gravity))
  , lifespan(std::move(other.lifespan))
  , rate(std::move(other.rate))
  , areal(std::move(other.areal))
  , areaw(std::move(other.areaw))
  , z_source(std::move(other.z_source))
  , enabled(std::move(other.enabled))
  , color_track(std::move(other.color_track))
  , alpha_track(std::move(other.alpha_track))
  , scale_track(std::move(other.scale_track))
  , cell_track(std::move(other.cell_track))
  , tail_length(other.tail_length)
  , render_head(other.render_head)
  , render_tail(other.render_tail)
  , slowdown(other.slowdown)
  , lifespan_vary(other.lifespan_vary)
  , rate_vary(other.rate_vary)
  , twinkle_speed(other.twinkle_speed)
  , twinkle_percent(other.twinkle_percent)
  , twinkle_base(other.twinkle_base)
  , twinkle_range(other.twinkle_range)
  , base_spin(other.base_spin)
  , base_spin_vary(other.base_spin_vary)
  , spin_speed(other.spin_speed)
  , spin_vary(other.spin_vary)
  , scale_vary(other.scale_vary)
  , tumble(other.tumble)
  , wind(other.wind)
  , wind_time(other.wind_time)
  , follow(other.follow)
  , follow_slope(other.follow_slope)
  , follow_intercept(other.follow_intercept)
  , pos(other.pos)
  , _texture_id(other._texture_id)
  , blend(other.blend)
  , order(other.order)
  , type(other.type)
  , manim(other.manim)
  , mtime(other.mtime)
  , manimtime(other.manimtime)
  , rows(other.rows)
  , cols(other.cols)
  , tiles(std::move(other.tiles))
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


void ParticleSystem::update(float dt, glm::mat4x4 const& instance_mat, ParticleEmitterInstance& state)
{
  float grav = gravity.getValue(manim, mtime, manimtime);

  // world-space emission frame for this placement: instance transform on top
  // of the animated bone. Directions only pick up the rotation part; the
  // uniform placement scale washes out in the normalize.
  glm::mat4x4 emit_mat = instance_mat * parent->mat;
  glm::mat4x4 emit_rot = glm::mat4x4(glm::mat3(instance_mat)) * parent->mrot;

  // FollowPosition: the emitter's frame movement, scaled by the speed fit,
  // is picked up by every particle past its spawn frame (MoveParticle
  // @ 0x979BB0). First frame after enable contributes nothing.
  glm::vec3 follow_delta(0.0f);
  if (follow && dt > 0.0f)
  {
    glm::vec3 emit_pos = glm::vec3(emit_mat * glm::vec4(pos, 1.0f));
    if (state.prev_emit_valid)
    {
      glm::vec3 dp = emit_pos - state.prev_emit_pos;
      float emit_speed = glm::length(dp) / dt;
      float factor = std::clamp(emit_speed * follow_slope + follow_intercept, 0.0f, 1.0f);
      follow_delta = dp * factor;
    }
    state.prev_emit_pos = emit_pos;
    state.prev_emit_valid = true;
  }

  ParticleList& particles = state.particles;

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
      float ftospawn = frate * dt + state.rem;
      int tospawn = static_cast<int>(ftospawn + 0.5f);
      state.rem = ftospawn - static_cast<float>(tospawn);

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
        float zs = z_source.getValue(manim, mtime, manimtime);

        // InheritBoneScale (disk flag 0x20 -> runtime 0x400): sprite size scales
        // with the emission frame's world scale (CParticleEmitter2::BuildVertex)
        float bone_scale = (flags & ParticleFlag_InheritBoneScale) ? glm::length(glm::vec3(emit_mat[0])) : 1.0f;

        for (int i = 0; i<tospawn; ++i) {
          Particle p = emitter->newParticle(this, manim, mtime, manimtime, w, l, spd, var, spr, spr2, zs, emit_mat, emit_rot);
          p.life = misc::frand() * dt;

          p.spin_angle = base_spin + base_spin_vary * misc::randfloat(-1.0f, 1.0f);
          p.spin_rate = spin_speed + spin_vary * misc::randfloat(-1.0f, 1.0f);

          // scale variance: x always rolled; y independent only with IndependentScaleY
          p.scale_mul.x = std::max(0.0f, 1.0f + scale_vary.x * misc::randfloat(-1.0f, 1.0f));
          p.scale_mul.y = (flags & ParticleFlag_IndependentScaleY)
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

    p.speed += glm::vec3(0, -1.0f, 0) * grav * dt;

    // wind: constant acceleration for the first wind_time seconds of a
    // particle's life, then it stops (no wind at all when wind_time is 0)
    if (wind_time > 0.0f && p.life <= wind_time)
      p.speed += wind * dt;

    p.pos += p.speed * dt;

    p.spin_angle += p.spin_rate * dt;
    p.life += dt;
    float rlife = p.life / std::max(p.maxlife, 0.001f);

    if (rlife >= 1.0f)
    {
      it = particles.erase (it);
      continue;
    }

    // 2*dt < life excludes the spawn frame so a brand-new particle isn't
    // double-translated
    if (follow && 2.0f * dt < p.life)
      p.pos += follow_delta;

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
                         , std::vector<ParticleEmitterInstance const*> const& states
)
{
  if ((!render_head && !render_tail) || _texture_id >= model->_textures.size())
  {
    return;
  }

  bool any_particles = false;
  for (auto const* state : states)
  {
    if (!state->particles.empty())
    {
      any_particles = true;
      break;
    }
  }

  if (!any_particles)
  {
    return;
  }

  if (!_uploaded)
  {
    upload();
  }

  float alpha_test = apply_m2_blend_state(blend, false);

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

  // one concatenated batch across every visible placement; the particle slot
  // index (twinkle noise / parity flip) restarts per placement like the
  // client's per-emitter-instance slots
  bool batch_full = false;
  for (auto const* state : states)
  {
    std::size_t pi = 0;
    for (ParticleList::const_iterator it = state->particles.begin(); it != state->particles.end(); ++it, ++pi)
    {
      if (it->tile >= tiles.size())
      {
        continue;
      }

      if (vertices.size() + 8 > 65535)
      {
        batch_full = true;
        break;
      }

      TexCoordSet const& tc = tiles[it->tile];

      // twinkle (CParticleEmitter2::BuildVertex prologue): a noise-table sample
      // indexed by particle slot + age*speed scales the quad, and twinklePercent
      // below 1 culls particles whose sample exceeds it. The common case
      // (percent >= 1, min == max) skips the sample and reduces to min.
      // NOTE: pi is the particle's current list position, which shifts as older
      // particles die; the client keeps a stable per-particle slot instead.
      float twinkle = twinkle_base;
      if (twinkle_percent < 1.0f || twinkle_range != 0.0f)
      {
        int frame = static_cast<int>(twinkle_speed * it->life);
        float sample = TWINKLE_TABLE[(pi + static_cast<std::size_t>(frame)) & 0x7F];
        if (twinkle_percent < 1.0f && twinkle_percent < sample)
        {
          continue;
        }
        twinkle = sample * twinkle_range + twinkle_base;
      }

      float const sx = it->size.x * it->scale_mul.x * twinkle;
      float const sy = it->size.y * it->scale_mul.y * twinkle;

      if (render_head)
      {
        glm::vec3 right = vRight;
        glm::vec3 up = vUp;

        // VelocityOrient (flag 0x4): head's long axis follows screen-space
        // velocity, falling back to the plain billboard when the projection is
        // too short (CParticleEmitter2::BuildVertex HEAD-1, threshold 1/1296)
        bool velocity_basis = false;
        if (flags & ParticleFlag_VelocityOrient)
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
          if ((flags & ParticleFlag_SpinParityFlip) && (pi & 1))
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

    if (batch_full)
    {
      break;
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

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const indices_binder (_indices_vbo);
  gl.drawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_SHORT, nullptr);
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

Particle PlaneParticleEmitter::newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2, float zs, glm::mat4x4 const& emit_mat, glm::mat4x4 const& emit_rot)
{
  Particle p;

  glm::vec3 local(misc::randfloat(-0.5f, 0.5f) * w, 0, misc::randfloat(-0.5f, 0.5f) * l);
  p.pos = emit_mat * glm::vec4(sys->pos + local, 1);

  // velocity in spherical coords off the emitter up axis, polar/azimuth signed
  // (CPlaneParticleEmitter::CreateParticle @ 0x9815C0)
  float polar = misc::randfloat(-spr, spr);
  float azim = misc::randfloat(-spr2, spr2);
  glm::vec3 dir(sinf(polar) * cosf(azim), cosf(polar), sinf(polar) * sinf(azim));

  // zSource > 0 overrides the launch direction: away from the emitter-local
  // point (0, zs, 0) (client-space (0,0,zSource)) through the spawn position
  if (zs > 0.0f)
  {
    glm::vec3 zdir = local - glm::vec3(0.0f, zs, 0.0f);
    float dlen = glm::length(zdir);
    if (dlen > 1e-6f)
      dir = zdir / dlen;
  }
  dir = emit_rot * glm::vec4(dir, 0);
  dir = glm::normalize(dir);

  p.speed = dir * spd * (1.0f - var * misc::frand());

  p.life = 0;
  p.maxlife = sys->lifespan.getValue(anim, time, animtime)
            + sys->lifespan_vary * misc::randfloat(-1.0f, 1.0f);
  p.size = glm::vec2(1.0f, 1.0f);
  p.color = glm::vec4(1.0f);

  p.tile = misc::randint(0, sys->rows*sys->cols - 1);
  return p;
}

Particle SphereParticleEmitter::newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2, float zs, glm::mat4x4 const& emit_mat, glm::mat4x4 const& emit_rot)
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
  glm::vec3 local = normal * radius;

  p.pos = emit_mat * glm::vec4(sys->pos + local, 1);

  glm::vec3 dir;
  // zSource > 0 overrides both the radial and flag-0x100 directions
  if (zs > 0.0f && glm::length(local - glm::vec3(0.0f, zs, 0.0f)) > 1e-6f)
    dir = emit_rot * glm::vec4(glm::normalize(local - glm::vec3(0.0f, zs, 0.0f)), 0);
  else if (sys->flags & ParticleFlag_TravelUp)
    dir = emit_rot * glm::vec4(0, 1, 0, 0);
  else
    dir = emit_rot * glm::vec4(normal, 0);

  float dlen = glm::length(dir);
  dir = dlen > 1e-6f ? dir / dlen : glm::vec3(0, 1, 0);

  p.speed = dir * spd * (1.0f - var * misc::frand());

  p.life = 0;
  p.maxlife = sys->lifespan.getValue(anim, time, animtime)
            + sys->lifespan_vary * misc::randfloat(-1.0f, 1.0f);
  p.size = glm::vec2(1.0f, 1.0f);
  p.color = glm::vec4(1.0f);

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
  , tex_slot (mta.texSlot, f, globals)
  , visibility (mta.visibility, f, globals)
  , parent (&model->bones[mta.bone])
  , pos (fixCoordSystem(mta.pos))
  , manim (0)
  , mtime (0)
  , manimtime (0)
  // load clamps from CRibbonEmitter::Initialize: rate is ceil'd, lifetime
  // floored at 0.25s
  , edges_per_second (std::ceil(mta.edgesPerSecond))
  , edge_lifetime (std::max(0.25f, mta.edgeLifetime))
  , gravity (mta.gravity)
  , rows (std::max<int>(1, mta.textureRows))
  , cols (std::max<int>(1, mta.textureCols))
  , tcolor (1.0f)
  , cur_tile (0)
  , blend (4)
  , _context(context)
{
  _texture_ids = Model::M2Array<uint16_t>(f, mta.ofsTextures, mta.nTextures);
  _material_ids = Model::M2Array<uint16_t>(f, mta.ofsMaterials, mta.nMaterials);

  // ring capacity: ceil(rate * lifetime) + slack, matching the client's
  // steady-state maximum
  max_edges = static_cast<std::size_t>(std::ceil(edges_per_second * edge_lifetime + 2.0f));
  max_edges = std::min<std::size_t>(std::max<std::size_t>(max_edges, 4), 256);

  // blend mode comes from the referenced material; additive is the fallback
  if (!_material_ids.empty() && _material_ids[0] < model->_render_flags.size())
    blend = model->_render_flags[_material_ids[0]].blend;
}

RibbonEmitter::RibbonEmitter(RibbonEmitter const& other)
  : model(other.model)
  , color(other.color)
  , opacity(other.opacity)
  , above(other.above)
  , below(other.below)
  , tex_slot(other.tex_slot)
  , visibility(other.visibility)
  , parent(other.parent)
  , pos(other.pos)
  , manim(other.manim)
  , mtime(other.mtime)
  , manimtime(other.manimtime)
  , edges_per_second(other.edges_per_second)
  , edge_lifetime(other.edge_lifetime)
  , gravity(other.gravity)
  , rows(other.rows)
  , cols(other.cols)
  , max_edges(other.max_edges)
  , tcolor(other.tcolor)
  , cur_tile(other.cur_tile)
  , blend(other.blend)
  , _texture_ids(other._texture_ids)
  , _material_ids(other._material_ids)
  , _context(other._context)
{

}

RibbonEmitter::RibbonEmitter(RibbonEmitter&& other)
  : model(other.model)
  , color(std::move(other.color))
  , opacity(std::move(other.opacity))
  , above(std::move(other.above))
  , below(std::move(other.below))
  , tex_slot(std::move(other.tex_slot))
  , visibility(std::move(other.visibility))
  , parent(other.parent)
  , pos(other.pos)
  , manim(other.manim)
  , mtime(other.mtime)
  , manimtime(other.manimtime)
  , edges_per_second(other.edges_per_second)
  , edge_lifetime(other.edge_lifetime)
  , gravity(other.gravity)
  , rows(other.rows)
  , cols(other.cols)
  , max_edges(other.max_edges)
  , tcolor(other.tcolor)
  , cur_tile(other.cur_tile)
  , blend(other.blend)
  , _texture_ids(std::move(other._texture_ids))
  , _material_ids(std::move(other._material_ids))
  , _context(other._context)
{

}

void RibbonEmitter::setup(int anim, int time, int animtime)
{
  manim = anim;
  mtime = time;
  manimtime = animtime;

  // color/alpha are broadcast to the whole strip per frame; the flipbook cell
  // comes from the animated tex slot track
  auto col = color.getValue(anim, time, animtime);
  tcolor = glm::vec4(col.x, col.y, col.z, opacity.getValue(anim, time, animtime));
  cur_tile = tex_slot.uses(anim) ? tex_slot.getValue(anim, time, animtime) : 0;
}

void RibbonEmitter::update(float dt, glm::mat4x4 const& instance_mat, RibbonEmitterInstance& state)
{
  // CRibbonEmitter::Update caps dt at the edge lifetime; with the ring sized
  // ceil(rate * lifetime) + 2 this makes overflow impossible in steady state
  dt = std::min(dt, edge_lifetime);

  bool visible = !visibility.uses(manim) || visibility.getValue(manim, mtime, manimtime) != 0;
  if (visible)
  {
    glm::mat4x4 emit_mat = instance_mat * parent->mat;
    glm::vec3 emit_pos = emit_mat * glm::vec4(pos, 1.0f);

    // the client widens the strip along the bone's Y axis; that basis vector
    // is -Z in the converted coordinate system
    glm::vec3 up = -glm::vec3(emit_mat[2]);
    float up_len = glm::length(up);
    up = up_len > 1e-6f ? up / up_len : glm::vec3(0.0f, 1.0f, 0.0f);

    // heights are captured per edge at spawn and never re-sampled
    float above_now = std::max(0.0f, above.getValue(manim, mtime, manimtime));
    float below_now = std::max(0.0f, below.getValue(manim, mtime, manimtime));

    // NOTE: a new edge only appears on accumulator rollover; the client also
    // re-places the leading edge at the emitter every update.
    state.accum += edges_per_second * dt;
    while (state.accum >= 1.0f)
    {
      state.accum -= 1.0f;
      state.edges.emplace_front(emit_pos, up, above_now, below_now);
      if (state.edges.size() > max_edges)
        state.edges.pop_back();
    }
  }

  // closed-form gravity sag: (age*2 + dt) * gravity * dt on the vertical,
  // cumulative g*t^2 (CRibbonEmitter::Update @ 0x98035C)
  for (auto& e : state.edges)
  {
    e.pos.y += (e.age * 2.0f + dt) * gravity * dt;
    e.age += dt;
  }

  while (!state.edges.empty() && state.edges.back().age >= edge_lifetime)
    state.edges.pop_back();
}

void RibbonEmitter::draw( OpenGL::Scoped::use_program& shader
                        , std::vector<RibbonEmitterInstance const*> const& states
                        )
{
  if (_texture_ids.empty() || _texture_ids[0] >= model->_textures.size())
  {
    return;
  }

  bool any_strip = false;
  for (auto const* state : states)
  {
    if (state->edges.size() >= 2)
    {
      any_strip = true;
      break;
    }
  }

  if (!any_strip)
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

  // same client blend table as particles, mode from the referenced material
  float alpha_test = apply_m2_blend_state(blend, true);
  shader.uniform("alpha_test", alpha_test);
  gl.depthMask(blend <= 1 ? GL_TRUE : GL_FALSE);

  shader.uniform("color", tcolor);

  // flipbook cell from the animated tex slot; UV.u sweeps one tile width over
  // each edge's lifetime, anchored at the cell column (CRibbonEmitter::Update)
  int tile = cur_tile % (rows * cols);
  float tile_w = 1.0f / static_cast<float>(cols);
  float tile_h = 1.0f / static_cast<float>(rows);
  float base_u = static_cast<float>(tile % cols) * tile_w;
  float v_top = static_cast<float>((tile / cols) % rows) * tile_h;
  float v_bot = v_top + tile_h;

  // one strip per placement, concatenated into a single batch; indices never
  // bridge two placements' strips
  for (auto const* state : states)
  {
    if (state->edges.size() < 2 || vertices.size() + state->edges.size() * 2 > 65535)
    {
      continue;
    }

    std::uint16_t const base_vertex = static_cast<std::uint16_t>(vertices.size());

    for (auto it = state->edges.begin(); it != state->edges.end(); ++it)
    {
      float u = base_u + (it->age / edge_lifetime) * tile_w;

      texcoords.emplace_back(u, v_top);
      vertices.push_back(it->pos + it->up * it->above);
      texcoords.emplace_back(u, v_bot);
      vertices.push_back(it->pos - it->up * it->below);
    }

    for (std::uint16_t e = 0; e + 1 < static_cast<std::uint16_t>(state->edges.size()); ++e)
    {
      std::uint16_t base = base_vertex + e * 2;
      indices.push_back(base + 0);
      indices.push_back(base + 1);
      indices.push_back(base + 2);

      indices.push_back(base + 2);
      indices.push_back(base + 1);
      indices.push_back(base + 3);
    }
  }

  if (indices.empty())
  {
    return;
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

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const indices_binder(_indices_vbo);
  gl.drawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_SHORT, nullptr);
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
