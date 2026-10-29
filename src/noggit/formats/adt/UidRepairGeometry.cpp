#include "UidRepairGeometry.hpp"
#include "RetailObjectTables.hpp"
#include "RetailObjectTransfer.hpp"
#include <noggit/MapHeaders.h>
#include <noggit/ModelHeaders.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <cmath>
#include <set>

namespace Noggit::Formats::ADT
{
  namespace
  {
    using Bytes = UidRepair::Bytes;
    struct Chunk { std::string tag; Bytes data; std::size_t oldOffset = 0; };
    template<class T> T read(Bytes const& b, std::size_t at)
    {
      if (at > b.size() || b.size()-at < sizeof(T)) throw std::runtime_error("Truncated data");
      T value; std::memcpy(&value,b.data()+at,sizeof(T)); return value;
    }
    template<class T> void put(Bytes& b, std::size_t at, T const& value)
    {
      if (at > b.size() || b.size()-at < sizeof(T)) throw std::runtime_error("Invalid output offset");
      std::memcpy(b.data()+at,&value,sizeof(T));
    }
    std::vector<Chunk> chunks(Bytes const& bytes, std::size_t begin = 0)
    {
      std::vector<Chunk> out;
      for (auto at=begin;at<bytes.size();)
      {
        auto size=read<std::uint32_t>(bytes,at+4);
        if (size>bytes.size()-at-8) throw std::runtime_error("Chunk outside file");
        out.push_back({std::string(reinterpret_cast<char const*>(bytes.data()+at),4),
          Bytes(bytes.begin()+at+8,bytes.begin()+at+8+size),at});
        at+=8+size;
      }
      return out;
    }
    Chunk* find(std::vector<Chunk>& c, std::string const& tag)
    { for (auto& x:c) if(x.tag==tag) return &x; return nullptr; }
    void replace(std::vector<Chunk>& c,std::string const& tag,Bytes data)
    { if(auto p=find(c,tag)) p->data=std::move(data); else c.push_back({tag,std::move(data)}); }
    Bytes pack(std::vector<Chunk> const& c)
    {
      Bytes out;
      for(auto const& x:c)
      {
        if(x.data.size()>UINT32_MAX) throw std::runtime_error("Chunk too large");
        auto at=out.size();out.resize(at+8+x.data.size());
        std::memcpy(out.data()+at,x.tag.data(),4);
        put(out,at+4,static_cast<std::uint32_t>(x.data.size()));
        std::copy(x.data.begin(),x.data.end(),out.begin()+at+8);
      }
      return out;
    }
    void valid(UidBounds const& b)
    {
      for(int i=0;i<3;++i)
        if(!std::isfinite(b.min[i]) || !std::isfinite(b.max[i]) || b.min[i]>b.max[i])
          throw std::runtime_error("Invalid/non-finite model bounds");
    }
    bool intersects(UidBounds const& b,float x,float z,float size)
    { return b.max.x>=x && b.min.x<=x+size && b.max.z>=z && b.min.z<=z+size; }
    UidBounds transform(UidBounds const& local,Bytes const& record,bool wmo,bool scaled)
    {
      auto pos=read<glm::vec3>(record,8), rot=read<glm::vec3>(record,20);
      for(int i=0;i<3;++i)
      {
        if(!std::isfinite(pos[i]) || !std::isfinite(rot[i])) throw std::runtime_error("Invalid placement transform");
        rot[i] -= std::floor((rot[i]+180.f)/360.f)*360.f;
        if(std::abs(rot[i])<1e-6f) rot[i]=0;
      }
      float scale=wmo && !scaled ? 1.f : read<std::uint16_t>(record,wmo?62:32)/1024.f;
      if(scale<=0) throw std::runtime_error("Invalid placement scale");
      auto matrix=glm::translate(glm::mat4(1),pos) *
        glm::eulerAngleYZX(glm::radians(rot.y-90.f),glm::radians(-rot.x),glm::radians(rot.z));
      matrix=glm::scale(matrix,glm::vec3(scale));
      UidBounds out{glm::vec3(std::numeric_limits<float>::max()),glm::vec3(std::numeric_limits<float>::lowest())};
      for(int i=0;i<8;++i)
      {
        glm::vec3 p{(i&1)?local.max.x:local.min.x,(i&2)?local.max.y:local.min.y,(i&4)?local.max.z:local.min.z};
        p=glm::vec3(matrix*glm::vec4(p.x,p.z,-p.y,1)); // same coordinates as SceneObject/aabb
        out.min=glm::min(out.min,p);out.max=glm::max(out.max,p);
      }
      valid(out);return out;
    }
    struct Placement
    {
      Bytes data;
      UidRepair::Reference asset;
      UidBounds bounds;
      bool wmo;
      std::uint32_t uid;
      RetailObjectTransfer::Instance metadata;
      std::optional<RetailObjectTransfer::Instance> lod;
    };
    Bytes indexBytes(std::vector<std::uint32_t> const& indices)
    { Bytes out(indices.size()*4);if(!out.empty())std::memcpy(out.data(),indices.data(),out.size());return out; }

    Bytes rebuildObjects(UidTile const& tile,std::vector<Placement const*> const& chosen,bool modern,
        std::map<std::uint32_t, RetailObjectTransfer::Instance> const& local)
    {
      if (modern) RetailObjectTables::validate(tile.objects, false);
      auto top=chunks(tile.objects);
      Bytes m2,wmo,mmdx,mmid,mwmo,mwid;
      std::vector<Placement const*> models,buildings;
      for(auto p:chosen)
      {
        auto data=p->data;
        auto& entries=p->wmo?wmo:m2;
        auto& names=p->wmo?mwmo:mmdx;
        auto& offsets=p->wmo?mwid:mmid;
        if(!modern)
        {
          if(p->asset.path.empty()) throw std::runtime_error("Legacy placement has no filename");
          put(data,0,static_cast<std::uint32_t>(offsets.size()/4));
          auto off=offsets.size();offsets.resize(off+4);put(offsets,off,static_cast<std::uint32_t>(names.size()));
          names.insert(names.end(),p->asset.path.begin(),p->asset.path.end());names.push_back(0);
        }
        entries.insert(entries.end(),data.begin(),data.end());
        (p->wmo?buildings:models).push_back(p);
      }
      replace(top,"FDDM",m2);replace(top,"FDOM",wmo);
      if(!modern)
      { replace(top,"XDMM",mmdx);replace(top,"DIMM",mmid);replace(top,"OMWM",mwmo);replace(top,"DIWM",mwid); }
      else
      {
        // Modern placements reference FDIDs; filename tables are dropped.
        top.erase(std::remove_if(top.begin(),top.end(),[](auto const& c)
          {return c.tag=="XDMM"||c.tag=="DIMM"||c.tag=="OMWM"||c.tag=="DIWM";}),top.end());
      }
      int n=0;
      std::set<std::pair<unsigned,unsigned>> coordinates;
      for(auto& c:top) if(c.tag=="KNCM")
      {
        if(n>=256) throw std::runtime_error("Too many object MCNKs");
        unsigned cx=n%16,cz=n/16;
        MapChunkHeader h{};
        if(!modern) { h=read<MapChunkHeader>(c.data,0);cx=h.ix;cz=h.iy; }
        if(cx>=16||cz>=16||!coordinates.emplace(cx,cz).second) throw std::runtime_error("Invalid MCNK coordinates");
        auto subs=chunks(c.data,modern?0:sizeof(h));
        std::vector<std::uint32_t> mr,wr;
        float x=(tile.x+cx/16.f)*TILESIZE,z=(tile.z+cz/16.f)*TILESIZE;
        for(std::size_t i=0;i<models.size();++i) if(intersects(models[i]->bounds,x,z,TILESIZE/16.f)) mr.push_back(static_cast<std::uint32_t>(i));
        for(std::size_t i=0;i<buildings.size();++i) if(intersects(buildings[i]->bounds,x,z,TILESIZE/16.f)) wr.push_back(static_cast<std::uint32_t>(i));
        if(modern) { replace(subs,"DRCM",indexBytes(mr));replace(subs,"WRCM",indexBytes(wr));c.data=pack(subs); }
        else
        {
          auto refs=mr;refs.insert(refs.end(),wr.begin(),wr.end());
          replace(subs,"FRCM",indexBytes(refs));
          h.nDoodadRefs=static_cast<std::uint32_t>(mr.size());h.nMapObjRefs=static_cast<std::uint32_t>(wr.size());
          std::size_t offset=8+sizeof(h);
          for(auto const& sub:subs)
          {
            auto at=static_cast<std::uint32_t>(offset);
            if(sub.tag=="TVCM")h.ofsHeight=at;else if(sub.tag=="RNCM")h.ofsNormal=at;
            else if(sub.tag=="YLCM")h.ofsLayer=at;else if(sub.tag=="FRCM")h.ofsRefs=at;
            else if(sub.tag=="LACM")h.ofsAlpha=at;else if(sub.tag=="HSCM")h.ofsShadow=at;
            else if(sub.tag=="ESCM")h.ofsSndEmitters=at;else if(sub.tag=="QLCM")h.ofsLiquid=at;
            else if(sub.tag=="VCCM")h.ofsMCCV=at;
            offset+=8+sub.data.size();
          }
          c.data.resize(sizeof(h));put(c.data,0,h);
          auto tail=pack(subs);c.data.insert(c.data.end(),tail.begin(),tail.end());
        }
        ++n;
      }
      if(n!=256) throw std::runtime_error("Object ADT must contain 256 MCNKs");
      if(!modern)
      {
        auto header=find(top,"RDHM"),index=find(top,"NICM");
        if(!header||header->data.size()!=sizeof(MHDR)||!index||index->data.size()!=sizeof(MCIN))
          throw std::runtime_error("Invalid legacy MHDR/MCIN");
        auto h=read<MHDR>(header->data,0);
        std::map<std::string,std::uint32_t> positions;
        std::uint32_t offset=0,cell=0;
        for(auto const& c:top)
        {
          positions[c.tag]=offset;
          if(c.tag=="KNCM") { put(index->data,cell*16,offset);put(index->data,cell*16+4,static_cast<std::uint32_t>(8+c.data.size()));++cell; }
          offset+=static_cast<std::uint32_t>(8+c.data.size());
        }
        auto pos=[&](char const* tag) {auto it=positions.find(tag);return it==positions.end()?0u:it->second-positions.at("RDHM")-8;};
        h.mcin=pos("NICM");h.mtex=pos("XETM");h.mmdx=pos("XDMM");h.mmid=pos("DIMM");
        h.mwmo=pos("OMWM");h.mwid=pos("DIWM");h.mddf=pos("FDDM");h.modf=pos("FDOM");
        h.mfbo=pos("OBFM");h.mh2o=pos("O2HM");h.mtxf=pos("FXTM");
        put(header->data,0,h);
      }
      auto result = pack(top);
      if (!modern) return result;
      std::vector<RetailObjectTransfer::Row> rows;
      for (auto p : chosen)
      {
        auto it = local.find(p->uid);
        auto data = it == local.end() ? p->metadata : it->second;
        std::copy_n(p->data.begin(), p->wmo ? 56 : 32, data.record.begin());
        rows.push_back({std::move(data), {}});
      }
      return RetailObjectTransfer::write(result, std::move(rows), false);
    }
  }

  // Merges the selected placements into a neighbour file. Other placements keep
  // their metadata and MCNK references; their assets are not loaded.
  static Bytes mergeNeighbour(Bytes const& original, Bytes const& repaired, bool lod,
      std::set<std::string> const& selected, UidCanonicalAsset const& canonical)
  {
    std::vector<RetailObjectTransfer::Row> rows;
    std::array<std::uint32_t, 2> counts{};
    std::array<std::array<std::vector<std::uint32_t>, 2>, 2> indices;
    bool removed = false;
    auto replacements = RetailObjectTransfer::read(repaired, lod);
    std::map<std::string, std::size_t> replacementOffsets;
    for (auto record : UidRepair::inspect(repaired, true))
    {
      if (canonical) canonical(record);
      replacementOffsets.emplace(record.identity, record.uidOffset - 4);
    }
    std::map<std::string, std::uint32_t> retainedIndices;
    std::array<std::set<std::uint32_t>, 2> retainedSlots;
    for (unsigned source = 0; source < 2; ++source)
    {
      auto const& bytes = source ? repaired : original;
      auto metadata = RetailObjectTransfer::read(bytes, lod);
      auto table = RetailObjectTables::chunks(bytes);
      for (auto record : UidRepair::inspect(bytes, true))
      {
        if (canonical) canonical(record);
        auto data = metadata.at(record.uidOffset - 4);
        auto type = data.wmo ? 1u : 0u;
        auto& map = indices[source][type];
        auto oldIndex = map.size();
        if (!source && selected.count(record.identity))
        {
          removed = true;
          auto replacement = replacementOffsets.find(record.identity);
          // Unchanged identities keep their primary slot.
          if (lod || replacement == replacementOffsets.end() || retainedIndices.count(record.identity))
          {
            map.push_back(UINT32_MAX);
            continue;
          }
          data = replacements.at(replacement->second);
          retainedIndices.emplace(record.identity, counts[type]);
          retainedSlots[type].insert(counts[type]);
        }
        if (source && retainedIndices.count(record.identity))
        {
          map.push_back(retainedIndices.at(record.identity));
          continue;
        }
        map.push_back(counts[type]++);
        RetailObjectTransfer::Row row{std::move(data), {}};
        if (lod)
        {
          auto const& bounds = table.at(type ? "XMLM" : "XDLM");
          row.extent = read<std::array<float, 7>>(bytes, bounds.offset + oldIndex * 28);
        }
        rows.push_back(std::move(row));
      }
    }
    if (!removed && indices[1][0].empty() && indices[1][1].empty()) return original;
    auto output = repaired;
    if (!lod)
    {
      auto oldTop = chunks(original), newTop = chunks(repaired);
      std::vector<Chunk const*> oldCells;
      for (auto const& c : oldTop) if (c.tag == "KNCM") oldCells.push_back(&c);
      std::size_t cell = 0;
      for (auto& c : newTop) if (c.tag == "KNCM")
      {
        auto oldSubs = chunks(oldCells.at(cell++)->data), newSubs = chunks(c.data);
        for (unsigned type = 0; type < 2; ++type)
        {
          auto tag = type ? "WRCM" : "DRCM";
          std::vector<std::uint32_t> refs;
          for (unsigned source = 0; source < 2; ++source)
            if (auto* chunk = find(source ? newSubs : oldSubs, tag))
              for (std::size_t at = 0; at < chunk->data.size(); at += 4)
              {
                auto index = indices[source][type].at(read<std::uint32_t>(chunk->data, at));
                if (!source && retainedSlots[type].count(index)) continue;
                if (index != UINT32_MAX) refs.push_back(index);
              }
          replace(newSubs, tag, indexBytes(refs));
        }
        c.data = pack(newSubs);
      }
      output = pack(newTop);
    }
    return RetailObjectTransfer::write(output, std::move(rows), lod);
  }

  UidBounds readUidAssetBounds(UidRepair::Bytes const& bytes,bool wmo)
  {
    UidBounds result;
    if(!wmo)
    {
      std::size_t offset=0,size=bytes.size();
      if(size>=8 && std::memcmp(bytes.data(),"MD21",4)==0)
      { size=read<std::uint32_t>(bytes,4);offset=8;if(size>bytes.size()-8)throw std::runtime_error("Invalid MD21 payload"); }
      if(size<sizeof(ModelHeader)||std::memcmp(bytes.data()+offset,"MD20",4)!=0)
        throw std::runtime_error("Invalid/truncated M2 header");
      auto h=read<ModelHeader>(bytes,offset);
      if(h.ofsVertices>size || h.nVertices>(size-h.ofsVertices)/48)
        throw std::runtime_error("M2 vertex array outside file");
      result={h.bounding_box_min,h.bounding_box_max};
      valid(result);
      if(glm::length(result.max-result.min)<0.01f) result={glm::vec3(-1),glm::vec3(1)};
    }
    else
    {
      auto c=chunks(bytes);auto h=find(c,"DHOM"),v=find(c,"REVM"),g=find(c,"IGOM");
      if(!v||v->data.size()!=4||read<std::uint32_t>(v->data,0)!=17||!h||h->data.size()<64)
        throw std::runtime_error("Invalid/truncated WMO header");
      result={read<glm::vec3>(h->data,36),read<glm::vec3>(h->data,48)};valid(result);
      auto count=read<std::uint32_t>(h->data,4);
      if(count && (!g || g->data.size()/32!=count || g->data.size()%32))
        throw std::runtime_error("Invalid WMO group bounds table");
      for(std::size_t i=0;i<count;++i)
      {
        UidBounds group{read<glm::vec3>(g->data,i*32+4),read<glm::vec3>(g->data,i*32+16)};valid(group);
        result.min=glm::min(result.min,group.min);result.max=glm::max(result.max,group.max);
      }
    }
    return result;
  }

  UidGeometryPlan planUidGeometry(std::vector<UidTile> tiles,bool modern,bool scaledWmos,
      UidAssetReader const& reader,UidCanonicalAsset const& canonical,UidLodWriter const& lodWriter,
      bool preserveUids)
  {
    UidGeometryPlan plan;
    std::map<std::string,Placement> placements;
    struct Asset { UidBounds bounds; std::string error; };
    std::map<std::string,Asset> assets;
    std::set<std::string> seen;
    std::map<std::string,std::uint32_t> ids;
    std::set<std::string> selected;
    std::map<std::uint32_t, std::string> owners;
    if (preserveUids)
      for (std::size_t n = 0; n < tiles.size(); ++n)
      {
        auto const& tile = tiles[n];
        for (auto const* bytes : {&tile.objects, &tile.lod})
        {
          if (bytes == &tile.lod && !tile.hasLod) continue;
          for (auto record : UidRepair::inspect(*bytes, modern))
          {
            if (canonical) canonical(record);
            auto uid = UidRepair::word(*bytes, record.uidOffset);
            plan.highest = std::max(plan.highest, uid);
            if (tile.editable) selected.insert(record.identity);
          }
        }
      }
    // UIDs of unselected placements are reserved. Neighbour copies of selected
    // placements are repaired together with them.
    if (preserveUids)
      for (auto const& tile : tiles)
        if (!tile.editable)
          for (auto const* bytes : {&tile.objects, &tile.lod})
          {
            if (bytes == &tile.lod && !tile.hasLod) continue;
            for (auto record : UidRepair::inspect(*bytes, modern))
            {
              if (canonical) canonical(record);
              if (!selected.count(record.identity))
                owners.emplace(UidRepair::word(*bytes, record.uidOffset), record.identity);
            }
          }
    auto selectedRecords = [&](Bytes const& bytes, bool split) {
      auto records = UidRepair::inspect(bytes, split);
      for (auto& record : records) if (canonical) canonical(record);
      if (preserveUids)
        records.erase(std::remove_if(records.begin(), records.end(), [&](auto const& record) {
          return !selected.count(record.identity);
        }), records.end());
      return records;
    };
    for(auto const& tile:tiles)
    {
      auto records = selectedRecords(tile.objects, modern);
      if (records.empty()) continue;
      auto metadata = modern ? RetailObjectTransfer::read(tile.objects, false)
        : std::map<std::size_t, RetailObjectTransfer::Instance>{};
      for(auto record:records)
      {
        auto asset=record.asset.path.empty()?"filedataid:"+std::to_string(record.asset.fileId):record.asset.path;
        bool wmo=record.identity[0]=='W';
        auto key=(wmo?"W:":"M:")+asset;
        if(!assets.count(key))
        {
          Asset a;
          try { a.bounds=readUidAssetBounds(reader(record.asset),wmo); }
          catch(std::exception const& e) { a.error=e.what(); }
          assets.emplace(key,a);
        }
        auto oldUid=UidRepair::word(tile.objects,record.uidOffset);
        std::string error=assets.at(key).error;
        auto start=record.uidOffset-4;
        Bytes data(tile.objects.begin()+start,tile.objects.begin()+start+(wmo?64:36));
        UidBounds bounds;
        if(error.empty())
          try { bounds=transform(assets.at(key).bounds,data,wmo,scaledWmos); }
          catch(std::exception const& e) { error=e.what(); }
        if(error.empty() && std::none_of(tiles.begin(),tiles.end(),[&](auto const& t)
            {return intersects(bounds,t.x*TILESIZE,t.z*TILESIZE,TILESIZE);}))
          error="Placement bounds do not intersect any available terrain tile";
        if(!error.empty())
        {
          plan.problems.push_back({asset,tile.path,error,oldUid});
          if(seen.insert(record.identity).second) ++plan.removed;
          continue;
        }
        if(placements.count(record.identity)) continue;
        auto uid = oldUid;
        if (!preserveUids || uid == 0 || (owners.count(uid) && owners.at(uid) != record.identity))
        {
          if(plan.highest>=UINT32_MAX-1)throw std::runtime_error("UID range exhausted");
          uid=++plan.highest;
        }
        owners.emplace(uid, record.identity);
        put(data,4,uid);
        plan.sourceUids.push_back(oldUid);
        plan.targetUids.push_back(uid);
        if(modern)
        {
          if(!record.asset.fileId)throw std::runtime_error("No FileDataID for "+asset);
          put(data,0,record.asset.fileId);
        }
        if(wmo) {put(data,32,bounds.min);put(data,44,bounds.max);}
        Placement placement{data,record.asset,bounds,wmo,uid,{},{}};
        if (modern) placement.metadata = metadata.at(start);
        placements.emplace(record.identity,std::move(placement));
        ids.emplace(record.identity,uid);
      }
    }
    // OBJ1 records have their own flags, set selectors and LOD memberships;
    // they are collected before any tile is rewritten.
    std::vector<std::map<std::uint32_t, RetailObjectTransfer::Instance>> localLod(tiles.size());
    if (modern)
      for (std::size_t n = 0; n < tiles.size(); ++n)
      {
        auto const& tile = tiles[n];
        if (!tile.hasLod) continue;
        auto records = selectedRecords(tile.lod, true);
        if (records.empty()) continue;
        auto metadata = RetailObjectTransfer::read(tile.lod, true);
        for (auto record : records)
        {
          auto it = placements.find(record.identity);
          if (it == placements.end())
          {
            if (seen.insert(record.identity).second)
            {
              ++plan.removed;
              auto asset=record.asset.path.empty()?"filedataid:"+std::to_string(record.asset.fileId):record.asset.path;
              plan.problems.push_back({asset,tile.path+" [OBJ1]","LOD placement has no surviving primary placement",
                UidRepair::word(tile.lod,record.uidOffset)});
            }
            continue;
          }
          auto data = metadata.at(record.uidOffset-4);
          auto& placement = it->second;
          if (!placement.lod) placement.lod = data;
          localLod[n].emplace(placement.uid, std::move(data));
        }
      }
    // Placements are written only to existing tiles.
    std::vector<Placement const*> ordered;
    for(auto const& p:placements)ordered.push_back(&p.second);
    std::sort(ordered.begin(),ordered.end(),[](auto a,auto b){return a->uid<b->uid;});
    for(std::size_t tileIndex=0;tileIndex<tiles.size();++tileIndex)
    {
      auto& tile=tiles[tileIndex];
      bool const neighbour = preserveUids && !tile.editable;
      std::vector<Placement const*> chosen;
      std::map<std::uint32_t,std::array<float,6>> bounds;
      for(auto p:ordered)if(intersects(p->bounds,tile.x*TILESIZE,tile.z*TILESIZE,TILESIZE))
      {chosen.push_back(p);bounds[p->uid]={p->bounds.min.x,p->bounds.min.y,p->bounds.min.z,p->bounds.max.x,p->bounds.max.y,p->bounds.max.z};}
      if (neighbour && chosen.empty() && selectedRecords(tile.objects, modern).empty()
          && (!tile.hasLod || selectedRecords(tile.lod, true).empty())) continue;
      plan.references+=chosen.size();
      std::map<std::uint32_t, RetailObjectTransfer::Instance> local;
      if (modern)
      {
        auto metadata = RetailObjectTransfer::read(tile.objects, false);
        for (auto record : UidRepair::inspect(tile.objects, true))
        {
          if (canonical) canonical(record);
          auto it = ids.find(record.identity);
          if (it != ids.end()) local.emplace(it->second, metadata.at(record.uidOffset-4));
        }
      }
      try {
        auto rebuilt = rebuildObjects(tile,chosen,modern,local);
        tile.objects = neighbour ? mergeNeighbour(tile.objects, rebuilt, false, selected, canonical) : std::move(rebuilt);
      }
      catch (std::exception const& e) { throw std::runtime_error(tile.path + ": " + e.what()); }
      if(tile.hasLod)
      {
        if (modern)
        {
          std::vector<RetailObjectTransfer::Row> rows;
          for (auto p : chosen)
          {
            // Primary-only placement without an OBJ1 record.
            if (!p->lod) continue;
            auto it = localLod[tileIndex].find(p->uid);
            auto data = it == localLod[tileIndex].end() ? *p->lod : it->second;
            std::copy_n(p->data.begin(), 32, data.record.begin());
            rows.push_back({std::move(data), RetailObjectTransfer::extent(bounds.at(p->uid))});
          }
          try {
            auto rebuilt = RetailObjectTransfer::write(tile.lod,std::move(rows),true);
            tile.lod = neighbour ? mergeNeighbour(tile.lod, rebuilt, true, selected, canonical) : std::move(rebuilt);
          }
          catch (std::exception const& e) { throw std::runtime_error(tile.path + " [OBJ1]: " + e.what()); }
          continue;
        }
        auto records=UidRepair::inspect(tile.lod,true);
        for(auto record:records)
        {
          if(canonical)canonical(record);
          auto it=ids.find(record.identity);
          if(it==ids.end() && seen.insert(record.identity).second)
          {
            ++plan.removed;
            auto asset=record.asset.path.empty()?"filedataid:"+std::to_string(record.asset.fileId):record.asset.path;
            plan.problems.push_back({asset,tile.path+" [OBJ1]","LOD placement has no surviving primary placement",
              UidRepair::word(tile.lod,record.uidOffset)});
          }
          put(tile.lod,record.uidOffset,it==ids.end()?0u:it->second);
        }
        if(!lodWriter)throw std::runtime_error("No OBJ1 writer");
        auto c=chunks(tile.objects);auto m=find(c,"FDDM"),w=find(c,"FDOM");
        try { tile.lod=lodWriter(tile.lod,m->data,w->data,bounds); }
        catch (std::exception const& e) { throw std::runtime_error(tile.path + " [OBJ1]: " + e.what()); }
      }
    }
    for(auto p:ordered)
      if(std::none_of(tiles.begin(),tiles.end(),[&](auto const& t)
          {return intersects(p->bounds,t.x*TILESIZE,t.z*TILESIZE,TILESIZE);}))
        throw std::runtime_error("Placement UID "+std::to_string(p->uid)+" has no available destination tile");
    plan.tiles=std::move(tiles);
    return plan;
  }
}
