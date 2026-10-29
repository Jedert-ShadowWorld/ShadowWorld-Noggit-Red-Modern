// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
namespace Noggit::Formats
{
  // PNG2BLP writes opaque alpha metadata even for BC2/BC3 payloads.
  // Retail selects the block decoder using alpha depth as well as encoding.
  inline void finalizeRetailMapTexture(void* data, std::size_t size)
  {
    auto* b = static_cast<unsigned char*>(data);
    auto read = [&](std::size_t p) { std::uint32_t n; std::memcpy(&n,b+p,4); return n; };
    if (size < 148 || std::memcmp(b,"BLP2",4) || read(4)!=1 || b[8]!=2
        || (b[10]!=0 && b[10]!=1 && b[10]!=7) || !read(12) || !read(16))
      throw std::runtime_error("Invalid Retail maptexture BLP header");
    auto w=read(12), h=read(16);
    unsigned levels=0; bool ended=false;
    for (unsigned i=0;i<16;++i)
    {
      auto offset=read(20+4*i), length=read(84+4*i);
      if (!offset && !length) { ended=true; continue; }
      auto expected=((std::uint64_t(w)+3)/4)*((std::uint64_t(h)+3)/4)*(b[10]==0 ? 8:16);
      if (ended || offset<148 || offset>size || length>size-offset || length!=expected)
        throw std::runtime_error("Invalid Retail maptexture mip range");
      ++levels;
      w=w>1?w/2:1; h=h>1?h/2:1;
    }
    if (!levels) throw std::runtime_error("Retail maptexture has no image data");
    b[9]=b[10]==0?0:8;
    b[11]=levels>1?1:0;
  }
}
