#include <noggit/DBCFile.h>
#include <chrono>
#include <iostream>
#include <limits>
#include <numeric>

// The standalone harness compiles the matching production DBCFile methods.
int main()
{
  try
  {
    auto require = [](bool condition) {
      if (!condition) throw std::runtime_error("Bulk DBC regression failed");
    };
    std::vector<unsigned int> ids(333900);
    std::iota(ids.begin(), ids.end(), 1u);
    auto start = std::chrono::steady_clock::now();
    auto table = DBCFile::createNewWithIds("SoundEntries.dbc", 30, 120, ids);
    require(table.getRecordCount() == ids.size());
    require(table.getRecordSize() == 120);
    for (size_t i = 0; i < ids.size(); ++i)
    {
      auto row = table.getRecord(i);
      require(row.getUInt(0) == ids[i]);
      require(row.getUInt(29) == 0);
      require(std::string(row.getString(2)).empty());
      row.writeString(2, "SoundKit " + std::to_string(ids[i]));
      row.write(24, 0.5f);
    }
    for (size_t i : {size_t{0}, size_t{1000}, ids.size() - 1})
    {
      auto row = table.getRecord(i);
      require(row.getUInt(0) == ids[i]);
      require(std::string(row.getString(2)) == "SoundKit " + std::to_string(ids[i]));
      require(row.getFloat(24) == 0.5f);
    }
    bool duplicate = false;
    try { DBCFile::createNewWithIds("duplicate", 2, 8, {4, 4}); }
    catch (DBCFile::AlreadyExists const&) { duplicate = true; }
    require(duplicate);
    bool invalid = false;
    try { DBCFile::createNewWithIds("invalid", 0, 0, {}); }
    catch (std::invalid_argument const&) { invalid = true; }
    require(invalid);
    invalid = false;
    try { DBCFile::createNewWithIds("invalid", 2, 4, {1}); }
    catch (std::invalid_argument const&) { invalid = true; }
    require(invalid);
    require(DBCFile::createNewWithIds("empty", 2, 8, {}).getRecordCount() == 0);
    auto boundary = DBCFile::createNewWithIds("boundary", 2, 8,
        {0, std::numeric_limits<unsigned int>::max()});
    require(boundary.getRecord(0).getUInt(0) == 0);
    require(boundary.getRecord(1).getUInt(0) == std::numeric_limits<unsigned int>::max());
    std::cout << "PASS: 333900 rows, IDs, zero initialization, strings, floats, duplicates, invalid layouts, empty table; "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
              << " ms\n";
    return 0;
  }
  catch (std::exception const& e) { std::cerr << e.what() << '\n'; return 1; }
}
