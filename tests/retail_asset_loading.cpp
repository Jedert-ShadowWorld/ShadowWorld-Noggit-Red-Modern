#include <noggit/GroundEffectAsset.hpp>
#include <noggit/formats/WmoPlaneGroup.hpp>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

void check(bool value) { if (!value) throw std::runtime_error("Retail asset regression failed"); }
int main(int argc, char** argv)
{
  using namespace Noggit::DetailDoodads;
  for (auto id : {218813u, 306986u, 331310u, 4294967295u})
  {
    auto ref = "filedataid:" + std::to_string(id);
    check(modelReference(ref, true) == ref);
    check(fileDataId(ref, true) == id);
    check(!fileDataId(ref, false));
    check(modelReference(ref, false) == "world/nodxt/detail/" + ref);
  }
  for (auto bad : {"filedataid:", "filedataid:0", "filedataid:-1", "filedataid:4294967296", "filedataid:1x"})
  {
    bool rejected = false;
    try { fileDataId(bad, true); } catch (std::runtime_error const&) { rejected = true; }
    check(rejected);
  }
  for (bool retail : {false, true})
  {
    check(modelReference("grass.MDX", retail) == "world/nodxt/detail/grass.m2");
    check(modelReference("grass.m2", retail) == "world/nodxt/detail/grass.m2");
  }
  using Noggit::Format::isPlaneOnlyGroup;
  std::vector<char> chunk(24);
  std::memcpy(chunk.data(), "LPOM", 4);
  std::uint32_t size = 16;
  std::memcpy(chunk.data() + 4, &size, 4);
  float plane[] = {0, 1, 0, -5};
  std::memcpy(chunk.data() + 8, plane, 16);
  check(isPlaneOnlyGroup(true, 0x08002080, 0, chunk));
  check(!isPlaneOnlyGroup(false, 0x08002080, 0, chunk));
  check(!isPlaneOnlyGroup(true, 0x2000, 0, chunk));
  check(!isPlaneOnlyGroup(true, 0x08002000, 0, chunk));
  check(!isPlaneOnlyGroup(true, 0x08002080, 1, chunk));
  check(!isPlaneOnlyGroup(true, 0x08002080, 0, {}));
  chunk.push_back(0);
  check(!isPlaneOnlyGroup(true, 0x08002080, 0, chunk));
  chunk.pop_back();
  plane[1] = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(chunk.data() + 8, plane, 16);
  check(!isPlaneOnlyGroup(true, 0x08002080, 0, chunk));
  plane[1] = 0;
  std::memcpy(chunk.data() + 8, plane, 16);
  check(!isPlaneOnlyGroup(true, 0x08002080, 0, chunk));
  // Optional local client fixture, never bundled with the tests.
  if (argc == 2)
  {
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
    check(bytes.size() >= 88);
    std::uint32_t flags;
    std::memcpy(&flags, bytes.data() + 28, 4);
    unsigned batches = 0;
    for (int offset : {60, 62, 64, 66})
    {
      std::uint16_t count;
      std::memcpy(&count, bytes.data() + offset, 2);
      batches += count;
    }
    check(isPlaneOnlyGroup(true, flags, batches, {bytes.data() + 88, bytes.size() - 88}));
  }
}
