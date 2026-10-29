// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace Noggit::Formats::ADT::RetailLiquid
{
  using Bytes = std::vector<std::uint8_t>;
  // Filled from the Retail client DB2 before tiles load; the defaults cover the basic liquid types.
  inline std::map<unsigned, unsigned> vertex_formats{{1,0},{2,2},{3,1},{4,1},{5,0}};
  inline std::map<unsigned, unsigned> default_objects{{2,42}};
  inline unsigned format(unsigned type, unsigned object_or_format)
  {
    if (object_or_format < 42)
    {
      if (object_or_format > 5) throw std::runtime_error("Unsupported MH2O vertex format.");
      return object_or_format;
    }
    auto it = vertex_formats.find(type);
    if (it == vertex_formats.end())
      throw std::runtime_error("LiquidMaterial vertex format unavailable for liquid " + std::to_string(type));
    return it->second;
  }
  inline void range(Bytes const& b, std::size_t p, std::size_t n)
  {
    if (p > b.size() || n > b.size()-p) throw std::runtime_error("Truncated Retail liquid data.");
  }
  inline unsigned word(Bytes const& b, std::size_t p, unsigned n = 4)
  {
    range(b,p,n); unsigned v = 0;
    for (unsigned i = 0; i < n; ++i) v |= unsigned(b[p+i]) << (i*8);
    return v;
  }
  inline float real(Bytes const& b, std::size_t p)
  {
    auto bits = word(b,p); float v; std::memcpy(&v,&bits,4);
    if (!std::isfinite(v)) throw std::runtime_error("Non-finite Retail liquid value.");
    return v;
  }
  inline void put(Bytes& b, std::size_t p, unsigned v, unsigned n = 4)
  {
    range(b,p,n);
    for (unsigned i = 0; i < n; ++i) b[p+i] = static_cast<std::uint8_t>(v >> (i*8));
  }
  inline void putf(Bytes& b, std::size_t p, float v)
  {
    if (!std::isfinite(v)) throw std::runtime_error("Invalid generated liquid value.");
    unsigned bits; std::memcpy(&bits,&v,4); put(b,p,bits);
  }
  struct Chunk { std::string name; Bytes data; };
  inline std::vector<Chunk> chunks(Bytes const& b)
  {
    std::vector<Chunk> out;
    for (std::size_t p = 0; p < b.size();)
    {
      range(b,p,8); auto n = word(b,p+4); range(b,p+8,n);
      out.push_back({std::string{char(b[p+3]),char(b[p+2]),char(b[p+1]),char(b[p])},
                     Bytes(b.begin()+p+8,b.begin()+p+8+n)});
      p += 8+n;
    }
    return out;
  }
  inline void append(Bytes& b, std::string const& name, Bytes const& body)
  {
    for (int i = 3; i >= 0; --i) b.push_back(name[i]);
    auto p = b.size(); b.resize(p+4); put(b,p,static_cast<unsigned>(body.size()));
    b.insert(b.end(),body.begin(),body.end());
  }
  inline Bytes mh2o(Bytes const& root)
  {
    Bytes result; bool found = false;
    for (auto const& c : chunks(root)) if (c.name == "MH2O")
    {
      if (found) throw std::runtime_error("Duplicate MH2O.");
      if (c.data.size() < 256*12) throw std::runtime_error("Incomplete MH2O header table.");
      result = c.data; found = true;
    }
    return result;
  }
  struct Cell
  {
    unsigned x, y, type, object;
    std::array<float,4> heights; // top left, top right, bottom left, bottom right
    std::array<unsigned,4> depth;
  };
  inline std::vector<Cell> read(Bytes const& data)
  {
    std::vector<Cell> cells;
    if (data.empty()) return cells;
    range(data,0,256*12);
    for (unsigned c = 0; c < 256; ++c)
    {
      auto off = word(data,c*12), count = word(data,c*12+4), attr = word(data,c*12+8);
      if (attr) range(data,attr,16);
      if (!count) continue;
      if (count > 64 || off < 256*12) throw std::runtime_error("Invalid MH2O instance array.");
      range(data,off,count*24);
      for (unsigned l = 0; l < count; ++l)
      {
        auto p = off+l*24;
        auto type = word(data,p,2), object = word(data,p+2,2), vf = format(type,object);
        auto min = real(data,p+4), max = real(data,p+8);
        if (min > max) throw std::runtime_error("Inverted MH2O height bounds.");
        unsigned x = data[p+12], y = data[p+13], w = data[p+14], h = data[p+15];
        if (object >= 42) { x = y = 0; w = h = 8; }
        if (!w || !h || x+w > 8 || y+h > 8) throw std::runtime_error("Invalid MH2O rectangle.");
        auto mask = word(data,p+16), vertices = word(data,p+20), n = (w+1)*(h+1);
        if (mask) range(data,mask,(w*h+7)/8);
        constexpr unsigned stride[]{5,8,1,9,8,12};
        if (vertices) range(data,vertices,n*stride[vf]);
        auto height = [&](unsigned i) {
          if (!vertices) return vf == 2 ? 0.f : min;
          return vf == 2 ? 0.f : real(data,vertices+(vf >= 4 ? i*8+4 : i*4));
        };
        auto depth = [&](unsigned i) -> unsigned {
          if (!vertices || vf == 1 || vf == 5) return 255;
          if (vf == 4) return data[vertices+i*8];
          return data[vertices+(vf == 0 ? n*4 : vf == 3 ? n*8 : 0)+i];
        };
        for (unsigned row = 0; row < h; ++row) for (unsigned col = 0; col < w; ++col)
        {
          auto bit = row*w+col;
          if (mask && !(data[mask+bit/8] & (1u << (bit%8)))) continue;
          Cell cell{c%16*8+x+col,c/16*8+y+row,type,object >= 42 ? object : 0,{},{}};
          unsigned a = row*(w+1)+col, ids[]{a,a+1,a+w+1,a+w+2};
          // Samples are not clamped to min/max; partial layers can have stale bounds.
          for (unsigned k = 0; k < 4; ++k)
          {
            cell.heights[k] = height(ids[k]); cell.depth[k] = depth(ids[k]);
          }
          cells.push_back(cell);
        }
      }
    }
    return cells;
  }
  inline std::pair<float,float> active_bounds(std::array<float,81> const& heights, std::uint64_t mask)
  {
    float lo = INFINITY, hi = -INFINITY;
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x)
      if (mask & (std::uint64_t{1} << (y*8+x)))
        for (unsigned i : {y*9+x,y*9+x+1,y*9+x+9,y*9+x+10})
        {
          if (!std::isfinite(heights[i])) throw std::runtime_error("Non-finite edited liquid height.");
          lo = std::min(lo,heights[i]); hi = std::max(hi,heights[i]);
        }
    return mask ? std::make_pair(lo,hi) : std::make_pair(0.f,0.f);
  }
  // MLLD RLE: big-endian 16-bit controls. Bit 15 marks a fill run; the other
  // 15 bits are a nonzero byte count.
  inline Bytes compress(Bytes const& in)
  {
    Bytes out;
    auto control = [&](unsigned n) { out.push_back(n >> 8); out.push_back(n & 255); };
    for (std::size_t p = 0; p < in.size();)
    {
      auto run = [&](std::size_t a) {
        std::size_t n = 1;
        while (a+n < in.size() && in[a+n] == in[a] && n < 32767) ++n;
        return n;
      };
      auto n = run(p);
      if (n >= 3) { control(static_cast<unsigned>(n)|0x8000); out.push_back(in[p]); p += n; }
      else
      {
        auto first = p; p += n;
        while (p < in.size() && p-first < 32765 && run(p) < 3) p += run(p);
        control(static_cast<unsigned>(p-first)); out.insert(out.end(),in.begin()+first,in.begin()+p);
      }
    }
    return out;
  }
  inline Bytes rebuild(Bytes const& root, Bytes const& lod, Bytes const* reference_root = nullptr)
  {
    auto cells = read(mh2o(root));
    using Location = std::tuple<unsigned,unsigned,unsigned>;
    using Appearance = std::pair<float,unsigned>;
    std::map<Location,std::vector<Appearance>> inherited, covered;
    if (reference_root)
      for (auto const& c : read(mh2o(*reference_root)))
        if (c.object) inherited[{c.x,c.y,c.type}].push_back({(c.heights[1]+c.heights[2])*0.5f,c.object});
    // Cells with an explicit vertex format take their LiquidObject from the
    // reference ROOT, then from the existing liquid LOD.
    struct Material { unsigned type, object; float x0, y0, x1, y1, height; };
    std::vector<Material> materials;
    unsigned type = 0, object = 0;
    Bytes liquid_indices;
    for (auto const& c : chunks(lod))
    {
      if (c.name == "MLLN")
      { range(c.data,0,24); type = word(c.data,12,2); object = word(c.data,14,2); liquid_indices.clear(); }
      if (c.name == "MLLI") liquid_indices = c.data;
      if (c.name == "MLLV" && object)
      {
        if (c.data.empty() || c.data.size()%12) throw std::runtime_error("Invalid MLLV.");
        Material m{type,object,INFINITY,INFINITY,-INFINITY,-INFINITY,real(c.data,8)};
        for (std::size_t p = 0; p < c.data.size(); p += 12)
        {
          auto x = real(c.data,p), y = real(c.data,p+4);
          m.x0 = std::min(m.x0,x); m.x1 = std::max(m.x1,x);
          m.y0 = std::min(m.y0,y); m.y1 = std::max(m.y1,y);
        }
        materials.push_back(m);
        if (liquid_indices.size()%6) throw std::runtime_error("Invalid MLLI triangles.");
        for (std::size_t i = 0; i < liquid_indices.size(); i += 6)
        {
          float x[3], y[3], z[3];
          for (unsigned k = 0; k < 3; ++k)
          {
            auto p = word(liquid_indices,i+k*2,2)*12;
            range(c.data,p,12);
            x[k] = real(c.data,p)/(533.333333333f/128.f);
            y[k] = real(c.data,p+4)/(533.333333333f/128.f);
            z[k] = real(c.data,p+8);
            if (std::abs(x[k]) > 4096 || std::abs(y[k]) > 4096)
              throw std::runtime_error("MLLV coordinates outside a tile-local range.");
          }
          auto area = (y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);
          if (std::abs(area) < 1e-8f) continue;
          auto xmin = std::max(0,int(std::floor(std::min({x[0],x[1],x[2]}))));
          auto xmax = std::min(127,int(std::floor(std::max({x[0],x[1],x[2]}))));
          auto ymin = std::max(0,int(std::floor(std::min({y[0],y[1],y[2]}))));
          auto ymax = std::min(127,int(std::floor(std::max({y[0],y[1],y[2]}))));
          for (int row = ymin; row <= ymax; ++row) for (int col = xmin; col <= xmax; ++col)
          {
            auto a = ((y[1]-y[2])*(col+0.5f-x[2])+(x[2]-x[1])*(row+0.5f-y[2]))/area;
            auto b = ((y[2]-y[0])*(col+0.5f-x[2])+(x[0]-x[2])*(row+0.5f-y[2]))/area;
            if (a >= -1e-5f && b >= -1e-5f && a+b <= 1.00001f)
              covered[{unsigned(col),unsigned(row),type}].push_back({a*z[0]+b*z[1]+(1-a-b)*z[2],object});
          }
        }
      }
    }
    for (auto& c : cells) if (!c.object)
    {
      auto it = inherited.find({c.x,c.y,c.type});
      auto old = covered.find({c.x,c.y,c.type});
      if (it != inherited.end() || old != covered.end())
      {
        auto const& candidates = it != inherited.end() ? it->second : old->second;
        auto height = (c.heights[1]+c.heights[2])*0.5f;
        c.object = std::min_element(candidates.begin(),candidates.end(),[&](auto a, auto b) {
          return std::abs(a.first-height) < std::abs(b.first-height);
        })->second;
      }
      else
      {
        float best = INFINITY, x = (c.x+0.5f)*(533.333333333f/128.f),
              y = (c.y+0.5f)*(533.333333333f/128.f);
        for (auto const& m : materials) if (m.type == c.type)
        {
          float distance = std::max({m.x0-x,0.f,x-m.x1})+std::max({m.y0-y,0.f,y-m.y1})
                         +std::abs(m.height-c.heights[0]);
          if (distance < best) { best = distance; c.object = m.object; }
        }
        if (!c.object && default_objects.count(c.type)) c.object = default_objects.at(c.type);
        if (!c.object) throw std::runtime_error("LiquidObject unavailable for new liquid type " + std::to_string(c.type));
      }
    }
    Bytes depth(128*128), alpha(128*128/8), generated;
    std::array<float,128*128> top;
    top.fill(-INFINITY);
    using Key = std::pair<unsigned,unsigned>;
    std::map<Key,std::vector<Cell>> groups;
    float lo = INFINITY, hi = -INFINITY;
    for (auto const& c : cells)
    {
      auto k = c.y*128+c.x;
      auto h = (c.heights[0]+c.heights[1]+c.heights[2]+c.heights[3])*0.25f;
      if (h >= top[k])
      {
        top[k] = h;
        depth[k] = static_cast<std::uint8_t>((c.depth[0]+c.depth[1]+c.depth[2]+c.depth[3]+2)/4);
      }
      alpha[k/8] |= 1u << (k%8);
      for (auto v : c.heights) { lo = std::min(lo,v); hi = std::max(hi,v); }
      groups[{c.type,c.object}].push_back(c);
    }
    if (!cells.empty())
    {
      auto d = compress(depth), a = compress(alpha);
      Bytes data(12); put(data,0,7); put(data,4,static_cast<unsigned>(d.size()),2);
      put(data,6,static_cast<unsigned>(a.size()),2);
      data.insert(data.end(),d.begin(),d.end()); data.insert(data.end(),a.begin(),a.end());
      while (data.size()%4) data.push_back(0);
      append(generated,"MLLD",data);
    }
    for (auto const& [key, list] : groups)
    {
      Bytes vertices, indices;
      std::map<std::tuple<unsigned,unsigned,float>,unsigned> vertex_ids;
      auto flush_mesh = [&] {
        if (indices.empty()) return;
        Bytes header(24);
        put(header,4,static_cast<unsigned>(indices.size()/2)); put(header,8,1);
        put(header,12,key.first,2); put(header,14,key.second,2);
        put(header,16,0xffffffff); put(header,20,0xffffffff);
        append(generated,"MLLN",header); append(generated,"MLLI",indices); append(generated,"MLLV",vertices);
        vertices.clear(); indices.clear(); vertex_ids.clear();
      };
      auto quad = [&](unsigned x, unsigned y, unsigned w, unsigned h, std::array<float,4> const& heights) {
        if (vertex_ids.size()+4 > 65535) flush_mesh();
        unsigned ids[4], xs[]{x,x+w,x,x+w}, ys[]{y,y,y+h,y+h};
        for (unsigned k = 0; k < 4; ++k)
        {
          auto [it, inserted] = vertex_ids.emplace(std::make_tuple(xs[k],ys[k],heights[k]),
                                                  static_cast<unsigned>(vertex_ids.size()));
          ids[k] = it->second;
          if (inserted)
          {
            auto p = vertices.size(); vertices.resize(p+12);
            putf(vertices,p,xs[k]*(533.333333333f/128.f));
            putf(vertices,p+4,ys[k]*(533.333333333f/128.f));
            putf(vertices,p+8,heights[k]);
          }
        }
        // Winding matches Retail liquid meshes.
        for (auto k : {0,2,1,1,2,3})
        { indices.push_back(ids[k]&255); indices.push_back(ids[k]>>8); }
      };
      // Collapse flat water into rectangles. Sloped surfaces retain all samples.
      std::map<float,std::array<bool,128*128>> flat;
      for (auto const& c : list)
      {
        if (std::all_of(c.heights.begin(),c.heights.end(),[&](float h){return h == c.heights[0];}))
          flat[c.heights[0]][c.y*128+c.x] = true;
        else quad(c.x,c.y,1,1,c.heights);
      }
      for (auto& [height, mask] : flat)
        for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x)
        {
          if (!mask[y*128+x]) continue;
          unsigned w = 1, h = 1;
          while (x+w < 128 && mask[y*128+x+w]) ++w;
          while (y+h < 128 && std::all_of(mask.begin()+(y+h)*128+x,
                 mask.begin()+(y+h)*128+x+w,[](bool v){return v;})) ++h;
          for (unsigned dy = 0; dy < h; ++dy)
            std::fill_n(mask.begin()+(y+dy)*128+x,w,false);
          quad(x,y,w,h,{height,height,height,height});
        }
      flush_mesh();
    }
    Bytes out;
    bool written = false;
    for (auto c : chunks(lod))
    {
      if (c.name == "MALP" || c.name == "MASD")
        throw std::runtime_error("PBR liquid patches (MALP/MASD) are not supported.");
      if (c.name == "MLLD" || c.name == "MLLN" || c.name == "MLLI" || c.name == "MLLV")
      {
        if (!written) { out.insert(out.end(),generated.begin(),generated.end()); written = true; }
        continue;
      }
      if (c.name == "MLHD")
      {
        range(c.data,0,28);
        putf(c.data,20,cells.empty() ? 1.f : lo); putf(c.data,24,cells.empty() ? -1.f : hi);
      }
      append(out,c.name,c.data);
    }
    if (!written) out.insert(out.end(),generated.begin(),generated.end());
    return out;
  }
  // MLDL is parallel to MLDD, not a placement ID table. Update in place without
  // disturbing order, UID, batching, dictionaries or LOD memberships.
  inline Bytes update_object_water(Bytes const& root, Bytes const& obj)
  {
    if (obj.empty()) return obj;
    auto all = chunks(obj);
    Bytes const* placements = nullptr;
    for (auto const& c : all) if (c.name == "MLDD") placements = &c.data;
    if (!placements) return obj;
    if (placements->size()%36) throw std::runtime_error("Invalid MLDD size for liquid update.");
    float origin_x = 0, origin_y = 0; bool found = false;
    for (auto const& c : chunks(root)) if (c.name == "MCNK" && !found)
    {
      // MCNK stores north/west server coordinates; placements use map-corner X/Z.
      origin_x = 17066.666666667f-real(c.data,108);
      origin_y = 17066.666666667f-real(c.data,104);
      found = true;
    }
    if (!found) throw std::runtime_error("Missing ROOT grid for object liquid update.");
    std::array<std::vector<Cell const*>,128*128> by_cell;
    auto cells = read(mh2o(root));
    for (auto const& c : cells) by_cell[c.y*128+c.x].push_back(&c);
    Bytes out;
    for (auto c : all)
    {
      if (c.name == "MLDL")
      {
        if (c.data.size() != placements->size()/36*4) throw std::runtime_error("MLDL/MLDD count mismatch.");
        for (std::size_t i = 0; i < placements->size()/36; ++i)
        {
          if (!(word(*placements,i*36+34,2)&8)) continue;
          auto x = (real(*placements,i*36+8)-origin_x)/(533.333333333f/128.f);
          auto y = (real(*placements,i*36+16)-origin_y)/(533.333333333f/128.f);
          // Placements outside this tile keep their existing value.
          if (x < 0 || y < 0 || x >= 128 || y >= 128) continue;
          auto ix = static_cast<unsigned>(x), iy = static_cast<unsigned>(y);
          float chosen = 0, distance = INFINITY;
          for (auto* cell : by_cell[iy*128+ix])
          {
            auto fx = x-ix, fy = y-iy;
            float h = cell->heights[0]*(1-fx)*(1-fy)+cell->heights[1]*fx*(1-fy)
                    +cell->heights[2]*(1-fx)*fy+cell->heights[3]*fx*fy;
            auto d = std::abs(h-real(*placements,i*36+12));
            if (d < distance) { distance = d; chosen = h; }
          }
          putf(c.data,i*4,chosen);
        }
      }
      append(out,c.name,c.data);
    }
    return out;
  }
  inline Bytes update_object_bounds(Bytes const& obj, Bytes const& lod)
  {
    if (obj.empty()) return lod;
    float lo = INFINITY, hi = -INFINITY;
    bool found = false;
    for (auto const& c : chunks(obj)) if (c.name == "MLDX" || c.name == "MLMX")
    {
      if (c.data.size()%28) throw std::runtime_error("Invalid object LOD bounds.");
      found = true;
      for (std::size_t p = 0; p < c.data.size(); p += 28)
      {
        auto a = real(c.data,p+8), b = real(c.data,p+20);
        if (a > b) throw std::runtime_error("Inverted object LOD height bounds.");
        lo = std::min(lo,a); hi = std::max(hi,b);
      }
    }
    if (!found) return lod;
    Bytes out;
    for (auto c : chunks(lod))
    {
      if (c.name == "MLHD")
      { putf(c.data,12,lo == INFINITY ? 1.f : lo); putf(c.data,16,hi == -INFINITY ? -1.f : hi); }
      append(out,c.name,c.data);
    }
    return out;
  }
}
