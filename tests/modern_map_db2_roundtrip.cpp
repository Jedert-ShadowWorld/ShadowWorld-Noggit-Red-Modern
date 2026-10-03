#include <formats/BinaryTable.h>
#include <schema/DbdParser.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#ifdef NOGGIT_VERIFY_DB2_READER
#include <BlizzardDatabase.h>
#include <fstream>
#endif

int main(int argc, char** argv)
{
  try
  {
    if (argc != 4) throw std::runtime_error("Usage: test Map.dbd Map.db2 output.db2");
    std::string error;
    auto dbd = dbc::DbdParser::Parse(argv[1], error);
    if (!dbd) throw std::runtime_error(error);
    auto header = dbc::InspectHeader(argv[2], error);
    auto schema = dbc::SelectSchema(*dbd, dbc::Profile(dbc::GameVersion::Forever), {}, header.layoutHash, error);
    if (!schema) throw std::runtime_error(error);
    auto columns = dbc::FlattenForWdc5(*dbd, schema->version);
    auto table = dbc::BinaryTableCodec::LoadWdc(argv[2], columns, error);
    if (!table) throw std::runtime_error(error);
    auto original = table->rows;
    auto id = std::find_if(columns.begin(), columns.end(), [](auto const& c) { return c.id; }) - columns.begin();
    std::int64_t newId = 1;
    for (auto const& row : original) newId = std::max(newId, std::stoll(dbc::CellToString(row.cells[id])) + 1);
    dbc::Row row;
    row.stableId = table->nextStableId++;
    for (auto const& column : columns)
    {
      dbc::CellValue value;
      auto text = column.type == dbc::ValueType::String ? "" : "0";
      if (!dbc::SetCellFromString(value, column.type, text, error)) throw std::runtime_error(error);
      row.cells.push_back(std::move(value));
    }
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
      std::string value;
      if (columns[i].id) value = std::to_string(newId);
      else if (columns[i].name == "Directory") value = "NoggitDB2Regression";
      else if (columns[i].name == "MapName_lang") value = "Native schema round trip";
      else if (columns[i].name == "ParentMapID" || columns[i].name == "CosmeticParentMapID"
               || columns[i].name == "CorpseMapID" || columns[i].name == "TimeOfDayOverride") value = "-1";
      else if (columns[i].name == "MinimapIconScale") value = "1";
      else if (columns[i].name == "Flags[2]") value = "64";
      else continue;
      if (!dbc::SetCellFromString(row.cells[i], columns[i].type, value, error)) throw std::runtime_error(error);
    }
    table->rows.push_back(row);
    if (!dbc::BinaryTableCodec::SaveWdc(*table, argv[3], error)) throw std::runtime_error(error);
    auto loaded = dbc::BinaryTableCodec::LoadWdc(argv[3], columns, error);
    if (!loaded || loaded->rows.size() != original.size() + 1 || loaded->format != header.format)
      throw std::runtime_error("Native container or row count changed.");
    for (std::size_t i = 0; i < original.size(); ++i)
      if (loaded->rows[i].cells != original[i].cells) throw std::runtime_error("Existing Map row changed.");
    if (loaded->rows.back().cells != row.cells) throw std::runtime_error("New Map row changed.");
#ifdef NOGGIT_VERIFY_DB2_READER
    std::ifstream file(argv[3], std::ios::binary | std::ios::ate);
    std::vector<char> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0); file.read(bytes.data(), bytes.size());
    BlizzardDatabaseLib::BlizzardDatabase reader(std::filesystem::path(argv[1]).parent_path().string(),
        BlizzardDatabaseLib::Structures::Build(schema->selectedBuild.ToString()), false);
    auto& checked = reader.LoadTable("Map", [&](std::string const&) {
      return std::make_shared<BlizzardDatabaseLib::Stream::IMemStream>(bytes.data(), bytes.size());
    });
    if (checked.RecordCount() != loaded->rows.size()) throw std::runtime_error("Noggit reader record count differs.");
    for (std::uint32_t i = 0; i < checked.RecordCount(); ++i)
    {
      auto decoded = checked.RecordByPosition(i);
      if (std::to_string(decoded.RecordId) != dbc::CellToString(loaded->rows[i].cells[id]))
        throw std::runtime_error("Noggit reader ID differs.");
      for (std::size_t c = 0; c < columns.size(); ++c)
      {
        if (columns[c].name != "Directory" && columns[c].name != "MapName_lang") continue;
        auto const& cell = decoded.Columns.at(columns[c].name);
        auto const& text = cell.Value.empty() && !cell.Values.empty() ? cell.Values.front() : cell.Value;
        if (text != dbc::CellToString(loaded->rows[i].cells[c])) throw std::runtime_error("Noggit reader text differs.");
      }
    }
#endif
    table->rows[0].cells[id] = row.cells[id];
    if (dbc::BinaryTableCodec::SaveWdc(*table, argv[3], error)) throw std::runtime_error("Duplicate ID accepted.");
    auto intact = dbc::BinaryTableCodec::LoadWdc(argv[3], columns, error);
    if (!intact || intact->rows.back().cells != row.cells) throw std::runtime_error("Failed save damaged output.");
    auto bfa = dbc::SelectSchema(*dbd, dbc::Profile(dbc::GameVersion::BattleForAzeroth),
                                dbc::Build::Parse("8.3.7.35662"), {}, error);
    auto sl = dbc::SelectSchema(*dbd, dbc::Profile(dbc::GameVersion::Shadowlands),
                               dbc::Build::Parse("9.2.7.45745"), {}, error);
    if (!bfa || !sl) throw std::runtime_error("Missing BfA/Shadowlands schema.");
    auto flagsSize = [](auto const& schema) {
      for (auto const& field : schema.fields) if (field.name == "Flags") return field.arraySize;
      return 0u;
    };
    if (flagsSize(bfa->version) != 2 || flagsSize(sl->version) != 3)
      throw std::runtime_error("BfA/Shadowlands array difference was not preserved.");
    std::cout << "PASS " << dbc::FormatName(header.format) << ": " << original.size()
              << " unchanged rows, new native row, extended flags, duplicate rejection, atomic failure, build schemas\n";
    return 0;
  }
  catch (std::exception const& e) { std::cerr << e.what() << '\n'; return 1; }
}
