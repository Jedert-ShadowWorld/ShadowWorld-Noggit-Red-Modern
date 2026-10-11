
#include <fstream>
#include <iostream>

void require(bool condition, char const* message)
{
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
  try {
    // The real 37_34 fixture has this angle: normalization crosses the
    // boundary a second time instead of being idempotent.
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
    if (argc == 1) return 0;
    if (argc == 4 && std::string(argv[1]) == "--preserve") {
      auto read = [](char const* path) {
        std::ifstream input(path, std::ios::binary);
        require(input.good(), "Retail fixture could not be opened");
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(input)), {});
      };
      auto obj0 = read(argv[2]);
      auto obj1 = read(argv[3]);
      auto chunks = parse_chunks(obj0, "Retail OBJ0");
      auto doodads = placement_records<ENTRY_MDDF>(obj0, *find_chunk(chunks, fourcc('M','D','D','F')), "MDDF");
      auto objects = placement_records<ENTRY_MODF>(obj0, *find_chunk(chunks, fourcc('M','O','D','F')), "MODF");
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
      bool rejected = false;
      try { rewrite_obj1(obj1, record_bytes(doodads), record_bytes(objects)); }
      catch (std::runtime_error const&) { rejected = true; }
      require(rejected, "Retail fixture did not reproduce the original failure");
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
      require(normalized_angles > 0, "Fixture does not exercise editor angle normalization");
      require(obj1_for_save(obj0, obj1, record_bytes(normalized_doodads), record_bytes(normalized_objects)) == obj1,
              "Editor angle normalization forced a LOD rewrite while painting");
      std::cout << "PASS " << normalized_angles << " normalized editor angles preserve OBJ1\n";
      normalized_doodads[0].rot[1] += 1.f;
      require(!same_placements<ENTRY_MDDF>(obj0, record_bytes(normalized_doodads), fourcc('M','D','D','F')),
              "Real M2 rotation change was ignored");
      normalized_objects[0].rot[1] += 1.f;
      require(!same_placements<ENTRY_MODF>(obj0, record_bytes(normalized_objects), fourcc('M','O','D','F')),
              "Real WMO rotation change was ignored");
      std::reverse(doodads.begin(), doodads.end());
      std::reverse(objects.begin(), objects.end());
      require(obj1_for_save(obj0, obj1, record_bytes(doodads), record_bytes(objects)) == obj1,
              "Placement serialization order changed original OBJ1 bytes");
      require(!objects.empty(), "Expected a WMO fixture");
      objects[0].extents[0].x += 1;
      objects[0].extents[1].y += 2;
      require(obj1_for_save(obj0, obj1, record_bytes(doodads), record_bytes(objects)) == obj1,
              "Derived WMO bounds forced a LOD rewrite");
      auto resized_objects = objects;
      resized_objects[0].scale += 1;
      rejected = false;
      try { obj1_for_save(obj0, obj1, record_bytes(doodads), record_bytes(resized_objects)); }
      catch (std::runtime_error const&) { rejected = true; }
      require(rejected, "Resized WMO bypassed LOD validation");
      require(!doodads.empty(), "Expected an M2 fixture");
      doodads[0].pos[0] += 10;
      rejected = false;
      try { obj1_for_save(obj0, obj1, record_bytes(doodads), record_bytes(objects)); }
      catch (std::runtime_error const&) { rejected = true; }
      require(rejected, "Moved objects bypassed LOD validation");
      std::cout << "PASS real Retail fixture: unchanged/reordered placements and derived WMO bounds preserve OBJ1; moved M2 and resized WMO reject unsupported layout\n";
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
