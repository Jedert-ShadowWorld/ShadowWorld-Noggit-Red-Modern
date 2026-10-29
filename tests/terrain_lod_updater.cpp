// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/formats/adt/TerrainLodUpdater.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <array>
#include <map>
#include <stdexcept>

using Bytes = std::vector<std::uint8_t>;
using Noggit::Formats::ADT::TerrainLodUpdater;
namespace
{
  void require(bool ok, char const* message) { if (!ok) throw std::runtime_error(message); }
  void put(Bytes& b, std::size_t p, std::uint32_t value)
  {
    for (int i = 0; i < 4; ++i) b.at(p + i) = static_cast<std::uint8_t>(value >> (8 * i));
  }
  void putf(Bytes& b, std::size_t p, float value)
  {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    put(b, p, bits);
  }
  void append(Bytes& b, char const* name, Bytes const& body)
  {
    auto start = b.size();
    for (int i = 3; i >= 0; --i) b.push_back(name[i]);
    b.resize(b.size() + 4);
    put(b, start + 4, static_cast<std::uint32_t>(body.size()));
    b.insert(b.end(), body.begin(), body.end());
  }
  float height(int x, int y) { return static_cast<float>(x * x + y * 3); }
  Bytes root(bool flat, float delta = 0)
  {
    Bytes result, version(4);
    put(version, 0, 18);
    append(result, "MVER", version);
    append(result, "MHDR", Bytes(64));
    for (int cy = 0; cy < 16; ++cy)
      for (int cx = 0; cx < 16; ++cx)
      {
        Bytes body(128), vertices(145 * 4);
        put(body, 4, cx); put(body, 8, cy);
        for (int row = 0, v = 0; row < 17; ++row)
          for (int col = 0; col < (row % 2 ? 8 : 9); ++col, ++v)
            putf(vertices, v * 4, (flat ? 0 : height(cx * 16 + col * 2 + row % 2, cy * 16 + row)) + delta);
        append(body, "MCVT", vertices);
        append(result, "MCNK", body);
      }
    return result;
  }
  Bytes lod(bool flat, int layout = 0, float delta = 0)
  {
    Bytes result, heights((129 * 129 + 128 * 128 + 1) * 4), header(28), skirts(2);
    std::size_t i = 0;
    for (int center = 0; center < 2; ++center)
      for (int outer = 0; outer < 129 - center; ++outer)
        for (int inner = 0; inner < 129 - center; ++inner)
        {
          int x = layout & 2 ? outer : inner, y = layout & 2 ? inner : outer;
          if (layout & 1) { x = 128 - center - x; y = 128 - center - y; }
          putf(heights, i++ * 4, (flat ? 0 : height(x * 2 + center, y * 2 + center)) + delta);
        }
    auto const skirt_height = (flat ? 0 : height(layout & 1 ? 256 : 0, layout & 1 ? 256 : 0)) + delta - 32;
    putf(heights, i * 4, skirt_height);
    putf(header, 4, std::min(delta, skirt_height));
    putf(header, 8, (flat ? 0 : height(256,256)) + delta);
    append(result, "MLHD", header); append(result, "MLVH", heights);
    append(result, "MLSI", skirts); append(result, "TEST", {1, 2, 3, 4, 5});
    return result;
  }
  template<class F> void rejects(F f)
  {
    try { f(); } catch (std::runtime_error const&) { return; }
    throw std::runtime_error("Malformed or ambiguous LOD update was accepted.");
  }
  Bytes read(std::filesystem::path const& p)
  {
    std::ifstream stream(p, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read " + p.string());
    return Bytes(std::istreambuf_iterator<char>(stream), {});
  }
  std::uint32_t get(Bytes const& b, std::size_t p)
  {
    return std::uint32_t(b.at(p)) | (std::uint32_t(b.at(p+1))<<8)
      | (std::uint32_t(b.at(p+2))<<16) | (std::uint32_t(b.at(p+3))<<24);
  }
  std::map<std::string, Bytes> bodies(Bytes const& b)
  {
    std::map<std::string, Bytes> result;
    for (std::size_t p = 0; p < b.size();)
    {
      auto size = get(b,p+4);
      require(size <= b.size()-p-8,"Bad test chunk");
      result.emplace(std::string(b.rbegin()+b.size()-p-4,b.rbegin()+b.size()-p),
                     Bytes(b.begin()+p+8,b.begin()+p+8+size));
      p += 8+size;
    }
    return result;
  }
  using HoleCells = std::array<bool,128*128>;
  void check_bounds(Bytes const& lod)
  {
    auto const parts = bodies(lod);
    auto const& heights = parts.at("MLVH");
    auto value = [&](Bytes const& b, std::size_t at) {
      auto bits = get(b, at); float result; std::memcpy(&result, &bits, 4); return result;
    };
    float minimum = value(heights, 0), maximum = minimum;
    for (std::size_t at = 4; at < heights.size(); at += 4)
    { minimum = std::min(minimum, value(heights, at)); maximum = std::max(maximum, value(heights, at)); }
    require(value(parts.at("MLHD"), 4) == minimum && value(parts.at("MLHD"), 8) == maximum,
      "Terrain culling bounds must enclose every terrain and skirt vertex");
  }
  Bytes with_holes(Bytes b, HoleCells const& mask, int layout = 0)
  {
    for (int y = 0; y < 128; ++y)
      for (int x = 0; x < 128; ++x)
      {
        if (!mask[y*128+x]) continue;
        int gx=x, gy=y;
        if (layout&2) std::swap(gx,gy);
        if (layout&1) { gx=127-gx; gy=127-gy; }
        auto p = 92 + ((gy/8)*16+gx/8)*724;
        put(b,p,0x10000);
        int bit=(gy%8)*8+gx%8;
        auto offset=p+20+(bit/32)*4;
        put(b,offset,get(b,offset)|(1u<<(bit%32)));
      }
    return b;
  }
  Bytes mesh_lod(HoleCells const& holes, int layout = 0)
  {
    Bytes b=lod(false,layout), nodes(85*24), levels(80), indices;
    struct Node { int x,y,size,level; };
    std::vector<Node> regions{{0,0,128,0}};
    for (std::size_t i=0;i<21;++i)
      for (int c=0;c<4;++c)
      {
        auto n=regions[i];
        regions.push_back({n.x+(c%2)*n.size/2,n.y+(c/2)*n.size/2,n.size/2,n.level+1});
      }
    auto emit=[&](std::uint16_t v) { indices.push_back(v&255);indices.push_back(v>>8); };
    for (std::size_t i=0;i<85;++i)
    {
      auto n=regions[i];
      if (i==0 || i==1 || i==5 || i==21)
      {
        putf(levels,n.level*20,32.0f/(1<<n.level));
        put(levels,n.level*20+4,static_cast<std::uint32_t>(indices.size()/2));
      }
      auto first=indices.size()/2;
      put(nodes,i*24,static_cast<std::uint32_t>(first));
      for(int y=n.y;y<n.y+n.size;y+=4)
        for(int x=n.x;x<n.x+n.size;x+=4)
          if (!holes[y*128+x])
          {
            // Long edges at hole boundaries exercise boundary subdivision.
            std::uint16_t a=y*129+x, b=a+4, c=a+4*129, d=c+4;
            for(auto v:{a,c,b,b,c,d}) emit(v);
          }
      put(nodes,i*24+4,static_cast<std::uint32_t>(indices.size()/2-first));
      for(int c=0;c<4;++c)
      {
        auto child=i<21 ? i*4+1+c : 0xffff;
        nodes[i*24+16+c*2]=child&255; nodes[i*24+17+c*2]=child>>8;
      }
      if (i==0 || i==4 || i==20 || i==84)
      {
        auto p=n.level*20;
        put(levels,p+8,static_cast<std::uint32_t>(indices.size()/2-get(levels,p+4)));
        // A separate skirt list must move along with the terrain range.
        put(levels,p+12,3);
        put(levels,p+16,static_cast<std::uint32_t>(indices.size()/2));
        emit(0);emit(33025);emit(129*4);
      }
    }
    append(b,"MLLL",levels);append(b,"MLND",nodes);append(b,"MLVI",indices);
    return b;
  }
  void check_mesh(Bytes const& b, HoleCells const& holes)
  {
    auto c=bodies(b);
    auto const& ix=c.at("MLVI"); auto const& levels=c.at("MLLL");
    auto index=[&](std::size_t p){return unsigned(ix.at(p*2))|(unsigned(ix.at(p*2+1))<<8);};
    auto point=[](unsigned v)->std::array<int,2> {
      require(v<33025,"Terrain references skirt vertex");
      return v<16641 ? std::array<int,2>{int(v%129)*2,int(v/129)*2}
        : std::array<int,2>{int((v-16641)%128)*2+1,int((v-16641)/128)*2+1};
    };
    auto cross=[](auto a,auto b,auto c){return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);};
    for(int level=0;level<4;++level)
    {
      auto start=get(levels,level*20+4), count=get(levels,level*20+8);
      std::int64_t area=0;
      std::array<unsigned,128*128> covered{};
      std::map<std::pair<unsigned,unsigned>,unsigned> edges;
      for(std::size_t i=start;i<start+count;i+=3)
      {
        auto a=point(index(i)), b=point(index(i+1)), c=point(index(i+2));
        auto ca=cross(a,b,c);
        require(ca<0,"Invalid winding after hole repair");
        area-=ca;
        for(int e=0;e<3;++e) ++edges[std::minmax(index(i+e),index(i+(e+1)%3))];
        // An asymmetric interior sample avoids the regular mesh diagonals.
        for(int y=std::min({a[1],b[1],c[1]})/2;y<(std::max({a[1],b[1],c[1]})+1)/2;++y)
          for(int x=std::min({a[0],b[0],c[0]})/2;x<(std::max({a[0],b[0],c[0]})+1)/2;++x)
          {
            std::array<double,2> q{x*2+0.6283185307179586,y*2+0.543656365691809};
            if(cross(a,b,q)<0 && cross(b,c,q)<0 && cross(c,a,q)<0)
              ++covered[y*128+x];
          }
      }
      auto hole_count=std::count(holes.begin(),holes.end(),true);
      require(area==(128*128-hole_count)*8,"Repaired LOD surface area differs");
      if (!hole_count)
        for(auto const& [edge,uses]:edges)
        {
          auto a=point(edge.first),b=point(edge.second);
          bool outer=(a[0]==b[0] && (a[0]==0 || a[0]==256))
            || (a[1]==b[1] && (a[1]==0 || a[1]==256));
          require(uses==(outer?1u:2u),"Unstitched node boundary or non-manifold edge");
        }
      for(int i=0;i<128*128;++i)
        require(covered[i]==(holes[i]?0u:1u),"Overlapping terrain or incorrect remaining hole");
      auto skirt=get(levels,level*20+16);
      require(get(levels,level*20+12)>=3 && index(skirt)==0 && index(skirt+1)==33025
        && index(skirt+2)==129*4,"Skirt list changed during hole repair");
      for (std::size_t i=skirt;i<skirt+get(levels,level*20+12);++i)
        require(index(i)<c.at("MLVH").size()/4,"Skirt index outside height table");
    }
  }
  void hole_tests()
  {
    HoleCells old{};
    for(int y=12;y<20;++y) for(int x=12;x<20;++x) old[y*128+x]=true;
    for(int y=40;y<44;++y) for(int x=40;x<44;++x) old[y*128+x]=true;
    auto after=old;
    for(int y=12;y<20;++y) for(int x=12;x<20;++x) after[y*128+x]=false;
    // A partially closed hole, including a node border, remains open here.
    after[16*128+16]=true;
    for(int layout=0;layout<4;++layout)
    {
      auto before=with_holes(root(false),old,layout), current=with_holes(root(false),after,layout);
      auto original=mesh_lod(old,layout);
      require(TerrainLodUpdater::update(before,original,before)==original,"Unchanged mesh was rewritten");
      auto result=TerrainLodUpdater::update(before,original,current);
      check_bounds(result);
      check_mesh(result,after);
      require(TerrainLodUpdater::update(current,result,current)==result,"Hole repair is not idempotent");
      require(TerrainLodUpdater::update(current,original,current)==result,"Previously saved stale mesh was not repaired");
      auto a=bodies(original),b=bodies(result);
      require(a["MLSI"]==b["MLSI"] && a["MLVH"]==b["MLVH"] && a["TEST"]==b["TEST"],
        "Hole repair changed height/skirt/unknown data");
      auto broken=original; auto cs=bodies(broken);
      // MLVI is the final chunk in this fixture.
      put(broken,broken.size()-cs["MLVI"].size()-4,0xffffffff);
      rejects([&]{(void)TerrainLodUpdater::update(before,broken,current);});
    }
    HoleCells border{};
    for(int y=0;y<16;++y) for(int x=0;x<16;++x) border[y*128+x]=true;
    auto before=with_holes(root(false),border), current=root(false);
    auto original=mesh_lod(border);
    auto result=TerrainLodUpdater::update(before,original,current);
    check_bounds(result);
    check_mesh(result,{});
    auto data=bodies(result);
    require(data["MLVH"].size()>bodies(original)["MLVH"].size(),"Missing new tile-border skirts");
    require(data["MLSI"].size()/2==data["MLVH"].size()/4-33025,"Skirt/height count mismatch");
    for(std::size_t i=0;i<data["MLSI"].size()/2;++i)
    {
      auto base=unsigned(data["MLSI"][i*2])|(unsigned(data["MLSI"][i*2+1])<<8);
      float top,bottom;
      auto bits=get(data["MLVH"],base*4); std::memcpy(&top,&bits,4);
      bits=get(data["MLVH"],(33025+i)*4); std::memcpy(&bottom,&bits,4);
      require(bottom==top-32,"New skirt height does not follow ROOT");
    }
    require(TerrainLodUpdater::update(current,result,current)==result,"Tile-border repair is not idempotent");
    HoleCells node_edge{};
    for(int y=4;y<8;++y) for(int x=12;x<16;++x) node_edge[y*128+x]=true;
    auto joined=TerrainLodUpdater::update(with_holes(root(false),node_edge),mesh_lod(node_edge),root(false));
    check_mesh(joined,{});
  }
  // Construct a supported height-only edit from real inputs. Keep the original
  // topology/liquids, replacing only each chunk's base height and MCVT.
  Bytes height_only(Bytes original, Bytes const& edited)
  {
    auto get = [](Bytes const& b, std::size_t p) {
      return std::uint32_t(b.at(p)) | (std::uint32_t(b.at(p + 1)) << 8)
        | (std::uint32_t(b.at(p + 2)) << 16) | (std::uint32_t(b.at(p + 3)) << 24);
    };
    auto find = [&](Bytes const& b, std::size_t begin, std::size_t end, char const* reversed) {
      std::vector<std::size_t> offsets;
      while (begin < end)
      {
        require(end - begin >= 8, "Truncated test input.");
        auto size = get(b, begin + 4);
        require(size <= end - begin - 8, "Invalid test input chunk.");
        if (std::memcmp(b.data() + begin, reversed, 4) == 0) offsets.push_back(begin + 8);
        begin += 8 + size;
      }
      return offsets;
    };
    auto a = find(original, 0, original.size(), "KNCM"), b = find(edited, 0, edited.size(), "KNCM");
    require(a.size() == 256 && b.size() == 256, "Invalid real MCNK count.");
    for (std::size_t i = 0; i < a.size(); ++i)
    {
      auto av = find(original, a[i] + 128, a[i] + get(original, a[i] - 4), "TVCM");
      auto bv = find(edited, b[i] + 128, b[i] + get(edited, b[i] - 4), "TVCM");
      require(av.size() == 1 && bv.size() == 1, "Invalid real MCVT count.");
      require(get(original, av[0] - 4) == 580 && get(edited, bv[0] - 4) == 580, "Invalid MCVT size.");
      std::copy_n(edited.begin() + b[i] + 112, 4, original.begin() + a[i] + 112);
      std::copy_n(edited.begin() + bv[0], 580, original.begin() + av[0]);
    }
    return original;
  }
}

int main(int argc, char** argv)
{
  try
  {
    for (int layout = 0; layout < 4; ++layout)
    {
      std::string diagnostics;
      auto result = TerrainLodUpdater::update(root(false), lod(false, layout), root(false, 10), &diagnostics);
      require(result == lod(false, layout, 10), "Orientation, skirts, bounds or preserved chunks differ.");
      check_bounds(result);
      require(diagnostics.find("XOuter/MaxCorner") != std::string::npos, "Missing layout diagnostics.");
    }
    require(TerrainLodUpdater::update(root(true), lod(true), root(true, 2)) == lod(true, 0, 2),
            "Equivalent ambiguous output must be allowed.");
    rejects([&] { (void)TerrainLodUpdater::update(root(true), lod(true), root(false)); });
    auto truncated = root(false); truncated.pop_back();
    rejects([&] { (void)TerrainLodUpdater::update(truncated, lod(false), root(false)); });
    auto bad_lod = lod(false); put(bad_lod, 4, 0xffffffff);
    rejects([&] { (void)TerrainLodUpdater::update(root(false), bad_lod, root(false)); });
    bad_lod = lod(false);
    // MLSI is immediately before the final TEST chunk; invalid grid index.
    bad_lod[bad_lod.size() - 15] = 255; bad_lod[bad_lod.size() - 14] = 255;
    rejects([&] { (void)TerrainLodUpdater::update(root(false), bad_lod, root(false)); });
    rejects([&] { (void)TerrainLodUpdater::update(root(false, 1000000), lod(false), root(false)); });
    auto nan_root = root(false); put(nan_root, 12 + 72 + 8 + 128 + 8, 0x7fc00000);
    rejects([&] { (void)TerrainLodUpdater::update(nan_root, lod(false), root(false)); });
    auto bad_root = root(false); put(bad_root, 8, 17);
    rejects([&] { (void)TerrainLodUpdater::update(bad_root, lod(false), root(false)); });
    bad_root = root(false); put(bad_root, 12 + 72 + 8 + 4, 1);
    rejects([&] { (void)TerrainLodUpdater::update(bad_root, lod(false), root(false)); });
    bad_root = root(false); put(bad_root, 12 + 72 + 8 + 128 + 4, 144 * 4);
    rejects([&] { (void)TerrainLodUpdater::update(bad_root, lod(false), root(false)); });
    bad_lod = lod(false); append(bad_lod, "MLHD", Bytes(28));
    rejects([&] { (void)TerrainLodUpdater::update(root(false), bad_lod, root(false)); });
    constexpr std::size_t first_header = 12 + 72 + 8;
    auto holes = root(false); put(holes, first_header + 60, 1);
    rejects([&] { (void)TerrainLodUpdater::update(root(false), lod(false), holes); });
    auto high_holes = root(false);
    put(high_holes, first_header, 0x10000); put(high_holes, first_header + 20, 0x303);
    require(TerrainLodUpdater::update(holes, lod(false), high_holes) == lod(false),
            "Equivalent low/high-resolution holes must be accepted.");
    put(high_holes, first_header + 20, 1);
    rejects([&] { (void)TerrainLodUpdater::update(holes, lod(false), high_holes); });
    auto water = root(false); append(water, "MH2O", {1,2,3,4});
    rejects([&] { (void)TerrainLodUpdater::update(root(false), lod(false), water); });
    rejects([&] { (void)TerrainLodUpdater::update(water, lod(false), water); });
    auto altered_water = water; altered_water.back() = 5;
    rejects([&] { (void)TerrainLodUpdater::update(water, lod(false), altered_water); });
    auto mclq_root = root(false);
    Bytes mclq; append(mclq, "MCLQ", {1,2,3,4});
    constexpr std::size_t first_end = first_header + 128 + 8 + 145 * 4;
    mclq_root.insert(mclq_root.begin() + first_end, mclq.begin(), mclq.end());
    put(mclq_root, first_header - 4, 128 + 8 + 145 * 4 + static_cast<std::uint32_t>(mclq.size()));
    rejects([&] { (void)TerrainLodUpdater::update(root(false), lod(false), mclq_root); });
    require(TerrainLodUpdater::update(mclq_root, lod(false), mclq_root) == lod(false),
            "Unchanged MCLQ must be accepted.");
    auto liquid_type = root(false); put(liquid_type, first_header, 4);
    require(TerrainLodUpdater::update(root(false), lod(false), liquid_type) == lod(false),
            "Legacy liquid flags must not block a Retail MH2O update.");
    hole_tests();
    std::cout << "PASS: four layouts, skirts, preserved bytes, ambiguity, confidence, malformed input, "
                 "closed and partial holes, node borders, long edges, active holes and idempotence.\n";
    // Optional read-only comparison: original dir, edited dir, reference LOD dir.
    if (argc == 4)
    {
      std::size_t compared = 0, unsupported = 0, height_only_compared = 0;
      for (auto const& entry : std::filesystem::directory_iterator(argv[3]))
      {
        auto name = entry.path().filename().string();
        if (!name.ends_with("_lod.adt")) continue;
        auto root_name = name.substr(0, name.size() - 8) + ".adt";
        auto original = read(std::filesystem::path(argv[1]) / root_name);
        auto source_lod = read(std::filesystem::path(argv[1]) / name);
        auto edited = read(std::filesystem::path(argv[2]) / root_name);
        require(TerrainLodUpdater::update(original, source_lod, height_only(original, edited))
                == read(entry.path()), "Height-only update differs from the reference fixture.");
        ++height_only_compared;
        try
        {
          auto generated = TerrainLodUpdater::update(read(std::filesystem::path(argv[1]) / root_name),
            read(std::filesystem::path(argv[1]) / name), read(std::filesystem::path(argv[2]) / root_name));
          require(generated == read(entry.path()), "LOD output differs from the reference fixture.");
          ++compared;
        }
        catch (std::runtime_error const& e)
        {
          if (std::string(e.what()).find("rebuild is unsupported:") == std::string::npos) throw;
          ++unsupported;
          std::cout << "REQUIRES REBUILD: " << name << ": " << e.what() << '\n';
        }
      }
      require(compared + unsupported > 0, "No oracle files found.");
      std::cout << "PASS: " << compared << " real LOD files match reference fixtures byte-for-byte; "
                << unsupported << " correctly rejected for unsupported topology/water changes.\n";
      std::cout << "PASS: all " << height_only_compared
                << " real height-only edits match reference fixtures byte-for-byte with topology/liquid safeguards enabled.\n";
    }
    return 0;
  }
  catch (std::exception const& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
