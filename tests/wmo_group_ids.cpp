// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/formats/WmoGroupIds.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

static void require(bool ok) { if (!ok) std::abort(); }
static void put32(std::vector<char>& out, std::uint32_t n)
{ auto p = reinterpret_cast<char const*>(&n); out.insert(out.end(), p, p+4); }
static void chunk(std::vector<char>& out, char const* tag, std::vector<char> const& data)
{ out.insert(out.end(), tag, tag+4); put32(out, static_cast<std::uint32_t>(data.size())); out.insert(out.end(), data.begin(), data.end()); }
static std::vector<char> root(unsigned groups)
{
  std::vector<char> out, version; put32(version,17); chunk(out,"REVM",version);
  std::vector<char> header(64); std::memcpy(header.data()+4,&groups,4); chunk(out,"DHOM",header); return out;
}
static void ids(std::vector<char>& out, std::initializer_list<std::uint32_t> list)
{ std::vector<char> data; for (auto id:list) put32(data,id); chunk(out,"DIFG",data); }
static void rejects(std::vector<char> const& data)
{ bool rejected=false; try { Noggit::Format::primaryWmoGroupIds(data); } catch (std::runtime_error const&) { rejected=true; } require(rejected); }
int main()
{
  std::vector<char> group, versionBytes; put32(versionBytes,17);
  chunk(group,"REVM",versionBytes); chunk(group,"PGOM",std::vector<char>(68));
  Noggit::Format::validateRetailWmoGroup(group);
  for (int failure=0; failure<4; ++failure)
  {
    auto broken=group;
    if (failure==0) broken.pop_back();
    if (failure==1) broken[8]=18;
    if (failure==2) broken[12]='X';
    if (failure==3) broken[16]=67;
    bool caught=false;
    try { Noggit::Format::validateRetailWmoGroup(broken); } catch (std::runtime_error const&) { caught=true; }
    require(caught);
  }
  auto data=root(2); require(Noggit::Format::primaryWmoGroupIds(data).empty());
  ids(data,{101,202,303,404,505,606});
  require(Noggit::Format::primaryWmoGroupIds(data)==std::vector<std::uint32_t>({101,202}));
  auto duplicate=data; ids(duplicate,{101,202}); rejects(duplicate);
  auto truncated=data; truncated.pop_back(); rejects(truncated);
  auto trailing=data; trailing.push_back(0); rejects(trailing);
  auto shortTable=root(2); ids(shortTable,{101}); rejects(shortTable);
  auto partialLod=root(2); ids(partialLod,{101,202,303}); rejects(partialLod);
  auto zero=root(2); ids(zero,{101,0}); rejects(zero);
  auto noGroups=root(0); ids(noGroups,{}); require(Noggit::Format::primaryWmoGroupIds(noGroups).empty());
  ids(noGroups,{1}); rejects(noGroups);
  auto badBytes=root(1); chunk(badBytes,"DIFG",{1,2,3}); rejects(badBytes);
  auto badHeader=root(1); badHeader[0]='X'; rejects(badHeader);
  auto version=root(1); version[8]=18; rejects(version);
  auto unknown=root(1); chunk(unknown,"ZZZZ",{1,2,3}); ids(unknown,{123});
  require(Noggit::Format::primaryWmoGroupIds(unknown)==std::vector<std::uint32_t>({123}));
  std::cout << "Retail WMO GFID validation passed\n";
}
