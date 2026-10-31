#include <noggit/formats/adt/UidRepair.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

using Repair = Noggit::Formats::ADT::UidRepair;
using Bytes = Repair::Bytes;
void require(bool ok, char const* message) { if (!ok) throw std::runtime_error(message); }
void put(Bytes& data, std::size_t at, std::uint32_t value) { std::memcpy(data.data()+at, &value, 4); }
void chunk(Bytes& data, char const* tag, Bytes const& payload)
{
  auto at = data.size();
  data.resize(at + 8 + payload.size());
  std::memcpy(data.data()+at, tag, 4);
  put(data, at+4, static_cast<std::uint32_t>(payload.size()));
  std::copy(payload.begin(), payload.end(), data.begin()+at+8);
}
Bytes file(char const* tag, std::size_t stride, std::uint32_t asset, std::uint32_t uid)
{
  Bytes result, version(4), entry(stride);
  put(version, 0, 18); chunk(result, "REVM", version);
  put(entry, 0, asset); put(entry, 4, uid);
  chunk(result, tag, entry);
  chunk(result, "ZZZZ", {1,2,3});
  return result;
}
void rejected(Bytes const& data, bool modern)
{
  bool failed = false;
  try { Repair::inspect(data, modern); } catch (std::exception const&) { failed = true; }
  require(failed, "Malformed input accepted");
}
int main(int argc, char** argv)
{
  try
  {
    Repair repair;
    auto first = file("FDDM",36,999999999,77); // nonexistent asset must not require loading
    auto duplicate = first;
    auto other = file("FDDM",36,999999998,77);
    auto differentPosition = first;
    put(differentPosition, 28, 1234);
    auto originals = first;
    auto records = Repair::inspect(first,true);
    require(records.size()==1 && records[0].asset.fileId==999999999, "FDID lost");
    repair.rewrite(first, records);
    repair.rewrite(duplicate, Repair::inspect(duplicate,true));
    repair.rewrite(other, Repair::inspect(other,true));
    repair.rewrite(differentPosition, Repair::inspect(differentPosition,true));
    require(first==duplicate, "Shared tile placement got different UID");
    require(Repair::word(first,24)!=Repair::word(other,24), "UID collision not fixed");
    require(Repair::word(first,24)!=Repair::word(differentPosition,24), "Transform collision not fixed");
    for (std::size_t i=0;i<first.size();++i)
      if (i<24 || i>=28) require(first[i]==originals[i], "Non-UID byte changed");
    auto lod = file("DDLM",36,999999999,77);
    repair.rewrite(lod, Repair::inspect(lod,true));
    require(Repair::word(lod,24)==Repair::word(first,24), "OBJ1 doodad UID differs");
    auto wmo = file("FDOM",64,123,77);
    auto lodWmo = file("DMLM",40,123,77);
    repair.rewrite(wmo,Repair::inspect(wmo,true));
    repair.rewrite(lodWmo,Repair::inspect(lodWmo,true));
    require(Repair::word(wmo,24)==Repair::word(lodWmo,24), "OBJ1 WMO UID differs");
    require(Repair::word(wmo,24)!=Repair::word(first,24), "M2/WMO collision");
    auto primarySets=file("FDOM",64,123,888);
    auto lodSets=file("DMLM",40,123,888);
    put(primarySets,20+56,0x0001008c); // primary MWDR entry 1
    put(lodSets,20+32,0x0000008e);     // LOD MWDR entry 0, same selected sets
    Bytes primaryRanges(16),lodRanges(8);
    put(primaryRanges,8,1);put(primaryRanges,12,2);put(lodRanges,4,1);
    chunk(primarySets,"RDWM",primaryRanges);chunk(primarySets,"SDWM",{9,0,1,0,2,0});
    chunk(lodSets,"RDWM",lodRanges);chunk(lodSets,"SDWM",{1,0,2,0});
    require(Repair::inspect(primarySets,true)[0].identity==Repair::inspect(lodSets,true)[0].identity,
      "Tile-local MWDS indices caused a false LOD orphan");
    auto differentSets=lodSets;differentSets.back()=1;
    require(Repair::inspect(primarySets,true)[0].identity!=Repair::inspect(differentSets,true)[0].identity,
      "Different WMO doodad sets were merged");
    auto invalidSets=lodSets;put(invalidSets,20+32,0x0001008e);rejected(invalidSets,true);
    auto legacy = file("FDDM",36,0,999);
    chunk(legacy,"DIMM",Bytes(4));
    std::string missing = "World\\Missing.M2";
    Bytes text(missing.begin(),missing.end()); text.push_back(0);
    chunk(legacy,"XDMM",text);
    auto legacyRecords = Repair::inspect(legacy,false);
    require(legacyRecords[0].asset.path=="world/missing.m2","Legacy filename not resolved");
    auto indexed = file("FDDM",36,0,999);
    Bytes offset(4); put(offset,0,4); chunk(indexed,"DIMM",offset);
    std::string alias = "World\\Missing.MDX";
    Bytes names = {'x','x','x',0};
    names.insert(names.end(),alias.begin(),alias.end()); names.push_back(0);
    chunk(indexed,"XDMM",names);
    require(Repair::inspect(indexed,false)[0].asset.path=="world/missing.m2",
            "MMID offset or MDX normalization ignored");
    chunk(indexed,"ZZZZ",{4,5,6}); // bounded unknown chunks may repeat
    require(Repair::inspect(indexed,false).size()==1,"Repeated unknown chunk rejected");
    Repair secondPass;
    auto idempotent = first;
    secondPass.rewrite(idempotent,Repair::inspect(idempotent,true));
    require(idempotent==first,"Repeat repair changed first assigned UID");
    auto invalid = legacy;
    put(invalid,20,4000000000u); rejected(invalid,false);
    invalid=legacy; invalid.back()='x'; rejected(invalid,false);
    invalid=first; put(invalid,16,0xffffffff); rejected(invalid,true);
    invalid=first; invalid.pop_back(); rejected(invalid,true);
    rejected(file("FDDM",35,1,2),true);
    rejected({},true);
    rejected(first,false);
    // Optional read-only corpus check. No ADTs are written.
    std::size_t checked = 0, count = 0, changed = 0;
    Repair corpusRepair;
    if(argc==4 && std::string(argv[1])=="--pair")
    {
      auto read=[](char const* name) {
        std::ifstream stream(name,std::ios::binary);
        require(stream.good(),"Cannot open UID fixture");
        return Bytes((std::istreambuf_iterator<char>(stream)),{});
      };
      auto primary=read(argv[2]),secondary=read(argv[3]);
      std::map<std::string,std::string> identities;
      auto key=[](Bytes const& b,Repair::Record const& record) {
        return record.identity.substr(0,record.identity.find('\0'))+":"+std::to_string(Repair::word(b,record.uidOffset));
      };
      for(auto const& r:Repair::inspect(primary,true))identities.emplace(key(primary,r),r.identity);
      std::size_t matched=0;
      for(auto const& r:Repair::inspect(secondary,true))
        if(auto it=identities.find(key(secondary,r));it!=identities.end())
        {
          require(it->second==r.identity,"Original OBJ0/OBJ1 identity differs for a shared instance");
          ++matched;
        }
      require(matched>0,"No shared fixture placements");
      std::cout<<"PASS original OBJ0/OBJ1 placement identities: "<<matched<<'\n';
    }
    if (argc==2)
      for (auto const& entry : std::filesystem::recursive_directory_iterator(argv[1]))
      {
        if (!entry.is_regular_file() || entry.path().extension()!=".adt") continue;
        std::ifstream input(entry.path(),std::ios::binary);
        Bytes data((std::istreambuf_iterator<char>(input)),{});
        auto records = Repair::inspect(data,true);
        auto patched = data;
        corpusRepair.rewrite(patched, records);
        auto restored = patched;
        for (auto const& record : records)
          std::memcpy(restored.data()+record.uidOffset, data.data()+record.uidOffset, 4);
        require(restored==data, "Corpus: bytes outside UID fields changed");
        require(Repair::inspect(patched,true).size()==records.size(), "Corpus: placement count changed");
        if (patched!=data) ++changed;
        count += records.size(); ++checked;
      }
    std::cout << "PASS: collisions, shared placements, missing FDID/path, OBJ1, byte preservation, malformed inputs";
    if (argc==2) std::cout << "; corpus files=" << checked << ", placements=" << count
                          << ", files patched in memory=" << changed;
    std::cout << '\n';
    return 0;
  }
  catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
