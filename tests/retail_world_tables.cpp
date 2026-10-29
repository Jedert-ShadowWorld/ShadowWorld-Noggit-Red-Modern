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
  auto offsets=wdlOffsets(wdl); auto patched=wdl;
  std::int16_t outer[289]{},inner[256]{}; outer[0]=120; inner[255]=-50;
  updateWdlTile(patched,offsets,0,outer,inner);
  check(patched.size()==wdl.size());
  for (std::size_t i=0;i<wdl.size();++i)
    if (i<mare || i>=mare+1090) check(patched[i]==wdl[i]);
  check(read(patched,maho)==0xffff && read(patched,maoe)==456 && read(patched,objects)==987);
  rejects([&]{updateWdlTile(patched,offsets,1,outer,inner);});
  put(wdl,maof+4,static_cast<std::uint32_t>(mare-8));
  rejects([&]{wdlOffsets(wdl);});
  put(wdl,maof+4,static_cast<std::uint32_t>(mare));
  rejects([&]{wdlOffsets(wdl);});
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
      std::memcpy(outer,original.data()+p,578);
      std::memcpy(inner,original.data()+p+578,512);
      updateWdlTile(realWdl,realOffsets,tile,outer,inner);
    }
    check(realWdl==original);
    std::cout << "Real WDT/WDL identity check passed; tiles=" << realOffsets.size() << '\n';
  }
  std::cout << "Retail WDT metadata and WDL non-height data preserved\n";
}
