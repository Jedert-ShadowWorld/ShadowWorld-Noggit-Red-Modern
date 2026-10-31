#include <ClientData.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

// Link the listfile parser independently of the archive backends.
std::string BlizzardArchive::ClientData::normalizeFilenameInternal(std::string value)
{
  std::replace(value.begin(), value.end(), '\\', '/');
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value;
}
std::string BlizzardArchive::ClientData::normalizeFilenameWoW(std::string value)
{
  std::replace(value.begin(), value.end(), '/', '\\');
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
  return value;
}
int main()
{
  auto const dir = std::filesystem::temp_directory_path() / "noggit-custom-listfile-test";
  std::filesystem::create_directories(dir);
  auto const base = dir / "listfile.csv";
  auto const custom = dir / "custom-listfile.csv";
  std::ofstream(base) << "100;world/base.blp\n200;world/old.blp\n";
  std::ofstream(custom) << "200;world/replacement.blp\n929200000;world/maps/halim/halim.wdt\n";
  BlizzardArchive::Listfile::Listfile listfile;
  listfile.initFromCSV(base.string());
  listfile.initFromCSV(custom.string());
  if (listfile.csvPath() != base.string()
      || listfile.getFileDataID("world/base.blp") != 100
      || listfile.getPath(200) != "WORLD\\REPLACEMENT.BLP"
      || listfile.getFileDataID("world/maps/halim/halim.wdt") != 929200000)
    throw std::runtime_error("Custom listfile overlay regression");
  std::ofstream(base) << "101;world/new.blp\n200;world/old.blp\n";
  BlizzardArchive::Listfile::Listfile refreshed;
  refreshed.initFromCSV(base.string());
  refreshed.initFromCSV(custom.string());
  if (refreshed.getFileDataID("world/new.blp") != 101
      || refreshed.getFileDataID("world/maps/halim/halim.wdt") != 929200000)
    throw std::runtime_error("Custom IDs lost after community listfile refresh");
  std::filesystem::remove(base);
  std::filesystem::remove(custom);
  std::filesystem::remove(dir);
  std::cout << "PASS: custom overrides preserved across community listfile refresh\n";
}
