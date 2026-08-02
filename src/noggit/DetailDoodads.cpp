// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DetailDoodads.hpp>

#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/texture_set.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace
{
  // g_rndNoiseTable256 (Wow.exe 12340 @ 0x9F1700), an exact permutation of 0..255
  constexpr std::uint8_t noise_table[256] = {
    0x8E, 0x14, 0x27, 0x99, 0xFD, 0xAA, 0xC7, 0x08, 0xD5, 0xE6, 0x3E, 0x1F, 0xF6, 0xBB, 0x55, 0xDA,
    0x75, 0xA0, 0x4A, 0x6A, 0xE8, 0xBD, 0x97, 0xFF, 0xDE, 0x9B, 0xBC, 0x9F, 0x81, 0x8A, 0xA1, 0x46,
    0x6E, 0x0B, 0xE3, 0x63, 0x76, 0x7A, 0x6C, 0x5D, 0x88, 0xD3, 0x69, 0xCA, 0xC3, 0x47, 0xB9, 0x25,
    0x83, 0xAB, 0xA2, 0x3F, 0xA6, 0x41, 0x7C, 0xBA, 0xE5, 0xAC, 0x95, 0x01, 0x7E, 0xCF, 0x09, 0xC1,
    0xD9, 0x62, 0x70, 0x71, 0x8D, 0xDB, 0x05, 0x02, 0x24, 0x87, 0xEF, 0x54, 0xC6, 0xD4, 0x37, 0x30,
    0xD0, 0x1B, 0xCB, 0x7B, 0xB8, 0xE4, 0xD8, 0xEC, 0x49, 0xCE, 0xAD, 0xDC, 0x13, 0xA9, 0x94, 0xC4,
    0x8F, 0x39, 0xAE, 0x0D, 0x18, 0x52, 0xDD, 0x0E, 0x78, 0xFA, 0xF5, 0x85, 0x58, 0xD2, 0xAF, 0x6D,
    0xA4, 0xB2, 0x53, 0x3B, 0x51, 0xA5, 0x50, 0xBE, 0xFC, 0x2D, 0xF4, 0x11, 0x48, 0x98, 0x16, 0xF1,
    0x86, 0xDF, 0x3D, 0x66, 0x5E, 0x44, 0x2E, 0x2F, 0x36, 0x07, 0x6B, 0x17, 0x8B, 0x29, 0x4C, 0xB6,
    0xE2, 0x89, 0x5F, 0xE7, 0xCD, 0xA7, 0x21, 0xE1, 0x4D, 0xC9, 0x65, 0xED, 0xFE, 0xEE, 0x9C, 0x23,
    0x33, 0x7D, 0xB7, 0x04, 0x9E, 0x9A, 0x2A, 0x40, 0xB3, 0x10, 0x5B, 0xF3, 0x82, 0x77, 0x1C, 0x92,
    0x20, 0x4E, 0x1E, 0x57, 0x22, 0x72, 0x06, 0x8C, 0x67, 0x2C, 0x73, 0xFB, 0x59, 0xC2, 0x0A, 0xBF,
    0x79, 0x5C, 0xF9, 0x0C, 0x28, 0x1A, 0x12, 0x68, 0x74, 0x34, 0x19, 0x42, 0xB1, 0xC0, 0x84, 0xF8,
    0x38, 0xF0, 0x15, 0x9D, 0x60, 0xF2, 0x3A, 0x6F, 0xB4, 0x90, 0xEB, 0x91, 0x1D, 0x7F, 0x35, 0x61,
    0x5A, 0x32, 0x03, 0x56, 0xA3, 0xC5, 0x2B, 0x93, 0x80, 0x0F, 0x4B, 0x43, 0xF7, 0xA8, 0xE0, 0x3C,
    0x96, 0xD1, 0x64, 0x26, 0xD7, 0x45, 0xCC, 0x4F, 0xC8, 0xB0, 0xE9, 0xB5, 0x00, 0xD6, 0x31, 0xEA,
  };

  // CRndSeed (SetSeed @ 0x4C1510, RandU32 @ 0x464580): a 4-tap lagged-Fibonacci
  // generator reading the noise table as little-endian dwords, moduli 47/53/59/61
  struct CRndSeed
  {
    std::uint32_t acc;
    std::uint32_t idx;
  };

  void set_seed(CRndSeed& s, std::uint32_t seed)
  {
    s.acc = seed;
    s.idx = (static_cast<std::uint32_t>(4 * (seed % 47)) << 24)
          | (static_cast<std::uint32_t>(4 * (seed % 53)) << 16)
          | (static_cast<std::uint32_t>(4 * (seed % 59)) << 8)
          |  static_cast<std::uint32_t>(4 * (seed % 61));
  }

  std::uint32_t rol32(std::uint32_t v, int n)
  {
    return (v << n) | (v >> (32 - n));
  }

  std::uint32_t table_dword(std::uint32_t byte_ofs)
  {
    std::uint32_t v;
    std::memcpy(&v, noise_table + byte_ofs, 4);
    return v;
  }

  std::uint32_t rand_u32(CRndSeed& s)
  {
    std::uint32_t i47 = (s.idx >> 24) & 0xFF;
    std::uint32_t i53 = (s.idx >> 16) & 0xFF;
    std::uint32_t i59 = (s.idx >> 8) & 0xFF;
    std::uint32_t i61 = s.idx & 0xFF;

    i47 = (i47 >= 4) ? i47 - 4 : i47 - 4 + 188;   // 188 = 4*47
    i53 = (i53 >= 12) ? i53 - 12 : i53 - 12 + 212; // 212 = 4*53
    i59 = (i59 >= 24) ? i59 - 24 : i59 - 24 + 236; // 236 = 4*59
    i61 = (i61 >= 28) ? i61 - 28 : i61 - 28 + 244; // 244 = 4*61

    std::uint32_t const out = (rol32(table_dword(i47), 1) ^ table_dword(i61)
                             ^ rol32(table_dword(i53), 2) ^ rol32(table_dword(i59), 3)) + s.acc;

    s.idx = i61 | (i59 << 8) | (i53 << 16) | (i47 << 24);
    s.acc = out;
    return out;
  }

  // [-1, 1]; the sign bit of the raw random picks the half
  float rand_signed(CRndSeed& s)
  {
    std::uint32_t const r = rand_u32(s);
    std::uint32_t const bits = 0x3F800000u | (r & 0x7FFFFFu); // float in [1, 2)
    float u;
    std::memcpy(&u, &bits, 4);
    return (static_cast<std::int32_t>(r) >= 0) ? (u - 2.0f) : (2.0f - u);
  }

  // one cell of the chunk's 8x8 doodad grid: the client's UNITSIZE,
  // TILESIZE / 128 (533.33333 / 128 yards)
  constexpr float CELL = 4.1666665f;
  constexpr float HALF_CELL = 2.0833333f;

  // the 4-triangle cell fan through the centre vertex (CFacet::Set @ 0x7912C0)
  constexpr int VTX_A[4] = { 17, 0, 18, 1 };
  constexpr int VTX_B[4] = { 0, 1, 17, 18 };
  constexpr int CRN_A[4] = { 3, 0, 2, 1 };
  constexpr int CRN_B[4] = { 0, 1, 3, 2 };
  constexpr float CORNER[4][2] = { { 0.f, 0.f }, { 0.f, -CELL }, { -CELL, -CELL }, { -CELL, 0.f } };

  struct Facet
  {
    float a, b, c, d; // unit plane normal + offset
  };

  // Axes follow the client convention: x = the x17 vertex row (noggit z),
  // y = the in-row column (noggit x). Heights are absolute, which only shifts
  // the plane's d, so the plane evaluates to absolute height.
  Facet build_facet(MapChunk* chunk, int row, int col, int t)
  {
    int const base = 17 * row + col;
    float const base_x = -CELL * row;
    float const base_y = -CELL * col;

    glm::vec3 const C{ base_x - HALF_CELL, base_y - HALF_CELL, chunk->mVertices[base + 9].y };
    glm::vec3 const A{ base_x + CORNER[CRN_A[t]][0], base_y + CORNER[CRN_A[t]][1], chunk->mVertices[base + VTX_A[t]].y };
    glm::vec3 const B{ base_x + CORNER[CRN_B[t]][0], base_y + CORNER[CRN_B[t]][1], chunk->mVertices[base + VTX_B[t]].y };

    glm::vec3 const u = B - C;
    glm::vec3 const v = A - C;
    glm::vec3 n{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
    n = glm::normalize(n);
    return { n.x, n.y, n.z, -glm::dot(n, C) };
  }

  std::atomic<std::uint32_t> dbc_stamp_counter{ 1 };

  // globally monotonic so a rebuilt cache never repeats a revision another
  // chunk (or a recycled chunk address) already handed to the GL batch cache
  std::atomic<std::uint32_t> revision_counter{ 0 };
}

std::uint32_t Noggit::DetailDoodads::dbcStamp()
{
  return dbc_stamp_counter.load();
}

void Noggit::DetailDoodads::bumpDbcStamp()
{
  ++dbc_stamp_counter;
}

void Noggit::DetailDoodads::generate(MapChunk* chunk, int density, NoggitRenderContext context, ChunkDetailDoodads& out)
{
  out.chunk_stamp = chunk->detailDoodadStamp();
  out.dbc_stamp = dbcStamp();
  out.density = density;
  out.revision = ++revision_counter;
  out.models.clear();
  out.placements.clear();

  TextureSet* texture_set = chunk->getTextureSet();
  if (!texture_set->num())
  {
    return;
  }

  // untouched chunks keep the doodadMapping stored in the ADT, which is what
  // the client renders from; only alpha edits force noggit's recompute
  if (chunk->doodadMappingNeedsUpdate())
  {
    texture_set->updateDoodadMapping();
    chunk->clearDoodadMappingNeedsUpdate();
  }

  std::uint16_t const* mapping = texture_set->getDoodadMappingBase();
  std::uint8_t const* stencil = texture_set->getDoodadStencilBase();

  // client seed: cOffset.x | (cOffset.y << 16) with the global chunk index on
  // client axes. CMapChunk::Create (0x7C64B0) derives topLeftCoords.x from
  // cOffset.y and topLeftCoords.y from cOffset.x, so cOffset.x is the world Y
  // axis (noggit x / px) and cOffset.y is the world X axis (noggit z / py).
  std::uint32_t const coffset_x = static_cast<std::uint32_t>(chunk->mt->index.x) * 16 + chunk->px;
  std::uint32_t const coffset_y = static_cast<std::uint32_t>(chunk->mt->index.z) * 16 + chunk->py;

  CRndSeed rnd;
  set_seed(rnd, coffset_x | (coffset_y << 16));

  // pass 1: cell picks, random with replacement; duplicates spawn again
  int const D = std::clamp(density, 16, 256);
  std::uint8_t pick_col[256];
  std::uint8_t pick_row[256];

  for (int i = 0; i < D; ++i)
  {
    pick_col[i] = rand_u32(rnd) & 7; // first random: column -> noggit x
    pick_row[i] = rand_u32(rnd) & 7; // second random: row -> noggit z
  }

  // the client's facet prebuild consumes no randoms, so building lazily keeps
  // the random sequence bit-identical
  Facet facets[64][4];
  bool facet_built[64][4] = {};

  struct EffectData
  {
    bool valid = false;
    std::int32_t tbl[16] = {};
    std::uint32_t amount = 0;
  };
  std::unordered_map<unsigned int, EffectData> effects;

  auto resolve_effect = [&](unsigned int effect_id) -> EffectData const&
  {
    auto it = effects.find(effect_id);
    if (it != effects.end())
    {
      return it->second;
    }

    EffectData& data = effects[effect_id];
    if (!effect_id || effect_id == 0xFFFFFFFF || !gGroundEffectTextureDB.CheckIfIdExists(effect_id))
    {
      return data;
    }

    DBCFile::Record record = gGroundEffectTextureDB.getByID(effect_id);

    std::int32_t ids[4];
    std::int32_t weights[4];
    for (int k = 0; k < 4; ++k)
    {
      ids[k] = record.getInt(GroundEffectTextureDB::Doodads + k);
      weights[k] = record.getInt(GroundEffectTextureDB::Weights + k);
    }

    // 16-slot table, stride 13 (coprime with 16 -> scatters runs); the last 16
    // writes win, the padding cycles the id slots including zeroes
    int w = 0;
    int total = 0;
    for (int k = 0; k < 4; ++k)
    {
      std::int32_t weight = weights[k];
      if (weight <= 0)
      {
        continue;
      }
      total += weight;
      while (weight--)
      {
        data.tbl[w & 15] = ids[k];
        w += 13;
      }
    }
    while (total < 16)
    {
      data.tbl[w & 15] = ids[total & 3];
      ++total;
      w += 13;
    }

    data.amount = record.getUInt(GroundEffectTextureDB::Amount);
    if (!data.amount)
    {
      data.amount = 8;
    }
    data.valid = true;
    return data;
  };

  struct DoodadModel
  {
    int index = -1;
    bool align = false;
  };
  std::unordered_map<std::int32_t, DoodadModel> doodad_models;

  auto resolve_doodad = [&](std::int32_t id) -> DoodadModel const&
  {
    auto it = doodad_models.find(id);
    if (it != doodad_models.end())
    {
      return it->second;
    }

    DoodadModel& dm = doodad_models[id];
    if (!gGroundEffectDoodadDB.CheckIfIdExists(id))
    {
      return dm;
    }

    DBCFile::Record record = gGroundEffectDoodadDB.getByID(id);

    std::string path = "world/nodxt/detail/";
    path += record.getString(GroundEffectDoodadDB::Filename);
    if (path.size() > 4)
    {
      std::string ext = path.substr(path.size() - 4);
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".mdx" || ext == ".mdl")
      {
        path.replace(path.size() - 4, 4, ".m2");
      }
    }

    dm.align = record.getUInt(GroundEffectDoodadDB::Flags) & 1;
    dm.index = static_cast<int>(out.models.size());
    out.models.emplace_back(path, context);
    return dm;
  };

  // pass 2: per pick, masks -> layer -> effect record -> spawn attempts
  for (int i = 0; i < D; ++i)
  {
    int const col = pick_col[i];
    int const row = pick_row[i];

    if ((stencil[row] >> col) & 1) // bit set = doodads disabled on this unit
    {
      continue;
    }
    if (chunk->holes & (1 << (4 * (row >> 1) + (col >> 1))))
    {
      continue;
    }

    unsigned int const layer = (mapping[row] >> (2 * col)) & 3;
    // like the client, no bound check against the layer count: unused entries
    // of the fixed layer array carry effect id 0 and get skipped below
    unsigned int const effect_id = texture_set->getEffectForLayer(layer);

    EffectData const& effect = resolve_effect(effect_id);
    if (!effect.valid)
    {
      continue;
    }

    for (std::uint32_t j = 0; j < effect.amount; ++j)
    {
      // both randoms are always consumed, even for attempts rejected below
      float const r1 = rand_signed(rnd);
      float const r2 = rand_signed(rnd);
      float const o_col = r1 * HALF_CELL + HALF_CELL; // [0, CELL] along noggit x
      float const o_row = r2 * HALF_CELL + HALF_CELL; // [0, CELL] along noggit z

      // the doodad pick is deterministic, not random
      std::int32_t const doodad_id = effect.tbl[(static_cast<std::uint8_t>(i) + static_cast<std::uint8_t>(j)) & 15];
      if (!doodad_id)
      {
        continue;
      }

      int const t = (o_col > o_row ? 1 : 0) + ((o_col + o_row > CELL) ? 2 : 0);
      int const cell = col + 8 * row;
      if (!facet_built[cell][t])
      {
        facets[cell][t] = build_facet(chunk, row, col, t);
        facet_built[cell][t] = true;
      }
      Facet const& f = facets[cell][t];

      if (f.c < 0.4f) // steeper than ~66 degrees
      {
        continue;
      }

      float const local_x = -(o_row + row * CELL);
      float const local_y = -(o_col + col * CELL);
      float const height = -((local_x * f.a + local_y * f.b + f.d) / f.c);

      float const rot = (rand_signed(rnd) + 1.0f) * 3.1415927f; // [0, 2pi]
      float const scale = rand_signed(rnd) * 0.33f + 1.0f;      // [0.67, 1.33]

      // all randoms of this attempt are consumed; unresolvable ids drop here
      DoodadModel const& dm = resolve_doodad(doodad_id);
      if (dm.index < 0)
      {
        continue;
      }

      // MCCV over the facet: the same jitter randoms drive the barycentric
      // weights, so colour and position stay correlated like the client
      float bary_w, edge_t;
      if (std::fabs(r2) >= std::fabs(r1))
      {
        bary_w = std::fabs(r2);
        edge_t = 0.5f - r1 * 0.5f;
      }
      else
      {
        bary_w = std::fabs(r1);
        edge_t = 0.5f - r2 * 0.5f;
      }
      if (o_row - o_col < 0.0f)
      {
        edge_t = 1.0f - edge_t;
      }

      int const base = 17 * row + col;
      glm::vec3 const& cC = chunk->mccv[base + 9];
      glm::vec3 const& cA = chunk->mccv[base + VTX_A[t]];
      glm::vec3 const& cB = chunk->mccv[base + VTX_B[t]];
      glm::vec3 rgb = cC + bary_w * (cA - cC) + (bary_w * edge_t) * (cB - cA);
      rgb = glm::min(rgb, glm::vec3(1.0f));

      // MCSH: a shadowed doodad is darkened to 70% (lit is 65254/65536);
      // 1.92 maps yards to shadow texels, 64 texels / (8 * CELL) yards
      float shade = 0.99570f;
      int sx = std::clamp(static_cast<int>(std::floor((o_col + col * CELL) * 1.92f)), 0, 63);
      int sy = std::clamp(static_cast<int>(std::floor((o_row + row * CELL) * 1.92f)), 0, 63);
      if (chunk->_shadow_map[sy * 64 + sx])
      {
        shade = 0.70f;
      }
      rgb *= shade;

      std::uint32_t const color = (static_cast<std::uint32_t>(rgb.r * 255.f))
                                | (static_cast<std::uint32_t>(rgb.g * 255.f) << 8)
                                | (static_cast<std::uint32_t>(rgb.b * 255.f) << 16)
                                | 0xFF000000u;

      DetailDoodadPlacement placement;
      placement.model_index = static_cast<std::uint16_t>(dm.index);
      placement.terrain_align = dm.align;
      placement.pos = { chunk->xbase + (o_col + col * CELL)
                      , height
                      , chunk->zbase + (o_row + row * CELL) };
      placement.rot = rot;
      placement.scale = scale;
      // facet normal converted from client axes to noggit axes
      placement.normal = { -f.b, f.c, -f.a };
      placement.color = color;
      placement.facet_idx = static_cast<std::uint16_t>(4 * cell + t);

      out.placements.push_back(placement);
    }
  }
}
