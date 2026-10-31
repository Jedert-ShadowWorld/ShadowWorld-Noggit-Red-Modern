// Exercises the production alpha blend and paint/replace methods. The doubles
// below isolate texture identity, cleanup and dirty notification from Qt/GL.
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

constexpr int MAX_TEXTURE_LAYERS = 16;
constexpr float CHUNKSIZE = 32.f, TEXDETAILSIZE = CHUNKSIZE / 64.f;
struct tmp_edit_alpha_values
{
  std::array<std::array<float, 4096>, MAX_TEXTURE_LAYERS> map{};
  auto& operator[](std::size_t i) { return map.at(i); }
};
namespace misc
{
  bool float_equals(double a, double b) { return std::abs(a - b) < 0.000001; }
  float dist(float x, float z, float a, float b) { return std::hypot(x - a, z - b); }
  float getShortestDist(float x, float z, float a, float b, float size)
  { return dist(x, z, std::clamp(x, a, a + size), std::clamp(z, b, b + size)); }
  bool square_is_in_circle(float x, float z, float r, float a, float b, float size)
  { return std::max({dist(x,z,a,b),dist(x,z,a+size,b),dist(x,z,a,b+size),dist(x,z,a+size,b+size)}) <= r; }
}
// ACTUAL_PAINT_HELPER

using scoped_blp_texture_reference = int;
struct Brush { float getRadius() const { return 4.f; } float getValue(float) const { return 1.f; } };
namespace ChunkUpdateFlags { constexpr int ALPHAMAP = 1; }
struct Chunk { int edits = 0; void registerChunkUpdate(int) { ++edits; } };
struct TextureSet
{
  std::vector<int> textures;
  std::size_t nTextures = 0;
  std::unique_ptr<tmp_edit_alpha_values> tmp_edit_values = std::make_unique<tmp_edit_alpha_values>();
  Chunk chunk;
  Chunk* _chunk = &chunk;
  bool _need_lod_texture_map_update = false;
  int texture_id(int texture) const
  { auto i = std::find(textures.begin(), textures.end(), texture); return i == textures.end() ? -1 : int(i - textures.begin()); }
  int addTexture(int texture)
  { if(nTextures == MAX_TEXTURE_LAYERS) return -1; textures.push_back(texture); ++chunk.edits; return int(nTextures++); }
  int get_texture_index_or_add(int texture, float target)
  { auto i=texture_id(texture); return i >= 0 ? i : target == 0.f ? -1 : addTexture(texture); }
  bool eraseUnusedTextures()
  {
    bool erased = false;
    for (int i = int(nTextures) - 1; i >= 0; --i)
      if (std::all_of((*tmp_edit_values)[i].begin(), (*tmp_edit_values)[i].end(), [](float a) { return a == 0.f; }))
      {
        for (int j = i; j < int(nTextures) - 1; ++j) (*tmp_edit_values)[j].swap((*tmp_edit_values)[j + 1]);
        (*tmp_edit_values)[nTextures - 1].fill(0.f);
        textures.erase(textures.begin() + i); --nTextures; erased = true;
      }
    return erased;
  }
  void create_temporary_alphamaps_if_needed() {}
  bool replace_texture(int, int) { throw std::logic_error("Unexpected whole-chunk path"); }
  bool paintTexture(float, float, float, float, Brush*, float, float, int);
  bool replace_texture(float, float, float, float, float, int const&, int, bool);
};
// ACTUAL_PAINT_METHODS

void require(bool condition, char const* message) { if(!condition) throw std::runtime_error(message); }
int main()
{
  try
  {
    auto values = std::make_unique<tmp_edit_alpha_values>();
    (*values)[0][0] = 255.f;
    require(paint_alpha(*values, 2, 1, 0, 255.f, 0.5), "Paint did not change alpha");
    require((*values)[0][0] == 127.5f && (*values)[1][0] == 127.5f, "Two-layer pressure changed");
    require(!paint_alpha(*values, 2, 1, 0, 255.f, 0.0), "Zero pressure changed alpha");
    require(!paint_alpha(*values, 2, 1, 0, 127.5f, 1.0), "Identical target changed alpha");
    for (int n : {4, 16})
    {
      for (int i = 0; i < n; ++i) (*values)[i][0] = 255.f / n;
      require(paint_alpha(*values, n, n - 1, 0, 128.f, 0.4), "Multi-layer paint failed");
      float total = 0.f;
      for (int i = 0; i < n; ++i)
      {
        float v = (*values)[i][0];
        require(std::isfinite(v) && v >= 0.f && v <= 255.f, "Invalid layer weight"); total += v;
        require((*values)[i][1] == 0.f, "Paint changed a texel outside its footprint");
      }
      require(std::abs(total - 255.f) < 0.0001f, "Layer weight was not conserved");
    }

    TextureSet single;
    single.addTexture(7); single.chunk.edits = 0;
    Brush brush;
    require(!single.paintTexture(0,0,8,8,&brush,255.f,1.f,7), "Same single layer marked changed");
    require(single.chunk.edits == 0, "No-op paint marked chunk dirty");
    TextureSet empty;
    require(empty.paintTexture(0,0,8,8,&brush,255.f,1.f,7), "First layer was not reported changed");
    require(empty.nTextures == 1, "First layer not added");

    TextureSet full;
    for (int i = 0; i < 16; ++i) { full.addTexture(100+i); (*full.tmp_edit_values)[i].fill(i == 1 ? 0.f : 17.f); }
    require(full.replace_texture(0,0,8,8,4,108,999,false), "Radius replacement failed after cleanup");
    int source=full.texture_id(108), replacement=full.texture_id(999), untouched=full.texture_id(109);
    int offset=16*64+16;
    require(source == 7 && replacement == 15, "Cleanup fixture did not shift source");
    require((*full.tmp_edit_values)[source][offset] == 0.f, "Replacement used stale source index");
    require((*full.tmp_edit_values)[replacement][offset] == 17.f, "Replacement lost source alpha");
    require((*full.tmp_edit_values)[untouched][offset] == 17.f, "Replacement erased unrelated layer");
    require((*full.tmp_edit_values)[source][0] == 17.f, "Replacement changed texel outside brush");
    std::cout << "PASS: production brush alpha conservation/no-op and radius replacement after layer cleanup\n";
    return 0;
  }
  catch(std::exception const& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
