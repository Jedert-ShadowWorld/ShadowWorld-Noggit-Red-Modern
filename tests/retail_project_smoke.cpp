#include <ClientData.hpp>
#include <ClientFile.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
#include <stdexcept>

int main(int argc, char** argv)
{
  if (argc != 3) return 2;
  try
  {
    BlizzardArchive::ClientData client(argv[1], BlizzardArchive::ClientVersion::RETAIL,
                                      BlizzardArchive::Locale::enUS, argv[2]);
    auto const base = std::filesystem::path(argv[2]) / "listfile.csv";
    if (client.listfile()->csvPath() != base.string())
      throw std::runtime_error("CASC preload listfile was replaced by the custom overlay");
    for (auto id : {1349477u, 5554791u, 929200000u, 928000001u, 929200003u})
    {
      BlizzardArchive::Listfile::FileKey key(id);
      if (!key.deduceOtherComponent(client.listfile()))
        throw std::runtime_error("Custom ID did not resolve: " + std::to_string(id));
      auto const path = client.getDiskPath(key);
      std::ifstream input(path, std::ios::binary);
      if (!input) throw std::runtime_error("Missing project override: " + path);
      std::string expected((std::istreambuf_iterator<char>(input)), {});
      BlizzardArchive::ClientFile file(key, &client);
      if (file.getSize() != expected.size()
          || std::string(file.getBuffer(), file.getSize()) != expected)
        throw std::runtime_error("Noggit read a different file: " + path);
      std::cout << "PASS " << id << ": " << file.getSize() << " bytes from project override\n";
    }
    BlizzardArchive::Listfile::FileKey stock(167163u);
    std::vector<char> bytes;
    if (!client.readFile(stock, bytes) || bytes.empty())
      throw std::runtime_error("Standard Retail CASC asset failed");
    std::cout << "PASS: custom map overrides and standard Retail CASC asset coexist\n";
    for (auto id : {3794112u, 4684646u, 4684647u, 5648066u, 3749306u, 4033885u, 3753184u, 3753183u, 3754010u, 3753185u}) {
      BlizzardArchive::ClientFile model(BlizzardArchive::Listfile::FileKey(id), &client);
      auto data = model.getBuffer();
      auto size = model.getSize();
      unsigned skin = 0;
      for (size_t offset = 0; offset + 8 <= size;) {
        unsigned length;
        std::memcpy(&length, data + offset + 4, 4);
        if (length > size - offset - 8) throw std::runtime_error("Truncated M2 container");
        if (!std::memcmp(data + offset, "SFID", 4) && length >= 4) {
          std::memcpy(&skin, data + offset + 8, 4);
          break;
        }
        offset += 8 + length;
      }
      if (!skin) throw std::runtime_error("Missing model SFID");
      BlizzardArchive::ClientFile file(BlizzardArchive::Listfile::FileKey(skin), &client);
      if (file.getSize() < 48 || std::memcmp(file.getBuffer(), "SKIN", 4))
        throw std::runtime_error("Invalid skin referenced by model " + std::to_string(id));
      std::cout << "PASS M2 " << id << " -> SFID " << skin << ": " << file.getSize() << " bytes\n";
    }
  }
  catch (std::exception const& e)
  {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
