#include <noggit/formats/adt/RetailObjectTables.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace Noggit::Formats::ADT::RetailObjectTables;

void check(bool ok, char const* why) { if (!ok) throw std::runtime_error(why); }
void put(Bytes& b, std::size_t at, std::uint32_t n) { std::memcpy(b.data()+at, &n, 4); }
void chunk(Bytes& b, char const* tag, Bytes const& data)
{
  auto at=b.size(); b.resize(at+8+data.size());
  std::memcpy(b.data()+at,tag,4);put(b,at+4,static_cast<std::uint32_t>(data.size()));
  std::copy(data.begin(),data.end(),b.begin()+at+8);
}
Bytes rows(bool lod, bool wmo, std::initializer_list<unsigned> ids)
{
  auto stride=wmo?(lod?40u:64u):36u;
  Bytes b(stride*ids.size());std::size_t at=0;
  for(auto id:ids) {put(b,at,100+id);put(b,at+4,id);at+=stride;}
  return b;
}
Bytes fixture(bool lod, std::initializer_list<unsigned> ids, Bytes const& metadata)
{
  Bytes b;
  chunk(b,lod?"DDLM":"FDDM",rows(lod,false,ids));
  chunk(b,lod?"DMLM":"FDOM",rows(lod,true,ids));
  chunk(b,"BMLM",metadata);
  if(lod)
  {
    chunk(b,"BDLM",metadata);
    chunk(b,"XDLM",Bytes(ids.size()*28));
    chunk(b,"XMLM",Bytes(ids.size()*28));
  }
  chunk(b,"ZZZZ",{11,22,33});
  return b;
}
Bytes payload(Bytes const& b,char const* tag)
{
  auto c=chunks(b).at(tag);
  return Bytes(b.begin()+c.offset,b.begin()+c.offset+c.size);
}
template<class F> void rejects(F&& action)
{
  bool rejected=false;try{action();}catch(std::runtime_error const&){rejected=true;}
  check(rejected,"Unsafe Retail object tables accepted");
}
int main(int argc, char** argv)
{
  try
  {
    if(argc == 2)
    {
      std::size_t checked=0, rejected=0;
      for(auto const& file:std::filesystem::recursive_directory_iterator(argv[1]))
      {
        if(!file.is_regular_file())continue;
        auto name=file.path().filename().string();
        bool lod=name.ends_with("_obj1.adt");
        if(!lod && !name.ends_with("_obj0.adt"))continue;
        ++checked;
        try
        {
          std::ifstream stream(file.path(),std::ios::binary);
          if(!stream)throw std::runtime_error("Cannot open fixture");
          Bytes bytes((std::istreambuf_iterator<char>(stream)),{});
          validate(bytes,lod);
        }
        catch(std::exception const& e){++rejected;std::cerr<<name<<": "<<e.what()<<'\n';}
      }
      std::cout<<"Retail object validation: checked="<<checked<<", rejected="<<rejected<<'\n';
      return checked && !rejected ? 0 : 1;
    }
    if(argc != 1)throw std::runtime_error("Usage: retail_object_tables [fixture directory]");
    for(bool lod:{false,true})
    {
      auto original=fixture(lod,{1,2,3},{0x19,0x40,0x80});
      auto sorted=fixture(lod,{3,1,2},{0x19,0x40,0x80});
      auto result=remap(original,sorted,lod);
      check(payload(result,"BMLM")==Bytes({0x80,0x19,0x40}),"MLMB lost instance association");
      if(lod)check(payload(result,"BDLM")==Bytes({0x80,0x19,0x40}),"MLDB lost instance association");
      check(payload(result,"ZZZZ")==Bytes({11,22,33}),"Unknown chunk modified");
      check(remap(result,result,lod)==result,"Repeated remap is not stable");
      auto removed=remap(original,fixture(lod,{2},{0,0,0}),lod);
      check(payload(removed,"BMLM")==Bytes({0x40}),"Removed WMO retained a stale byte");
      rejects([&]{validate(fixture(lod,{1,2},{0x40}),lod);});
      rejects([&]{remap(original,fixture(lod,{1,4},{0,0,0}),lod);});
      rejects([&]{remap(fixture(lod,{1,1},{0x19,0x80}),fixture(lod,{1},{0}),lod);});
      auto referenced=original;chunk(referenced,"RDWM",Bytes(8));chunk(referenced,"SDWM",Bytes(2));
      auto changed=referenced;auto c=chunks(changed).at(lod?"DMLM":"FDOM");
      changed[c.offset+(lod?34:58)]=1;
      rejects([&]{remap(referenced,changed,lod);});
      check(remap(referenced,referenced,lod)==referenced,"Stable MWDR/MWDS references changed");
    }
    auto malformed=fixture(true,{1},{0x40});
    Bytes ranges(48);put(ranges,0,0xFFFFFFFF);put(ranges,12,2);chunk(malformed,"DFLM",ranges);
    rejects([&]{validate(malformed,true);});
    auto shortIndex=fixture(true,{1},{0x40});chunk(shortIndex,"ILDM",{});
    rejects([&]{validate(shortIndex,true);});
    std::cout<<"PASS Retail auxiliary tables: permutation, deletion, counts, missing metadata, MWDR/MWDS, MLFD overflow, MDLI\n";
    return 0;
  }
  catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
