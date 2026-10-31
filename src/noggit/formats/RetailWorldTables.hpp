// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
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
  inline std::vector<Chunk> chunks(Bytes const& b, std::size_t begin = 0,
                                  std::size_t end = std::string::npos)
  {
    if (end == std::string::npos) end = b.size();
    if (begin > end || end > b.size()) throw std::runtime_error("Invalid world table chunk range");
    std::vector<Chunk> result;
    for (std::size_t p = begin; p < end;)
    {
      if (end - p < 8) throw std::runtime_error("Truncated world table chunk");
      auto n = read(b, p + 4);
      if (n > end - p - 8) throw std::runtime_error("World table chunk exceeds file");
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
  // Older path-addressed split maps have no terrain-LOD asset at all. Only
  // recognize their conservative legacy header; missing modern assets must
  // continue to fail rather than being reported as successfully rebuilt.
  inline bool legacyMapWithoutTerrainLod(Bytes const& wdt)
  {
    auto list = chunks(wdt);
    auto version = required(list, "MVER", 4);
    auto header = required(list, "MPHD", 32);
    required(list, "MAIN", 32768);
    return read(wdt, version.payload) == 18
      && (read(wdt, header.payload) & ~0xfu) == 0
      && std::none_of(list.begin(), list.end(), [](auto const& c) { return c.tag == "MAID"; });
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
  using WdlHoles = std::array<std::uint16_t, 16>;
  struct WdlTileOffsets
  {
    std::size_t heights;
    std::optional<std::size_t> holes;
  };
  inline std::int16_t wdlHeight(float height)
  {
    // Check before conversion: an out-of-range floating-point to integer cast
    // is undefined. Keep the existing truncation for representable heights.
    if (!std::isfinite(height) || height < std::numeric_limits<std::int16_t>::min()
        || height > std::numeric_limits<std::int16_t>::max())
      throw std::runtime_error("WDL height is not finite or exceeds the signed 16-bit range");
    return static_cast<std::int16_t>(height);
  }
  inline std::map<unsigned, std::size_t> occlusionOffsets(Bytes const& bytes)
  {
    auto list = chunks(bytes);
    auto version = required(list, "MVER", 4);
    if (read(bytes, version.payload) != 18) throw std::runtime_error("Unsupported Retail occlusion table");
    std::optional<Chunk> index, heights;
    for (auto const& c : list)
      if (c.tag == "MAOI" || c.tag == "MAOH")
      {
        auto& target = c.tag == "MAOI" ? index : heights;
        if (target) throw std::runtime_error("Duplicate occlusion chunk");
        target = c;
      }
    if (!index || !heights || index->size % 12)
      throw std::runtime_error("Invalid occlusion index");
    std::map<unsigned, std::size_t> result;
    std::set<std::size_t> ranges;
    for (std::size_t p = index->payload; p < index->payload + index->size; p += 12)
    {
      auto xy = read(bytes, p), x = xy & 0xffffu, y = xy >> 16;
      auto offset = read(bytes, p + 4), size = read(bytes, p + 8);
      // Retail CASC uses only the 17x17 outer grid here, unlike WDL MARE.
      if (x >= 64 || y >= 64 || size != 578 || offset % 2
          || offset > heights->size || size > heights->size - offset
          || !result.emplace(y * 64 + x, heights->payload + offset).second)
        throw std::runtime_error("Unsupported or invalid occlusion tile");
      auto next = ranges.lower_bound(offset);
      if ((next != ranges.end() && *next < offset + size)
          || (next != ranges.begin() && *std::prev(next) + size > offset))
        throw std::runtime_error("Shared occlusion height range");
      ranges.insert(offset);
    }
    return result;
  }
  inline std::size_t lowerOcclusionTile(Bytes& bytes, std::map<unsigned, std::size_t> const& offsets,
                                        unsigned tile, Bytes const& root)
  {
    auto target = offsets.find(tile);
    if (target == offsets.end()) return 0; // A tile without an occluder needs none added.
    if (target->second > bytes.size() || bytes.size() - target->second < 578)
      throw std::runtime_error("Occlusion tile exceeds file");
    auto list = chunks(root);
    auto version = required(list, "MVER", 4);
    if (read(root, version.payload) != 18) throw std::runtime_error("Unsupported occlusion ROOT");
    required(list, "MHDR", 64);
    std::array<std::int16_t, 256> minima{};
    std::set<unsigned> seen;
    auto height = [&](std::size_t p) {
      auto bits = read(root, p); float value; std::memcpy(&value, &bits, 4);
      if (!std::isfinite(value)) throw std::runtime_error("Non-finite occlusion ROOT height");
      return value;
    };
    for (auto const& c : list)
    {
      if (c.tag != "MCNK") continue;
      if (c.size < 128) throw std::runtime_error("Truncated occlusion ROOT MCNK");
      auto x = read(root, c.payload + 4), y = read(root, c.payload + 8);
      if (x >= 16 || y >= 16 || !seen.insert(y * 16 + x).second)
        throw std::runtime_error("Invalid occlusion ROOT grid");
      auto mcvt = required(chunks(root, c.payload + 128, c.payload + c.size), "MCVT", 580);
      auto base = height(c.payload + 112);
      float minimum = std::numeric_limits<float>::infinity();
      for (unsigned v = 0; v < 145; ++v)
      {
        auto value = height(mcvt.payload + v * 4) + base;
        if (!std::isfinite(value)) throw std::runtime_error("Occlusion ROOT height overflow");
        minimum = std::min(minimum, value);
      }
      auto lower = wdlHeight(std::floor(minimum));
      bool hole = read(root, c.payload) & 0x10000u
        ? (read(root, c.payload + 20) | read(root, c.payload + 24)) != 0
        : (read(root, c.payload + 60) & 0xffffu) != 0;
      // A coarse quad cannot represent a hole; keep its occluder below the world.
      minima[y * 16 + x] = hole ? std::numeric_limits<std::int16_t>::min() : lower;
    }
    if (seen.size() != 256) throw std::runtime_error("Incomplete occlusion ROOT grid");
    std::size_t changed = 0;
    for (int y = 0; y <= 16; ++y)
      for (int x = 0; x <= 16; ++x)
      {
        auto p = target->second + (y * 17 + x) * 2;
        std::int16_t value; std::memcpy(&value, bytes.data() + p, 2);
        auto original = value;
        // All four vertices of each quad are <= its lowest terrain vertex.
        // Thus either diagonal stays below the entire piecewise-linear surface.
        for (int cy = std::max(0, y - 1); cy <= std::min(15, y); ++cy)
          for (int cx = std::max(0, x - 1); cx <= std::min(15, x); ++cx)
            value = std::min(value, minima[cy * 16 + cx]);
        if (value != original) { std::memcpy(bytes.data() + p, &value, 2); ++changed; }
      }
    return changed;
  }
  inline WdlHoles wdlHoles(Bytes const& root)
  {
    auto list = chunks(root);
    auto version = required(list, "MVER", 4);
    if (read(root, version.payload) != 18)
      throw std::runtime_error("Unsupported Retail ROOT for WDL holes");
    required(list, "MHDR", 64);
    WdlHoles result{};
    std::set<unsigned> seen;
    for (auto const& c : list)
    {
      if (c.tag != "MCNK") continue;
      if (c.size < 128) throw std::runtime_error("Truncated ROOT MCNK for WDL holes");
      auto x = read(root, c.payload + 4), y = read(root, c.payload + 8);
      if (x >= 16 || y >= 16 || !seen.insert(y * 16 + x).second)
        throw std::runtime_error("Invalid ROOT MCNK grid for WDL holes");
      // MAHO has one bit per whole MCNK, not per terrain cell. A partial
      // high-resolution hole must not remove the remaining solid terrain.
      bool empty = read(root, c.payload) & 0x10000u
        ? read(root, c.payload + 20) == 0xffffffffu && read(root, c.payload + 24) == 0xffffffffu
        : (read(root, c.payload + 60) & 0xffffu) == 0xffffu;
      if (empty) result[y] |= std::uint16_t(1u << x);
    }
    if (seen.size() != 256) throw std::runtime_error("Incomplete ROOT MCNK grid for WDL holes");
    return result;
  }
  inline std::map<unsigned, WdlTileOffsets> wdlOffsets(Bytes const& bytes)
  {
    auto list = chunks(bytes);
    auto version = required(list, "MVER", 4);
    if (read(bytes, version.payload) != 18) throw std::runtime_error("Unsupported Retail WDL");
    auto maof = required(list, "MAOF", 16384);
    std::map<std::size_t, Chunk> byOffset;
    for (auto const& c : list) byOffset.emplace(c.header, c);
    std::map<unsigned, WdlTileOffsets> result;
    std::set<std::size_t> used;
    for (unsigned i = 0; i < 4096; ++i)
    {
      auto p = read(bytes, maof.payload + i*4);
      if (!p) continue;
      auto c = byOffset.find(p);
      if (c == byOffset.end() || c->second.tag != "MARE" || c->second.size != 1090 || !used.insert(p).second)
        throw std::runtime_error("Invalid or shared Retail WDL MARE offset");
      std::optional<std::size_t> holes;
      auto next = byOffset.find(c->second.payload + c->second.size);
      if (next != byOffset.end() && next->second.tag == "MAHO")
      {
        if (next->second.size != 32) throw std::runtime_error("Invalid Retail WDL MAHO size");
        holes = next->second.payload;
        auto following = byOffset.find(next->second.payload + next->second.size);
        if (following != byOffset.end() && following->second.tag == "MAHO")
          throw std::runtime_error("Duplicate Retail WDL MAHO for one tile");
      }
      result.emplace(i, WdlTileOffsets{c->second.payload, holes});
    }
    return result;
  }
  inline void updateWdlTile(Bytes& bytes, std::map<unsigned, WdlTileOffsets> const& offsets,
                            unsigned tile, void const* outer, void const* inner, WdlHoles const& holes)
  {
    // Only the selected MARE and its adjacent MAHO may change. Do not insert
    // chunks here: MAOF and other offset-bearing data must retain their bytes.
    auto it = offsets.find(tile);
    if (it == offsets.end()) throw std::runtime_error("Retail WDL template has no tile; topology rebuild is unsupported");
    auto const& target = it->second;
    if (target.heights > bytes.size() || bytes.size() - target.heights < 1090
        || (target.holes && (*target.holes > bytes.size() || bytes.size() - *target.holes < 32)))
      throw std::runtime_error("Retail WDL tile offsets exceed file");
    if (!target.holes)
      for (auto row : holes)
        if (row) throw std::runtime_error("Retail WDL tile needs a missing MAHO chunk; WDL remains pending");
    std::memcpy(bytes.data() + target.heights, outer, 578);
    std::memcpy(bytes.data() + target.heights + 578, inner, 512);
    if (target.holes) std::memcpy(bytes.data() + *target.holes, holes.data(), 32);
  }
}
