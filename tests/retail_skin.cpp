#include <noggit/formats/RetailSkin.hpp>
#include <stdexcept>
#include <limits>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstring>
#include <iostream>
void check(bool ok) { if (!ok) throw std::runtime_error("SKIN regression failed"); }
int main(int argc, char** argv)
{
  using namespace Noggit::Format;
  check((previewSkinFileIds({}, 0) == std::array<std::uint32_t, 2>{0, 0}));
  check((previewSkinFileIds({10, 11, 12, 13}, 4) == std::array<std::uint32_t, 2>{0, 0}));
  check((previewSkinFileIds({10, 11, 12, 13}, 99) == std::array<std::uint32_t, 2>{0, 0}));
  check((previewSkinFileIds({10, 11, 12, 13}, 0) == std::array<std::uint32_t, 2>{0, 0}));
  check((previewSkinFileIds({10, 11, 12, 13, 20, 21}, 4) == std::array<std::uint32_t, 2>{20, 21}));
  check((previewSkinFileIds({10, 20}, 1) == std::array<std::uint32_t, 2>{20, 20}));
  check((previewSkinFileIds({10, 0, 21}, 1) == std::array<std::uint32_t, 2>{0, 21}));
  check(skinIndexStart(12, 0, true) == 12);
  check(skinIndexStart(12, 1, true) == 65548);
  check(skinIndexStart(65535, 65535, true) == std::numeric_limits<std::uint32_t>::max());
  check(skinIndexStart(12, 1, false) == 12);
  validateSkinRange(65548, 6, 65554);
  validateSkinRange(0, 0, 0);
  validateSkinRange(65554, 0, 65554);
  for (auto test : {0, 1, 2})
  {
    bool failed = false;
    try
    {
      if (test == 0) validateSkinRange(65548, 7, 65554);
      if (test == 1) validateSkinRange(65555, 0, 65554);
      if (test == 2) validateSkinRange(0xffffffffu, 1, 0xffffffffu);
    }
    catch (std::runtime_error const&) { failed = true; }
    check(failed);
  }
  if (argc == 2)
  {
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> b{std::istreambuf_iterator<char>(input), {}};
    check(b.size() >= 48 && !std::memcmp(b.data(), "SKIN", 4));
    auto u32 = [&](std::size_t off) { std::uint32_t n; check(off + 4 <= b.size()); std::memcpy(&n,b.data()+off,4); return n; };
    auto u16 = [&](std::size_t off) { std::uint16_t n; check(off + 2 <= b.size()); std::memcpy(&n,b.data()+off,2); return n; };
    unsigned wide = 0;
    for (std::uint32_t i = 0; i < u32(28); ++i)
    {
      auto off = std::size_t(u32(32)) + i * 48;
      auto start = skinIndexStart(u16(off+8), u16(off+2), true);
      validateSkinRange(start, u16(off+10), u32(12));
      validateSkinRange(u16(off+4),u16(off+6),u32(4));
      wide += u16(off+2) != 0;
    }
    std::cout << "SKIN sections=" << u32(28) << ", wide starts=" << wide << '\n';
  }

}
