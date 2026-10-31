
#include <fstream>
#include <iostream>

void require(bool condition, char const* message)
{
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
  try {
    // Normalizing angles next to +/-180 degrees must not cross the boundary twice.
    for (auto angle : {std::nextafter(180.f, 0.f), std::nextafter(-180.f, -181.f), 180.f, -180.f}) {
      ENTRY_MDDF before{};
      before.uniqueID = 42;
      before.nameID = 123;
      before.scale = 1024;
      before.rot[2] = angle;
      auto after = before;
      after.rot[2] = angle - std::floor((angle + 180.f) / 360.f) * 360.f;
      auto original = make_chunk(fourcc('M','D','D','F'), record_bytes(std::vector<ENTRY_MDDF>{before}));
      require(same_placements<ENTRY_MDDF>(original, record_bytes(std::vector<ENTRY_MDDF>{after}), fourcc('M','D','D','F')),
              "One-step normalization near 180 degrees changed a placement");
      after.rot[2] += 0.01f;
      require(!same_placements<ENTRY_MDDF>(original, record_bytes(std::vector<ENTRY_MDDF>{after}), fourcc('M','D','D','F')),
              "Real small rotation change was ignored");
    }
    std::cout << "PASS rotation boundary normalization; actual 0.01-degree edits remain detectable\n";
    {
      ENTRY_MDDF a{}, b{};
      a.nameID=101; a.uniqueID=1; a.scale=1024; a.flags=0x40;
      b=a; b.nameID=102; b.uniqueID=2;
      LodMapObject wa{}, wb{};
      wa.nameID=201; wa.uniqueID=3; wa.flags=0x0e; wa.scale=1024;
      wb=wa; wb.nameID=202; wb.uniqueID=4;
      LodExtent small{}, large{};
      small.max[0]=small.radius=1; large.max[0]=large.radius=10;
      std::vector<std::uint8_t> original;
      auto add=[&](std::uint32_t tag, auto data) {
        auto c=make_chunk(tag,data); original.insert(original.end(),c.begin(),c.end());
      };
      add(fourcc('M','L','D','D'),record_bytes(std::vector<ENTRY_MDDF>{a,b}));
      add(fourcc('M','L','D','X'),record_bytes(std::vector<LodExtent>{small,large}));
      add(fourcc('M','L','D','B'),std::vector<std::uint8_t>{0x19,0x40});
      add(fourcc('M','L','M','D'),record_bytes(std::vector<LodMapObject>{wa,wb}));
      add(fourcc('M','L','M','X'),record_bytes(std::vector<LodExtent>{small,large}));
      add(fourcc('M','L','M','B'),std::vector<std::uint8_t>{0x26,0x80});
      std::vector<std::uint32_t> levels(12);
      levels[5]=levels[11]=2;
      add(fourcc('M','L','F','D'),record_bytes(levels));
      std::vector<ENTRY_MODF> objects(2);
      for(std::size_t i=0;i<2;++i)
      {
        auto const& source=i?wb:wa;
        std::memcpy(&objects[i],&source,32);
        objects[i].flags=0x0c; objects[i].scale=1024;
      }
      auto output=rewrite_obj1(original,record_bytes(std::vector<ENTRY_MDDF>{a,b}),record_bytes(objects));
      auto c=parse_chunks(output,"regression");
      auto values=[&](std::uint32_t tag) {
        auto p=find_chunk(c,tag);
        return std::vector<std::uint8_t>(output.begin()+p->payload,output.begin()+p->payload+p->size);
      };
      require(values(fourcc('M','L','D','B'))==std::vector<std::uint8_t>({0x40,0x19}),"Sorted MLDB did not follow its models");
      require(values(fourcc('M','L','M','B'))==std::vector<std::uint8_t>({0x80,0x26}),"Sorted MLMB did not follow its WMOs");
      auto saved=placement_records<LodMapObject>(output,*find_chunk(c,fourcc('M','L','M','D')),"saved");
      require(saved[0].flags==0x0e && saved[1].flags==0x0e,"OBJ0 flags overwrote WMO LOD flags");
      require(rewrite_obj1(output,record_bytes(std::vector<ENTRY_MDDF>{a,b}),record_bytes(objects))==output,"Repeated third-LOD save changed bytes");
      // OBJ0 and OBJ1 may use different MWDS selectors for the same UID.
      auto mwdsOriginal=original;
      wa.flags=0x8e;wa.doodadSet=0;
      mwdsOriginal=replace_top_level_chunk(mwdsOriginal,fourcc('M','L','M','D'),
        make_chunk(fourcc('M','L','M','D'),record_bytes(std::vector<LodMapObject>{wa,wb})));
      auto mwds=make_chunk(fourcc('M','W','D','S'),record_bytes(std::vector<std::uint16_t>{1,2}));
      mwdsOriginal.insert(mwdsOriginal.end(),mwds.begin(),mwds.end());
      auto mwdr=make_chunk(fourcc('M','W','D','R'),record_bytes(std::vector<std::uint32_t>{0,1}));
      mwdsOriginal.insert(mwdsOriginal.end(),mwdr.begin(),mwdr.end());
      auto primary=objects;primary[0].flags=0x8c;primary[0].doodadSet=1;
      auto retained=rewrite_obj1(mwdsOriginal,record_bytes(std::vector<ENTRY_MDDF>{a,b}),record_bytes(primary));
      auto rc=parse_chunks(retained,"MWDS regression");
      auto rw=placement_records<LodMapObject>(retained,*find_chunk(rc,fourcc('M','L','M','D')),"MLMD");
      auto instance=std::find_if(rw.begin(),rw.end(),[](auto const& x){return x.uniqueID==3;});
      require(instance!=rw.end() && instance->doodadSet==0 && instance->flags==0x8e,
        "OBJ0 MWDS selector overwrote an OBJ1-local reference");
      require(rewrite_obj1(retained,record_bytes(std::vector<ENTRY_MDDF>{a,b}),record_bytes(primary))==retained,
        "Repeated MWDS save changed bytes");
      RetailObjectTransfer::WmoSource wmoSource;
      wmoSource.primary.wmo=true;
      wmoSource.primary.record=record_bytes(std::vector<ENTRY_MODF>{primary[0]});
      wmoSource.primary.sets={1,2};wmoSource.lodKnown=true;
      for(auto const& [at,data]:RetailObjectTransfer::read(mwdsOriginal,true))
        if(data.wmo && RetailObjectTables::word(data.record,4)==3)wmoSource.lod=data;
      RetailObjectTransfer::WmoSources runtimeSources{{3,wmoSource},{999,wmoSource}};
      auto pasted=primary;
      auto pastedObject=primary[0];pastedObject.uniqueID=999;pastedObject.pos[0]+=10;
      pasted.push_back(pastedObject);
      auto pastedLod=rewrite_obj1(mwdsOriginal,record_bytes(std::vector<ENTRY_MDDF>{a,b}),record_bytes(pasted),nullptr,{},&runtimeSources);
      bool foundCopy=false;
      for(auto const& [at,data]:RetailObjectTransfer::read(pastedLod,true))
        if(data.wmo && RetailObjectTables::word(data.record,4)==999)
        {
          foundCopy=true;
          require(data.sets==std::vector<std::uint16_t>({1,2}) && data.byte==0x26 && data.levels==4,
            "Pasted WMO lost semantic sets, opaque data or LOD membership");
          require((RetailObjectTables::word(data.record,32)&0xffff)==0x8e,
            "Pasted WMO copied primary flags over LOD flags");
        }
      require(foundCopy,"Pasted WMO LOD placement was omitted");
      auto newM2=a;newM2.uniqueID=123456;
      auto mixedSaved=rewrite_obj1(mwdsOriginal,record_bytes(std::vector<ENTRY_MDDF>{a,b,newM2}),record_bytes(pasted),nullptr,{},&runtimeSources);
      require(mixedSaved==pastedLod,"New primary-only M2 changed existing M2/WMO LOD metadata");
      RetailObjectTables::validate(mixedSaved,true);
      require(rewrite_obj1(mixedSaved,record_bytes(std::vector<ENTRY_MDDF>{a,b,newM2}),record_bytes(pasted),nullptr,{},&runtimeSources)==mixedSaved,
        "Repeated save with new primary-only M2 changed OBJ1 bytes");
      require(rewrite_obj1(pastedLod,record_bytes(std::vector<ENTRY_MDDF>{a,b}),record_bytes(pasted),nullptr,{},&runtimeSources)==pastedLod,
        "Repeated pasted WMO save changed bytes");
      auto directSelection=primary;
      directSelection[0].flags=0x0c;directSelection[0].doodadSet=7;directSelection[0].nameSet=9;
      runtimeSources.at(3).primary.sets.clear();
      auto directLod=rewrite_obj1(mwdsOriginal,record_bytes(std::vector<ENTRY_MDDF>{a,b}),record_bytes(directSelection),nullptr,{},&runtimeSources);
      for(auto const& [at,data]:RetailObjectTransfer::read(directLod,true))
        if(data.wmo && RetailObjectTables::word(data.record,4)==3)
          require(data.sets.empty() && RetailObjectTables::word(data.record,32)==(7u<<16|0x0e)
            && (RetailObjectTables::word(data.record,36)&0xffff)==9,
            "Explicit doodad/name-set selection failed to reach OBJ1");
      // Saving after a cross-tile repair must accept mixed base/third-level M2s.
      std::vector<std::uint32_t> mixedLevels(12);
      mixedLevels[2]=1;mixedLevels[3]=1;mixedLevels[5]=1;
      mixedLevels[8]=1;mixedLevels[9]=1;mixedLevels[11]=1;
      auto mixed=replace_top_level_chunk(original,fourcc('M','L','F','D'),
        make_chunk(fourcc('M','L','F','D'),record_bytes(mixedLevels)));
      auto moved=a;moved.pos[0]+=10;
      auto savedMixed=rewrite_obj1(mixed,record_bytes(std::vector<ENTRY_MDDF>{moved,b}),record_bytes(objects));
      auto mixedRows=RetailObjectTransfer::read(savedMixed,true);
      for(auto const& [offset,row]:mixedRows)
      {
        auto uid=RetailObjectTables::word(row.record,4);
        require(row.levels==((uid==1||uid==3)?1:4),"Later save changed mixed LOD membership");
      }
      auto withPrimaryOnly=objects;
      auto only=objects.front();only.uniqueID=999;withPrimaryOnly.push_back(only);
      auto beforePrimary=make_chunk(fourcc('M','D','D','F'),record_bytes(std::vector<ENTRY_MDDF>{a,b}));
      auto beforeWmos=make_chunk(fourcc('M','O','D','F'),record_bytes(withPrimaryOnly));
      beforePrimary.insert(beforePrimary.end(),beforeWmos.begin(),beforeWmos.end());
      auto retainedAbsence=obj1_for_save(beforePrimary,mixed,
        record_bytes(std::vector<ENTRY_MDDF>{moved,b}),record_bytes(withPrimaryOnly));
      require(RetailObjectTables::count(RetailObjectTables::chunks(retainedAbsence),"DMLM",40)==2,
        "Later save synthesized an intentionally absent LOD WMO");
      bool rejected=false;
      auto bad=replace_top_level_chunk(original,fourcc('M','L','M','B'),make_chunk(fourcc('M','L','M','B'),{0x26}));
      auto obj0=make_chunk(fourcc('M','D','D','F'),record_bytes(std::vector<ENTRY_MDDF>{a,b}));
      auto wmos=make_chunk(fourcc('M','O','D','F'),record_bytes(objects));
      obj0.insert(obj0.end(),wmos.begin(),wmos.end());
      auto refs=make_chunk(fourcc('M','C','R','D'),record_bytes(std::vector<std::uint32_t>{1,0}));
      auto wrefs=make_chunk(fourcc('M','C','R','W'),record_bytes(std::vector<std::uint32_t>{0,1}));
      refs.insert(refs.end(),wrefs.begin(),wrefs.end());
      auto mcnk=make_chunk(fourcc('M','C','N','K'),refs);
      obj0.insert(obj0.end(),mcnk.begin(),mcnk.end());
      auto reorderedObjects=objects;std::reverse(reorderedObjects.begin(),reorderedObjects.end());
      reorderedObjects[0].extents[0].x=123.f;reorderedObjects[1].rot[0]=360.f;
      auto reorderedA=a;reorderedA.rot[2]=360.f;
      auto unchangedPrimary=primary_for_save(obj0,record_bytes(std::vector<ENTRY_MDDF>{b,reorderedA}),record_bytes(reorderedObjects),{},true);
      require(unchangedPrimary==obj0,"Semantic no-op changed original record order, MCNK indices, bounds or normalized angles");
      require(obj1_for_save(obj0,original,record_bytes(std::vector<ENTRY_MDDF>{b,reorderedA}),record_bytes(reorderedObjects),nullptr,unchangedPrimary!=obj0)==original,
        "Semantic no-op rebuilt original OBJ1");
      try{obj1_for_save(obj0,bad,record_bytes(std::vector<ENTRY_MDDF>{a,b}),record_bytes(objects));}
      catch(std::runtime_error const&){rejected=true;}
      require(rejected,"Unchanged placement save accepted corrupted MLMB");
      std::cout<<"PASS Retail OBJ1 writer: third LOD, sorting, parallel bytes, WMO flags, repeated save, malformed unchanged input\n";
    }
    if (argc == 1) return 0;
    if (argc == 4 && (std::string(argv[1]) == "--preserve" || std::string(argv[1]) == "--check-pair")) {
      bool const check_pair=std::string(argv[1])=="--check-pair";
      auto read = [](char const* path) {
        std::ifstream input(path, std::ios::binary);
        require(input.good(), "Retail fixture could not be opened");
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(input)), {});
      };
      auto obj0 = read(argv[2]);
      auto obj1 = read(argv[3]);
      RetailObjectTables::validate(obj0,false);
      RetailObjectTables::validate(obj1,true);
      auto chunks = parse_chunks(obj0, "Retail OBJ0");
      auto doodads = placement_records<ENTRY_MDDF>(obj0, *find_chunk(chunks, fourcc('M','D','D','F')), "MDDF");
      auto objects = placement_records<ENTRY_MODF>(obj0, *find_chunk(chunks, fourcc('M','O','D','F')), "MODF");
      auto semanticSources=RetailObjectTransfer::readWmoSources(obj0,obj1,true);
      auto reversedDoodads=doodads;std::reverse(reversedDoodads.begin(),reversedDoodads.end());
      auto reversedWmos=objects;std::reverse(reversedWmos.begin(),reversedWmos.end());
      for(auto& entry:reversedWmos) { entry.extents[0].x+=1.f;if(entry.flags&0x80)++entry.doodadSet; }
      require(primary_for_save(obj0,record_bytes(reversedDoodads),record_bytes(reversedWmos),semanticSources,true)==obj0,
        "Real-tile paint-only save changed placement order, bounds, local selectors or chunk references");
      std::vector<std::uint8_t> strings;
      std::vector<std::uint32_t> offsets;
      auto legacy_doodads = doodads;
      for (size_t i = 0; i < doodads.size(); ++i) {
        auto name = std::to_string(doodads[i].nameID);
        offsets.push_back(static_cast<std::uint32_t>(strings.size()));
        strings.insert(strings.end(), name.begin(), name.end());
        strings.push_back(0);
        legacy_doodads[i].nameID = static_cast<std::uint32_t>(i);
        legacy_doodads[i].flags = 0;
      }
      auto legacy = make_chunk(fourcc('M','D','D','F'), record_bytes(legacy_doodads));
      auto names = make_chunk(fourcc('M','M','D','X'), strings);
      legacy.insert(legacy.end(), names.begin(), names.end());
      auto indices = make_chunk(fourcc('M','M','I','D'), record_bytes(offsets));
      legacy.insert(legacy.end(), indices.begin(), indices.end());
      require(modern_placements<ENTRY_MDDF>(legacy, fourcc('M','D','D','F'),
              fourcc('M','M','I','D'), fourcc('M','M','D','X'), obj0) == record_bytes(doodads),
              "Retail serialization changed original M2 flags or placement data");
      auto rewritten= rewrite_obj1(obj1, record_bytes(doodads), record_bytes(objects));
      RetailObjectTables::validate(rewritten, true);
      require(obj1_for_save(obj0, obj1, record_bytes(doodads), record_bytes(objects)) == obj1,
              "Painting changed original OBJ1 bytes");
      auto normalized_doodads = doodads;
      auto normalized_objects = objects;
      unsigned normalized_angles = 0;
      auto normalize = [&](auto& entries) {
        for (auto& entry : entries)
          for (auto& angle : entry.rot) {
            auto const normalized = angle - std::floor((angle + 180.f) / 360.f) * 360.f;
            if (angle != normalized) ++normalized_angles;
            angle = normalized;
          }
      };
      normalize(normalized_doodads);
      normalize(normalized_objects);
      if(!check_pair)require(normalized_angles > 0, "Fixture does not exercise editor angle normalization");
      require(obj1_for_save(obj0, obj1, record_bytes(normalized_doodads), record_bytes(normalized_objects)) == obj1,
              "Editor angle normalization forced a LOD rewrite while painting");
      std::cout << "PASS " << normalized_angles << " normalized editor angles preserve OBJ1\n";
      if(!normalized_doodads.empty())
      {
        normalized_doodads[0].rot[1] += 1.f;
        require(!same_placements<ENTRY_MDDF>(obj0, record_bytes(normalized_doodads), fourcc('M','D','D','F')),
                "Real M2 rotation change was ignored");
      }
      if(!normalized_objects.empty())
      {
        normalized_objects[0].rot[1] += 1.f;
        require(!same_placements<ENTRY_MODF>(obj0, record_bytes(normalized_objects), fourcc('M','O','D','F')),
                "Real WMO rotation change was ignored");
      }
      std::reverse(doodads.begin(), doodads.end());
      std::reverse(objects.begin(), objects.end());
      require(obj1_for_save(obj0, obj1, record_bytes(doodads), record_bytes(objects)) == obj1,
              "Placement serialization order changed original OBJ1 bytes");
      if(check_pair && objects.empty())return 0;
      require(!objects.empty(), "Expected a WMO fixture");
      objects[0].extents[0].x += 1;
      objects[0].extents[1].y += 2;
      require(obj1_for_save(obj0, obj1, record_bytes(doodads), record_bytes(objects)) == obj1,
              "Derived WMO bounds forced a LOD rewrite");
      auto resized_objects = objects;
      resized_objects[0].scale += 1;
      RetailObjectTables::validate(obj1_for_save(obj0, obj1, record_bytes(doodads), record_bytes(resized_objects)), true);
      if(check_pair && doodads.empty())return 0;
      require(!doodads.empty(), "Expected an M2 fixture");
      doodads[0].pos[0] += 10;
      RetailObjectTables::validate(obj1_for_save(obj0, obj1, record_bytes(doodads), record_bytes(objects)), true);
      std::cout << "PASS real Retail fixture: unchanged/reordered placements and derived WMO bounds preserve OBJ1; moved M2 and resized WMO retain valid auxiliary tables\n";
      return 0;
    }
    for (int fixture = 1; fixture < argc; ++fixture) {
      std::ifstream input(argv[fixture], std::ios::binary);
      require(input.good(), "Fixture could not be opened");
      std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
      auto chunks = parse_chunks(bytes, "fixture");
      auto doodads = placement_records<ENTRY_MDDF>(bytes, *find_chunk(chunks, fourcc('M','L','D','D')), "MLDD");
      auto lod_objects = placement_records<LodMapObject>(bytes, *find_chunk(chunks, fourcc('M','L','M','D')), "MLMD");
      auto extents = placement_records<LodExtent>(bytes, *find_chunk(chunks, fourcc('M','L','M','X')), "MLMX");
      std::vector<ENTRY_MODF> objects;
      for (std::size_t i = 0; i < lod_objects.size(); ++i) {
        ENTRY_MODF object{};
        std::memcpy(&object, &lod_objects[i], 32);
        std::memcpy(reinterpret_cast<std::uint8_t*>(&object) + 56,
                    reinterpret_cast<std::uint8_t const*>(&lod_objects[i]) + 32, 8);
        object.extents[0] = {17066.666f - extents[i].max[1], extents[i].min[2], 17066.666f - extents[i].max[0]};
        object.extents[1] = {17066.666f - extents[i].min[1], extents[i].max[2], 17066.666f - extents[i].min[0]};
        objects.push_back(object);
      }
      bool had_lod = find_chunk(chunks, fourcc('M','L','D','L')) != nullptr;
      bool had_groups = find_chunk(chunks, fourcc('M','L','F','D')) != nullptr;
      for (int scenario = 0; scenario < 4; ++scenario) {
        auto edited = doodads;
        auto edited_objects = objects;
        if (scenario == 1) {
          if (!edited.empty()) edited.at(0).pos[0] += 10;
          if (!edited_objects.empty()) edited_objects.at(0).pos[0] += 10;
        } else if (scenario == 2) {
          auto added = edited.empty() ? ENTRY_MDDF{} : edited.at(0);
          added.scale = 1024;
          added.uniqueID = 0xFFFFFF00u;
          added.pos[0] += 20;
          edited.push_back(added);
          auto added_object = edited_objects.empty() ? ENTRY_MODF{} : edited_objects.at(0);
          added_object.scale = 1024;
          added_object.uniqueID = 0xFFFFFF01u;
          edited_objects.push_back(added_object);
        } else if (scenario == 3) {
          if (!edited.empty()) edited.pop_back();
          if (!edited_objects.empty()) edited_objects.pop_back();
        }
        auto output = rewrite_obj1(bytes, record_bytes(edited), record_bytes(edited_objects));
        auto saved = parse_chunks(output, "saved");
        require((find_chunk(saved, fourcc('M','L','D','L')) != nullptr) == had_lod, "Optional MLDL presence changed");
        require((find_chunk(saved, fourcc('M','L','F','D')) != nullptr) == had_groups, "Optional MLFD presence changed");
        require(find_chunk(saved, fourcc('M','L','D','D'))->size == edited.size() * sizeof(ENTRY_MDDF), "Doodad count wrong");
        require(find_chunk(saved, fourcc('M','L','M','D'))->size == edited_objects.size() * sizeof(LodMapObject), "WMO count wrong");
        require(rewrite_obj1(output, record_bytes(edited), record_bytes(edited_objects)) == output, "Repeated save is not stable");
        auto saved_doodads = placement_records<ENTRY_MDDF>(output, *find_chunk(saved, fourcc('M','L','D','D')), "saved MLDD");
        for (auto const& entry : edited) {
          auto found = std::find_if(saved_doodads.begin(), saved_doodads.end(), [&](auto const& item) { return item.uniqueID == entry.uniqueID && item.nameID == entry.nameID; });
          require(found != saved_doodads.end() && std::equal(std::begin(entry.pos), std::end(entry.pos), std::begin(found->pos)), "Edited position missing");
        }
        std::cout << "PASS fixture " << fixture << " scenario " << scenario << " MLDL=" << had_lod << '\n';
      }
      if (had_lod) {
        auto malformed = replace_top_level_chunk(bytes, fourcc('M','L','D','L'), make_chunk(fourcc('M','L','D','L'), {}));
        bool rejected = false;
        try { rewrite_obj1(malformed, record_bytes(doodads), record_bytes(objects)); }
        catch (std::runtime_error const&) { rejected = true; }
        require(rejected, "Malformed present MLDL accepted");
        std::cout << "PASS malformed MLDL rejected\n";
      }
    }
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
