#include "../src/noggit/client_data/ModernDB2Delta.hpp"
#include <formats/BinaryTable.h>
#include <schema/DbdParser.h>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
using Noggit::ClientData::applyNativeEditorChanges;
using Noggit::ClientData::NativeRowChange;

void require(bool condition, char const* message)
{
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 4) return 2;
    using Noggit::ClientData::sampleNativeLightBand;
    require(sampleNativeLightBand({{0,10},{100,30}},50,false) == 20, "Float interpolation failed.");
    require(sampleNativeLightBand({{100,0},{2780,100}},0,false) == 50, "Midnight interpolation failed.");
    require(sampleNativeLightBand({{0,0},{100,0xffffff}},50,true) == 0x808080, "Packed color interpolation failed.");
    bool emptyRejected = false;
    try { sampleNativeLightBand({},0,false); }
    catch (std::exception const&) { emptyRejected = true; }
    require(emptyRejected, "Empty LightData channel was accepted.");
    fs::create_directories(argv[3]);
    int failures = 0;
    for (auto const* name : {"AreaTable", "AreaTrigger", "Light", "LightParams", "LightData", "LightSkybox",
                            "GroundEffectTexture", "GroundEffectDoodad", "SoundAmbience", "ZoneMusic", "ZoneIntroMusicTable"})
    {
      try
      {
      std::string error;
      auto input = fs::path(argv[2]) / (std::string(name) + ".db2");
      auto header = dbc::InspectHeader(input, error);
      require(error.empty(), error.c_str());
      auto schema = dbc::DbdParser::Parse(fs::path(argv[1]) / (std::string(name) + ".dbd"), error);
      require(schema.has_value(), error.c_str());
      auto selected = dbc::SelectSchema(*schema, dbc::Profile(dbc::GameVersion::Forever), {}, header.layoutHash, error);
      require(selected.has_value(), error.c_str());
      auto columns = dbc::FlattenForWdc5(*schema, selected->version);
      if (std::getenv("NOGGIT_DB2_TEST_COLUMNS"))
      {
        std::cout << name << " columns:";
        for (auto const& column : columns) std::cout << ' ' << column.name;
        std::cout << '\n';
      }
      auto loaded = dbc::BinaryTableCodec::LoadWdc(input, columns, error);
      require(loaded.has_value(), error.c_str());
      require(loaded->writable, loaded->readOnlyReason.c_str());
      require(loaded->rows.size() > 2, "Fixture needs at least three rows.");
      auto id = std::find_if(columns.begin(), columns.end(), [](auto const& c) { return c.id; }) - columns.begin();
      require(id < columns.size(), "ID field missing.");
      auto original = *loaded;
      auto target = std::find_if(columns.begin(), columns.end(), [](auto const& c) { return !c.id && !c.padding && !c.nonInline; }) - columns.begin();
      require(target < columns.size(), "Editable field missing.");
      auto rowID = static_cast<std::uint32_t>(std::stoul(dbc::CellToString(loaded->rows[0].cells[id])));
      auto deletedID = static_cast<std::uint32_t>(std::stoul(dbc::CellToString(loaded->rows[1].cells[id])));
      auto replacement = loaded->rows[2].cells[target];
      applyNativeEditorChanges(*loaded, id, {{rowID, false, {{target, replacement}}}}, {deletedID});
      require(loaded->rows.size() + 1 == original.rows.size(), "Deletion count mismatch.");
      for (std::size_t c = 0; c < columns.size(); ++c)
        require(loaded->rows[0].cells[c] == (c == target ? replacement : original.rows[0].cells[c]), "Unedited field changed.");
      require(loaded->rows[1].cells == original.rows[2].cells, "Untouched row changed.");
      auto beforeConflict = loaded->rows;
      bool rejected = false;
      try { applyNativeEditorChanges(*loaded, id, {{rowID, false, {{target, original.rows[0].cells[target]}}}, {rowID, true, {}}}, {}); }
      catch (std::exception const&) { rejected = true; }
      require(rejected, "Duplicate/conflicting ID was accepted.");
      require(loaded->rows[0].cells == beforeConflict[0].cells, "Conflict partially changed the table.");
      std::uint32_t newID = 1;
      for (auto const& row : original.rows)
        newID = std::max(newID, static_cast<std::uint32_t>(std::stoul(dbc::CellToString(row.cells[id])) + 1));
      NativeRowChange added{newID, true, {}};
      for (std::size_t c = 0; c < columns.size(); ++c)
        if (c != id) added.fields.emplace(c, original.rows[0].cells[c]);
      applyNativeEditorChanges(*loaded, id, {added}, {});
      require(loaded->rows.size() == original.rows.size(), "Added record count mismatch.");
      require(dbc::CellToString(loaded->rows.back().cells[id]) == std::to_string(newID), "Added ID mismatch.");
      auto output = fs::path(argv[3]) / (std::string(name) + ".db2");
      require(dbc::BinaryTableCodec::SaveWdc(*loaded, output, error), error.c_str());
      auto reread = dbc::BinaryTableCodec::LoadWdc(output, columns, error);
      require(reread.has_value(), error.c_str());
      require(reread->rows.size() == loaded->rows.size(), "Readback count mismatch.");
      for (std::size_t r = 0; r < reread->rows.size(); ++r)
        require(reread->rows[r].cells == loaded->rows[r].cells, "Readback cell mismatch.");
      auto after = dbc::InspectHeader(output, error);
      require(after.format == header.format && after.layoutHash == header.layoutHash, "Native format/layout changed.");
      std::cout << name << ": PASS, " << original.rows.size() << " rows, " << columns.size() << " fields\n";
      }
      catch (std::exception const& error)
      {
        ++failures;
        std::cerr << name << ": FAIL: " << error.what() << '\n';
      }
    }
    return failures ? 1 : 0;
  }
  catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
