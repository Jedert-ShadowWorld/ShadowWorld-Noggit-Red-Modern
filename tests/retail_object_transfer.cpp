#include <noggit/formats/adt/RetailObjectTransfer.hpp>
#include <iostream>
#include <filesystem>
#include <fstream>

using namespace Noggit::Formats::ADT::RetailObjectTransfer;
void check(bool value, char const* message) { if (!value) throw std::runtime_error(message); }
void chunk(Bytes& file, char const* tag, Bytes const& data)
{
  auto at=file.size();file.resize(at+8+data.size());
  std::memcpy(file.data()+at,tag,4);store(file,at+4,static_cast<std::uint32_t>(data.size()));
  std::copy(data.begin(),data.end(),file.begin()+at+8);
}
Bytes payload(Bytes const& file,char const* tag)
{
  auto c=chunks(file).at(tag);
  return Bytes(file.begin()+c.offset,file.begin()+c.offset+c.size);
}
template<class F> void rejects(F action)
{
  bool rejected=false;try{action();}catch(std::exception const&){rejected=true;}
  check(rejected,"Invalid transfer accepted");
}
Row row(bool wmo,unsigned uid,unsigned levels,float radius,bool lod=true)
{
  Row r;r.data.wmo=wmo;r.data.levels=static_cast<std::uint8_t>(levels);
  r.data.record.resize(wmo?(lod?40:64):36);
  store(r.data.record,0,100u+uid);store(r.data.record,4,uid);
  store<std::uint16_t>(r.data.record,wmo?(lod?32:56):34,wmo?0x8c:0x60);
  if(wmo)r.data.sets={1,2};
  r.extent={-1,-2,-3,1,2,3,radius};return r;
}
int main(int argc, char** argv)
{
  try
  {
    Bytes destination;chunk(destination,"ZZZZ",{7,8,9});
    for(bool lod:{false,true})
    {
      auto a=row(true,1,1,5,lod),b=row(true,2,4,15,lod),c=row(true,3,4,10,lod);
      a.data.byte=25;b.data.byte=76;
      b.data.sets={7,8,9};c.data.sets=b.data.sets;
      // Source indices intentionally collide; only the decoded values are portable.
      store<std::uint16_t>(b.data.record,lod?34:58,0);
      auto result=write(destination,{a,b,c},lod);
      validate(result,lod);
      auto decoded=read(result,lod);
      std::map<unsigned,Instance> byId;
      for(auto const& [offset,data]:decoded)byId.emplace(word(data.record,4),data);
      check(byId.at(1).sets==a.data.sets && byId.at(2).sets==b.data.sets
        && byId.at(3).sets==c.data.sets,"WMO sets changed across local index collision");
      check(payload(result,"RDWM").size()==16 && payload(result,"SDWM").size()==10,
        "WMO selectors were not deduplicated");
      check(byId.at(1).byte==25 && byId.at(2).byte==76 && byId.at(3).byte==128,
        "MLMB/default lost association");
      check(payload(result,"ZZZZ")==Bytes({7,8,9}),"Unknown destination chunk changed");
      auto removed=write(result,{c},lod);
      check(payload(removed,"RDWM").size()==8 && payload(removed,"SDWM").size()==6
        && payload(removed,"BMLM")==Bytes({128}),"Removed metadata retained");
      if(lod)
      {
        check(byId.at(1).levels==1 && byId.at(2).levels==4,"Mixed LOD membership changed");
        check(word(payload(result,"DMLM"),4)==1 && word(payload(result,"DMLM"),44)==2,
          "WMO group/radius ordering changed");
      }
    }
    auto a=row(false,1,1,5),b=row(false,2,2,15),c=row(false,3,4,10),d=row(false,4,4,20);
    a.data.definition=Bytes(32,11);b.data.definition=Bytes(32,22);
    c.data.definition=a.data.definition;
    a.data.lod=13;b.data.lod=27;
    a.data.byte=12;b.data.byte=32;
    auto result=write(destination,{c,b,a,d},true);
    auto decoded=read(result,true);
    std::map<unsigned,Instance> byId;
    std::vector<Row> repeated;
    for(auto const& [at,data]:decoded)
    {
      byId.emplace(word(data.record,4),data);
      auto bounds=chunks(result).at("XDLM").offset+(at-chunks(result).at("DDLM").offset)/36*28;
      Row r{data,{}};std::memcpy(r.extent.data(),result.data()+bounds,28);repeated.push_back(r);
    }
    check(byId.at(1).definition==a.data.definition && byId.at(2).definition==b.data.definition
      && byId.at(3).definition==a.data.definition && !byId.at(4).definition,"MDLI points to a foreign MDLD record");
    check(payload(result,"DLDM").size()==64,"MDLD payloads were not deduplicated");
    check(byId.at(1).lod==13 && byId.at(2).lod==27 && byId.at(4).lod==0,"MLDL lost association");
    check(byId.at(1).byte==12 && byId.at(3).byte==64,"MLDB/default lost association");
    check(write(result,repeated,true)==result,"Transfer is not byte-stable");
    auto group=payload(result,"DFLM");
    check(word(group,0)==0 && word(group,4)==1 && word(group,8)==2
      && word(group,12)==1 && word(group,16)==1 && word(group,20)==2,"Three LOD ranges not rebuilt");
    check(word(payload(result,"DDLM"),2*36+4)==4,"LOD radius sorting lost");
    auto bad=result;
    store(bad,chunks(bad).at("ILDM").offset,2u);
    rejects([&]{read(bad,true);});
    auto shortDefinition=destination;chunk(shortDefinition,"DLDM",Bytes(31));
    rejects([&]{validate(shortDefinition,true);});
    // Shared level memberships must remain contiguous, not be flattened to one level.
    auto overlap=write(destination,{row(false,1,1,5),row(false,2,3,4),row(false,3,2,3)},true);
    auto membership=payload(overlap,"DFLM");
    check(word(membership,12)==2 && word(membership,16)==2,"Overlapping LOD membership lost");
    auto impossible=destination;
    rejects([&]{write(impossible,{row(false,1,3,5),row(false,2,5,4),row(false,3,6,3)},true);});
    auto bounds=extent({10,20,30,40,50,60});
    check(bounds[0]==17066.666f-60 && bounds[2]==20 && bounds[5]==50
      && std::abs(bounds[6]-std::sqrt(2700.f))<.01f,"LOD bounds coordinate conversion");
    Bytes primaryTemplate;
    chunk(primaryTemplate,"FDDM",{});chunk(primaryTemplate,"FDOM",{});chunk(primaryTemplate,"KNCM",{});
    auto imported=row(true,1,1,1,false);imported.data.byte=25;
    auto ordered=write(primaryTemplate,{imported},false);
    std::vector<std::string> tags;
    for(std::size_t at=0;at<ordered.size();at+=8+word(ordered,at+4))
      tags.emplace_back(reinterpret_cast<char const*>(ordered.data()+at),4);
    check(tags==std::vector<std::string>({"FDDM","FDOM","BMLM","RDWM","SDWM","KNCM"}),
      "Imported dictionaries placed after MCNK");
    Bytes lodTemplate;
    chunk(lodTemplate,"DDLM",{});chunk(lodTemplate,"XDLM",{});chunk(lodTemplate,"DMLM",{});chunk(lodTemplate,"XMLM",{});
    auto orderedLod=write(lodTemplate,{a,b},true);
    auto positions=chunks(orderedLod);
    check(positions.at("DFLM").offset<positions.at("DDLM").offset
      && positions.at("BDLM").offset<positions.at("DMLM").offset
      && positions.at("ILDM").offset<positions.at("DLDM").offset
      && positions.at("DLDM").offset<positions.at("DMLM").offset,"Imported OBJ1 chunks in wrong order");
    // One shared runtime WMO can be referenced by unrelated tile-local selectors.
    auto shared=row(true,154955,1,3,false);shared.data.byte=25;shared.data.sets={1,4,5,8};
    auto neighbor=row(true,100,1,2,false);neighbor.data.sets={2,3};
    auto firstTile=write(destination,{neighbor,shared},false);
    auto secondTile=write(destination,{shared},false);
    auto source=readWmoSources(secondTile,{});
    auto rawSave=firstTile;
    auto modfAt=chunks(rawSave).at("FDOM").offset+64;
    store<std::uint16_t>(rawSave,modfAt+58,0); // shared instance came from secondTile
    auto allSources=readWmoSources(firstTile,{});
    allSources.at(154955)=source.at(154955);
    auto saved=writePrimary(firstTile,rawSave,allSources);
    auto savedSources=readWmoSources(saved,{});
    check(savedSources.at(154955).primary.sets==shared.data.sets,
      "Ordinary save replaced semantic doodad sets with a foreign local selector");
    check(savedSources.at(154955).primary.byte==25,"Ordinary save lost local opaque WMO metadata");
    check(writePrimary(saved,saved,allSources)==saved,"Ordinary WMO save is not stable");
    auto selected=rawSave;
    store<std::uint16_t>(selected,modfAt+56,0x0c);
    store<std::uint16_t>(selected,modfAt+58,7);
    allSources.at(154955).primary.sets.clear();
    auto direct=readWmoSources(writePrimary(firstTile,selected,allSources),{}).at(154955).primary;
    check(direct.sets.empty() && !(word(direct.record,56)&0x80)
      && (word(direct.record,56)>>16)==7,"Explicit single-set edit was ignored");
    auto copied=shared;store(copied.data.record,4,999u);
    auto copiedFile=write(destination,{copied},false);
    auto copySources=source;copySources.emplace(999,source.at(154955));
    auto copiedSaved=readWmoSources(writePrimary(destination,copiedFile,copySources),{});
    check(copiedSaved.at(999).primary.sets==shared.data.sets
      && copiedSaved.at(999).primary.byte==25,"Copied WMO lost source semantic or opaque metadata");
    rejects([&]{writePrimary(firstTile,rawSave,{});});
    auto wrongSource=copySources;store(wrongSource.at(999).primary.record,0,42u);
    rejects([&]{writePrimary(destination,copiedFile,wrongSource);});
    auto unknownLod=copySources;unknownLod.at(999).lodKnown=false;
    rejects([&]{writePrimary(destination,copiedFile,unknownLod);});
    auto authoredLod=copySources;authoredLod.at(999).lod=row(true,154955,4,3).data;
    rejects([&]{writePrimary(destination,copiedFile,authoredLod,false);});
    if (argc == 2)
    {
      auto readFile=[](std::filesystem::path const& path)
      { std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Cannot open corpus file");return Bytes((std::istreambuf_iterator<char>(input)),{}); };
      WmoSources sharedSources;
      std::vector<std::filesystem::path> files;
      for(auto const& file:std::filesystem::recursive_directory_iterator(argv[1]))
      {
        if(!file.is_regular_file() || !file.path().filename().string().ends_with("_obj0.adt"))continue;
        files.push_back(file.path());
        for(auto const& [uid,data]:readWmoSources(readFile(file.path()),{}))sharedSources.emplace(uid,data);
      }
      std::size_t foreignSelectors=0,checkedWmos=0;
      for(auto const& file:files)
      {
        auto original=readFile(file),edited=original;
        auto local=readWmoSources(original,{});
        auto table=chunks(edited);
        for(std::size_t i=0;i<count(table,"FDOM",64);++i)
        {
          auto at=table.at("FDOM").offset+i*64;
          if(!(word(edited,at+56)&0x80))continue;
          auto const& sourceRecord=sharedSources.at(word(edited,at+4)).primary.record;
          auto sharedIndex=static_cast<std::uint16_t>(word(sourceRecord,56)>>16);
          foreignSelectors+=sharedIndex!=(word(edited,at+56)>>16);
          store(edited,at+58,sharedIndex);
        }
        auto fixed=writePrimary(original,edited,sharedSources);
        auto after=readWmoSources(fixed,{});
        for(auto const& [uid,data]:local)
        {
          ++checkedWmos;
          check(after.at(uid).primary.sets==data.primary.sets,"Corpus semantic set mismatch");
          check(after.at(uid).primary.byte==data.primary.byte,"Corpus opaque byte mismatch");
        }
        check(writePrimary(fixed,fixed,sharedSources)==fixed,"Corpus save not idempotent");
      }
      check(!files.empty(),"Empty corpus");
      std::cout<<"Read-only ordinary-save corpus: files="<<files.size()<<", WMO references="<<checkedWmos<<", foreign selectors="<<foreignSelectors<<'\n';
    }
    else if(argc!=1)throw std::runtime_error("Usage: retail_object_transfer [read-only corpus directory]");
    std::cout<<"PASS Retail transfer: cross-tile selectors, opaque bytes, MDLD relocation/deduplication, three LOD groups, overlap, sorting, defaults, malformed references, stability\n";
    return 0;
  }
  catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
