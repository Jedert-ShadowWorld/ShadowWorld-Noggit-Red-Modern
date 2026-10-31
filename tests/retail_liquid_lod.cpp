// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/formats/adt/RetailLiquidLod.hpp>
#include <iostream>
#include <fstream>
#include <iterator>
#include <exception>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace Noggit::Formats::ADT::RetailLiquid;
namespace
{
  void check(bool v, char const* message) { if (!v) throw std::runtime_error(message); }
  template<class F> void rejects(F f)
  {
    try { f(); } catch (std::runtime_error const&) { return; }
    throw std::runtime_error("Invalid liquid accepted.");
  }
  Bytes water(unsigned vf = 0, float height = 12.f, unsigned object = 0)
  {
    Bytes b(3072+24+8);
    put(b,0,3072); put(b,4,1);
    put(b,3072,5,2); put(b,3074,object ? object : vf,2);
    putf(b,3076,vf == 2 ? 0.f : height); putf(b,3080,vf == 2 ? 0.f : height);
    b[3086] = b[3087] = 8;
    put(b,3088,3096);
    std::fill(b.begin()+3096,b.end(),255);
    auto p = b.size(); put(b,3092,static_cast<unsigned>(p));
    unsigned n = 81, strides[]{5,8,1,9,8,12}; b.resize(p+n*strides[vf]);
    for (unsigned i = 0; i < n; ++i)
    {
      if (vf != 2) putf(b,p+(vf >= 4 ? i*8+4 : i*4),height);
      if (vf == 0 || vf == 2 || vf == 3 || vf == 4)
        b[p+(vf == 0 ? n*4+i : vf == 2 ? i : vf == 3 ? n*8+i : i*8)] = 127;
    }
    return b;
  }
  Bytes root(Bytes const& liquid)
  {
    Bytes b; if (!liquid.empty()) append(b,"MH2O",liquid);
    Bytes mcnk(128); putf(mcnk,104,17066.666666667f); putf(mcnk,108,17066.666666667f);
    append(b,"MCNK",mcnk); return b;
  }
  Bytes lod()
  {
    Bytes b; append(b,"MLHD",Bytes(28));
    append(b,"MBMB",{3,4,5}); append(b,"UNKN",{8,7,6,5}); return b;
  }
  Bytes body(Bytes const& b, std::string const& name)
  {
    for (auto const& c : chunks(b)) if (c.name == name) return c.data;
    return {};
  }
  Bytes decode(Bytes const& in, std::size_t off, std::size_t size, std::size_t expected)
  {
    range(in,off,size); Bytes out; auto end = off+size;
    while (off < end)
    {
      check(end-off >= 2,"RLE header");
      unsigned control = (unsigned(in[off])<<8)|in[off+1]; off += 2;
      unsigned n = control&32767; check(n && n <= expected-out.size(),"RLE count");
      if (control&32768)
      { check(off < end,"RLE fill"); out.insert(out.end(),n,in[off++]); }
      else
      { check(n <= end-off,"RLE literal"); out.insert(out.end(),in.begin()+off,in.begin()+off+n); off += n; }
    }
    check(out.size() == expected,"RLE decoded length"); return out;
  }
  void validate(Bytes const& b)
  {
    unsigned expected = 0; Bytes indices;
    for (auto const& c : chunks(b))
    {
      if (c.name == "MLLD")
      {
        check(word(c.data,0) == 7 && word(c.data,8) == 0,"Retail liquid header");
        auto ds = word(c.data,4,2), as = word(c.data,6,2);
        (void)decode(c.data,12,ds,16384); (void)decode(c.data,12+ds,as,2048);
        check(c.data.size() >= 12+ds+as && c.data.size() < 16+ds+as,"Liquid padding");
      }
      if (c.name == "MLLN") { check(c.data.size() == 24,"MLLN size"); expected = word(c.data,4); }
      if (c.name == "MLLI") { check(c.data.size() == expected*2 && expected%3 == 0,"MLLI count"); indices = c.data; }
      if (c.name == "MLLV")
      {
        check(c.data.size()%12 == 0,"MLLV stride");
        for (std::size_t i = 0; i < indices.size(); i += 2)
          check(word(indices,i,2) < c.data.size()/12,"MLLI index");
        for (std::size_t i = 0; i < indices.size(); i += 6)
        {
          unsigned a = word(indices,i,2)*12, d = word(indices,i+2,2)*12, e = word(indices,i+4,2)*12;
          float cross = (real(c.data,d)-real(c.data,a))*(real(c.data,e+4)-real(c.data,a+4))
                      -(real(c.data,d+4)-real(c.data,a+4))*(real(c.data,e)-real(c.data,a));
          check(cross < 0,"Liquid triangle winding/area");
        }
      }
    }
  }
#ifdef _WIN32
  void low_stack_regression(Bytes const& terrain, Bytes const& objects)
  {
    struct Work
    {
      Bytes const& terrain;
      Bytes const& objects;
      Bytes originalLod, expectedLod, expectedObjects;
      std::exception_ptr failure;
    } work{terrain, objects, lod(), {}, {}, {}};
    work.expectedLod = rebuild(terrain, work.originalLod);
    work.expectedObjects = update_object_water(terrain, objects);
    // Exercise the production liquid routines with less stack than the editor's
    // caller chain leaves available. Large per-cell tables must live on the heap.
    auto run = [](void* parameter) -> DWORD
    {
      auto& w = *static_cast<Work*>(parameter);
      try
      {
        auto generated = rebuild(w.terrain, w.originalLod);
        check(generated == w.expectedLod, "Low-stack liquid generation changed bytes");
        check(rebuild(w.terrain, generated) == generated, "Low-stack liquid generation is not idempotent");
        check(update_object_water(w.terrain, w.objects) == w.expectedObjects,
          "Low-stack object liquid update changed bytes");
      }
      catch (...) { w.failure = std::current_exception(); }
      return 0;
    };
    auto thread = CreateThread(nullptr, 256*1024, run, &work,
      STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    check(thread != nullptr, "Cannot create low-stack liquid regression thread");
    auto const waited = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    check(waited == WAIT_OBJECT_0, "Cannot wait for low-stack liquid regression");
    if (work.failure) std::rethrow_exception(work.failure);
  }
#endif
}
int main(int argc, char** argv)
{
  try
  {
    default_objects[5] = 491;
    std::array<float,81> edge_heights{};
    edge_heights[10] = -9; edge_heights[1] = 23; edge_heights[80] = 400;
    check(active_bounds(edge_heights,1) == std::make_pair(-9.f,23.f),"Partial layer lost border bounds");
    for (unsigned vf = 0; vf <= 5; ++vf)
    {
      auto b = water(vf); auto cells = read(b);
      check(cells.size() == 64 && cells[0].heights[0] == (vf == 2 ? 0.f : 12.f),"MH2O format heights");
      check(cells[0].depth[0] == (vf == 1 || vf == 5 ? 255u : 127u),"MH2O format depth");
      validate(rebuild(root(b),lod()));
    }
    auto b = water(0,12,491); b[3084] = b[3085] = 255; b[3086] = b[3087] = 0;
    check(read(b).size() == 64,"LiquidObject effective rectangle");
    auto out = rebuild(root(b),lod()); validate(out);
    check(word(body(out,"MLLN"),14,2) == 491,"LiquidObject lost");
    check(body(out,"MLLV").size() == 48,"Flat water not merged");
    check(body(out,"MBMB") == body(lod(),"MBMB") && body(out,"UNKN") == body(lod(),"UNKN"),"Opaque/object chunks changed");
    check(rebuild(root(b),out) == out,"Water rebuild not idempotent");
    auto raised = rebuild(root(water(0,50)),out);
    check(real(body(raised,"MLHD"),20) == 50 && real(body(raised,"MLHD"),24) == 50,"Liquid bounds stale");
    auto removed = rebuild(root({}),out);
    check(body(removed,"MLLD").empty() && body(removed,"MLLN").empty(),"Deleted liquid survived");
    check(real(body(removed,"MLHD"),20) == 1 && real(body(removed,"MLHD"),24) == -1,"Empty bounds sentinel");
    b = water(); b[3096] &= ~1; out = rebuild(root(b),lod()); validate(out);
    auto data = body(out,"MLLD"); auto mask = decode(data,12+word(data,4,2),word(data,6,2),2048);
    check(!(mask[0]&1) && (mask[0]&2),"Liquid mask orientation/hole");
    b = water(); putf(b,3080,14); putf(b,3104,14);
    out = rebuild(root(b),lod()); validate(out);
    check(body(out,"MLLV").size() > 48,"Sloped samples dropped");
    // Stacked water survives as two independent surfaces with topmost opacity.
    b = water();
    auto instance = Bytes(b.begin()+3072,b.begin()+3096);
    put(instance,4,0x41a00000); put(instance,8,0x41a00000); // 20.f
    put(instance,16,0); put(instance,20,0);
    b.insert(b.begin()+3096,instance.begin(),instance.end());
    put(b,4,2); put(b,3088,3120); put(b,3092,3128);
    out = rebuild(root(b),lod()); validate(out);
    check(read(b).size() == 128 && body(out,"MLLV").size() == 8*12,"Stacked layers lost");
    data = body(out,"MLLD");
    check(decode(data,12,word(data,4,2),16384)[0] == 255,"Top layer opacity not selected");
    // Two appearances at the same height with overlapping bounding boxes:
    // use triangle coverage, not the bounding box, to recover converted LVFs.
    b = water(0,12,491);
    instance = Bytes(b.begin()+3072,b.begin()+3096);
    put(instance,2,550,2); put(instance,16,static_cast<unsigned>(b.size()+24)); put(instance,20,0);
    b.insert(b.begin()+3096,instance.begin(),instance.end());
    put(b,4,2); put(b,3088,3120); put(b,3092,3128);
    b[3120] &= ~1;
    auto mask_start = b.size(); b.resize(mask_start+8); b[mask_start] = 1;
    out = rebuild(root(b),lod());
    put(b,3074,0,2); put(b,3098,0,2);
    check(rebuild(root(b),out) == out,"LiquidObject inheritance changed across repeated saves");
    b = water(); put(b,3092,0xfffffff0); rejects([&]{(void)read(b);});
    b = water(); b[3086] = 9; rejects([&]{(void)read(b);});
    b = water(); put(b,3076,0x7fc00000); rejects([&]{(void)read(b);});
    b = water(); put(b,4,0xffffffff); rejects([&]{(void)read(b);});
    b = water(); put(b,3074,41,2); rejects([&]{(void)read(b);});
    Bytes opaque = lod(); append(opaque,"MASD",{});
    rejects([&]{(void)rebuild(root(water()),opaque);});
    Bytes random(32768);
    for (unsigned i = 0; i < random.size(); ++i) random[i] = static_cast<std::uint8_t>((i*31+i/257)%256);
    auto packed = compress(random);
    check(decode(packed,0,packed.size(),random.size()) == random,"RLE literal boundaries");
    Bytes obj, placement(36); put(placement,34,8,2);
    putf(placement,8,2); putf(placement,12,20); putf(placement,16,2);
    append(obj,"MLDD",placement); append(obj,"MLDL",Bytes(4)); append(obj,"MDLD",{9,8,7});
    auto updated = update_object_water(root(water()),obj);
    check(real(body(updated,"MLDL"),0) == 12,"Object water height not updated");
    check(body(updated,"MLDD") == placement && body(updated,"MDLD") == body(obj,"MDLD"),"Object identity/data modified");
    check(real(body(update_object_water(root({}),updated),"MLDL"),0) == 0,"Removed water leaves object height");
#ifdef _WIN32
    low_stack_regression(root(water()), obj);
#endif
    Bytes extents(28); putf(extents,8,-2); putf(extents,20,30); append(obj,"MLDX",extents);
    auto bounded = body(update_object_bounds(obj,lod()),"MLHD");
    check(real(bounded,12) == -2 && real(bounded,16) == 30,"Object bounds stale");
    if (argc == 2)
    {
      std::ifstream f(argv[1],std::ios::binary); check(bool(f),"Cannot read fixture");
      Bytes native(std::istreambuf_iterator<char>(f),{});
      validate(native);
    }
    std::cout << "PASS: Retail liquid layouts, masks, geometry, compression, objects and malformed data.\n";
    return 0;
  }
  catch (std::exception const& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
