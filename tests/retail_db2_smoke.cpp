#include <BlizzardDatabase.h>
#include <stream/StreamReader.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>

int main(int argc, char** argv)
{
  if (argc != 4) return 2;
  try {
    BlizzardDatabaseLib::BlizzardDatabase database(argv[1],
      BlizzardDatabaseLib::Structures::Build("12.1.5.70575"), false);
    std::string name(argv[3]);
    std::ifstream input(std::string(argv[2]) + "/" + name + ".db2", std::ios::binary);
    if (!input) throw std::runtime_error("Missing DB2");
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    auto& table = database.LoadTable(name, [&](std::string const&) {
      return std::make_shared<BlizzardDatabaseLib::Stream::IMemStream>(bytes.data(), bytes.size());
    });
    // WMOAreaTable still uses the existing native-only safety path in Noggit.
    std::set<unsigned> indices;
    if (name != "WMOAreaTable") {
      for (unsigned i = 0; i < std::min(16u, table.RecordCount()); ++i) indices.insert(i);
      if (table.RecordCount()) indices.insert(table.RecordCount() - 1);
    }
    for (auto i : indices) {
      auto row = table.RecordByPosition(i);
      if (row.Columns.empty()) throw std::runtime_error("Empty decoded row");
    }
    if (name == "Map") {
      for (auto id : {2560u, 2775u, 2777u, 3561u, 3562u, 3563u}) {
        auto row = table.RecordById(id);
        auto directory = row.Columns.at("Directory").Value;
        if (directory.empty()) throw std::runtime_error("Missing custom map directory");
        std::cout << "CUSTOM MAP " << id << ": " << directory << '\n';
      }
    }
    std::cout << "PASS " << name << ": " << table.RecordCount()
              << " records, " << indices.size() << " decoded sample rows\n";
    return 0;
  } catch (std::exception const& e) {
    std::cerr << "FAIL " << argv[3] << ": " << e.what() << '\n';
    return 1;
  }
}
