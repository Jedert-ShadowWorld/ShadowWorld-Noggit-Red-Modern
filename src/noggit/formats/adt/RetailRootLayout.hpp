// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace Noggit::Formats::ADT
{
  // Takes an MCNK payload (including its 128-byte header).
  inline std::vector<std::uint8_t> finalizeRetailRootLayout(std::vector<std::uint8_t> const& input)
  {
    if (input.size()<128) throw std::runtime_error("Truncated Retail MCNK header");
    std::vector<std::uint8_t> output(input.begin(),input.begin()+128);
    std::map<std::string,std::pair<std::uint32_t,std::uint32_t>> parts;
    auto read=[](auto const& b,std::size_t p){std::uint32_t n;std::memcpy(&n,b.data()+p,4);return n;};
    for (std::size_t p=128;p<input.size();)
    {
      if (input.size()-p<8) throw std::runtime_error("Truncated Retail MCNK subchunk");
      auto size=read(input,p+4);
      if(size>input.size()-p-8) throw std::runtime_error("Retail MCNK subchunk exceeds bounds");
      std::string tag{char(input[p+3]),char(input[p+2]),char(input[p+1]),char(input[p])};
      auto start=output.size();
      output.insert(output.end(),input.begin()+p,input.begin()+p+8+size);
      if(tag=="MCNR")
      {
        if(size!=435 && size!=448) throw std::runtime_error("Unsupported Retail MCNR size");
        if(size==435)
        {
          output.resize(output.size()+13,0);
          std::uint32_t padded=448;std::memcpy(output.data()+start+4,&padded,4);
        }
      }
      if(!parts.emplace(tag,std::make_pair(static_cast<std::uint32_t>(start+8),size)).second)
        throw std::runtime_error("Duplicate Retail MCNK subchunk");
      p+=8+size;
    }
    auto put=[&](unsigned p,std::uint32_t n){std::memcpy(output.data()+p,&n,4);};
    auto offset=[&](char const* tag) {auto i=parts.find(tag);return i==parts.end()||!i->second.second?0u:i->second.first;};
    auto size=[&](char const* tag) {auto i=parts.find(tag);return i==parts.end()?0u:i->second.second;};
    if(size("MCVT")!=580 || !parts.count("MCNR")) throw std::runtime_error("Retail MCNK requires heights and normals");
    // With high-resolution holes (flag 0x10000), offsets 20 and 24 hold the hole mask.
    if(!(read(input,0)&0x10000)) {put(20,offset("MCVT"));put(24,offset("MCNR"));}
    put(44,offset("MCSH"));put(48,size("MCSH")?size("MCSH")+8:0);
    if(size("MCSE")%28) throw std::runtime_error("Invalid Retail sound emitter records");
    put(88,offset("MCSE"));put(92,size("MCSE")/28);
    put(116,offset("MCCV"));put(120,offset("MCLV"));
    return output;
  }
}
