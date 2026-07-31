// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <cstdint>
#include <vector>

namespace BlizzardArchive
{
  class ClientFile;
}

static constexpr int MAX_TEXTURE_LAYERS = 16;
static constexpr int MAX_ALPHAMAPS = MAX_TEXTURE_LAYERS - 1;

// The terrain shader reads the first 4 layers (3 alphamaps) of a chunk from its
// base RGB alphamap slice. Alphamaps of the layers beyond that are packed 3 per
// slice into extension slices appended after the 256 base slices of a tile's
// alphamap array.
static constexpr int BASE_RENDER_TEXTURE_LAYERS = 4;
static constexpr int EXT_RENDER_TEXTURE_LAYERS = MAX_TEXTURE_LAYERS - BASE_RENDER_TEXTURE_LAYERS;
static constexpr int EXT_ALPHAMAP_SLICES = (EXT_RENDER_TEXTURE_LAYERS + 2) / 3;

class Alphamap
{
public:
  Alphamap();
  Alphamap(BlizzardArchive::ClientFile* f, unsigned int flags, bool use_big_alphamaps, bool do_not_fix_alpha_map);

  void setAlpha(size_t offset, unsigned char value);
  void setAlpha(unsigned char *pAmap);

  [[nodiscard]]
  unsigned char getAlpha(size_t offset) const;

  const unsigned char *getAlpha();

  [[nodiscard]]
  std::vector<uint8_t> compress() const;

private:
  void readCompressed(BlizzardArchive::ClientFile *f);
  void readBigAlpha(BlizzardArchive::ClientFile *f);
  void readNotCompressed(BlizzardArchive::ClientFile *f, bool do_not_fix_alpha_map);

  void createNew(); 

  uint8_t amap[64 * 64];
};
