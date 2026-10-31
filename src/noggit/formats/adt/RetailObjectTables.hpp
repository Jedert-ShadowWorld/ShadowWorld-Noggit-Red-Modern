#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace Noggit::Formats::ADT::RetailObjectTables
{
  using Bytes = std::vector<std::uint8_t>;
  struct Chunk { std::size_t offset, size; };
  using Chunks = std::map<std::string, Chunk>;

  inline std::uint32_t word(Bytes const& bytes, std::size_t at)
  {
    if (at > bytes.size() || bytes.size() - at < 4)
      throw std::runtime_error("Truncated Retail object table");
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
  }

  inline Chunks chunks(Bytes const& bytes)
  {
    Chunks result;
    for (std::size_t at = 0; at < bytes.size();)
    {
      if (bytes.size() - at < 8)
        throw std::runtime_error("Truncated Retail object chunk");
      auto size = word(bytes, at + 4);
      if (size > bytes.size() - at - 8)
        throw std::runtime_error("Retail object chunk extends beyond file");
      std::string tag(reinterpret_cast<char const*>(bytes.data() + at), 4);
      if (tag != "KNCM" && !result.emplace(tag, Chunk{at + 8, size}).second)
        throw std::runtime_error("Duplicate Retail object chunk: " + tag);
      at += 8 + size;
    }
    return result;
  }

  inline std::size_t count(Chunks const& table, char const* tag, std::size_t stride)
  {
    auto it = table.find(tag);
    if (it == table.end()) return 0;
    if (it->second.size % stride)
      throw std::runtime_error("Invalid Retail placement record size");
    return it->second.size / stride;
  }

  inline void validate(Bytes const& bytes, bool lod)
  {
    auto table = chunks(bytes);
    auto models = count(table, lod ? "DDLM" : "FDDM", 36);
    auto wmos = count(table, lod ? "DMLM" : "FDOM", lod ? 40 : 64);
    auto parallel = [&](char const* tag, std::size_t expected, char const* name)
    {
      auto it = table.find(tag);
      if (it != table.end() && it->second.size != expected)
        throw std::runtime_error(std::string("Retail ") + name
          + " does not match its placement count; restore the original object files before saving.");
    };
    parallel("BMLM", wmos, "MLMB");
    for (auto const* tag : {"RDWM", "SDWM"})
      if (auto it = table.find(tag); it != table.end() && it->second.size % (tag[0] == 'R' ? 8 : 2))
        throw std::runtime_error("Invalid Retail MWDR/MWDS table size");
    auto wmoTag = lod ? "DMLM" : "FDOM";
    for (std::size_t i = 0; i < wmos; ++i)
    {
      auto options = word(bytes, table.at(wmoTag).offset + i * (lod ? 40 : 64) + (lod ? 32 : 56));
      if (!(options & 0x80)) continue;
      auto ranges = table.find("RDWM"), sets = table.find("SDWM");
      auto index = options >> 16;
      if (ranges == table.end() || sets == table.end() || index >= ranges->second.size / 8)
        throw std::runtime_error("Retail WMO refers to a missing MWDR/MWDS entry");
      auto start = word(bytes, ranges->second.offset + index * 8ull);
      auto end = word(bytes, ranges->second.offset + index * 8ull + 4);
      if (end < start || end >= sets->second.size / 2)
        throw std::runtime_error("Retail MWDR range extends beyond MWDS");
    }
    if (!lod) return;
    // MLDB holds one opaque byte per MLDD record.
    parallel("BDLM", models, "MLDB");
    parallel("LDLM", models * 4, "MLDL");
    parallel("ILDM", models * 4, "MDLI");
    parallel("XDLM", models * 28, "MLDX");
    parallel("XMLM", wmos * 28, "MLMX");
    auto definitions = count(table, "DLDM", 32);
    if (auto it = table.find("ILDM"); it != table.end())
      for (std::size_t i = 0; i < models; ++i)
      {
        auto index = word(bytes, it->second.offset + i * 4);
        if (index != UINT32_MAX && index >= definitions)
          throw std::runtime_error("Retail MDLI refers to a missing MDLD definition");
      }
    if (auto it = table.find("DFLM"); it != table.end())
    {
      if (it->second.size != 48) throw std::runtime_error("Invalid Retail MLFD size");
      for (std::size_t kind = 0; kind < 2; ++kind)
        for (std::size_t level = 0; level < 3; ++level)
        {
          auto at = it->second.offset + kind * 24 + level * 4;
          std::uint64_t end = word(bytes, at);
          end += word(bytes, at + 12);
          if (end > (kind ? wmos : models))
            throw std::runtime_error("Retail MLFD range extends beyond its placements");
        }
    }
  }

  // Reorders per-instance auxiliary tables to follow placements by FDID/UID.
  // Placements absent from the original have no auxiliary data and are rejected.
  inline Bytes remap(Bytes const& original, Bytes const& updated, bool lod)
  {
    validate(original, lod);
    auto old = chunks(original), next = chunks(updated);
    std::map<std::string, Bytes> replacements;
    for (bool wmo : {false, true})
    {
      auto tag = wmo ? (lod ? "DMLM" : "FDOM") : (lod ? "DDLM" : "FDDM");
      auto stride = wmo ? (lod ? 40u : 64u) : 36u;
      auto oldCount = count(old, tag, stride), newCount = count(next, tag, stride);
      auto key = [](Bytes const& b, std::size_t at)
      { return (std::uint64_t(word(b, at)) << 32) | word(b, at + 4); };
      std::map<std::uint64_t, std::size_t> indices;
      for (std::size_t i = 0; i < oldCount; ++i)
      {
        auto [it, inserted] = indices.emplace(key(original, old.at(tag).offset + i * stride), i);
        if (!inserted) it->second = std::size_t(-1);
      }
      auto remapTable = [&](char const* name, std::size_t width)
      {
        auto source = old.find(name);
        if (source == old.end()) return;
        Bytes data;
        data.reserve(newCount * width);
        for (std::size_t i = 0; i < newCount; ++i)
        {
          auto at = next.at(tag).offset + i * stride;
          auto it = indices.find(key(updated, at));
          if (it == indices.end())
            throw std::runtime_error("Retail placement UID " + std::to_string(word(updated, at + 4))
              + " needs auxiliary data from its source tile; this edit is unsupported.");
          if (it->second == std::size_t(-1))
            throw std::runtime_error("Ambiguous Retail placement UID in auxiliary tables");
          auto start = original.begin() + source->second.offset + it->second * width;
          data.insert(data.end(), start, start + width);
        }
        replacements.emplace(name, std::move(data));
      };
      if (wmo) remapTable("BMLM", 1);
      else if (lod)
      {
        remapTable("BDLM", 1);
        remapTable("ILDM", 4);
        remapTable("LDLM", 4);
      }
      // MWDR/MWDS are indexed by instance options, not by placement index,
      // and are local to this file.
      if (wmo)
        for (std::size_t i = 0; i < newCount; ++i)
        {
          auto at = next.at(tag).offset + i * stride;
          auto flagsOffset = lod ? 32u : 56u;
          auto flags = word(updated, at + flagsOffset) & 0xffff;
          if (!(flags & 0x80) && !old.count("RDWM") && !old.count("SDWM")) continue;
          auto it = indices.find(key(updated, at));
          auto tail = lod ? 34u : 58u;
          if (it == indices.end() || it->second == std::size_t(-1)
              || ((word(original, old.at(tag).offset + it->second * stride + flagsOffset) ^ flags) & 0x80)
              || std::memcmp(updated.data() + at + tail,
                original.data() + old.at(tag).offset + it->second * stride + tail, 4) != 0)
            throw std::runtime_error("Retail WMO UID " + std::to_string(word(updated, at + 4))
              + " changes tile-local MWDR/MWDS references; this edit is unsupported.");
        }
    }
    Bytes result;
    for (std::size_t at = 0; at < updated.size();)
    {
      auto size = word(updated, at + 4);
      std::string tag(reinterpret_cast<char const*>(updated.data() + at), 4);
      auto it = replacements.find(tag);
      if (it == replacements.end())
        result.insert(result.end(), updated.begin() + at, updated.begin() + at + 8 + size);
      else
      {
        result.insert(result.end(), updated.begin() + at, updated.begin() + at + 4);
        auto length = static_cast<std::uint32_t>(it->second.size());
        auto p = reinterpret_cast<std::uint8_t const*>(&length);
        result.insert(result.end(), p, p + 4);
        result.insert(result.end(), it->second.begin(), it->second.end());
      }
      at += 8 + size;
    }
    validate(result, lod);
    return result;
  }
}
