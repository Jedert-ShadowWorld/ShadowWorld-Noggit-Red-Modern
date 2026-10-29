// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/formats/adt/TerrainLodUpdater.hpp>
#include <noggit/formats/adt/RetailLiquidLod.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Noggit::Formats::ADT
{
  namespace
  {
    using Bytes = std::vector<std::uint8_t>;
    using Heights = std::array<std::array<float, 145>, 256>;
    constexpr std::size_t grid_count = 129 * 129 + 128 * 128;

    struct Chunk { std::string name; std::size_t offset, size; };
    std::uint32_t u32(Bytes const& b, std::size_t p)
    {
      return std::uint32_t(b.at(p)) | (std::uint32_t(b.at(p + 1)) << 8)
        | (std::uint32_t(b.at(p + 2)) << 16) | (std::uint32_t(b.at(p + 3)) << 24);
    }
    float f32(Bytes const& b, std::size_t p)
    {
      auto bits = u32(b, p);
      float value;
      std::memcpy(&value, &bits, 4);
      if (!std::isfinite(value)) throw std::runtime_error("LOD/root contains a non-finite height.");
      return value;
    }
    void put_float(Bytes& b, std::size_t p, float value)
    {
      if (!std::isfinite(value)) throw std::runtime_error("Generated LOD height is not finite.");
      std::uint32_t bits;
      std::memcpy(&bits, &value, 4);
      for (int i = 0; i < 4; ++i) b.at(p + i) = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    std::vector<Chunk> chunks(Bytes const& b, std::size_t begin, std::size_t end)
    {
      if (begin > end || end > b.size()) throw std::runtime_error("Invalid ADT chunk range.");
      std::vector<Chunk> result;
      while (begin < end)
      {
        if (end - begin < 8) throw std::runtime_error("Truncated ADT chunk header.");
        auto size = u32(b, begin + 4);
        if (size > end - begin - 8) throw std::runtime_error("ADT chunk extends beyond file.");
        std::string name;
        for (int i = 3; i >= 0; --i) name += static_cast<char>(b[begin + i]);
        result.push_back({name, begin + 8, size});
        begin += 8 + size;
      }
      return result;
    }
    Chunk required(std::vector<Chunk> const& list, char const* name)
    {
      auto it = std::find_if(list.begin(), list.end(), [=](Chunk const& c) { return c.name == name; });
      if (it == list.end() || std::count_if(list.begin(), list.end(),
          [=](Chunk const& c) { return c.name == name; }) != 1)
        throw std::runtime_error(std::string("Expected exactly one ") + name + " chunk.");
      return *it;
    }
    Heights read_heights(Bytes const& bytes)
    {
      auto all = chunks(bytes, 0, bytes.size());
      auto version = required(all, "MVER");
      if (version.size != 4 || u32(bytes, version.offset) != 18 || required(all, "MHDR").size != 64)
        throw std::runtime_error("LOD update requires a version 18 root ADT with a complete MHDR.");
      Heights result{};
      std::size_t index = 0;
      for (auto const& c : all)
      {
        if (c.name != "MCNK") continue;
        if (index >= 256 || c.size < 128 || u32(bytes, c.offset + 4) != index % 16
            || u32(bytes, c.offset + 8) != index / 16)
          throw std::runtime_error("Invalid root MCNK grid or header.");
        auto const height = required(chunks(bytes, c.offset + 128, c.offset + c.size), "MCVT");
        if (height.size != 145 * 4) throw std::runtime_error("MCVT must contain 145 floats.");
        auto const base = f32(bytes, c.offset + 112);
        for (std::size_t v = 0; v < 145; ++v)
        {
          result[index][v] = f32(bytes, height.offset + v * 4) + base;
          if (!std::isfinite(result[index][v])) throw std::runtime_error("Root height overflow.");
        }
        ++index;
      }
      if (index != 256) throw std::runtime_error("Root ADT must contain exactly 256 MCNK chunks.");
      return result;
    }
    void unchanged_liquid(Bytes const& before, std::vector<Chunk> const& old_chunks,
                          Bytes const& after, std::vector<Chunk> const& new_chunks, char const* name)
    {
      std::vector<Chunk> a, b;
      for (auto const& c : old_chunks) if (c.name == name) a.push_back(c);
      for (auto const& c : new_chunks) if (c.name == name) b.push_back(c);
      if (a.size() > 1 || b.size() > 1)
        throw std::runtime_error(std::string("Duplicate liquid chunk: ") + name);
      if (a.size() != b.size() || (!a.empty() && (a[0].size != b[0].size
          || !std::equal(before.begin() + a[0].offset, before.begin() + a[0].offset + a[0].size,
                         after.begin() + b[0].offset))))
        throw std::runtime_error(std::string("LOD water rebuild is unsupported: ") + name + " changed.");
    }
    std::uint64_t hole_mask(Bytes const& bytes, Chunk const& c)
    {
      // MapChunkHeader: bit 16 selects the 8x8 mask stored in ofsHeight/ofsNormal.
      // The legacy low 16 bits of holes describe 4x4 blocks of 2x2 cells.
      if (u32(bytes, c.offset) & 0x10000u)
        return std::uint64_t(u32(bytes, c.offset + 20))
             | (std::uint64_t(u32(bytes, c.offset + 24)) << 32);
      auto const low = u32(bytes, c.offset + 60) & 0xffffu;
      std::uint64_t result = 0;
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
          if (low & (1u << (y * 4 + x)))
          {
            auto const first = y * 16 + x * 2;
            result |= (std::uint64_t{3} << first) | (std::uint64_t{3} << (first + 8));
          }
      return result;
    }
    void validate_preserved_topology(Bytes const& before, Bytes const& after)
    {
      auto old_chunks = chunks(before, 0, before.size()), new_chunks = chunks(after, 0, after.size());
      std::vector<Chunk> a, b;
      for (auto const& c : old_chunks) if (c.name == "MCNK") a.push_back(c);
      for (auto const& c : new_chunks) if (c.name == "MCNK") b.push_back(c);
      // read_heights has already validated both grids and their headers.
      for (std::size_t i = 0; i < a.size(); ++i)
      {
        if (hole_mask(after, b[i]) & ~hole_mask(before, a[i]))
          throw std::runtime_error("LOD topology rebuild is unsupported: new terrain holes require cutting the existing mesh.");
        // With MH2O the legacy MCNK liquid flags do not describe its layers.
        unchanged_liquid(before, chunks(before, a[i].offset + 128, a[i].offset + a[i].size),
          after, chunks(after, b[i].offset + 128, b[i].offset + b[i].size), "MCLQ");
      }
    }
    std::vector<float> build(Heights const& heights, std::vector<std::uint16_t> const& skirts,
                             std::size_t count, int layout)
    {
      std::vector<float> result;
      result.reserve(count);
      // The samples coincide exactly with ADT vertices. Integer grid indexing
      // avoids floating-point division at chunk boundaries.
      for (int centers = 0; centers < 2; ++centers)
      {
        auto const side = centers ? 128 : 129;
        for (int outer = 0; outer < side; ++outer)
          for (int inner = 0; inner < side; ++inner)
          {
            int x = layout & 2 ? outer : inner;
            int y = layout & 2 ? inner : outer;
            if (layout & 1) { x = side - 1 - x; y = side - 1 - y; }
            int cx = std::min(x / 8, 15), cy = std::min(y / 8, 15);
            int lx = x - cx * 8, ly = y - cy * 8;
            result.push_back(heights[cy * 16 + cx][17 * ly + (centers ? 9 : 0) + lx]);
          }
      }
      for (std::size_t i = 0; i < count - grid_count; ++i)
      {
        auto h = result[skirts[i]] - 32.0f;
        if (!std::isfinite(h)) throw std::runtime_error("LOD skirt height overflow.");
        result.push_back(h);
      }
      return result;
    }

    using Holes = std::array<bool, 128 * 128>;
    Holes read_holes(Bytes const& root, int layout)
    {
      Holes result{};
      for (auto const& c : chunks(root, 0, root.size()))
      {
        if (c.name != "MCNK") continue;
        auto mask = hole_mask(root, c);
        int cx = u32(root, c.offset + 4), cy = u32(root, c.offset + 8);
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; ++x)
          {
            int gx = cx * 8 + x, gy = cy * 8 + y;
            if (layout & 1) { gx = 127 - gx; gy = 127 - gy; }
            if (layout & 2) std::swap(gx, gy);
            result[gy * 128 + gx] = (mask & (std::uint64_t{1} << (y * 8 + x))) != 0;
          }
      }
      return result;
    }

    // MLVI contains clockwise triangle lists. MLND indexes the four levels of
    // the quadtree; MLLL indexes each level's terrain and separate skirt lists.
    // Only uncovered unit cells which are solid in ROOT are added. Existing
    // terrain, active holes and skirt geometry retain their original vertices.
    std::size_t close_lod_holes(Bytes& lod, Holes const& holes, bool masks_changed)
    {
      auto all = chunks(lod, 0, lod.size());
      bool const has_mesh = std::any_of(all.begin(), all.end(), [](auto const& c) {
        return c.name == "MLND" || c.name == "MLVI" || c.name == "MLLL";
      });
      if (!has_mesh)
      {
        if (masks_changed) throw std::runtime_error("Cannot close LOD holes without MLND/MLVI/MLLL.");
        return 0;
      }
      auto nc = required(all, "MLND"), ic = required(all, "MLVI"), lc = required(all, "MLLL");
      auto hc = required(all, "MLVH");
      if (nc.size != 85 * 24 || lc.size != 4 * 20 || ic.size % 2)
        throw std::runtime_error("Invalid Retail LOD quadtree or triangle list size.");
      auto get16 = [&](std::size_t p) { return std::uint16_t(lod.at(p) | (lod.at(p + 1) << 8)); };
      using Triangle = std::array<std::uint16_t, 3>;
      using Edge = std::pair<std::uint16_t, std::uint16_t>;
      struct Point { int x, y; }; // Half-cell units, including MLVH center vertices.
      auto point = [](std::uint16_t v) -> Point {
        if (v >= grid_count) throw std::runtime_error("Skirt vertex in LOD terrain triangles.");
        if (v < 129 * 129) return {2 * (v % 129), 2 * (v / 129)};
        v -= 129 * 129;
        return {2 * (v % 128) + 1, 2 * (v / 128) + 1};
      };
      auto cross = [](Point a, Point b, Point p) {
        return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
      };
      auto vertex = [](int x, int y) { return static_cast<std::uint16_t>(y * 129 + x); };
      // Preserve collinear boundary vertices when splitting an existing triangle.
      auto triangulate = [&](std::vector<std::uint16_t> polygon, std::vector<std::uint16_t>& out) {
        while (polygon.size() > 3)
        {
          bool clipped = false;
          for (std::size_t i = 0; i < polygon.size(); ++i)
          {
            auto prev = (i+polygon.size()-1)%polygon.size(), next = (i+1)%polygon.size();
            auto a = point(polygon[prev]), b = point(polygon[i]), c = point(polygon[next]);
            if (cross(a,b,c) >= 0) continue;
            bool occupied = false;
            for (std::size_t j = 0; j < polygon.size(); ++j)
              if (j != prev && j != i && j != next)
              {
                auto q = point(polygon[j]);
                occupied |= cross(a,b,q) <= 0 && cross(b,c,q) <= 0 && cross(c,a,q) <= 0;
              }
            if (occupied) continue;
            out.insert(out.end(), {polygon[prev], polygon[i], polygon[next]});
            polygon.erase(polygon.begin()+i);
            clipped = true;
            break;
          }
          if (!clipped) throw std::runtime_error("Cannot triangulate repaired LOD boundary.");
        }
        out.insert(out.end(), polygon.begin(), polygon.end());
      };
      struct Node { int x = 0, y = 0, side = 128, level = 0; bool assigned = false; };
      std::array<Node, 85> nodes{};
      nodes[0].assigned = true;
      std::array<std::vector<std::uint16_t>, 85> repaired;
      std::array<std::size_t, 4> ends{};
      std::array<std::vector<std::uint16_t>, 4> skirts;
      std::array<std::set<Edge>, 4> new_border_edges;
      std::size_t cursor = 0;
      for (int level = 0; level < 4; ++level)
      {
        auto p = lc.offset + level * 20;
        auto start = u32(lod, p + 4), length = u32(lod, p + 8);
        auto skirt_count = u32(lod, p + 12), skirt_start = u32(lod, p + 16);
        if (start != cursor || length % 3 || skirt_count % 3
            || length > ic.size / 2 - cursor)
          throw std::runtime_error("Invalid MLLL terrain range.");
        cursor += length;
        ends[level] = cursor;
        if ((skirt_count && skirt_start != cursor) || skirt_count > ic.size / 2 - cursor)
          throw std::runtime_error("Invalid MLLL skirt range.");
        for (std::size_t i = 0; i < skirt_count; ++i)
        {
          auto v = get16(ic.offset + (cursor + i) * 2);
          if (v >= hc.size / 4) throw std::runtime_error("MLLL skirt index outside MLVH.");
          skirts[level].push_back(v);
        }
        cursor += skirt_count;
      }
      if (cursor != ic.size / 2) throw std::runtime_error("Unreferenced MLVI indices.");
      cursor = 0;
      std::size_t filled = 0;
      Holes added_anywhere{};
      for (std::size_t ni = 0; ni < nodes.size(); ++ni)
      {
        auto n = nodes[ni];
        if (!n.assigned) throw std::runtime_error("Unreachable MLND node.");
        auto p = nc.offset + ni * 24;
        auto start = u32(lod, p), length = u32(lod, p + 4);
        if (start != cursor || cursor > ends[n.level] || length > ends[n.level] - cursor
            || length % 3 || u32(lod, p + 8) || u32(lod, p + 12))
          throw std::runtime_error("Invalid or unsupported MLND terrain range.");
        for (int child = 0; child < 4; ++child)
        {
          auto id = get16(p + 16 + child * 2);
          if (n.level == 3)
          {
            if (id != 0xffff) throw std::runtime_error("Invalid MLND leaf.");
          }
          else
          {
            // Retail stores the complete tree breadth-first.
            if (id != ni * 4 + 1 + child || id >= nodes.size() || nodes[id].assigned)
              throw std::runtime_error("Invalid MLND child.");
            nodes[id] = {n.x + (child % 2) * n.side / 2, n.y + (child / 2) * n.side / 2,
                         n.side / 2, n.level + 1, true};
          }
        }
        std::vector<Triangle> triangles;
        std::map<Edge, unsigned> edge_counts;
        Holes covered{}, added{};
        for (std::size_t i = 0; i < length; i += 3)
        {
          Triangle t{get16(ic.offset + (start+i)*2), get16(ic.offset + (start+i+1)*2),
                     get16(ic.offset + (start+i+2)*2)};
          auto a = point(t[0]), b = point(t[1]), c = point(t[2]);
          for (auto v : {a,b,c})
            if (v.x < n.x*2 || v.x > (n.x+n.side)*2 || v.y < n.y*2 || v.y > (n.y+n.side)*2)
              throw std::runtime_error("MLND triangle outside its quadtree region.");
          if (cross(a,b,c) >= 0) throw std::runtime_error("Invalid MLVI triangle winding or degenerate triangle.");
          triangles.push_back(t);
          for (int e = 0; e < 3; ++e)
          {
            Edge edge = std::minmax(t[e], t[(e+1)%3]);
            if (++edge_counts[edge] > 2) throw std::runtime_error("Non-manifold LOD terrain edge.");
          }
          for (int y = std::min({a.y,b.y,c.y})/2; y < (std::max({a.y,b.y,c.y})+1)/2; ++y)
            for (int x = std::min({a.x,b.x,c.x})/2; x < (std::max({a.x,b.x,c.x})+1)/2; ++x)
            {
              Point q{x*2+1,y*2+1};
              if (cross(a,b,q) <= 0 && cross(b,c,q) <= 0 && cross(c,a,q) <= 0)
                covered[y*128+x] = true;
            }
        }
        for (int y = n.y; y < n.y+n.side; ++y)
          for (int x = n.x; x < n.x+n.side; ++x)
            if (!covered[y*128+x] && !holes[y*128+x])
            {
              added[y*128+x] = added_anywhere[y*128+x] = true;
              ++filled;
            }
        bool const changed = std::any_of(added.begin(), added.end(), [](bool v) { return v; });
        std::map<Edge, std::vector<std::uint16_t>> splits;
        if (changed)
          for (auto const& [edge, uses] : edge_counts)
          {
            if (uses != 1) continue;
            auto a = point(edge.first), b = point(edge.second);
            bool perimeter = (a.x == b.x && (a.x == 2*n.x || a.x == 2*(n.x+n.side)))
                          || (a.y == b.y && (a.y == 2*n.y || a.y == 2*(n.y+n.side)));
            if (perimeter) continue;
            // Cells are only added along cell-aligned boundaries.
            if ((a.x != b.x && a.y != b.y) || ((a.x|a.y|b.x|b.y)&1))
              throw std::runtime_error("Cannot close LOD holes with a non-cell-aligned boundary.");
            bool touches = false;
            for (int y = std::max(n.y, std::min(a.y,b.y)/2-1); y <= std::min(n.y+n.side-1, std::max(a.y,b.y)/2); ++y)
              for (int x = std::max(n.x, std::min(a.x,b.x)/2-1); x <= std::min(n.x+n.side-1, std::max(a.x,b.x)/2); ++x)
                touches |= added[y*128+x];
            if (!touches) continue;
            auto& vertices = splits[edge];
            int steps = (std::abs(b.x-a.x)+std::abs(b.y-a.y))/2;
            for (int step = 1; step < steps; ++step)
              vertices.push_back(vertex((a.x+(b.x-a.x)*step/steps)/2,
                                        (a.y+(b.y-a.y)*step/steps)/2));
          }
        auto& out = repaired[ni];
        for (auto const& t : triangles)
        {
          std::vector<std::uint16_t> polygon;
          for (int e = 0; e < 3; ++e)
          {
            auto a = t[e], b = t[(e+1)%3];
            polygon.push_back(a);
            auto it = splits.find(std::minmax(a,b));
            if (it == splits.end()) continue;
            if (a < b) polygon.insert(polygon.end(), it->second.begin(), it->second.end());
            else polygon.insert(polygon.end(), it->second.rbegin(), it->second.rend());
          }
          triangulate(std::move(polygon), out);
        }
        for (int y = n.y; y < n.y+n.side; ++y)
          for (int x = n.x; x < n.x+n.side; ++x)
            if (added[y*128+x])
            {
              auto c = static_cast<std::uint16_t>(129*129 + y*128+x);
              auto a = vertex(x,y), b = vertex(x+1,y), d = vertex(x,y+1), e = vertex(x+1,y+1);
              out.insert(out.end(), {c,b,a, c,e,b, c,d,e, c,a,d});
              if (x == 0) new_border_edges[n.level].emplace(a,d);
              if (x == 127) new_border_edges[n.level].emplace(e,b);
              if (y == 0) new_border_edges[n.level].emplace(b,a);
              if (y == 127) new_border_edges[n.level].emplace(d,e);
            }
        cursor += length;
        if (ni == 0 || ni == 4 || ni == 20 || ni == 84)
        {
          if (cursor != ends[n.level]) throw std::runtime_error("MLND does not cover its MLLL level.");
          cursor += skirts[n.level].size();
        }
      }
      if (!filled) return 0;
      // Split node-boundary edges next to added cells in every node to avoid
      // T-junctions; all levels use the same samples.
      for (std::size_t ni = 0; ni < nodes.size(); ++ni)
      {
        auto n = nodes[ni];
        std::vector<std::uint16_t> joined;
        auto const& input = repaired[ni];
        for (std::size_t i = 0; i < input.size(); i += 3)
        {
          std::vector<std::uint16_t> polygon;
          for (int e = 0; e < 3; ++e)
          {
            auto av = input[i+e], bv = input[i+(e+1)%3];
            auto a = point(av), b = point(bv);
            polygon.push_back(av);
            bool const vertical = a.x == b.x && (a.x == 2*n.x || a.x == 2*(n.x+n.side));
            bool const horizontal = a.y == b.y && (a.y == 2*n.y || a.y == 2*(n.y+n.side));
            if ((!vertical && !horizontal) || ((a.x|a.y|b.x|b.y)&1)) continue;
            int const steps = (std::abs(b.x-a.x)+std::abs(b.y-a.y))/2;
            if (steps <= 1) continue;
            bool touches = false;
            for (int y = std::max(0, std::min(a.y,b.y)/2-1); y <= std::min(127, std::max(a.y,b.y)/2); ++y)
              for (int x = std::max(0, std::min(a.x,b.x)/2-1); x <= std::min(127, std::max(a.x,b.x)/2); ++x)
                touches |= added_anywhere[y*128+x];
            if (!touches) continue;
            for (int step = 1; step < steps; ++step)
              polygon.push_back(vertex((a.x+(b.x-a.x)*step/steps)/2,
                                       (a.y+(b.y-a.y)*step/steps)/2));
          }
          triangulate(std::move(polygon), joined);
        }
        repaired[ni] = std::move(joined);
      }
      auto put32 = [](Bytes& b, std::size_t p, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) b.at(p+i) = static_cast<std::uint8_t>(value >> (8*i));
      };
      auto sc = required(all, "MLSI");
      Bytes extra_heights, extra_skirts;
      std::map<std::uint16_t, std::uint16_t> skirt_vertices;
      for (std::size_t i = 0; i < hc.size/4-grid_count; ++i)
        skirt_vertices.emplace(get16(sc.offset+i*2), static_cast<std::uint16_t>(grid_count+i));
      auto skirt_vertex = [&](std::uint16_t base) {
        auto it = skirt_vertices.find(base);
        if (it != skirt_vertices.end()) return it->second;
        auto index = hc.size/4+extra_heights.size()/4;
        if (index > 0xffff || sc.size/2 != hc.size/4-grid_count)
          throw std::runtime_error("Cannot extend LOD skirt vertex table.");
        auto id = static_cast<std::uint16_t>(index);
        extra_skirts.push_back(base&255); extra_skirts.push_back(base>>8);
        auto p = extra_heights.size();
        extra_heights.resize(p+4);
        put_float(extra_heights,p,f32(lod,hc.offset+base*4)-32.0f);
        skirt_vertices.emplace(base,id);
        return id;
      };
      for (int level = 0; level < 4; ++level)
      {
        // The finest level has no skirts.
        if (skirts[level].empty()) continue;
        std::vector<Edge> covered;
        for (std::size_t i = 0; i < skirts[level].size(); i += 3)
        {
          std::vector<std::uint16_t> top;
          for (int j = 0; j < 3; ++j)
            if (skirts[level][i+j] < grid_count) top.push_back(skirts[level][i+j]);
          if (top.size() == 2) covered.emplace_back(top[0],top[1]);
        }
        for (auto const& [a,b] : new_border_edges[level])
        {
          auto pa = point(a), pb = point(b);
          bool present = std::any_of(covered.begin(),covered.end(),[&](Edge const& e) {
            auto u = point(e.first), v = point(e.second);
            return cross(u,v,pa) == 0 && cross(u,v,pb) == 0
              && pa.x >= std::min(u.x,v.x) && pa.x <= std::max(u.x,v.x)
              && pb.x >= std::min(u.x,v.x) && pb.x <= std::max(u.x,v.x)
              && pa.y >= std::min(u.y,v.y) && pa.y <= std::max(u.y,v.y)
              && pb.y >= std::min(u.y,v.y) && pb.y <= std::max(u.y,v.y);
          });
          if (present) continue;
          auto sa = skirt_vertex(a), sb = skirt_vertex(b);
          skirts[level].insert(skirts[level].end(), {a,sa,b, b,sa,sb});
        }
      }
      Bytes indices;
      auto emit = [&](std::vector<std::uint16_t> const& vertices) {
        for (auto v : vertices) { indices.push_back(v & 255); indices.push_back(v >> 8); }
      };
      std::size_t ni = 0;
      for (int level = 0; level < 4; ++level)
      {
        auto p = lc.offset + level*20;
        auto first = indices.size()/2;
        put32(lod,p+4,static_cast<std::uint32_t>(first));
        while (ni < nodes.size() && nodes[ni].level == level)
        {
          put32(lod,nc.offset+ni*24,static_cast<std::uint32_t>(indices.size()/2));
          put32(lod,nc.offset+ni*24+4,static_cast<std::uint32_t>(repaired[ni].size()));
          emit(repaired[ni++]);
        }
        put32(lod,p+8,static_cast<std::uint32_t>(indices.size()/2-first));
        put32(lod,p+12,static_cast<std::uint32_t>(skirts[level].size()));
        if (!skirts[level].empty()) put32(lod,p+16,static_cast<std::uint32_t>(indices.size()/2));
        emit(skirts[level]);
      }
      Bytes rebuilt;
      for (auto const& c : all)
      {
        rebuilt.insert(rebuilt.end(),lod.begin()+c.offset-8,lod.begin()+c.offset);
        auto size_pos = rebuilt.size()-4, begin = rebuilt.size();
        if (c.name == "MLVI") rebuilt.insert(rebuilt.end(),indices.begin(),indices.end());
        else
        {
          rebuilt.insert(rebuilt.end(),lod.begin()+c.offset,lod.begin()+c.offset+c.size);
          if (c.name == "MLVH") rebuilt.insert(rebuilt.end(),extra_heights.begin(),extra_heights.end());
          if (c.name == "MLSI") rebuilt.insert(rebuilt.end(),extra_skirts.begin(),extra_skirts.end());
        }
        put32(rebuilt,size_pos,static_cast<std::uint32_t>(rebuilt.size()-begin));
      }
      lod = std::move(rebuilt);
      return filled;
    }
  }

  std::vector<std::uint8_t> TerrainLodUpdater::update(
    Bytes const& reference_root, Bytes const& original_lod, Bytes const& updated_root,
    std::string* diagnostics)
  {
    if (diagnostics) diagnostics->clear();
    auto all = chunks(original_lod, 0, original_lod.size());
    auto h = required(all, "MLVH"), header = required(all, "MLHD"), skirt = required(all, "MLSI");
    if (h.size % 4 || h.size < grid_count * 4 || header.size < 28 || skirt.size % 2)
      throw std::runtime_error("Invalid MLVH, MLHD or MLSI size.");
    auto const count = h.size / 4;
    if (count - grid_count > skirt.size / 2) throw std::runtime_error("MLSI does not cover the LOD skirts.");
    std::vector<std::uint16_t> skirts(count - grid_count);
    for (std::size_t i = 0; i < skirts.size(); ++i)
    {
      skirts[i] = original_lod[skirt.offset + i * 2] | (original_lod[skirt.offset + i * 2 + 1] << 8);
      if (skirts[i] >= grid_count) throw std::runtime_error("MLSI references a vertex outside the grid.");
    }
    std::vector<float> reference(count);
    for (std::size_t i = 0; i < count; ++i) reference[i] = f32(original_lod, h.offset + i * 4);
    auto before = read_heights(reference_root);
    auto after = read_heights(updated_root);
    validate_preserved_topology(reference_root, updated_root);
    struct Score { int layout; std::size_t matches; };
    std::array<Score, 4> scores{};
    std::ostringstream description;
    for (int layout = 0; layout < 4; ++layout)
    {
      auto candidate = build(before, skirts, count, layout);
      std::size_t matches = 0;
      for (std::size_t i = 0; i < count; ++i)
        if (std::abs(reference[i] - candidate[i]) <= 0.01f) ++matches;
      scores[layout] = {layout, matches};
      if (layout) description << ", ";
      description << (layout & 2 ? "XOuter" : "YOuter") << "/" << (layout & 1 ? "MaxCorner" : "ZeroCorner")
                  << "=" << matches << "/" << count << " (" << 100.0 * matches / count << "%)";
    }
    auto const details = description.str();
    if (diagnostics) *diagnostics = details;
    std::stable_sort(scores.begin(), scores.end(), [](Score a, Score b) { return a.matches > b.matches; });
    if (scores[0].matches < count * 0.5)
      throw std::runtime_error("LOD layout confidence below 50%: " + details);
    auto output = build(after, skirts, count, scores[0].layout);
    auto const holes = read_holes(updated_root, scores[0].layout);
    for (std::size_t i = 1; i < scores.size(); ++i)
    {
      if (scores[0].matches - scores[i].matches >= count * 0.05) continue;
      auto alternative = build(after, skirts, count, scores[i].layout);
      if (std::memcmp(output.data(), alternative.data(), count * sizeof(float)) != 0)
        throw std::runtime_error("Ambiguous LOD orientation produces different heights: " + details);
      if (holes != read_holes(updated_root, scores[i].layout))
        throw std::runtime_error("Ambiguous LOD orientation produces different hole masks: " + details);
    }
    auto result = original_lod;
    for (std::size_t i = 0; i < count; ++i) put_float(result, h.offset + i * 4, output[i]);
    auto filled = close_lod_holes(result, holes, holes != read_holes(reference_root, scores[0].layout));
    // MLHD terrain bounds include skirt vertices, including appended ones.
    auto const rebuilt_chunks = chunks(result, 0, result.size());
    auto const final_heights = required(rebuilt_chunks, "MLVH");
    auto const final_header = required(rebuilt_chunks, "MLHD");
    float minimum = f32(result, final_heights.offset), maximum = minimum;
    for (std::size_t i = 4; i < final_heights.size; i += 4)
    {
      auto const value = f32(result, final_heights.offset + i);
      minimum = std::min(minimum, value);
      maximum = std::max(maximum, value);
    }
    put_float(result, final_header.offset + 4, minimum);
    put_float(result, final_header.offset + 8, maximum);
    if (diagnostics) *diagnostics += "; closed LOD cells across four levels=" + std::to_string(filled);
    auto const liquid = RetailLiquid::mh2o(updated_root);
    (void)RetailLiquid::read(liquid);
    // Liquid LOD is rebuilt from the current MH2O even if it is unchanged.
    if (!liquid.empty() || std::any_of(all.begin(),all.end(),[](Chunk const& c) {
          return c.name == "MLLD" || c.name == "MLLN" || c.name == "MLLI" || c.name == "MLLV";
        }))
    {
      result = RetailLiquid::rebuild(updated_root, result, &reference_root);
      if (diagnostics) *diagnostics += "; liquid mesh/depth/visibility rebuilt";
    }
    return result;
  }
}
