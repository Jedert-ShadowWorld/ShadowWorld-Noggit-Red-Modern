#include <noggit/formats/adt/UidRepairGeometry.hpp>
#include <noggit/formats/adt/RetailObjectTransfer.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/ModelHeaders.h>
#include <iostream>

using namespace Noggit::Formats::ADT;
using Bytes=UidRepair::Bytes;
void check(bool ok,char const* message){if(!ok)throw std::runtime_error(message);}
template<class T> void put(Bytes& b,std::size_t at,T value){std::memcpy(b.data()+at,&value,sizeof(value));}
void chunk(Bytes& b,char const* tag,Bytes const& p)
{
  auto at=b.size();b.resize(at+8+p.size());std::memcpy(b.data()+at,tag,4);
  put(b,at+4,static_cast<std::uint32_t>(p.size()));std::copy(p.begin(),p.end(),b.begin()+at+8);
}
Bytes payload(Bytes const& b,char const* tag)
{
  for(std::size_t at=0;at<b.size();)
  {auto n=UidRepair::word(b,at+4);if(!std::memcmp(b.data()+at,tag,4))return Bytes(b.begin()+at+8,b.begin()+at+8+n);at+=8+n;}
  throw std::runtime_error("Missing test chunk");
}
Bytes model()
{
  ModelHeader h{};std::memcpy(h.id,"MD20",4);
  h.bounding_box_min=glm::vec3(-4);h.bounding_box_max=glm::vec3(4);
  Bytes b(sizeof(h));put(b,0,h);return b;
}
Bytes objects(bool populate)
{
  Bytes b,v(4),records;put(v,0,18u);chunk(b,"REVM",v);
  if(populate)
  {
    ENTRY_MDDF good{};good.nameID=1;good.uniqueID=88;good.pos[0]=TILESIZE;good.pos[2]=100;good.scale=1024;
    records.resize(72);put(records,0,good);good.nameID=13;put(records,36,good);
  }
  chunk(b,"FDDM",records);chunk(b,"FDOM",{});
  for(int n=0;n<256;++n) {Bytes c;chunk(c,"DRCM",{});chunk(c,"WRCM",{});chunk(b,"KNCM",c);}
  chunk(b,"ZZZZ",{10,20,30});return b;
}
int main()
{
  try
  {
    std::vector<UidTile> input{{0,0,"map_0_0_obj0.adt",objects(true)},{1,0,"map_1_0_obj0.adt",objects(false)}};
    auto original=input[0].objects;
    auto read=[](UidRepair::Reference const& asset){if(asset.fileId==13)throw std::runtime_error("Missing");return model();};
    auto plan=planUidGeometry(input,true,true,read,{},{});
    check(input[0].objects==original,"Planning mutated input");
    check(plan.removed==1 && plan.highest==1 && plan.problems.size()==1,"Missing placement not reported/removed");
    auto a=payload(plan.tiles[0].objects,"FDDM"),b=payload(plan.tiles[1].objects,"FDDM");
    check(a.size()==36 && a==b,"Cross-tile placement/reference not rebuilt");
    check(UidRepair::word(a,4)==1,"UID not assigned");
    check(payload(plan.tiles[0].objects,"ZZZZ")==Bytes({10,20,30}),"Unknown chunk changed");
    std::size_t references=0;
    for(auto const& tile:plan.tiles)
      for(std::size_t at=0;at<tile.objects.size();)
      {
        auto n=UidRepair::word(tile.objects,at+4);
        if(!std::memcmp(tile.objects.data()+at,"KNCM",4))
        {
          Bytes c(tile.objects.begin()+at+8,tile.objects.begin()+at+8+n);
          auto r=payload(c,"DRCM");check(r.size()%4==0,"Invalid references");
          for(std::size_t i=0;i<r.size();i+=4){check(UidRepair::word(r,i)==0,"Bad placement index");++references;}
        }
        at+=8+n;
      }
    check(references>=2 && references<256,"Chunk coverage not recalculated");
    // Partial repair keeps client-only bytes and UIDs stable. Unrelated missing
    // assets in those tiles must not be loaded or proposed for removal.
    auto scoped = input;
    scoped[1].editable = false;
    scoped[1].objects = objects(true);
    auto localAt = UidRepair::inspect(scoped[0].objects, true)[0].uidOffset;
    auto remoteRows = UidRepair::inspect(scoped[1].objects, true);
    put(scoped[0].objects, localAt + 4, 100.f); // safely inside tile 0
    for (auto const& row : remoteRows)
      put(scoped[1].objects, row.uidOffset + 4, TILESIZE + 100.f);
    put(scoped[1].objects, remoteRows[1].uidOffset - 4, 14u);
    int reads = 0;
    auto countedRead = [&](UidRepair::Reference const& asset) { ++reads; return read(asset); };
    // The local invalid model also lives inside tile 0.
    put(scoped[0].objects, UidRepair::inspect(scoped[0].objects, true)[1].uidOffset + 4, 100.f);
    auto partial = planUidGeometry(scoped, true, true, countedRead, {}, {}, true);
    check(partial.tiles[1].objects == scoped[1].objects, "Partial repair rewrote a client-only tile");
    check(partial.sourceUids == std::vector<std::uint32_t>{88}
      && partial.targetUids == std::vector<std::uint32_t>{89}, "Partial repair reused a reserved UID");
    check(partial.removed == 1 && reads == 2, "Partial repair processed unrelated client-only assets");
    auto repeated = planUidGeometry(partial.tiles, true, true, read, {}, {}, true);
    check(repeated.tiles[0].objects == partial.tiles[0].objects
      && repeated.tiles[1].objects == scoped[1].objects, "Partial repair is not idempotent");
    check(repeated.sourceUids == repeated.targetUids, "Partial repair renumbered a valid UID");
    // Boundary references are added without selecting unrelated neighbour objects.
    auto boundary = input;
    boundary[1].editable = false;
    boundary[1].objects = scoped[1].objects;
    // Give unrelated neighbours authored chunk membership, independent of bounds.
    Bytes remoteWithRefs;
    bool firstCell=true;
    for (std::size_t at=0;at<boundary[1].objects.size();)
    {
      auto const& bytes=boundary[1].objects;
      auto size=UidRepair::word(bytes,at+4);
      if (firstCell && !std::memcmp(bytes.data()+at,"KNCM",4))
      {
        Bytes cell;chunk(cell,"DRCM",{0,0,0,0,1,0,0,0});chunk(cell,"WRCM",{});
        chunk(remoteWithRefs,"KNCM",cell);firstCell=false;
      }
      else remoteWithRefs.insert(remoteWithRefs.end(),bytes.begin()+at,bytes.begin()+at+8+size);
      at+=8+size;
    }
    boundary[1].objects=std::move(remoteWithRefs);
    boundary.push_back({2,0,"map_2_0_obj0.adt",objects(false)});
    boundary[2].editable = false;
    reads = 0;
    auto boundaryPlan = planUidGeometry(boundary,true,true,countedRead,{},{},true);
    auto merged = payload(boundaryPlan.tiles[1].objects,"FDDM");
    auto untouched = payload(boundary[1].objects,"FDDM");
    check(merged.size()==108 && std::equal(untouched.begin(),untouched.end(),merged.begin()),
      "Neighbour placements were modified or removed");
    check(reads==2 && boundaryPlan.tiles[2].objects==boundary[2].objects,
      "Boundary repair expanded to unrelated objects or tiles");
    check(payload(payload(boundaryPlan.tiles[1].objects,"KNCM"),"DRCM")==Bytes({0,0,0,0,1,0,0,0}),
      "Unrelated neighbour chunk membership changed");
    check(boundary[0].objects==original,"Boundary planning mutated input");
    auto invalidShared=input;
    invalidShared[1].editable=false;
    invalidShared[1].objects=original;
    auto removedShared=planUidGeometry(invalidShared,true,true,read,{},{},true);
    check(removedShared.removed==1 && payload(removedShared.tiles[1].objects,"FDDM").size()==36,
      "Removed selected placement left a dangling neighbour copy");
    auto boundaryAgain=planUidGeometry(boundaryPlan.tiles,true,true,read,{},{},true);
    check(boundaryAgain.tiles[1].objects==boundaryPlan.tiles[1].objects,
      "Neighbour repair is not idempotent");
    // Existing shared references are valid, and both copies retain the UID.
    auto shared = plan.tiles;
    shared[1].editable = false;
    auto sharedPlan = planUidGeometry(shared, true, true, read, {}, {}, true);
    check(sharedPlan.tiles[1].objects == shared[1].objects
      && sharedPlan.sourceUids == sharedPlan.targetUids, "Shared client reference was renumbered");
    // A new destination has no matching local rows or dictionaries. Transfer the
    // exact source LOD flags, payload and level together with the primary record.
    auto withLod=input;
    for(auto& tile:withLod)
    {
      tile.hasLod=true;
      chunk(tile.lod,"REVM",payload(tile.objects,"REVM"));
      chunk(tile.lod,"DDLM",payload(tile.objects,"FDDM"));
      chunk(tile.lod,"DMLM",{});
      auto size=payload(tile.objects,"FDDM").size()/36;
      chunk(tile.lod,"XDLM",Bytes(size*28));chunk(tile.lod,"XMLM",{});
      Bytes groups(48);put(groups,20,static_cast<std::uint32_t>(size));
      chunk(tile.lod,"DFLM",groups);
      if(size)
      {
        chunk(tile.lod,"BDLM",{12,32});
        Bytes index(8);put(index,4,1u);chunk(tile.lod,"ILDM",index);
        Bytes definitions(64,0x77);definitions[0]=0x55;chunk(tile.lod,"DLDM",definitions);
        Bytes levels(8);put(levels,0,17u);put(levels,4,29u);chunk(tile.lod,"LDLM",levels);
      }
    }
    auto lodPlan=planUidGeometry(withLod,true,true,read,{},{});
    for(auto const& tile:lodPlan.tiles)
    {
      auto metadata=RetailObjectTransfer::read(tile.lod,true);
      check(metadata.size()==1,"Missing model was not removed from LOD");
      auto const& row=metadata.begin()->second;
      check(row.byte==12 && row.lod==17 && row.levels==4,"Cross-tile LOD data not transferred");
      check(row.definition && row.definition->size()==32 && row.definition->at(0)==0x55,
        "Cross-tile MDLD definition not transferred");
      check(UidRepair::word(row.record,4)==1,"Primary/LOD UID mismatch");
    }
    check(withLod[0].objects==original,"LOD transfer mutated source files");
    auto neighbourLod = withLod;
    neighbourLod[1].editable = false;
    neighbourLod[1].objects = scoped[1].objects;
    neighbourLod[1].lod = withLod[0].lod;
    auto remoteLodRows=UidRepair::inspect(neighbourLod[1].lod,true);
    for (auto const& row : remoteLodRows)
      put(neighbourLod[1].lod,row.uidOffset+4,TILESIZE+100.f);
    put(neighbourLod[1].lod,remoteLodRows[1].uidOffset-4,14u);
    auto neighbourLodPlan=planUidGeometry(neighbourLod,true,true,read,{},{},true);
    auto originalMetadata=RetailObjectTransfer::read(neighbourLod[1].lod,true);
    auto mergedMetadata=RetailObjectTransfer::read(neighbourLodPlan.tiles[1].lod,true);
    check(mergedMetadata.size()==3,"Neighbour LOD placements lost");
    for (auto const& entry : originalMetadata)
    {
      auto const& old=entry.second;
      auto found=std::find_if(mergedMetadata.begin(),mergedMetadata.end(),[&](auto const& row) {
        return row.second.record==old.record;
      });
      check(found!=mergedMetadata.end(),"Unrelated LOD record changed");
      auto const& kept=found->second;
      check(kept.byte==old.byte && kept.lod==old.lod && kept.definition==old.definition
        && kept.levels==old.levels,"Unrelated LOD metadata changed");
    }
    auto neighbourLodAgain=planUidGeometry(neighbourLodPlan.tiles,true,true,read,{},{},true);
    check(neighbourLodAgain.tiles[1].lod==neighbourLodPlan.tiles[1].lod,
      "Neighbour LOD repair is not idempotent");
    auto scopedLod = lodPlan.tiles;
    scopedLod[1].editable = false;
    auto scopedLodPlan = planUidGeometry(scopedLod,true,true,read,{},{},true);
    check(scopedLodPlan.tiles[1].objects == scopedLod[1].objects
      && scopedLodPlan.tiles[1].lod == scopedLod[1].lod
      && scopedLodPlan.sourceUids == scopedLodPlan.targetUids,
      "Partial repair changed shared client-only OBJ0/OBJ1 or their UID");
    auto colocated=withLod;
    auto nearAt=RetailObjectTables::chunks(colocated[0].objects).at("FDDM").offset+36;
    auto farAt=RetailObjectTables::chunks(colocated[0].lod).at("DDLM").offset+36;
    put(colocated[0].objects,nearAt,1u);put(colocated[0].objects,nearAt+4,89u);
    put(colocated[0].objects,nearAt+34,std::uint16_t(0x240));
    put(colocated[0].lod,farAt,1u);put(colocated[0].lod,farAt+4,89u);
    put(colocated[0].lod,farAt+34,std::uint16_t(0x260));
    auto cp=planUidGeometry(colocated,true,true,read,{},{});
    check(cp.highest==2 && cp.sourceUids==std::vector<std::uint32_t>({88,89}),
      "Co-located source UIDs merged");
    for(auto const& tile:cp.tiles)
    {
      auto rows=RetailObjectTransfer::read(tile.lod,true);
      check(rows.size()==2,"Co-located LOD instance lost");
      for(auto const& [at,row]:rows)
      {
        auto uid=UidRepair::word(row.record,4);
        check(row.byte==(uid==1?12:32) && row.lod==(uid==1?17:29),
          "Co-located metadata crossed between distinct UIDs");
      }
    }
    // Renumbering must keep per-instance WMO bytes attached to the same rows.
    auto withWmos=objects(false);
    ENTRY_MODF wa{};wa.nameID=101;wa.uniqueID=444;
    wa.pos[0]=wa.pos[2]=100;wa.scale=1024;wa.flags=0x0c;
    auto wb=wa;wb.nameID=102;wb.uniqueID=777;
    Bytes wmos(128);put(wmos,0,wa);put(wmos,64,wb);
    Bytes modern;
    for(std::size_t at=0;at<withWmos.size();)
    {
      auto n=UidRepair::word(withWmos,at+4);
      if(!std::memcmp(withWmos.data()+at,"FDOM",4))chunk(modern,"FDOM",wmos);
      else modern.insert(modern.end(),withWmos.begin()+at,withWmos.begin()+at+8+n);
      at+=8+n;
    }
    chunk(modern,"BMLM",{0x19,0x80});
    auto wmoReader=[](UidRepair::Reference const&)
    {
      Bytes result,v(4),h(64);put(v,0,17u);chunk(result,"REVM",v);
      put(h,36,glm::vec3(-4));put(h,48,glm::vec3(4));chunk(result,"DHOM",h);
      return result;
    };
    auto wp=planUidGeometry({{0,0,"wmo_obj0.adt",modern}},true,true,wmoReader,{},{});
    check(payload(wp.tiles[0].objects,"BMLM")==Bytes({0x19,0x80}),"UID repair lost WMO auxiliary bytes");
    auto savedWmos=payload(wp.tiles[0].objects,"FDOM");
    check(UidRepair::word(savedWmos,4)==1 && UidRepair::word(savedWmos,68)==2,"WMO UIDs not rebuilt");
    auto bad=model();float nan=std::numeric_limits<float>::quiet_NaN();
    put(bad,offsetof(ModelHeader,bounding_box_min),nan);
    bool rejected=false;try{readUidAssetBounds(bad,false);}catch(...){rejected=true;}check(rejected,"NaN bounds accepted");
    Bytes wrapped;chunk(wrapped,"MD21",model());
    check(readUidAssetBounds(wrapped,false).max.x==4,"MD21 not decoded");
    // Legacy rewrite: offsets, records and MCRF references must agree.
    Bytes legacy,v(4);put(v,0,18u);chunk(legacy,"REVM",v);
    chunk(legacy,"RDHM",Bytes(sizeof(MHDR)));chunk(legacy,"NICM",Bytes(sizeof(MCIN)));
    chunk(legacy,"XDMM",{'a','.','m','2',0});chunk(legacy,"DIMM",Bytes(4));
    chunk(legacy,"OMWM",{});chunk(legacy,"DIWM",{});
    ENTRY_MDDF e{};e.uniqueID=8;e.pos[0]=100;e.pos[2]=100;e.scale=1024;
    Bytes rec(sizeof(e));put(rec,0,e);chunk(legacy,"FDDM",rec);chunk(legacy,"FDOM",{});
    for(unsigned n=0;n<256;++n)
    {
      MapChunkHeader h{};h.ix=n%16;h.iy=n/16;
      Bytes cell(sizeof(h));put(cell,0,h);chunk(cell,"FRCM",{});chunk(legacy,"KNCM",cell);
    }
    auto lp=planUidGeometry({{0,0,"legacy.adt",legacy}},false,false,read,{},{});
    auto const& output=lp.tiles[0].objects;
    auto mhdr=payload(output,"RDHM"),mcin=payload(output,"NICM");
    auto mddf=UidRepair::word(mhdr,28)+20;
    check(!std::memcmp(output.data()+mddf,"FDDM",4),"MHDR offsets stale");
    for(int n=0;n<256;++n)
    {
      auto offset=UidRepair::word(mcin,n*16);
      check(!std::memcmp(output.data()+offset,"KNCM",4),"MCIN offset stale");
      check(UidRepair::word(mcin,n*16+4)==UidRepair::word(output,offset+4)+8,"MCIN size stale");
    }
    std::cout<<"PASS: planning isolation, removal/report, M2/MD21 bounds, cross-tile and chunk references, legacy offsets, unknown chunks, malformed bounds\n";
    return 0;
  }
  catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
