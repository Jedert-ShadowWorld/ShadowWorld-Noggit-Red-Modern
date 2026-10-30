#include <noggit/formats/RetailWorldTables.hpp>
#include <iostream>
#include <fstream>
#include <iterator>
using namespace Noggit::Formats::RetailWorldTables;
void check(bool ok) { if (!ok) throw std::runtime_error("World table regression"); }
void put(Bytes& b, std::size_t p, std::uint32_t n) { std::memcpy(b.data()+p, &n, 4); }
std::size_t add(Bytes& b, char const* tag, std::size_t size)
{
  auto p = b.size(); b.resize(p+8+size);
  for (unsigned i=0; i<4; ++i) b[p+i]=tag[3-i];
  put(b,p+4,static_cast<std::uint32_t>(size)); return p+8;
}
template<class F> void rejects(F f)
{
  bool failed=false;
  try { f(); } catch (std::runtime_error const&) { failed=true; }
  check(failed);
}
int main(int argc, char** argv)
{
  Bytes wdt;
  put(wdt, add(wdt,"MVER",4),18);
  auto mphd=add(wdt,"MPHD",32); put(wdt,mphd,0x3ca); put(wdt,mphd+4,123);
  auto main=add(wdt,"MAIN",32768); put(wdt,main,0x81); put(wdt,main+4,42);
  auto maid=add(wdt,"MAID",131072); put(wdt,maid,999);
  auto unknown=add(wdt,"TEST",4); put(wdt,unknown,333);
  std::vector<std::uint32_t> tiles(4096); tiles[1]=1;
  auto result=updateWdt(wdt,0xa,tiles);
  check(result.size()==wdt.size() && read(result,mphd)==0x3ca);
  check(read(result,mphd+4)==123 && read(result,main)==0x80 && read(result,main+4)==42);
  check(read(result,main+8)==1 && read(result,maid)==999 && read(result,unknown)==333);
  auto damaged=wdt; damaged.pop_back(); rejects([&]{updateWdt(damaged,0,tiles);});

  Bytes wdl;
  put(wdl,add(wdl,"MVER",4),18);
  auto objects=add(wdl,"MSSO",16); put(wdl,objects,987);
  auto maof=add(wdl,"MAOF",16384);
  auto mare=add(wdl,"MARE",1090); put(wdl,maof,static_cast<std::uint32_t>(mare-8));
  auto maho=add(wdl,"MAHO",32); put(wdl,maho,0xffff);
  auto maoe=add(wdl,"MAOE",8); put(wdl,maoe,456);
  auto otherMare=add(wdl,"MARE",1090); put(wdl,maof+7*4,static_cast<std::uint32_t>(otherMare-8));
  auto otherMaho=add(wdl,"MAHO",32); put(wdl,otherMaho,0x12345678);
  auto offsets=wdlOffsets(wdl); auto patched=wdl;
  std::int16_t outer[289]{},inner[256]{}; outer[0]=120; inner[255]=-50;
  WdlHoles holes{}; holes[2]=0x8001;
  updateWdlTile(patched,offsets,0,outer,inner,holes);
  check(patched.size()==wdl.size());
  for (std::size_t i=0;i<wdl.size();++i)
    if ((i<mare || i>=mare+1090) && (i<maho || i>=maho+32)) check(patched[i]==wdl[i]);
  check(read(patched,maho)==0 && read(patched,maho+4)==0x8001);
  check(read(patched,otherMaho)==0x12345678 && read(patched,maoe)==456 && read(patched,objects)==987);
  auto identical=patched;
  updateWdlTile(patched,offsets,0,outer,inner,holes);
  check(patched==identical);
  rejects([&]{updateWdlTile(patched,offsets,1,outer,inner,holes);});
  put(wdl,maof+4,static_cast<std::uint32_t>(mare-8));
  rejects([&]{wdlOffsets(wdl);});
  put(wdl,maof+4,static_cast<std::uint32_t>(mare));
  rejects([&]{wdlOffsets(wdl);});
  put(wdl,maof+4,0);
  auto malformed=wdl; put(malformed,maho-4,30);
  rejects([&]{wdlOffsets(malformed);});
  auto duplicate=wdl;
  Bytes secondMaho; add(secondMaho,"MAHO",32);
  duplicate.insert(duplicate.begin()+maho+32,secondMaho.begin(),secondMaho.end());
  put(duplicate,maof+7*4,static_cast<std::uint32_t>(otherMare-8+secondMaho.size()));
  rejects([&]{wdlOffsets(duplicate);});

  Bytes withoutHoles;
  put(withoutHoles,add(withoutHoles,"MVER",4),18);
  auto noHoleOffsets=add(withoutHoles,"MAOF",16384);
  auto noHoleMare=add(withoutHoles,"MARE",1090);
  put(withoutHoles,noHoleOffsets,static_cast<std::uint32_t>(noHoleMare-8));
  add(withoutHoles,"TEST",4);
  auto beforeFailure=withoutHoles;
  auto noHoleIndex=wdlOffsets(withoutHoles);
  rejects([&]{updateWdlTile(withoutHoles,noHoleIndex,0,outer,inner,holes);});
  check(withoutHoles==beforeFailure); // Missing MAHO must not partially patch MARE.
  updateWdlTile(withoutHoles,noHoleIndex,0,outer,inner,WdlHoles{});
  check(withoutHoles.size()==beforeFailure.size()); // Absent MAHO means no holes.

  Bytes root;
  put(root,add(root,"MVER",4),18);
  add(root,"MHDR",64);
  std::array<std::size_t,256> chunkOffsets{};
  for (unsigned y=0;y<16;++y)
    for (unsigned x=0;x<16;++x)
    {
      auto p=add(root,"MCNK",128); chunkOffsets[y*16+x]=p;
      put(root,p+4,x); put(root,p+8,y);
    }
  put(root,chunkOffsets[2*16+3]+60,0xffff); // Fully empty low-resolution chunk.
  auto partial=chunkOffsets[2*16+4]; put(root,partial,0x10000);
  put(root,partial+20,0x00550055); put(root,partial+24,0x00550055);
  // One hole per 2x2 cell group becomes 0xffff in the editor's lossy mask,
  // but the remaining terrain must still appear in the coarse WDL.
  auto full=chunkOffsets[2*16+5]; put(root,full,0x10000);
  put(root,full+20,0xffffffff); put(root,full+24,0xffffffff);
  put(root,chunkOffsets[2*16+6]+60,0x7fff);
  WdlHoles expected{}; expected[2]=(1u<<3)|(1u<<5);
  check(wdlHoles(root)==expected);
  auto invalidRoot=root; put(invalidRoot,chunkOffsets.back()+4,14);
  rejects([&]{wdlHoles(invalidRoot);});

  // Occlusion is a 17x17 lower envelope, not the WDL's 545 height samples.
  Bytes occ;
  put(occ,add(occ,"MVER",4),18);
  auto oi=add(occ,"MAOI",24);
  put(occ,oi,33u | (29u<<16)); put(occ,oi+8,578);
  put(occ,oi+12,34u | (29u<<16)); put(occ,oi+16,578); put(occ,oi+20,578);
  auto oh=add(occ,"MAOH",1156);
  std::int16_t high=200;
  for (unsigned i=0;i<578;++i) std::memcpy(occ.data()+oh+i*2,&high,2);
  put(occ,add(occ,"TEST",4),0x12345678);
  Bytes terrain;
  put(terrain,add(terrain,"MVER",4),18); add(terrain,"MHDR",64);
  for (unsigned y=0;y<16;++y)
    for (unsigned x=0;x<16;++x)
    {
      auto p=add(terrain,"MCNK",128+8+580);
      put(terrain,p+4,x); put(terrain,p+8,y);
      float base=100; std::memcpy(terrain.data()+p+112,&base,4);
      std::memcpy(terrain.data()+p+128,"TVCM",4); put(terrain,p+132,580);
      if (x==4 && y==8)
      {
        // An interior depression is invisible to corner-only sampling.
        float depression=-125.25f; std::memcpy(terrain.data()+p+136+72*4,&depression,4);
      }
      if (x==10 && y==12) { put(terrain,p,0x10000); put(terrain,p+20,1); }
    }
  auto oiOffsets=occlusionOffsets(occ);
  auto originalOcc=occ;
  check(lowerOcclusionTile(occ,oiOffsets,29*64+33,terrain)==289);
  auto sample=[&](unsigned x,unsigned y) {
    std::int16_t value; std::memcpy(&value,occ.data()+oh+(y*17+x)*2,2); return value;
  };
  check(sample(0,0)==100 && sample(16,16)==100);
  check(sample(4,8)==-26 && sample(5,9)==-26 && sample(6,9)==100);
  check(sample(10,12)==-32768 && sample(11,13)==-32768);
  for (std::size_t i=0;i<occ.size();++i)
    if (i<oh || i>=oh+578) check(occ[i]==originalOcc[i]);
  check(lowerOcclusionTile(occ,oiOffsets,29*64+33,terrain)==0);
  check(lowerOcclusionTile(occ,oiOffsets,0,Bytes{})==0);
  auto badOcc=originalOcc; put(badOcc,oi+16,2);
  rejects([&]{occlusionOffsets(badOcc);});
  badOcc=originalOcc; put(badOcc,oi+20,1090);
  rejects([&]{occlusionOffsets(badOcc);});
  badOcc=originalOcc; put(badOcc,oi+12,read(badOcc,oi));
  rejects([&]{occlusionOffsets(badOcc);});
  badOcc=originalOcc; put(badOcc,oi+16,1156);
  rejects([&]{occlusionOffsets(badOcc);});
  auto badTerrain=terrain; badTerrain.pop_back();
  auto beforeOcclusionFailure=occ;
  rejects([&]{lowerOcclusionTile(occ,oiOffsets,29*64+33,badTerrain);});
  check(occ==beforeOcclusionFailure);
  badTerrain=terrain; put(badTerrain,12+72+8+136,0x7fc00000);
  rejects([&]{lowerOcclusionTile(occ,oiOffsets,29*64+33,badTerrain);});
  check(occ==beforeOcclusionFailure);
  invalidRoot=root; invalidRoot.resize(invalidRoot.size()-136);
  rejects([&]{wdlHoles(invalidRoot);});

  check(wdlHeight(32767.0f)==32767 && wdlHeight(-32768.0f)==-32768);
  check(wdlHeight(12.75f)==12 && wdlHeight(-12.75f)==-12);
  check(wdlHeight(std::nextafter(32767.0f,0.0f))==32766);
  check(wdlHeight(std::nextafter(-32768.0f,0.0f))==-32767);
  rejects([&]{wdlHeight(std::nextafter(32767.0f,std::numeric_limits<float>::infinity()));});
  rejects([&]{wdlHeight(std::nextafter(-32768.0f,-std::numeric_limits<float>::infinity()));});
  rejects([&]{wdlHeight(std::numeric_limits<float>::quiet_NaN());});
  rejects([&]{wdlHeight(std::numeric_limits<float>::infinity());});
  rejects([&]{wdlHeight(-std::numeric_limits<float>::infinity());});
  if (argc == 3)
  {
    auto load = [](char const* path) {
      std::ifstream f(path, std::ios::binary);
      if (!f) throw std::runtime_error("Cannot open fixture");
      return Bytes(std::istreambuf_iterator<char>(f), {});
    };
    auto realWdt=load(argv[1]);
    auto list=chunks(realWdt);
    auto realMain=required(list,"MAIN",32768);
    auto realMphd=required(list,"MPHD",32);
    for (unsigned i=0;i<4096;++i) tiles[i]=read(realWdt,realMain.payload+i*8);
    check(updateWdt(realWdt,read(realWdt,realMphd.payload),tiles)==realWdt);
    auto realWdl=load(argv[2]); auto original=realWdl;
    auto realOffsets=wdlOffsets(realWdl);
    for (auto const& [tile,p] : realOffsets)
    {
      std::memcpy(outer,original.data()+p.heights,578);
      std::memcpy(inner,original.data()+p.heights+578,512);
      WdlHoles originalHoles{};
      if (p.holes) std::memcpy(originalHoles.data(),original.data()+*p.holes,32);
      updateWdlTile(realWdl,realOffsets,tile,outer,inner,originalHoles);
    }
    check(realWdl==original);
    std::cout << "Real WDT/WDL identity check passed; tiles=" << realOffsets.size() << '\n';
  }
  std::cout << "Retail WDT metadata and scoped WDL heights/holes checks passed\n";
}
