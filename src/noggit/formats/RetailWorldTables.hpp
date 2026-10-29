// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace Noggit::Formats::RetailWorldTables
{
  using Bytes = std::vector<char>;
  struct Chunk { std::string tag; std::size_t header, payload, size; };
  inline std::uint32_t read(Bytes const& b, std::size_t p)
  {
    if (p > b.size() || b.size() - p < 4) throw std::runtime_error("Truncated world table");
    std::uint32_t n; std::memcpy(&n, b.data() + p, 4); return n;
  }
  inline std::vector<Chunk> chunks(Bytes const& b)
  {
    std::vector<Chunk> result;
    for (std::size_t p = 0; p < b.size();)
    {
      if (b.size() - p < 8) throw std::runtime_error("Truncated world table chunk");
      auto n = read(b, p + 4);
      if (n > b.size() - p - 8) throw std::runtime_error("World table chunk exceeds file");
      result.push_back({std::string{b[p+3], b[p+2], b[p+1], b[p]}, p, p+8, n});
      p += 8 + n;
    }
    return result;
  }
  inline Chunk required(std::vector<Chunk> const& list, std::string const& tag, std::size_t size)
  {
    Chunk result{}; unsigned count = 0;
    for (auto const& c : list) if (c.tag == tag) { result = c; ++count; }
    if (count != 1 || result.size != size) throw std::runtime_error("Invalid world table " + tag);
    return result;
  }
  inline Bytes updateWdt(Bytes original, std::uint32_t flags, std::vector<std::uint32_t> const& tiles)
  {
    auto list = chunks(original);
    auto version = required(list, "MVER", 4);
    if (read(original, version.payload) != 18 || tiles.size() != 4096)
      throw std::runtime_error("Unsupported Retail WDT");
    auto mphd = required(list, "MPHD", 32);
    auto main = required(list, "MAIN", 32768);
    // Only MPHD flags 0x1-0x8 and MAIN bit 0 are written; other flags, MPHD file
    // references, MAID and unknown chunks are preserved.
    flags = (read(original, mphd.payload) & ~0xFu) | (flags & 0xFu);
    std::memcpy(original.data() + mphd.payload, &flags, 4);
    for (std::size_t i = 0; i < tiles.size(); ++i)
    {
      auto value = (read(original, main.payload + i*8) & ~1u) | (tiles[i] & 1u);
      std::memcpy(original.data() + main.payload + i*8, &value, 4);
    }
    return original;
  }
  inline std::map<unsigned, std::size_t> wdlOffsets(Bytes const& bytes)
  {
    auto list = chunks(bytes);
    auto version = required(list, "MVER", 4);
    if (read(bytes, version.payload) != 18) throw std::runtime_error("Unsupported Retail WDL");
    auto maof = required(list, "MAOF", 16384);
    std::map<std::size_t, Chunk> byOffset;
    for (auto const& c : list) byOffset.emplace(c.header, c);
    std::map<unsigned, std::size_t> result;
    std::set<std::size_t> used;
    for (unsigned i = 0; i < 4096; ++i)
    {
      auto p = read(bytes, maof.payload + i*4);
      if (!p) continue;
      auto c = byOffset.find(p);
      if (c == byOffset.end() || c->second.tag != "MARE" || c->second.size != 1090 || !used.insert(p).second)
        throw std::runtime_error("Invalid or shared Retail WDL MARE offset");
      result.emplace(i, c->second.payload);
    }
    return result;
  }
  inline void updateWdlTile(Bytes& bytes, std::map<unsigned, std::size_t> const& offsets,
                            unsigned tile, void const* outer, void const* inner)
  {
    // Writes MARE heights only; MAHO, MAOE and other chunks keep their bytes.
    auto it = offsets.find(tile);
    if (it == offsets.end()) throw std::runtime_error("Retail WDL template has no tile; topology rebuild is unsupported");
    std::memcpy(bytes.data() + it->second, outer, 578);
    std::memcpy(bytes.data() + it->second + 578, inner, 512);
  }
}
