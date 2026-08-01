// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ContextObject.hpp>
#include <noggit/ModelManager.h>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <utility>
#include <vector>

class MapChunk;

namespace Noggit
{
  // Client-matching ground effect doodad placements for one chunk, cached and
  // regenerated when the chunk is edited, the ground effect DBCs change or the
  // density setting moves.
  struct ChunkDetailDoodads
  {
    std::uint32_t chunk_stamp = 0;
    std::uint32_t dbc_stamp = 0;
    int density = -1;
    std::vector<std::pair<scoped_model_reference, std::vector<glm::mat4x4>>> models;
  };

  namespace DetailDoodads
  {
    // bumped when ground effect DBC records are edited so cached placements rebuild
    std::uint32_t dbcStamp();
    void bumpDbcStamp();

    // Reproduces CMapChunk::CreateDetailDoodads (Wow.exe 12340 @ 0x7D3390):
    // per-chunk deterministic seed, cell picks, stride-13 weighted doodad
    // table and the jitter/slope/plane-snap math, so the editor shows the
    // same layout the client will generate from the saved data.
    void generate(MapChunk* chunk, int density, NoggitRenderContext context, ChunkDetailDoodads& out);
  }
}
