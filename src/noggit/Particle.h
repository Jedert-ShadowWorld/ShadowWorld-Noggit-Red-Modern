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

// M2 particle emitter flags (disk values); only the ones the simulation and
// renderer use are named here
enum M2ParticleFlags : uint32_t
{
  ParticleFlag_VelocityOrient    = 0x4,     // head's long axis follows screen-space velocity
  ParticleFlag_InheritBoneScale  = 0x20,    // sprite size scales with the emission frame's world scale
  ParticleFlag_TravelUp          = 0x100,   // sphere emitter launches along model-space up
  ParticleFlag_SpinParityFlip    = 0x200,   // spin sign flips for every other particle
  ParticleFlag_Tumble            = 0x1000,
  ParticleFlag_FollowPosition    = 0x4000,
  ParticleFlag_RenderHead        = 0x20000,
  ParticleFlag_RenderTail        = 0x40000,
  ParticleFlag_IndependentScaleY = 0x80000, // y scale variance rolled independently of x
};

struct Particle {
  glm::vec3 pos, speed;
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
  virtual Particle newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2, float zs, glm::mat4x4 const& emit_mat, glm::mat4x4 const& emit_rot) = 0;
};

class PlaneParticleEmitter : public ParticleEmitter {
public:
  explicit PlaneParticleEmitter() {}
  Particle newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2, float zs, glm::mat4x4 const& emit_mat, glm::mat4x4 const& emit_rot);
};

class SphereParticleEmitter : public ParticleEmitter {
public:
  explicit SphereParticleEmitter() {}
  Particle newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2, float zs, glm::mat4x4 const& emit_mat, glm::mat4x4 const& emit_rot);
};

// mutable emitter state owned by a single placement: the ParticleSystem /
// RibbonEmitter objects in the shared Model hold only the immutable
// definition (tracks, load-time params), each ModelInstance sims its own
// world-space copy of this state (client: one CParticleEmitter2 per CM2Model)
struct ParticleEmitterInstance
{
  ParticleList particles;
  float rem = 0.0f;
  glm::vec3 prev_emit_pos = glm::vec3(0.0f);
  bool prev_emit_valid = false;
};

struct TexCoordSet {
    glm::vec2 tc[4];
};

class ParticleSystem 
{
  Model *model;
  int emitter_type;
  std::unique_ptr<ParticleEmitter> emitter;
  Animation::M2Value<float> speed, variation, spread, lat, gravity, lifespan, rate, areal, areaw, z_source;
  Animation::M2Value<uint8_t> enabled;
  FBlockTrack<glm::vec3> color_track;
  FBlockTrack<float> alpha_track;
  FBlockTrack<glm::vec2> scale_track;
  FBlockTrack<uint16_t> cell_track;
  float tail_length;
  bool render_head, render_tail;
  float slowdown;
  float lifespan_vary, rate_vary;
  float twinkle_speed, twinkle_percent, twinkle_base, twinkle_range;
  float base_spin, base_spin_vary, spin_speed, spin_vary;
  glm::vec2 scale_vary;
  bool tumble;
  glm::vec3 wind;   // constant acceleration, applied while life <= wind_time
  float wind_time;
  bool follow;      // FollowPosition: particles inherit a fraction of emitter movement
  float follow_slope, follow_intercept;
  glm::vec3 pos;
  uint16_t _texture_id;
  int blend, order, type;
  int manim, mtime;
  int manimtime;
  int rows, cols;
  std::vector<TexCoordSet> tiles;
  void initTile(glm::vec2 *tc, int num);

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

  void update(float dt, glm::mat4x4 const& instance_mat, ParticleEmitterInstance& state);

  void setup(int anim, int time, int animtime);
  void draw( glm::mat4x4 const& model_view
           , OpenGL::Scoped::use_program& shader
           , std::vector<ParticleEmitterInstance const*> const& states
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


// one strip cross-section, spawned at the emitter's current position; the
// heights are captured at spawn and never re-sampled (CRibbonEmitter::PlaceEdge)
struct RibbonEdge
{
  glm::vec3 pos, up;
  float age;
  float above, below;
  RibbonEdge (glm::vec3 pos_, glm::vec3 up_, float above_, float below_)
    : pos (pos_)
    , up (up_)
    , age (0.f)
    , above (above_)
    , below (below_)
  {}
};

// per-placement ribbon state, same ownership model as ParticleEmitterInstance
struct RibbonEmitterInstance
{
  std::list<RibbonEdge> edges; // front = newest
  float accum = 0.0f;
};

// all per-placement emitter state for one ModelInstance; sized lazily against
// the model's emitter lists once it has loaded. Copies deliberately start a
// fresh sim instead of sharing the source's particles.
struct ModelEmitterStates
{
  std::vector<ParticleEmitterInstance> particles;
  std::vector<RibbonEmitterInstance> ribbons;

  ModelEmitterStates() = default;
  ModelEmitterStates(ModelEmitterStates const&) {}
  ModelEmitterStates& operator=(ModelEmitterStates const&)
  {
    particles.clear();
    ribbons.clear();
    return *this;
  }
  ModelEmitterStates(ModelEmitterStates&&) = default;
  ModelEmitterStates& operator=(ModelEmitterStates&&) = default;
};

class RibbonEmitter
{
  Model *model;

  Animation::M2Value<glm::vec3> color;
  Animation::M2Value<float, int16_t> opacity;
  Animation::M2Value<float> above, below;
  Animation::M2Value<uint16_t> tex_slot;
  Animation::M2Value<uint8_t> visibility;

  Bone *parent;

  glm::vec3 pos;

  int manim, mtime;
  int manimtime;

  float edges_per_second;
  float edge_lifetime;
  float gravity;
  int rows, cols;
  std::size_t max_edges;

  glm::vec4 tcolor;
  int cur_tile;
  int blend;

  std::vector<uint16_t> _texture_ids;
  std::vector<uint16_t> _material_ids;

public:
  RibbonEmitter(Model*, const BlizzardArchive::ClientFile &f, ModelRibbonEmitterDef const& mta, int *globals
                , Noggit::NoggitRenderContext context);

  RibbonEmitter(RibbonEmitter const& other);
  RibbonEmitter(RibbonEmitter&&);
  RibbonEmitter& operator= (RibbonEmitter const&) = delete;
  RibbonEmitter& operator= (RibbonEmitter&&) = delete;

  void setup(int anim, int time, int animtime);
  void update(float dt, glm::mat4x4 const& instance_mat, RibbonEmitterInstance& state);
  void draw( OpenGL::Scoped::use_program& shader
           , std::vector<RibbonEmitterInstance const*> const& states
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
