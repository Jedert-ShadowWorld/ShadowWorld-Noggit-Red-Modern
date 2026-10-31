// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/formats/adt/TerrainLodUpdater.hpp>
#include <algorithm>
#include <cmath>
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
using Noggit::Formats::ADT::TerrainLodLayout;
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
  std::size_t body_offset(Bytes const& b, char const* name)
  {
    for (std::size_t p = 0; p < b.size(); p += 8 + get(b, p + 4))
      if (b[p] == name[3] && b[p+1] == name[2] && b[p+2] == name[1] && b[p+3] == name[0])
        return p + 8;
    throw std::runtime_error("Missing test chunk.");
  }
  Bytes noisy_flat_root(float amplitude)
  {
    auto b = root(true);
    for (int cy = 0; cy < 16; ++cy)
      for (int cx = 0; cx < 16; ++cx)
        for (int row = 0, v = 0; row < 17; ++row)
          for (int col = 0; col < (row % 2 ? 8 : 9); ++col, ++v)
          {
            auto const x = cx * 16 + col * 2 + row % 2, y = cy * 16 + row;
            // Shared chunk-border samples agree; noise is asymmetric in X/Y.
            auto const noise = amplitude * (float((x + 3*y) % 11) / 5.f - 1.f);
            putf(b, 92 + (cy * 16 + cx) * 724 + 128 + 8 + v * 4, noise);
          }
    return b;
  }
  void reference_tests()
  {
    for (int layout = 0; layout < 4; ++layout)
    {
      auto const inspection = TerrainLodUpdater::inspectReference(root(false), lod(false, layout));
      require(inspection.usable && !inspection.equivalent_layouts
           && inspection.unique_layout == static_cast<TerrainLodLayout>(layout)
           && inspection.candidate_layouts == (1u << layout), "Unique reference layout not identified");
    }
    auto const flat = TerrainLodUpdater::inspectReference(root(true), lod(true));
    require(flat.usable && flat.equivalent_layouts && !flat.unique_layout && flat.candidate_layouts == 15,
            "Flat reference must not claim a unique orientation");

    auto const stale = TerrainLodUpdater::inspectReference(root(false, 1000000), lod(false));
    require(!stale.usable && !stale.unique_layout && !stale.candidate_layouts
         && stale.diagnostics.find("below 50%") != std::string::npos,
            "Stale ROOT/LOD pair was accepted as a usable reference");

    // The largest score alone is insufficient: a wrong orientation can win
    // while no candidate matches even half of a stale/corrupted height table.
    auto wrong = lod(false, 2);
    auto const start = body_offset(wrong, "MLVH");
    auto const count = get(wrong, start - 4) / 4;
    for (std::size_t i = count * 2 / 5; i < count; ++i)
      putf(wrong, start + i * 4, 1000000.f);
    auto const low = TerrainLodUpdater::inspectReference(root(false), wrong);
    require(!low.usable && !low.unique_layout && low.matches[2] > low.matches[0]
         && low.matches[2] > low.matches[1] && low.matches[2] > low.matches[3],
            "Wrong best layout bypassed the minimum confidence threshold");
    rejects([&] { (void)TerrainLodUpdater::update(root(false), wrong, root(false)); });

    auto const noise = noisy_flat_root(0.00025f);
    auto const noisy = TerrainLodUpdater::inspectReference(noise, lod(true));
    require(noisy.usable && noisy.equivalent_layouts && !noisy.unique_layout,
            "Sub-tolerance flat noise should remain equivalent, not unique");
    require(TerrainLodUpdater::update(noise, lod(true), noise) == lod(true),
            "Unchanged equivalent noisy reference rewrote MLVH bytes");
    auto const updated = TerrainLodUpdater::update(root(true), lod(true), noise);
    require(TerrainLodUpdater::update(noise, updated, noise) == updated,
            "Equivalent noisy update is not byte-idempotent");
    auto const excessive_noise = noisy_flat_root(0.002f);
    require(!TerrainLodUpdater::inspectReference(excessive_noise, lod(true)).usable,
            "Different tied reference heights were accepted");
    rejects([&] { (void)TerrainLodUpdater::update(root(true), lod(true), excessive_noise); });

    auto asymmetric_holes = noise;
    put(asymmetric_holes, 92 + 60, 1);
    require(!TerrainLodUpdater::inspectReference(asymmetric_holes, lod(true)).usable,
            "Flat height tolerance must not permit different hole masks");
    auto nonfinite = noise;
    put(nonfinite, 92 + 128 + 8, 0x7fc00000);
    rejects([&] { (void)TerrainLodUpdater::inspectReference(nonfinite, lod(true)); });
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
  Bytes mesh_lod(HoleCells const& holes, int layout = 0, bool complete_skirts = false, int requested_step = 4)
  {
    Bytes b=lod(false,layout), nodes(85*24), levels(80), indices;
    if (complete_skirts)
    {
      auto parts=bodies(b);
      auto& heights=parts["MLVH"];
      heights.resize(heights.size()+4);
      putf(heights,heights.size()-4,height(0,8)-32);
      parts["MLSI"].insert(parts["MLSI"].end(),{4,2}); // base vertex 516
      b.clear();
      for(auto name:{"MLHD","MLVH","MLSI","TEST"}) append(b,name,parts[name]);
    }
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
      auto step=std::min(requested_step,n.size);
      if (i==0 || i==1 || i==5 || i==21)
      {
        putf(levels,n.level*20,32.0f/(1<<n.level));
        put(levels,n.level*20+4,static_cast<std::uint32_t>(indices.size()/2));
      }
      auto first=indices.size()/2;
      put(nodes,i*24,static_cast<std::uint32_t>(first));
      for(int y=n.y;y<n.y+n.size;y+=step)
        for(int x=n.x;x<n.x+n.size;x+=step)
          if (!holes[y*128+x])
          {
            // Long edges at hole boundaries exercise boundary subdivision.
            std::uint16_t a=y*129+x, b=a+step, c=a+step*129, d=c+step;
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
        put(levels,p+12,complete_skirts?6:3);
        put(levels,p+16,static_cast<std::uint32_t>(indices.size()/2));
        emit(0);emit(33025);emit(129*4);
        if(complete_skirts) { emit(129*4);emit(33025);emit(33026); }
      }
    }
    append(b,"MLLL",levels);append(b,"MLND",nodes);append(b,"MLVI",indices);
    return b;
  }
  void check_mesh(Bytes const& b, HoleCells const& holes, bool original_skirts = true)
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
      if(original_skirts)
        require(get(levels,level*20+12)>=3 && index(skirt)==0 && index(skirt+1)==33025
          && index(skirt+2)==129*4,"Skirt list changed during hole repair");
      for (std::size_t i=skirt;i<skirt+get(levels,level*20+12);++i)
        require(index(i)<c.at("MLVH").size()/4,"Skirt index outside height table");
    }
  }
  void calibration_tests()
  {
    auto const reference_root=root(false), reference=mesh_lod({});
    auto current=reference;
    auto hp=body_offset(current,"MLVH"),header=body_offset(current,"MLHD");
    for(std::size_t i=0;i<bodies(current)["MLVH"].size()/4;++i)putf(current,hp+i*4,1000000.f);
    for(unsigned i=4;i<28;i+=4)putf(current,header+i,500.f+i);
    current[body_offset(current,"TEST")]=42;
    auto calibrated=TerrainLodUpdater::calibrateReference(reference_root,reference,current);
    auto expected=current;
    std::copy_n(reference.begin()+hp,bodies(reference)["MLVH"].size(),expected.begin()+hp);
    require(calibrated==expected,"Calibration changed non-height template bytes");
    require(TerrainLodUpdater::inspectReference(reference_root,calibrated).usable,
            "Calibrated template is not coherent with its reference ROOT");
    require(bodies(TerrainLodUpdater::update(reference_root,calibrated,root(false,5)))["MLVH"]
            ==bodies(lod(false,0,5))["MLVH"],"Calibrated template did not update from the current ROOT");
    auto liquid=current;
    append(liquid,"MLLN",{1,2,3});append(liquid,"MLLN",{4,5,6});
    auto with_liquid=TerrainLodUpdater::calibrateReference(reference_root,reference,liquid);
    auto liquid_expected=liquid;
    std::copy_n(reference.begin()+hp,bodies(reference)["MLVH"].size(),liquid_expected.begin()+hp);
    require(with_liquid==liquid_expected,"Calibration altered repeated liquid chunks");
    for(auto const* name:{"MLVI","MLND","MLLL","MLSI","MLHD"})
    {
      auto bad=current;bad[body_offset(bad,name)]^=1;
      rejects([&]{(void)TerrainLodUpdater::calibrateReference(reference_root,reference,bad);});
    }
    rejects([&]{(void)TerrainLodUpdater::calibrateReference(root(false,1000000),reference,current);});
    auto versioned=reference,versioned_current=current;Bytes version(4);put(version,0,18);
    append(versioned,"MVER",version);put(version,0,19);append(versioned_current,"MVER",version);
    rejects([&]{(void)TerrainLodUpdater::calibrateReference(reference_root,versioned,versioned_current);});
    auto changed_count=bodies(current);changed_count["MLVH"].resize(changed_count["MLVH"].size()+4);
    changed_count["MLSI"].resize(changed_count["MLSI"].size()+2);Bytes extra;
    for(auto const& [name,bytes]:changed_count)append(extra,name.c_str(),bytes);
    rejects([&]{(void)TerrainLodUpdater::calibrateReference(reference_root,reference,extra);});
  }
  void sparse_refinement_tests()
  {
    auto before=root(false),after=before,original=mesh_lod({},0,true,128);
    for(int cy=0;cy<16;++cy) for(int cx=0;cx<16;++cx)
      for(int row=0,v=0;row<17;++row)
        for(int col=0;col<(row%2?8:9);++col,++v)
        {
          int x=cx*16+col*2+row%2,y=cy*16+row;
          putf(before,92+(cy*16+cx)*724+136+v*4,float(x+3*y));
          float h=float(x+3*y)+120.f*std::sin(x*.04f)*std::cos(y*.03f)+14.f*std::sin(x*.21f+y*.09f);
          putf(after,92+(cy*16+cx)*724+136+v*4,h);
        }
    auto hp=body_offset(original,"MLVH"),header=body_offset(original,"MLHD");
    for(unsigned i=0;i<33025;++i)
    {
      auto v=i<16641?i:i-16641;unsigned side=i<16641?129:128,center=i<16641?0:1;
      putf(original,hp+i*4,float(2*(v%side)+center+3*(2*(v/side)+center)));
    }
    putf(original,hp+33025*4,-32);putf(original,hp+33026*4,24-32);
    putf(original,header+4,-32);putf(original,header+8,1024);
    auto result=TerrainLodUpdater::update(before,original,after);
    check_mesh(result,{},false);check_bounds(result);
    require(TerrainLodUpdater::update(after,result,after,nullptr,true)==result,
            "Sparse multi-pass terrain refinement is not byte-idempotent");
  }
  void refinement_tests()
  {
    sparse_refinement_tests();
    auto make_root=[](bool peaks, bool rough=false) {
      auto b=root(false);
      for(int cy=0;cy<16;++cy) for(int cx=0;cx<16;++cx)
        for(int row=0,v=0;row<17;++row)
          for(int col=0;col<(row%2?8:9);++col,++v)
          {
            int x=cx*16+col*2+row%2,y=cy*16+row;
            float h=float(x+3*y);
            if(peaks)
              for(auto center:{std::array<int,2>{32,16},std::array<int,2>{128,16},std::array<int,2>{0,4}})
              {
                auto weight=std::max(0.f,1.f-float(std::abs(x-center[0])+std::abs(y-center[1]))/6.f);
                h+=40.f*weight*weight;
              }
            if(rough) h+=(x&1)?128.f:-128.f;
            putf(b,92+(cy*16+cx)*724+136+v*4,h);
          }
      return b;
    };
    auto before=make_root(false), current=make_root(true), original=mesh_lod({},0,true);
    auto hp=body_offset(original,"MLVH"), sp=body_offset(original,"MLSI"), header=body_offset(original,"MLHD");
    auto affine=[](unsigned i) {
      if(i<16641) return float(2*(i%129)+6*(i/129));
      i-=16641; return float(2*(i%128)+1+3*(2*(i/128)+1));
    };
    for(unsigned i=0;i<33025;++i) putf(original,hp+i*4,affine(i));
    for(unsigned i=0;i<2;++i)
    {
      auto base=unsigned(original[sp+i*2])|(unsigned(original[sp+i*2+1])<<8);
      putf(original,hp+(33025+i)*4,affine(base)-32);
    }
    putf(original,header+4,-32); putf(original,header+8,1024);
    require(TerrainLodUpdater::update(before,original,before)==original,"Unchanged mesh was refined");
    std::string diagnostics;
    auto result=TerrainLodUpdater::update(before,original,current,&diagnostics);
    check_mesh(result,{},false); check_bounds(result);
    auto parts=bodies(result), old=bodies(original);
    require(parts["MLVI"].size()>old["MLVI"].size(),"Changed coast was not refined");
    require(parts["MLSI"].size()>old["MLSI"].size(),"Refined border did not split its skirts");
    require(parts["TEST"]==old["TEST"],"Refinement modified an opaque chunk");
    require(TerrainLodUpdater::update(current,result,current)==result,"Refinement is not byte-idempotent");
    require(TerrainLodUpdater::update(current,result,current,nullptr,true)==result,
            "Repeated explicit refinement is not byte-idempotent");
    auto height_patched=original;
    std::copy_n(parts["MLVH"].begin(),old["MLVH"].size(),height_patched.begin()+hp);
    std::copy(parts["MLHD"].begin(),parts["MLHD"].end(),height_patched.begin()+header);
    require(TerrainLodUpdater::update(current,height_patched,current)==height_patched,
            "Ordinary save changed already coherent authored topology");
    require(TerrainLodUpdater::update(current,height_patched,current,nullptr,true)==result,
            "Explicit repair did not refine an already height-updated sparse mesh");
    require(diagnostics.find("height refinement added triangles=")!=std::string::npos,"Missing refinement diagnostic");
    auto real=[](Bytes const& bytes,std::size_t p) { auto bits=get(bytes,p);float f;std::memcpy(&f,&bits,4);return f; };
    auto point=[](unsigned i) -> std::array<int,2> {
      return i<16641?std::array<int,2>{int(i%129)*2,int(i/129)*2}
          :std::array<int,2>{int((i-16641)%128)*2+1,int((i-16641)/128)*2+1};
    };
    auto const& ix=parts["MLVI"];auto const& levels=parts["MLLL"];auto const& heights=parts["MLVH"];
    auto index=[&](std::size_t p){return unsigned(ix.at(p*2))|(unsigned(ix.at(p*2+1))<<8);};
    auto cross=[](auto a,auto b,auto p){return (b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0]);};
    for(unsigned level=0;level<4;++level)
    {
      constexpr double tolerances[]{8,4,2,1};
      auto start=get(levels,level*20+4),count=get(levels,level*20+8);
      require(count/3<=8192,"Adaptive fixture exceeded its triangle budget");
      for(unsigned i=start;i<start+count;i+=3)
      {
        auto a=point(index(i)),b=point(index(i+1)),c=point(index(i+2));
        for(int y=std::min({a[1],b[1],c[1]});y<=std::max({a[1],b[1],c[1]});++y)
          for(int x=std::min({a[0],b[0],c[0]});x<=std::max({a[0],b[0],c[0]});++x)
          {
            if((x&1)!=(y&1))continue;
            std::array<int,2> p{x,y};
            if(cross(a,b,p)>0||cross(b,c,p)>0||cross(c,a,p)>0)continue;
            auto wa=double(cross(b,c,p))/cross(a,b,c),wb=double(cross(c,a,p))/cross(a,b,c);
            unsigned sample=(x&1)?16641+(y/2)*128+x/2:(y/2)*129+x/2;
            auto error=std::abs(real(heights,sample*4)-(wa*real(heights,index(i)*4)+wb*real(heights,index(i+1)*4)+(1-wa-wb)*real(heights,index(i+2)*4)));
            require(error<=tolerances[level]+1e-4,"Refined mesh exceeds the height-error bound");
          }
      }
    }
    // Structural mixed-level checks. They bound sampled disagreement and check
    // actual skirt coverage; they do not emulate the client's LOD selection.
    auto surface=[&](unsigned node,std::array<int,2> p) {
      auto const& nodes=parts["MLND"];
      auto start=get(nodes,node*24), count=get(nodes,node*24+4);
      for(unsigned i=start;i<start+count;i+=3)
      {
        auto a=point(index(i)),b=point(index(i+1)),c=point(index(i+2));
        if(cross(a,b,p)>0||cross(b,c,p)>0||cross(c,a,p)>0)continue;
        auto wa=double(cross(b,c,p))/cross(a,b,c),wb=double(cross(c,a,p))/cross(a,b,c);
        return wa*real(heights,index(i)*4)+wb*real(heights,index(i+1)*4)+(1-wa-wb)*real(heights,index(i+2)*4);
      }
      throw std::runtime_error("Mixed-level boundary is not covered by its node");
    };
    // A level-1 node beside a level-2 node, on x=128 in half-cell units.
    double node_gap=0;
    for(int y=0;y<=64;y+=2)
      node_gap=std::max(node_gap,std::abs(surface(2,{128,y})-surface(6,{128,y})));
    require(node_gap>0.001&&node_gap<=6.0001,
            "Coarse/fine boundary must exercise different surfaces within their combined sampled error bound");
    auto skirt_span=[&](unsigned level,int y) {
      double low=std::numeric_limits<double>::infinity(),high=-low;
      auto base=[&](unsigned v) {
        if(v<33025)return v;
        auto p=(v-33025)*2;
        return unsigned(parts["MLSI"].at(p))|(unsigned(parts["MLSI"].at(p+1))<<8);
      };
      auto start=get(levels,level*20+16),count=get(levels,level*20+12);
      for(unsigned i=start;i<start+count;i+=3)
        for(unsigned e=0;e<3;++e)
        {
          auto av=index(i+e),bv=index(i+(e+1)%3);
          auto a=point(base(av)),b=point(base(bv));
          if(a[0]||b[0]||y<std::min(a[1],b[1])||y>std::max(a[1],b[1]))continue;
          auto add=[&](double h){low=std::min(low,h);high=std::max(high,h);};
          if(a[1]==b[1]) { add(real(heights,av*4));add(real(heights,bv*4)); }
          else { auto t=double(y-a[1])/(b[1]-a[1]);add((1-t)*real(heights,av*4)+t*real(heights,bv*4)); }
        }
      require(std::isfinite(low)&&std::isfinite(high),"Mixed-level border has a gap in its skirts");
      return std::array<double,2>{low,high};
    };
    double border_gap=0;
    for(int y=0;y<=8;y+=2)
    {
      auto coarse=surface(0,{0,y}),fine=surface(21,{0,y});
      auto a=skirt_span(0,y),b=skirt_span(3,y);
      border_gap=std::max(border_gap,std::abs(coarse-fine));
      require(std::abs(a[1]-coarse)<1e-4&&std::abs(b[1]-fine)<1e-4,
              "Split skirt no longer follows its terrain edge");
      auto const& higher=coarse>=fine?a:b;
      auto lower=std::min(coarse,fine);
      require(higher[0]<=lower+1e-4&&higher[1]>=lower-1e-4,
              "Skirt does not cover the sampled coarse/fine border height difference");
    }
    require(border_gap>0.001,"Skirt fixture did not exercise different coarse/fine border heights");
    bool bounded=false;
    try { (void)TerrainLodUpdater::update(before,original,make_root(false,true)); }
    catch(std::runtime_error const& e) { bounded=std::string(e.what()).find("refinement triangle budget")!=std::string::npos; }
    require(bounded,"Excessive refinement did not fail at its triangle budget");
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

namespace
{
  void check_cut_surface(Bytes const& data, HoleCells const& holes)
  {
    check_mesh(data, holes, false);
    auto const parts = bodies(data);
    auto const& indices = parts.at("MLVI");
    auto const& levels = parts.at("MLLL");
    using Point = std::array<double, 2>;
    using Polygon = std::vector<Point>;
    auto index = [&](std::size_t i) { return unsigned(indices.at(i*2)) | (unsigned(indices.at(i*2+1)) << 8); };
    auto point = [](unsigned v) -> Point {
      require(v < 33025, "Cut terrain references a skirt vertex");
      return v < 16641 ? Point{double(v%129)*2, double(v/129)*2}
        : Point{double((v-16641)%128)*2+1, double((v-16641)/128)*2+1};
    };
    auto cross = [](Point a, Point b, Point p) {
      return (b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0]);
    };
    // Independent polygon clipping measures area, including intersections that
    // miss cell centres. The production cut uses separating axes instead.
    auto clip = [&](Polygon const& polygon, Point a, Point b) {
      Polygon output;
      if (polygon.empty()) return output;
      auto previous = polygon.back();
      auto d = cross(a, b, previous);
      for (auto p : polygon)
      {
        auto next = cross(a, b, p);
        if ((d <= 0) != (next <= 0))
        {
          auto t = d/(d-next);
          output.push_back({previous[0]+t*(p[0]-previous[0]), previous[1]+t*(p[1]-previous[1])});
        }
        if (next <= 0) output.push_back(p);
        previous = p; d = next;
      }
      return output;
    };
    auto area = [](Polygon const& polygon) {
      double value = 0;
      for (std::size_t i=0; i<polygon.size(); ++i)
      {
        auto a=polygon[i], b=polygon[(i+1)%polygon.size()];
        value += a[0]*b[1]-a[1]*b[0];
      }
      return std::abs(value)*0.5;
    };
    for (unsigned level=0; level<4; ++level)
    {
      std::vector<std::vector<Polygon>> cells(128*128);
      std::map<std::pair<unsigned,unsigned>, unsigned> edges;
      auto start=get(levels,level*20+4), count=get(levels,level*20+8);
      for (unsigned i=start; i<start+count; i+=3)
      {
        Polygon triangle{point(index(i)),point(index(i+1)),point(index(i+2))};
        for (unsigned e=0; e<3; ++e) ++edges[std::minmax(index(i+e),index(i+(e+1)%3))];
        int xmin=int(std::min({triangle[0][0],triangle[1][0],triangle[2][0]})/2);
        int xmax=int(std::ceil(std::max({triangle[0][0],triangle[1][0],triangle[2][0]})/2));
        int ymin=int(std::min({triangle[0][1],triangle[1][1],triangle[2][1]})/2);
        int ymax=int(std::ceil(std::max({triangle[0][1],triangle[1][1],triangle[2][1]})/2));
        for (int y=ymin; y<ymax; ++y) for (int x=xmin; x<xmax; ++x)
        {
          Point a{double(x*2),double(y*2)}, b{double(x*2),double(y*2+2)},
            c{double(x*2+2),double(y*2+2)}, d{double(x*2+2),double(y*2)};
          auto piece=clip(clip(clip(clip(triangle,a,b),b,c),c,d),d,a);
          if (area(piece)<=1e-8) continue;
          auto& existing=cells[y*128+x];
          for (auto const& other:existing)
          {
            auto overlap=piece;
            for (std::size_t e=0; e<other.size(); ++e)
              overlap=clip(overlap,other[e],other[(e+1)%other.size()]);
            require(area(overlap)<=1e-8,"Cut mesh contains positive-area triangle overlap");
          }
          existing.push_back(std::move(piece));
        }
      }
      for (unsigned i=0; i<cells.size(); ++i)
      {
        double covered=0;
        for (auto const& p:cells[i]) covered+=area(p);
        require(std::abs(covered-(holes[i]?0.:4.))<1e-8,
          "Cut mesh crosses an active hole or leaves solid cell area uncovered");
      }
      for (auto const& [edge, uses]:edges)
      {
        require(uses<=2,"Cut mesh has a non-manifold edge");
        if (uses==2) continue;
        auto a=point(edge.first), b=point(edge.second);
        bool horizontal=a[1]==b[1], vertical=a[0]==b[0];
        require(horizontal || vertical,"Cut mesh has an unmatched diagonal edge");
        int fixed=int(horizontal?a[1]:a[0]);
        if (fixed==0 || fixed==256) continue;
        require(fixed%2==0,"Cut boundary is not on the cell grid");
        int lo=int(horizontal?std::min(a[0],b[0]):std::min(a[1],b[1]));
        int hi=int(horizontal?std::max(a[0],b[0]):std::max(a[1],b[1]));
        for (int t=lo/2; t<hi/2; ++t)
        {
          int x=horizontal?t:fixed/2, y=horizontal?fixed/2:t;
          require(holes[y*128+x] != holes[(y-(horizontal?1:0))*128+x-(vertical?1:0)],
            "Cut mesh has a T-junction or an unmatched solid edge");
        }
      }
      // The original fixture has a complete skirt along x=0, y=0..4 cells.
      // Its solid intervals must survive, while the newly opened interval is gone.
      std::array<unsigned,4> skirt_coverage{};
      auto skirt_start=get(levels,level*20+16), skirt_count=get(levels,level*20+12);
      for (unsigned i=skirt_start; i<skirt_start+skirt_count; i+=3)
      {
        std::vector<unsigned> top;
        for (unsigned e=0;e<3;++e) if (index(i+e)<33025) top.push_back(index(i+e));
        if (top.size()!=2) continue;
        auto a=point(top[0]),b=point(top[1]);
        if (a[0] || b[0]) continue;
        for (int y=0;y<4;++y)
          if (std::min(a[1],b[1])<y*2+1 && std::max(a[1],b[1])>y*2+1) ++skirt_coverage[y];
      }
      for (unsigned y=0;y<4;++y)
        require(skirt_coverage[y]==(holes[y*128]?0u:1u),"Perimeter skirt bridges a hole or lost a solid interval");
    }
  }

  void hole_cut_tests()
  {
    HoleCells holes{};
    for (auto p : {std::array<int,2>{9,11}, {15,15}, {16,15}, {15,16}, {16,16},
                   {31,7}, {32,7}, {63,63}, {64,63}, {63,64}, {64,64}, {0,1}, {127,127}})
      holes[p[1]*128+p[0]]=true;
    for (int layout=0;layout<4;++layout)
    {
      auto before=root(false), current=with_holes(before,holes,layout);
      auto original=mesh_lod({},layout,true);
      auto result=TerrainLodUpdater::update(before,original,current);
      check_bounds(result);
      check_cut_surface(result,holes);
      require(bodies(result).at("TEST")==bodies(original).at("TEST"),"Hole cut changed opaque bytes");
      require(TerrainLodUpdater::update(current,result,current)==result,"Hole cut is not byte-idempotent");
    }
  }
}

int main(int argc, char** argv)
{
  try
  {
    reference_tests();
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
    auto oversized_parts = bodies(lod(false));
    oversized_parts["MLVH"].resize(65537 * 4);
    oversized_parts["MLSI"].resize((65537 - 33025) * 2);
    Bytes oversized_lod;
    for (auto const& [name, bytes] : oversized_parts) append(oversized_lod, name.c_str(), bytes);
    rejects([&] { (void)TerrainLodUpdater::inspectReference(root(false), oversized_lod); });
    rejects([&] { (void)TerrainLodUpdater::update(root(false), oversized_lod, root(false)); });
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
    // This fixture has no mesh index chunks, so it cannot cut or close holes.
    bool missing_mesh=false;
    try { (void)TerrainLodUpdater::update(root(false), lod(false), holes); }
    catch(std::runtime_error const& e) { missing_mesh=std::string(e.what()).find("without MLND/MLVI/MLLL")!=std::string::npos; }
    require(missing_mesh,"Changed holes without mesh indices were accepted");
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
    hole_cut_tests();
    calibration_tests();
    refinement_tests();
    std::cout << "PASS: reference inspection, flat noise, four layouts, skirts, preserved bytes, ambiguity, confidence, malformed input, "
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
