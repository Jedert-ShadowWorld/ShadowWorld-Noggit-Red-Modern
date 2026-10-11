// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/formats/adt/TextureMetadata.hpp>
#include <array>
#include <iostream>
#include <string>

using Noggit::Formats::ADT::remapTextureMetadata;

void require(bool value, char const* message)
{
  if (!value)
    throw std::runtime_error(message);
}

template<typename Function>
void rejects(Function function)
{
  try { function(); }
  catch (std::runtime_error const&) { return; }
  throw std::runtime_error("Invalid metadata was accepted.");
}

int main()
{
  try
  {
    std::array<std::uint32_t, 3> old_ids{10, 20, 30};
    std::array<std::uint32_t, 3> new_ids{30, 10, 40};
    std::array<std::uint8_t, 16> fallback{};
    fallback[10] = 128;
    fallback[11] = 63; // IEEE-754 height offset 1.0f.
    std::vector<std::uint8_t> mtxp(48);
    for (std::size_t i = 0; i < mtxp.size(); ++i)
      mtxp[i] = static_cast<std::uint8_t>(i + 1);
    auto remapped = remapTextureMetadata(old_ids, new_ids, mtxp, fallback);
    require(remapped.size() == 48, "MTXP record count changed.");
    require(std::equal(mtxp.begin() + 32, mtxp.end(), remapped.begin()), "MTXP reorder failed.");
    require(std::equal(mtxp.begin(), mtxp.begin() + 16, remapped.begin() + 16), "MTXP preservation failed.");
    require(std::equal(fallback.begin(), fallback.end(), remapped.begin() + 32), "New texture default failed.");

    std::array<std::uint8_t, 4> no_height{};
    std::array<std::uint8_t, 12> mhid{1,0,0,0, 2,0,0,0, 3,0,0,0};
    require(remapTextureMetadata(old_ids, new_ids, mhid, no_height)
      == std::vector<std::uint8_t>({3,0,0,0, 1,0,0,0, 0,0,0,0}), "MHID reorder failed.");
    require(remapTextureMetadata(old_ids, new_ids, std::span(mhid).first(4), no_height)
      == std::vector<std::uint8_t>({0,0,0,0, 1,0,0,0, 0,0,0,0}), "Short table default failed.");
    require(remapTextureMetadata({}, new_ids, {}, no_height).size() == 12, "Empty table failed.");
    rejects([&] { remapTextureMetadata(old_ids, new_ids, std::span(mhid).first(3), no_height); });
    rejects([&] { remapTextureMetadata({}, new_ids, mhid, no_height); });
    rejects([&] { remapTextureMetadata(old_ids, new_ids, mhid, {}); });
    std::array<std::uint32_t, 2> duplicates{10, 10};
    rejects([&] { remapTextureMetadata(duplicates, new_ids, std::span(mhid).first(8), no_height); });
    std::array<std::uint8_t, 8> identical{1,0,0,0, 1,0,0,0};
    require(remapTextureMetadata(duplicates, new_ids, identical, no_height)[4] == 1, "Duplicate equal records failed.");
    std::cout << "PASS: MTXP/MHID reorder, preservation, defaults and invalid metadata.\n";
    return 0;
  }
  catch (std::exception const& error)
  {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
