#pragma once

#include "RetailObjectTables.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace Noggit::Formats::ADT::RetailObjectTransfer
{
  using namespace RetailObjectTables;

  template<class T> inline void store(Bytes& bytes, std::size_t at, T value)
  {
    if (at > bytes.size() || sizeof(T) > bytes.size() - at)
      throw std::runtime_error("Retail transfer output offset outside record");
    std::memcpy(bytes.data() + at, &value, sizeof(T));
  }

  // Default MLDB (M2) and MLMB (WMO) values for instances without a stored byte.
  constexpr std::uint8_t defaultModelByte = 64, defaultWmoByte = 128;

  struct Instance
  {
    Bytes record;
    bool wmo = false;
    std::optional<std::uint8_t> byte;
    std::optional<std::uint32_t> lod;
    std::optional<Bytes> definition; // one opaque 32-byte MDLD record
    std::vector<std::uint16_t> sets; // doodad set values, not MWDR indices
    std::uint8_t levels = 1;
  };

  // Source OBJ0/OBJ1 metadata of a WMO placement. MWDR/MWDS selectors are
  // rebuilt for each destination file from the stored set values.
  struct WmoSource
  {
    Instance primary;
    std::optional<Instance> lod;
    bool lodKnown = false;
  };
  using WmoSources = std::map<std::uint32_t, WmoSource>;

  struct WmoState
  {
    std::uint16_t flags = 0, nameSet = 0, doodadSet = 0;
    std::optional<WmoSource> source;
  };

  inline std::map<std::size_t, Instance> read(Bytes const& bytes, bool lod)
  {
    validate(bytes, lod);
    auto const table = chunks(bytes);
    std::map<std::size_t, Instance> result;
    for (bool wmo : {false, true})
    {
      auto const tag = wmo ? (lod ? "DMLM" : "FDOM") : (lod ? "DDLM" : "FDDM");
      auto const stride = wmo ? (lod ? 40u : 64u) : 36u;
      for (std::size_t i = 0; i < count(table, tag, stride); ++i)
      {
        auto at = table.at(tag).offset + i * stride;
        Instance data;
        data.wmo = wmo;
        data.record.assign(bytes.begin() + at, bytes.begin() + at + stride);
        if (auto it = table.find(wmo ? "BMLM" : "BDLM"); it != table.end())
          data.byte = bytes[it->second.offset + i];
        if (wmo && (word(bytes, at + (lod ? 32 : 56)) & 0x80))
        {
          auto index = word(bytes, at + (lod ? 32 : 56)) >> 16;
          auto range = table.at("RDWM").offset + index * 8ull;
          auto first = word(bytes, range), last = word(bytes, range + 4);
          for (std::uint64_t n = first; n <= last; ++n)
          {
            std::uint16_t value;
            std::memcpy(&value, bytes.data() + table.at("SDWM").offset + n * 2ull, 2);
            data.sets.push_back(value);
          }
        }
        if (lod)
        {
          if (!wmo)
          {
            if (auto it = table.find("LDLM"); it != table.end())
              data.lod = word(bytes, it->second.offset + i * 4);
            if (auto it = table.find("ILDM"); it != table.end())
            {
              auto index = word(bytes, it->second.offset + i * 4);
              if (index != UINT32_MAX)
              {
                auto start = table.at("DLDM").offset + index * 32ull;
                data.definition = Bytes(bytes.begin() + start, bytes.begin() + start + 32);
              }
            }
          }
          if (auto it = table.find("DFLM"); it != table.end())
          {
            data.levels = 0;
            for (unsigned level = 0; level < 3; ++level)
            {
              auto offset = it->second.offset + (wmo ? 24 : 0) + level * 4;
              auto first = word(bytes, offset), length = word(bytes, offset + 12);
              if (i >= first && i - first < length) data.levels |= 1 << level;
            }
          }
        }
        result.emplace(at, std::move(data));
      }
    }
    return result;
  }

  inline WmoSources readWmoSources(Bytes const& primary, std::optional<Bytes> const& lod, bool lodKnown = true)
  {
    WmoSources result;
    for (auto const& [offset, data] : read(primary, false))
      if (data.wmo)
      {
        auto [it, added] = result.emplace(word(data.record, 4), WmoSource{data, {}, lodKnown});
        if (!added) throw std::runtime_error("Ambiguous Retail WMO UID in primary metadata");
      }
    if (lod)
      for (auto const& [offset, data] : read(*lod, true))
        if (data.wmo)
          if (auto it = result.find(word(data.record, 4)); it != result.end())
          {
            if (word(it->second.primary.record, 0) != word(data.record, 0) || it->second.lod)
              throw std::runtime_error("Ambiguous Retail WMO LOD metadata");
            it->second.lod = data;
          }
    return result;
  }

  struct Row
  {
    Instance data;
    std::array<float, 7> extent{};
  };

  inline std::array<float, 7> extent(std::array<float, 6> const& world)
  {
    constexpr float midpoint = 17066.666f;
    std::array<float, 7> result{midpoint-world[5], midpoint-world[3], world[1],
      midpoint-world[2], midpoint-world[0], world[4], 0};
    for (int axis = 0; axis < 3; ++axis)
      if (!std::isfinite(result[axis]) || !std::isfinite(result[axis+3])
          || result[axis] > result[axis+3])
        throw std::runtime_error("Invalid Retail LOD bounds");
    result[6] = std::sqrt(std::pow(result[3]-result[0], 2.f)
      + std::pow(result[4]-result[1], 2.f) + std::pow(result[5]-result[2], 2.f));
    if (!std::isfinite(result[6])) throw std::runtime_error("Invalid Retail LOD radius");
    return result;
  }

  // Each MLFD level is one contiguous interval. Rows are grouped by level
  // membership (overlap allowed) and sorted by descending radius within a group.
  inline void order(std::vector<Row>& rows)
  {
    std::vector<unsigned> masks;
    for (auto const& row : rows)
      if (std::find(masks.begin(), masks.end(), row.data.levels) == masks.end())
        masks.push_back(row.data.levels);
    std::sort(masks.begin(), masks.end());
    auto contiguous = [&]
    {
      for (unsigned bit : {1u, 2u, 4u})
      {
        bool seen = false, ended = false;
        for (auto mask : masks)
          if (mask & bit) { if (ended) return false; seen = true; }
          else if (seen) ended = true;
      }
      return true;
    };
    while (!contiguous())
      if (!std::next_permutation(masks.begin(), masks.end()))
        throw std::runtime_error("Conflicting Retail LOD memberships cannot form MLFD intervals");
    std::array<unsigned, 8> rank{};
    for (unsigned i = 0; i < masks.size(); ++i) rank[masks[i]] = i;
    std::stable_sort(rows.begin(), rows.end(), [&](auto const& a, auto const& b)
    {
      auto ra = rank[a.data.levels], rb = rank[b.data.levels];
      if (ra != rb) return ra < rb;
      if (a.extent[6] != b.extent[6]) return a.extent[6] > b.extent[6];
      return word(a.data.record, 4) < word(b.data.record, 4);
    });
  }

  inline Bytes write(Bytes const& original, std::vector<Row> rows, bool lod)
  {
    auto const table = chunks(original);
    std::vector<Row> models, wmos;
    for (auto& row : rows)
    {
      if (row.data.record.size() != (row.data.wmo ? (lod ? 40u : 64u) : 36u)
          || row.data.levels > 7)
        throw std::runtime_error("Invalid Retail transfer record");
      (row.data.wmo ? wmos : models).push_back(std::move(row));
    }
    if (lod) { order(models); order(wmos); }
    std::map<std::string, Bytes> replacements;
    std::map<std::vector<std::uint16_t>, std::uint16_t> selectors;
    std::map<Bytes, std::uint32_t> definitions;
    Bytes ranges, sets, definitionBytes, groupBytes(48);
    for (bool wmo : {false, true})
    {
      auto& entries = wmo ? wmos : models;
      bool hasByte = table.count(wmo ? "BMLM" : "BDLM") != 0;
      bool hasLod = !wmo && lod && table.count("LDLM");
      bool hasDefinition = !wmo && lod && (table.count("ILDM") || table.count("DLDM"));
      Bytes records, bounds, byteValues, lodValues, indices;
      for (auto const& row : entries)
      {
        auto const& data = row.data;
        auto record = data.record;
        if (wmo && (word(record, lod ? 32 : 56) & 0x80))
        {
          if (data.sets.empty()) throw std::runtime_error("Missing Retail WMO set values");
          auto it = selectors.find(data.sets);
          if (it == selectors.end())
          {
            if (selectors.size() > UINT16_MAX || sets.size()/2 + data.sets.size() > UINT32_MAX)
              throw std::runtime_error("Retail WMO selector table is too large");
            auto index = static_cast<std::uint16_t>(selectors.size());
            it = selectors.emplace(data.sets, index).first;
            auto at = ranges.size(); ranges.resize(at+8);
            store(ranges, at, static_cast<std::uint32_t>(sets.size()/2));
            store(ranges, at+4, static_cast<std::uint32_t>(sets.size()/2+data.sets.size()-1));
            for (auto value : data.sets)
            { auto start = sets.size(); sets.resize(start+2); store(sets, start, value); }
          }
          store(record, lod ? 34 : 58, it->second);
        }
        records.insert(records.end(), record.begin(), record.end());
        hasByte |= data.byte.has_value();
        byteValues.push_back(data.byte.value_or(wmo ? defaultWmoByte : defaultModelByte));
        if (lod)
        {
          auto at = bounds.size(); bounds.resize(at+28);
          std::memcpy(bounds.data()+at, row.extent.data(), 28);
          if (!wmo)
          {
            hasLod |= data.lod.has_value();
            at = lodValues.size(); lodValues.resize(at+4);
            store(lodValues, at, data.lod.value_or(0));
            auto index = UINT32_MAX;
            if (data.definition)
            {
              if (data.definition->size() != 32) throw std::runtime_error("Invalid Retail MDLD record");
              hasDefinition = true;
              auto [it, added] = definitions.emplace(*data.definition,
                static_cast<std::uint32_t>(definitions.size()));
              index = it->second;
              if (added) definitionBytes.insert(definitionBytes.end(), data.definition->begin(), data.definition->end());
            }
            at = indices.size(); indices.resize(at+4); store(indices, at, index);
          }
        }
      }
      replacements[wmo ? (lod ? "DMLM" : "FDOM") : (lod ? "DDLM" : "FDDM")] = std::move(records);
      if (wmo || lod)
        if (hasByte) replacements[wmo ? "BMLM" : "BDLM"] = std::move(byteValues);
      if (lod)
      {
        replacements[wmo ? "XMLM" : "XDLM"] = std::move(bounds);
        if (hasLod) replacements["LDLM"] = std::move(lodValues);
        if (hasDefinition) { replacements["ILDM"] = std::move(indices); replacements["DLDM"] = definitionBytes; }
        for (unsigned level = 0; level < 3; ++level)
        {
          std::uint32_t first = 0, length = 0;
          for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].data.levels & (1 << level))
            { if (!length) first = static_cast<std::uint32_t>(i); ++length; }
          auto at = (wmo ? 24 : 0) + level * 4;
          store(groupBytes, at, first); store(groupBytes, at+12, length);
        }
      }
    }
    if (lod) replacements["DFLM"] = std::move(groupBytes);
    if (!ranges.empty() || table.count("RDWM") || table.count("SDWM"))
    { replacements["RDWM"] = std::move(ranges); replacements["SDWM"] = std::move(sets); }
    Bytes result;
    auto append = [&](std::string const& tag, Bytes const& data)
    {
      if (data.size() > UINT32_MAX) throw std::runtime_error("Retail object chunk too large");
      auto at = result.size(); result.resize(at+8+data.size());
      std::memcpy(result.data()+at, tag.data(), 4);
      store(result, at+4, static_cast<std::uint32_t>(data.size()));
      std::copy(data.begin(), data.end(), result.begin()+at+8);
    };
    // Added chunks follow Retail chunk order: in OBJ0 the dictionaries precede
    // MCNK; in OBJ1 MLFD precedes the placement arrays.
    std::vector<std::string> const chunkOrder = lod
      ? std::vector<std::string>{"REVM","DFLM","DDLM","XDLM","LDLM","BDLM","ILDM","DLDM","DMLM","XMLM","BMLM","RDWM","SDWM"}
      : std::vector<std::string>{"REVM","FDDM","FDOM","BMLM","RDWM","SDWM","KNCM"};
    for (std::size_t at = 0; at < original.size();)
    {
      auto size = word(original, at+4);
      std::string tag(reinterpret_cast<char const*>(original.data()+at), 4);
      auto position = std::find(chunkOrder.begin(), chunkOrder.end(), tag);
      if (position != chunkOrder.end())
        for (auto next = chunkOrder.begin(); next != position; ++next)
          if (!table.count(*next))
            if (auto it = replacements.find(*next); it != replacements.end())
            { append(it->first, it->second); replacements.erase(it); }
      if (auto it = replacements.find(tag); it != replacements.end())
      { append(tag, it->second); replacements.erase(it); }
      else result.insert(result.end(), original.begin()+at, original.begin()+at+8+size);
      at += 8+size;
    }
    for (auto const& tag : chunkOrder)
      if (auto it = replacements.find(tag); it != replacements.end())
      { append(it->first, it->second); replacements.erase(it); }
    if (!replacements.empty()) throw std::runtime_error("Unordered Retail transfer chunk");
    validate(result, lod);
    return result;
  }

  inline Bytes writePrimary(Bytes const& original, Bytes const& updated, WmoSources const& sources,
                            bool destinationHasLod = true)
  {
    auto const prior = read(original, false);
    auto const next = chunks(updated);
    std::map<std::pair<bool, std::uint64_t>, Instance> identities;
    for (auto const& [offset, data] : prior)
    {
      auto key = std::make_pair(data.wmo, (std::uint64_t(word(data.record, 0)) << 32) | word(data.record, 4));
      if (!identities.emplace(key, data).second)
        throw std::runtime_error("Ambiguous Retail placement identity");
    }
    std::vector<Row> rows;
    for (bool wmo : {false, true})
    {
      auto const* tag = wmo ? "FDOM" : "FDDM";
      std::size_t const stride = wmo ? 64 : 36;
      for (std::size_t i = 0; i < count(next, tag, stride); ++i)
      {
        auto const at = next.at(tag).offset + i * stride;
        auto const id = word(updated, at), uid = word(updated, at + 4);
        auto const local = identities.find({wmo, (std::uint64_t(id) << 32) | uid});
        Instance data;
        if (local != identities.end()) data = local->second;
        auto const source = wmo ? sources.find(uid) : sources.end();
        if (source != sources.end())
        {
          if (source->second.primary.record.size() != 64 || !source->second.primary.wmo
              || word(source->second.primary.record, 0) != id)
            throw std::runtime_error("Retail WMO source metadata belongs to a different asset");
          if (local == identities.end()) data = source->second.primary;
          if (local == identities.end() && !source->second.lodKnown)
            throw std::runtime_error("Retail WMO source LOD could not be read; reload its source tile before copying or moving it");
          if (local == identities.end() && source->second.lod && !destinationHasLod)
            throw std::runtime_error("Retail WMO transfer requires a destination OBJ1 template to preserve its source LOD metadata");
          data.sets = source->second.primary.sets;
        }
        data.wmo = wmo;
        data.record.assign(updated.begin() + at, updated.begin() + at + stride);
        if (wmo && (word(data.record, 56) & 0x80) && source == sources.end())
          throw std::runtime_error("Retail WMO multiset placement has no semantic source metadata");
        if (wmo && !(word(data.record, 56) & 0x80)) data.sets.clear();
        rows.push_back({std::move(data), {}});
      }
    }
    return write(updated, std::move(rows), false);
  }
}
