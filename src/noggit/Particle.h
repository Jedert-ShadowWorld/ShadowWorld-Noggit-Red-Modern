// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/Animated.h> // Animation::M2Value
#include <opengl/scoped.hpp>

#include <algorithm>
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

class Bone;
class Model;
class ParticleSystem;
class RibbonEmitter;

namespace OpenGL::Scoped
{
  struct use_program;
}

namespace BlizzardArchive
{
  class ClientFile;
}

struct Particle {
  glm::vec3 pos, speed, down, origin, dir;
  //glm::vec3 tpos;
  glm::vec2 size;
  glm::vec2 scale_mul;  // per-particle scale variance roll, applied on top of the size track
  float life, maxlife;
  float spin_angle;     // current cumulative rotation (radians), integrated per frame
  float spin_rate;
  unsigned int tile;
  glm::vec4 color;
};

typedef std::list<Particle> ParticleList;

// M2 particle lifetime track (FBlock): fixed 1/32767 time axis sampled at life ratio,
// endpoints pinned for 2-3 key tracks (CParticleEmitter2::FBlockKeyframeLookup)
template<typename T>
struct FBlockTrack
{
  std::vector<uint16_t> times;
  std::vector<T> keys;

  void lookup(float life_ratio, std::size_t* seg, float* u) const
  {
    std::size_t count = keys.size();
    if (count == 2)
    {
      *seg = 0;
      *u = life_ratio;
      return;
    }
    if (count == 3)
    {
      float split = times.size() > 1 ? times[1] / 32767.0f : 0.5f;
      if (life_ratio <= split)
      {
        *seg = 0;
        *u = split > 0.0f ? life_ratio / split : 0.0f;
      }
      else
      {
        float denom = 1.0f - split;
        *seg = 1;
        *u = denom > 0.0f ? (life_ratio - split) / denom : 0.0f;
      }
      return;
    }

    std::size_t s = 0;
    for (std::size_t i = 1; i < count && i < times.size(); ++i)
    {
      if (times[i] / 32767.0f <= life_ratio)
        s = i;
      else
        break;
    }
    s = std::min(s, count - 2);

    if (times.size() > s + 1)
    {
      float k0 = times[s] / 32767.0f;
      float k1 = times[s + 1] / 32767.0f;
      *u = (k1 - k0) != 0.0f ? (life_ratio - k0) / (k1 - k0) : 0.0f;
    }
    else
    {
      *u = life_ratio;
    }
    *seg = s;
  }

  T sample(float life_ratio, T fallback) const
  {
    if (keys.empty())
      return fallback;
    if (keys.size() == 1)
      return keys[0];

    std::size_t seg;
    float u;
    lookup(life_ratio, &seg, &u);
    return keys[seg] + (keys[seg + 1] - keys[seg]) * u;
  }

  T sampleStep(float life_ratio) const
  {
    if (keys.size() < 2)
      return keys.empty() ? T{} : keys[0];

    std::size_t seg;
    float u;
    lookup(life_ratio, &seg, &u);
    return u >= 1.0f ? keys[seg + 1] : keys[seg];
  }
};

class ParticleEmitter {
public:
  explicit ParticleEmitter() {}
  virtual ~ParticleEmitter() {}
  virtual Particle newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2) = 0;
};

class PlaneParticleEmitter : public ParticleEmitter {
public:
  explicit PlaneParticleEmitter() {}
  Particle newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2);
};

class SphereParticleEmitter : public ParticleEmitter {
public:
  explicit SphereParticleEmitter() {}
  Particle newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2);
};

struct TexCoordSet {
    glm::vec2 tc[4];
};

class ParticleSystem 
{
  Model *model;
  int emitter_type;
  std::unique_ptr<ParticleEmitter> emitter;
  Animation::M2Value<float> speed, variation, spread, lat, gravity, lifespan, rate, areal, areaw, deacceleration;
  Animation::M2Value<uint8_t> enabled;
  FBlockTrack<glm::vec3> color_track;
  FBlockTrack<float> alpha_track;
  FBlockTrack<glm::vec2> scale_track;
  FBlockTrack<uint16_t> cell_track;
  float tail_length;
  bool render_head, render_tail;
  float slowdown;
  float lifespan_vary, rate_vary;
  float base_spin, base_spin_vary, spin_speed, spin_vary;
  glm::vec2 scale_vary;
  bool tumble;
  glm::vec3 pos;
  uint16_t _texture_id;
  ParticleList particles;
  int blend, order, type;
  int manim, mtime;
  int manimtime;
  int rows, cols;
  std::vector<TexCoordSet> tiles;
  void initTile(glm::vec2 *tc, int num);

  float rem;
  //bool transform;

  // unknown parameters omitted for now ...
  Bone *parent;
  int32_t flags;

public:
  float tofs;

  ParticleSystem(Model*, const BlizzardArchive::ClientFile& f, const ModelParticleEmitterDef &mta,
                 int *globals, Noggit::NoggitRenderContext context);

  ParticleSystem(ParticleSystem const& other);
  ParticleSystem(ParticleSystem&&);
  ParticleSystem& operator= (ParticleSystem const&) = delete;
  ParticleSystem& operator= (ParticleSystem&&) = delete;

  void update(float dt);

  void setup(int anim, int time, int animtime);
  void draw( glm::mat4x4 const& model_view
           , OpenGL::Scoped::use_program& shader
           , GLuint const& transform_vbo
           , int instances_count
           );

  friend class PlaneParticleEmitter;
  friend class SphereParticleEmitter;

  void unload();

private:
  bool _uploaded = false;
  void upload();

  OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vertex_array;
  GLuint const& _vao = _vertex_array[0];
  OpenGL::Scoped::deferred_upload_buffers<5> _buffers;
  GLuint const& _vertices_vbo = _buffers[0];
  GLuint const& _offsets_vbo = _buffers[1];
  GLuint const& _colors_vbo = _buffers[2];
  GLuint const& _texcoord_vbo = _buffers[3];
  GLuint const& _indices_vbo = _buffers[4];
  Noggit::NoggitRenderContext _context;
};


struct RibbonSegment
{
  glm::vec3 pos, up, back;
  float len, len0;
  RibbonSegment (::glm::vec3 pos_, float len_)
    : pos (pos_)
    , up (0.f)
    , back (0.f)
    , len (len_)
    , len0 (0.f)
  {}
};

class RibbonEmitter 
{
  Model *model;

  Animation::M2Value<glm::vec3> color;
  Animation::M2Value<float, int16_t> opacity;
  Animation::M2Value<float> above, below;

  Bone *parent;

  glm::vec3 pos;

  int manim, mtime;
  int seglen;
  float length;

  glm::vec3 tpos;
  glm::vec4 tcolor;
  float tabove, tbelow;

  std::vector<uint16_t> _texture_ids;
  std::vector<uint16_t> _material_ids;

  std::list<RibbonSegment> segs;

public:
  RibbonEmitter(Model*, const BlizzardArchive::ClientFile &f, ModelRibbonEmitterDef const& mta, int *globals
                , Noggit::NoggitRenderContext context);

  RibbonEmitter(RibbonEmitter const& other);
  RibbonEmitter(RibbonEmitter&&);
  RibbonEmitter& operator= (RibbonEmitter const&) = delete;
  RibbonEmitter& operator= (RibbonEmitter&&) = delete;

  void setup(int anim, int time, int animtime);
  void draw( OpenGL::Scoped::use_program& shader
           , GLuint const& transform_vbo
           , int instances_count
           );

  void unload();

private:
  bool _uploaded = false;
  void upload();

  OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vertex_array;
  GLuint const& _vao = _vertex_array[0];
  OpenGL::Scoped::deferred_upload_buffers<3> _buffers;
  GLuint const& _vertices_vbo = _buffers[0];
  GLuint const& _texcoord_vbo = _buffers[1];
  GLuint const& _indices_vbo = _buffers[2];
  Noggit::NoggitRenderContext _context;
};
