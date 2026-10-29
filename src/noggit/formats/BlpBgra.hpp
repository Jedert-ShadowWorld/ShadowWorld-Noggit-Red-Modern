// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

namespace Noggit::Format
{
  struct BgraBlp
  {
    std::uint32_t width = 0, height = 0;
    std::map<int, std::vector<std::uint32_t>> mipmaps;
  };

  // BLP2 encoding 3 stores four BGRA bytes per pixel, including the alpha byte.
  inline BgraBlp read_bgra_blp(char const* bytes, std::size_t size)
  {
    auto const* data = reinterpret_cast<unsigned char const*>(bytes);
    auto word = [&](std::size_t offset)
    {
      if (offset > size || size - offset < 4) throw std::runtime_error("Truncated BGRA BLP header");
      return std::uint32_t(data[offset]) | (std::uint32_t(data[offset+1])<<8)
          | (std::uint32_t(data[offset+2])<<16) | (std::uint32_t(data[offset+3])<<24);
    };
    if (size < 148 || word(0) != 0x32504c42 || word(4) != 1 || data[8] != 3)
      throw std::runtime_error("Expected BLP2 encoding 3");
    BgraBlp result;
    result.width = word(12); result.height = word(16);
    if (!result.width || !result.height || result.width > 16384 || result.height > 16384)
      throw std::runtime_error("Invalid BGRA BLP dimensions");
    std::uint32_t width=result.width, height=result.height;
    std::vector<std::pair<std::size_t,std::size_t>> ranges;
    bool ended=false;
    for (int level=0;level<16;++level)
    {
      std::size_t const offset=word(20+level*4), length=word(84+level*4);
      if (!offset && !length) {ended=true;continue;}
      std::size_t const expected=std::size_t(width)*height*4;
      if (ended || offset<148 || length!=expected || offset>size || length>size-offset)
        throw std::runtime_error("Invalid BGRA BLP mip range");
      for (auto const& range:ranges)
        if (offset<range.second && range.first<offset+length)
          throw std::runtime_error("Overlapping BGRA BLP mip data");
      ranges.emplace_back(offset,offset+length);
      auto& rgba=result.mipmaps[level];rgba.resize(std::size_t(width)*height);
      for (std::size_t pixel=0;pixel<rgba.size();++pixel)
      {
        auto const* bgra=data+offset+pixel*4;
        rgba[pixel]=std::uint32_t(bgra[2]) | (std::uint32_t(bgra[1])<<8)
            | (std::uint32_t(bgra[0])<<16) | (std::uint32_t(bgra[3])<<24);
      }
      width=std::max(1u,width/2);height=std::max(1u,height/2);
    }
    if (result.mipmaps.empty()) throw std::runtime_error("BGRA BLP has no mip data");
    return result;
  }
}
