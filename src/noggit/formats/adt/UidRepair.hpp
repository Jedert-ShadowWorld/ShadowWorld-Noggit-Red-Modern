#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace Noggit::Formats::ADT
{
  // Assigns UIDs by placement identity without loading assets or rebuilding
  // bounds/references. One instance spans all tiles and OBJ0/OBJ1, so copies
  // of a placement receive the same UID.
  class UidRepair
  {
  public:
    struct Reference { std::string path; std::uint32_t fileId = 0; };
    struct Record { std::size_t uidOffset; std::string identity; Reference asset; };
    using Bytes = std::vector<std::uint8_t>;

    static std::uint32_t word(Bytes const& data, std::size_t at)
    {
      if (at > data.size() || data.size() - at < 4) throw std::runtime_error("Truncated ADT value");
      std::uint32_t value;
      std::memcpy(&value, data.data() + at, 4);
      return value;
    }

    static std::vector<Record> inspect(Bytes const& bytes, bool modern)
    {
      struct Chunk { std::size_t offset, size; };
      std::map<std::string, Chunk> chunks;
      for (std::size_t at = 0; at < bytes.size();)
      {
        if (bytes.size() - at < 8) throw std::runtime_error("Truncated ADT chunk header");
        auto size = word(bytes, at + 4);
        if (size > bytes.size() - at - 8) throw std::runtime_error("ADT chunk extends beyond file");
        std::string tag(reinterpret_cast<char const*>(bytes.data() + at), 4);
        bool const interpreted = tag == "REVM" || tag == "FDDM" || tag == "FDOM" ||
            tag == "DDLM" || tag == "DMLM" || tag == "OMWM" || tag == "XDMM" ||
            tag == "DIWM" || tag == "DIMM" || tag == "RDWM" || tag == "SDWM";
        if (interpreted && !chunks.emplace(tag, Chunk{at + 8, size}).second)
          throw std::runtime_error("Duplicate ADT chunk: " + tag);
        at += 8 + size;
      }
      auto ver = chunks.find("REVM");
      if (ver == chunks.end() || ver->second.size != 4 || word(bytes, ver->second.offset) != 18)
        throw std::runtime_error("ADT must have MVER 18");
      std::vector<Record> records;
      for (auto const& tag : {"FDDM", "FDOM", "DDLM", "DMLM"})
      {
        auto chunk = chunks.find(tag);
        if (chunk == chunks.end()) continue;
        bool const wmo = std::string(tag) == "FDOM" || std::string(tag) == "DMLM";
        bool const lod = std::string(tag) == "DDLM" || std::string(tag) == "DMLM";
        std::size_t const stride = wmo ? (lod ? 40 : 64) : 36;
        if (chunk->second.size % stride) throw std::runtime_error("Invalid placement record size: " + std::string(tag));
        for (auto at = chunk->second.offset; at < chunk->second.offset + chunk->second.size; at += stride)
        {
          Reference reference;
          auto name = word(bytes, at);
          auto strings = chunks.find(wmo ? "OMWM" : "XDMM");
          auto offsets = chunks.find(wmo ? "DIWM" : "DIMM");
          if (strings != chunks.end() && offsets != chunks.end())
          {
            if (offsets->second.size % 4 || name >= offsets->second.size / 4)
              throw std::runtime_error("Placement nameID is outside MMID/MWID");
            auto offset = word(bytes, offsets->second.offset + 4ull * name);
            if (offset >= strings->second.size) throw std::runtime_error("Invalid placement string offset");
            auto first = bytes.data() + strings->second.offset + offset;
            auto end = static_cast<std::uint8_t const*>(std::memchr(first, 0, strings->second.size - offset));
            if (!end) throw std::runtime_error("Unterminated placement filename");
            reference.path.assign(reinterpret_cast<char const*>(first), end - first);
            if (reference.path.empty()) throw std::runtime_error("Empty placement filename");
            std::transform(reference.path.begin(), reference.path.end(), reference.path.begin(),
              [](unsigned char c) { return c == '\\' ? '/' : (c >= 'A' && c <= 'Z' ? c + 32 : c); });
            if (!wmo && (reference.path.ends_with(".mdx") || reference.path.ends_with(".mdl")))
              reference.path.replace(reference.path.size() - 4, 4, ".m2");
          }
          else if (modern && strings == chunks.end() && offsets == chunks.end())
            reference.fileId = name;
          else throw std::runtime_error("Placement filename tables are missing/incomplete");
          // Identity: asset, old UID and transform.
          std::string key = wmo ? "W:" : "M:";
          key += reference.path.empty() ? "#" + std::to_string(reference.fileId) : reference.path;
          key.push_back('\0');
          key.append(reinterpret_cast<char const*>(bytes.data() + at + 4), 28);
          // MODF extents are excluded; OBJ1 stores bounds separately.
          auto tail = at + (wmo ? (lod ? 34 : 58) : 32);
          if (modern && wmo && (word(bytes, tail - 2) & 0x80))
          {
            // MWDS indices are file-local; the identity uses the selected set values.
            auto ranges = chunks.find("RDWM"), sets = chunks.find("SDWM");
            auto index = word(bytes, tail - 2) >> 16;
            if (ranges == chunks.end() || sets == chunks.end()
                || ranges->second.size % 8 || sets->second.size % 2
                || index >= ranges->second.size / 8)
              throw std::runtime_error("Invalid Retail WMO doodad-set reference");
            auto start = word(bytes, ranges->second.offset + index * 8ull);
            auto end = word(bytes, ranges->second.offset + index * 8ull + 4);
            if (end < start || end >= sets->second.size / 2)
              throw std::runtime_error("Retail MWDR range extends beyond MWDS");
            auto count = end - start + 1;
            key.push_back('S');
            key.append(reinterpret_cast<char const*>(&count), sizeof(count));
            key.append(reinterpret_cast<char const*>(bytes.data() + sets->second.offset + start * 2ull), count * 2ull);
            key.append(reinterpret_cast<char const*>(bytes.data() + tail + 2), 4);
          }
          else
          {
            if (modern && wmo) key.push_back('I');
            key.append(reinterpret_cast<char const*>(bytes.data() + tail), wmo ? 6 : 2);
          }
          records.push_back({at + 4, std::move(key), std::move(reference)});
        }
      }
      return records;
    }

    void rewrite(Bytes& bytes, std::vector<Record> const& records)
    {
      for (auto const& record : records)
      {
        auto found = ids.find(record.identity);
        if (found == ids.end())
        {
          if (ids.size() >= std::numeric_limits<std::uint32_t>::max() - 1ull)
            throw std::runtime_error("UID range exhausted");
          found = ids.emplace(record.identity, static_cast<std::uint32_t>(ids.size() + 1)).first;
        }
        std::memcpy(bytes.data() + record.uidOffset, &found->second, 4);
      }
    }
    std::uint32_t highest() const { return static_cast<std::uint32_t>(ids.size()); }
  private:
    std::map<std::string, std::uint32_t> ids;
  };
}
