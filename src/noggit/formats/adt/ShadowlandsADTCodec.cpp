// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/formats/adt/ShadowlandsADTCodec.hpp>

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/Log.h>

#include <ClientFile.hpp>
#include <ClientData.hpp>
#include <Listfile.hpp>

#include <QSaveFile>
#include <QFile>
#include <memory>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace Noggit::Formats::ADT
{
  namespace
  {
    struct ChunkView
    {
      std::uint32_t magic = 0;
      std::size_t offset = 0;
      std::size_t payload = 0;
      std::uint32_t size = 0;
    };

    constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept
    {
      return (static_cast<std::uint32_t>(a) << 24)
           | (static_cast<std::uint32_t>(b) << 16)
           | (static_cast<std::uint32_t>(c) << 8)
           | static_cast<std::uint32_t>(d);
    }

    constexpr std::size_t part_index(ShadowlandsADTPart part) noexcept
    {
      return static_cast<std::size_t>(part);
    }

    std::string split_path(std::string const& root_path, char const* suffix)
    {
      auto const extension = root_path.rfind(".adt");
      if (extension == std::string::npos)
        return root_path + suffix;

      return root_path.substr(0, extension) + suffix + ".adt";
    }

    ShadowlandsADTRawPart read_part(std::string const& path, bool required)
    {
      try
      {
        BlizzardArchive::Listfile::FileKey key(path);
        BlizzardArchive::ClientFile file(
          key,
          Noggit::Application::NoggitApplication::instance()->clientData());

        ShadowlandsADTRawPart result;
        result.present = true;
        auto const size = file.getSize();
        auto const* buffer = reinterpret_cast<std::uint8_t const*>(file.getBuffer());
        result.bytes.assign(buffer, buffer + size);
        return result;
      }
      catch (...)
      {
        if (required)
          throw;
        return {};
      }
    }

    std::vector<ChunkView> parse_chunks(std::vector<std::uint8_t> const& bytes,
                                        std::size_t begin,
                                        std::size_t end,
                                        char const* label,
                                        bool legacy_mcnr_padding = false)
    {
      if (end > bytes.size() || begin > end)
        throw std::runtime_error(std::string(label) + " has an invalid chunk range.");

      std::vector<ChunkView> result;
      std::size_t pos = begin;
      while (pos + 8 <= end)
      {
        ChunkView chunk;
        chunk.offset = pos;
        chunk.payload = pos + 8;
        std::memcpy(&chunk.magic, bytes.data() + pos, 4);
        std::memcpy(&chunk.size, bytes.data() + pos + 4, 4);
        if (chunk.size > end - chunk.payload)
          throw std::runtime_error(std::string(label) + " contains a truncated chunk.");
        result.push_back(chunk);
        pos = chunk.payload + chunk.size;

        // Noggit's canonical WotLK serializer retains the 13 unknown bytes
        // following MCNR. They are padding, not another subchunk header.
        if (legacy_mcnr_padding && chunk.magic == fourcc('M','C','N','R'))
        {
          constexpr std::size_t mcnr_padding_size = 13;
          if (mcnr_padding_size > end - pos)
            throw std::runtime_error(std::string(label) + " has truncated MCNR padding.");
          pos += mcnr_padding_size;
        }
      }
      if (pos != end)
        throw std::runtime_error(std::string(label) + " does not end on a chunk boundary.");
      return result;
    }

    std::vector<ChunkView> parse_chunks(std::vector<std::uint8_t> const& bytes,
                                        char const* label)
    {
      return parse_chunks(bytes, 0, bytes.size(), label);
    }

    ChunkView const* find_chunk(std::vector<ChunkView> const& chunks, std::uint32_t magic)
    {
      auto const it = std::find_if(chunks.begin(), chunks.end(),
        [magic](ChunkView const& chunk) { return chunk.magic == magic; });
      return it == chunks.end() ? nullptr : &*it;
    }

    void append_range(std::vector<std::uint8_t>& output,
                      std::vector<std::uint8_t> const& input,
                      std::size_t begin,
                      std::size_t end)
    {
      output.insert(output.end(), input.begin() + begin, input.begin() + end);
    }

    std::vector<std::uint8_t> make_chunk(std::uint32_t magic,
                                         std::vector<std::uint8_t> const& payload)
    {
      if (payload.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("ADT chunk is too large to serialize.");
      std::vector<std::uint8_t> result(8 + payload.size());
      auto const size = static_cast<std::uint32_t>(payload.size());
      std::memcpy(result.data(), &magic, 4);
      std::memcpy(result.data() + 4, &size, 4);
      if (!payload.empty())
        std::memcpy(result.data() + 8, payload.data(), payload.size());
      return result;
    }

    std::vector<std::uint8_t> chunk_bytes(std::vector<std::uint8_t> const& bytes,
                                          ChunkView const& chunk)
    {
      return {bytes.begin() + chunk.offset, bytes.begin() + chunk.payload + chunk.size};
    }

    std::vector<std::uint8_t> rewrite_mcnk(
      std::vector<std::uint8_t> const& modern,
      ChunkView const& modern_chunk,
      std::vector<std::uint8_t> const& legacy,
      ChunkView const& legacy_chunk,
      bool root_part)
    {
      std::size_t const modern_header_size = root_part ? sizeof(MapChunkHeader) : 0;
      std::size_t const legacy_header_size = sizeof(MapChunkHeader);
      if (modern_chunk.size < modern_header_size || legacy_chunk.size < legacy_header_size)
        throw std::runtime_error("ADT MCNK header is truncated while saving.");

      auto const modern_sub_begin = modern_chunk.payload + modern_header_size;
      auto const legacy_sub_begin = legacy_chunk.payload + legacy_header_size;
      auto const modern_subs = parse_chunks(
        modern, modern_sub_begin, modern_chunk.payload + modern_chunk.size, "modern MCNK");
      auto const legacy_subs = parse_chunks(
        legacy, legacy_sub_begin, legacy_chunk.payload + legacy_chunk.size, "legacy MCNK", true);

      std::set<std::uint32_t> replaceable = root_part
        ? std::set<std::uint32_t>{fourcc('M','C','V','T'), fourcc('M','C','N','R'),
                                  fourcc('M','C','C','V'), fourcc('M','C','S','H'),
                                  fourcc('M','C','S','E')}
        : std::set<std::uint32_t>{fourcc('M','C','L','Y'), fourcc('M','C','A','L')};

      std::vector<std::uint8_t> payload;
      if (root_part)
      {
        append_range(payload, modern, modern_chunk.payload, modern_chunk.payload + modern_header_size);
        MapChunkHeader modern_header{};
        MapChunkHeader legacy_header{};
        std::memcpy(&modern_header, payload.data(), sizeof(modern_header));
        std::memcpy(&legacy_header, legacy.data() + legacy_chunk.payload, sizeof(legacy_header));
        modern_header.flags = legacy_header.flags;
        modern_header.ix = legacy_header.ix;
        modern_header.iy = legacy_header.iy;
        modern_header.areaid = legacy_header.areaid;
        if (modern_header.flags.flags.high_res_holes)
        {
          auto high_res = static_cast<std::uint64_t>(modern_header.ofsHeight)
                        | (static_cast<std::uint64_t>(modern_header.ofsNormal) << 32);
          auto const edited_low_res = legacy_header.holes & 0xFFFF;
          for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
            {
              auto const first = 2 * y * 8 + 2 * x;
              auto const block = (std::uint64_t{3} << first)
                               | (std::uint64_t{3} << (first + 8));
              auto const bit = 1u << (y * 4 + x);
              auto const was_hole = (high_res & block) != 0;
              auto const is_hole = (edited_low_res & bit) != 0;
              if (was_hole != is_hole)
                high_res = is_hole ? high_res | block : high_res & ~block;
            }
          modern_header.ofsHeight = static_cast<std::uint32_t>(high_res);
          modern_header.ofsNormal = static_cast<std::uint32_t>(high_res >> 32);
        }
        else
          modern_header.holes = (modern_header.holes & 0xFFFF0000u)
                              | (legacy_header.holes & 0xFFFFu);
        modern_header.ypos = legacy_header.ypos;
        std::copy(std::begin(legacy_header.doodadMapping), std::end(legacy_header.doodadMapping),
                  std::begin(modern_header.doodadMapping));
        std::copy(std::begin(legacy_header.doodadStencil), std::end(legacy_header.doodadStencil),
                  std::begin(modern_header.doodadStencil));
        std::memcpy(payload.data(), &modern_header, sizeof(modern_header));
      }

      std::set<std::uint32_t> emitted;
      for (auto const& current : modern_subs)
      {
        auto const* replacement = replaceable.count(current.magic)
          ? find_chunk(legacy_subs, current.magic) : nullptr;
        if (replacement)
        {
          append_range(payload, legacy, replacement->offset, replacement->payload + replacement->size);
          emitted.insert(current.magic);
        }
        else
        {
          append_range(payload, modern, current.offset, current.payload + current.size);
        }
      }

      for (auto const magic : replaceable)
      {
        if (emitted.count(magic))
          continue;
        if (auto const* replacement = find_chunk(legacy_subs, magic))
          append_range(payload, legacy, replacement->offset, replacement->payload + replacement->size);
      }
      return make_chunk(fourcc('M','C','N','K'), payload);
    }

    std::vector<std::uint8_t> rewrite_mcnks(
      std::vector<std::uint8_t> const& modern,
      std::vector<std::uint8_t> const& legacy,
      bool root_part)
    {
      auto const modern_chunks = parse_chunks(modern, root_part ? "modern ROOT" : "modern TEX0");
      auto const legacy_chunks = parse_chunks(legacy, "legacy ADT");
      std::vector<ChunkView const*> legacy_mcnks;
      for (auto const& chunk : legacy_chunks)
        if (chunk.magic == fourcc('M','C','N','K'))
          legacy_mcnks.push_back(&chunk);
      if (legacy_mcnks.size() != 256)
        throw std::runtime_error("Legacy serializer did not produce exactly 256 MCNK chunks.");

      std::vector<std::uint8_t> output;
      std::size_t mcnk_index = 0;
      for (auto const& chunk : modern_chunks)
      {
        if (chunk.magic != fourcc('M','C','N','K'))
        {
          append_range(output, modern, chunk.offset, chunk.payload + chunk.size);
          continue;
        }
        if (mcnk_index >= legacy_mcnks.size())
          throw std::runtime_error("Modern split ADT contains too many MCNK chunks.");
        auto rewritten = rewrite_mcnk(
          modern, chunk, legacy, *legacy_mcnks[mcnk_index++], root_part);
        output.insert(output.end(), rewritten.begin(), rewritten.end());
      }
      if (mcnk_index != 256)
        throw std::runtime_error("Modern split ADT does not contain exactly 256 MCNK chunks.");
      return output;
    }

    std::vector<std::uint8_t> rewrite_obj0_refs(
      std::vector<std::uint8_t> const& obj0,
      std::vector<std::uint8_t> const& legacy,
      std::size_t m2_count,
      std::size_t wmo_count)
    {
      auto const modern_chunks = parse_chunks(obj0, "modern OBJ0");
      auto const legacy_chunks = parse_chunks(legacy, "legacy ADT");
      std::vector<ChunkView const*> legacy_mcnks;
      for (auto const& chunk : legacy_chunks)
        if (chunk.magic == fourcc('M','C','N','K'))
          legacy_mcnks.push_back(&chunk);
      if (legacy_mcnks.size() != 256)
        throw std::runtime_error("Legacy ADT does not contain exactly 256 MCNK chunks for OBJ0 references.");

      std::vector<std::uint8_t> output;
      std::size_t mcnk_index = 0;
      for (auto const& chunk : modern_chunks)
      {
        if (chunk.magic != fourcc('M','C','N','K'))
        {
          append_range(output, obj0, chunk.offset, chunk.payload + chunk.size);
          continue;
        }
        if (mcnk_index >= legacy_mcnks.size())
          throw std::runtime_error("Modern OBJ0 contains too many MCNK chunks.");

        auto const& source = *legacy_mcnks[mcnk_index++];
        if (source.size < sizeof(MapChunkHeader))
          throw std::runtime_error("Legacy MCNK header is truncated while rebuilding OBJ0 references.");
        MapChunkHeader header{};
        std::memcpy(&header, legacy.data() + source.payload, sizeof(header));
        auto const legacy_subs = parse_chunks(
          legacy, source.payload + sizeof(header), source.payload + source.size,
          "legacy MCNK", true);
        auto const* refs = find_chunk(legacy_subs, fourcc('M','C','R','F'));
        auto const doodad_bytes = static_cast<std::size_t>(header.nDoodadRefs) * 4;
        auto const wmo_bytes = static_cast<std::size_t>(header.nMapObjRefs) * 4;
        if (!refs || refs->size != doodad_bytes + wmo_bytes)
          throw std::runtime_error("Legacy MCNK MCRF size does not match object reference counts.");

        for (std::size_t i = 0; i < header.nDoodadRefs + header.nMapObjRefs; ++i)
        {
          std::uint32_t index = 0;
          std::memcpy(&index, legacy.data() + refs->payload + i * 4, 4);
          if (index >= (i < header.nDoodadRefs ? m2_count : wmo_count))
            throw std::runtime_error("Legacy MCNK references an invalid OBJ0 placement index.");
        }

        auto const doodad_refs = std::vector<std::uint8_t>(
          legacy.begin() + refs->payload, legacy.begin() + refs->payload + doodad_bytes);
        auto const wmo_refs = std::vector<std::uint8_t>(
          legacy.begin() + refs->payload + doodad_bytes,
          legacy.begin() + refs->payload + doodad_bytes + wmo_bytes);
        auto const modern_subs = parse_chunks(
          obj0, chunk.payload, chunk.payload + chunk.size, "modern OBJ0 MCNK");
        std::vector<std::uint8_t> payload;
        bool emitted_doodads = false;
        bool emitted_wmos = false;
        for (auto const& current : modern_subs)
        {
          if (current.magic == fourcc('M','C','R','D'))
          {
            if (emitted_doodads)
              throw std::runtime_error("Modern OBJ0 MCNK has duplicate MCRD chunks.");
            auto rewritten = make_chunk(current.magic, doodad_refs);
            payload.insert(payload.end(), rewritten.begin(), rewritten.end());
            emitted_doodads = true;
          }
          else if (current.magic == fourcc('M','C','R','W'))
          {
            if (emitted_wmos)
              throw std::runtime_error("Modern OBJ0 MCNK has duplicate MCRW chunks.");
            auto rewritten = make_chunk(current.magic, wmo_refs);
            payload.insert(payload.end(), rewritten.begin(), rewritten.end());
            emitted_wmos = true;
          }
          else
            append_range(payload, obj0, current.offset, current.payload + current.size);
        }
        if (!emitted_doodads && !doodad_refs.empty())
        {
          auto rewritten = make_chunk(fourcc('M','C','R','D'), doodad_refs);
          payload.insert(payload.end(), rewritten.begin(), rewritten.end());
        }
        if (!emitted_wmos && !wmo_refs.empty())
        {
          auto rewritten = make_chunk(fourcc('M','C','R','W'), wmo_refs);
          payload.insert(payload.end(), rewritten.begin(), rewritten.end());
        }
        auto rewritten = make_chunk(fourcc('M','C','N','K'), payload);
        output.insert(output.end(), rewritten.begin(), rewritten.end());
      }
      if (mcnk_index != 256)
        throw std::runtime_error("Modern OBJ0 does not contain exactly 256 MCNK chunks.");
      return output;
    }

    std::vector<std::string> legacy_string_table(std::vector<std::uint8_t> const& legacy,
                                                  std::uint32_t magic)
    {
      auto const chunks = parse_chunks(legacy, "legacy ADT");
      auto const* chunk = find_chunk(chunks, magic);
      if (!chunk)
        return {};
      std::vector<std::string> result;
      std::size_t pos = chunk->payload;
      auto const end = chunk->payload + chunk->size;
      while (pos < end)
      {
        auto const* value = reinterpret_cast<char const*>(legacy.data() + pos);
        auto const remaining = end - pos;
        auto const length = strnlen(value, remaining);
        if (length == remaining)
          throw std::runtime_error("Legacy ADT contains an unterminated string table entry.");
        result.emplace_back(value, length);
        pos += length + 1;
      }
      return result;
    }

    std::vector<std::uint32_t> resolve_file_ids(std::vector<std::string> const& paths)
    {
      auto* listfile = Noggit::Application::NoggitApplication::instance()->clientData()->listfile();
      std::vector<std::uint32_t> result;
      result.reserve(paths.size());
      for (auto const& path : paths)
      {
        auto const id = listfile->getFileDataID(path);
        if (!id)
          throw std::runtime_error("No FileDataID found for modern ADT asset: " + path);
        result.push_back(id);
      }
      return result;
    }

    std::vector<std::uint8_t> uint32_payload(std::vector<std::uint32_t> const& values)
    {
      std::vector<std::uint8_t> result(values.size() * sizeof(std::uint32_t));
      if (!result.empty())
        std::memcpy(result.data(), values.data(), result.size());
      return result;
    }

    std::vector<std::uint8_t> replace_top_level_chunk(
      std::vector<std::uint8_t> const& input,
      std::uint32_t magic,
      std::vector<std::uint8_t> const& replacement)
    {
      auto const chunks = parse_chunks(input, "split ADT");
      std::vector<std::uint8_t> output;
      bool replaced = false;
      for (auto const& chunk : chunks)
      {
        if (chunk.magic == magic)
        {
          if (!replaced)
            output.insert(output.end(), replacement.begin(), replacement.end());
          replaced = true;
        }
        else
          append_range(output, input, chunk.offset, chunk.payload + chunk.size);
      }
      if (!replaced)
        output.insert(output.end(), replacement.begin(), replacement.end());
      return output;
    }

    struct LodExtent
    {
      float min[3];
      float max[3];
      float radius;
    };
    static_assert(sizeof(LodExtent) == 28);

    struct LodMapObject
    {
      std::uint32_t nameID;
      std::uint32_t uniqueID;
      float pos[3];
      float rot[3];
      std::uint16_t flags;
      std::uint16_t doodadSet;
      std::uint16_t nameSet;
      std::uint16_t scale;
    };
    static_assert(sizeof(LodMapObject) == 40);

    template<typename Entry>
    std::vector<Entry> placement_records(std::vector<std::uint8_t> const& bytes,
                                         ChunkView const& chunk,
                                         char const* label)
    {
      if (chunk.size % sizeof(Entry))
        throw std::runtime_error(std::string(label) + " has an invalid record size.");
      std::vector<Entry> records(chunk.size / sizeof(Entry));
      if (chunk.size)
        std::memcpy(records.data(), bytes.data() + chunk.payload, chunk.size);
      return records;
    }

    template<typename Entry>
    std::vector<std::uint8_t> record_bytes(std::vector<Entry> const& records)
    {
      std::vector<std::uint8_t> bytes(records.size() * sizeof(Entry));
      if (!bytes.empty())
        std::memcpy(bytes.data(), records.data(), bytes.size());
      return bytes;
    }

    std::uint64_t placement_key(std::uint32_t name_id, std::uint32_t unique_id)
    {
      return (static_cast<std::uint64_t>(unique_id) << 32) | name_id;
    }

    std::array<float, 3> server_position(float const (&position)[3])
    {
      constexpr float map_midpoint = 17066.666f;
      return {map_midpoint - position[2], map_midpoint - position[0], position[1]};
    }

    LodExtent moved_extent(LodExtent const& old_extent,
                           float const (&old_position)[3],
                           float const (&new_position)[3],
                           float scale_ratio,
                           bool rotation_changed)
    {
      auto const before = server_position(old_position);
      auto const after = server_position(new_position);
      LodExtent result = old_extent;
      float radius = 0.0f;
      for (int axis = 0; axis < 3; ++axis)
      {
        radius = std::max(radius, std::abs(old_extent.min[axis] - before[axis]));
        radius = std::max(radius, std::abs(old_extent.max[axis] - before[axis]));
      }
      for (int axis = 0; axis < 3; ++axis)
      {
        if (rotation_changed)
        {
          result.min[axis] = after[axis] - radius * scale_ratio;
          result.max[axis] = after[axis] + radius * scale_ratio;
        }
        else
        {
          result.min[axis] = after[axis] + (old_extent.min[axis] - before[axis]) * scale_ratio;
          result.max[axis] = after[axis] + (old_extent.max[axis] - before[axis]) * scale_ratio;
        }
      }
      result.radius = old_extent.radius * scale_ratio;
      return result;
    }

    LodExtent new_doodad_extent(ENTRY_MDDF const& placement)
    {
      auto const center = server_position(placement.pos);
      auto const radius = 32.0f * static_cast<float>(placement.scale) / 1024.0f;
      LodExtent result{};
      for (int axis = 0; axis < 3; ++axis)
      {
        result.min[axis] = center[axis] - radius;
        result.max[axis] = center[axis] + radius;
      }
      result.radius = radius;
      return result;
    }

    LodExtent map_object_extent(ENTRY_MODF const& placement)
    {
      LodExtent result{};
      constexpr float map_midpoint = 17066.666f;
      auto const& low = placement.extents[0];
      auto const& high = placement.extents[1];
      result.min[0] = map_midpoint - high.z;
      result.max[0] = map_midpoint - low.z;
      result.min[1] = map_midpoint - high.x;
      result.max[1] = map_midpoint - low.x;
      result.min[2] = low.y;
      result.max[2] = high.y;
      result.radius = std::sqrt(
        std::pow(result.max[0] - result.min[0], 2.0f)
        + std::pow(result.max[1] - result.min[1], 2.0f)
        + std::pow(result.max[2] - result.min[2], 2.0f));
      return result;
    }

    std::vector<std::uint8_t> rewrite_obj1(
      std::vector<std::uint8_t> const& obj1,
      std::vector<std::uint8_t> const& mddf_bytes,
      std::vector<std::uint8_t> const& modf_bytes)
    {
      auto const chunks = parse_chunks(obj1, "modern OBJ1");
      auto const* mldd_chunk = find_chunk(chunks, fourcc('M','L','D','D'));
      auto const* mldx_chunk = find_chunk(chunks, fourcc('M','L','D','X'));
      auto const* mldl_chunk = find_chunk(chunks, fourcc('M','L','D','L'));
      auto const* mdli_chunk = find_chunk(chunks, fourcc('M','D','L','I'));
      auto const* mlmd_chunk = find_chunk(chunks, fourcc('M','L','M','D'));
      auto const* mlmx_chunk = find_chunk(chunks, fourcc('M','L','M','X'));
      auto const* mlfd_chunk = find_chunk(chunks, fourcc('M','L','F','D'));
      if (!mldd_chunk || !mldx_chunk || !mlmd_chunk || !mlmx_chunk
          || (mlfd_chunk && mlfd_chunk->size != 12 * sizeof(std::uint32_t)))
        throw std::runtime_error("OBJ1 has an unsupported LOD placement layout.");

      ChunkView const new_mddf{0, 0, 0, static_cast<std::uint32_t>(mddf_bytes.size())};
      ChunkView const new_modf{0, 0, 0, static_cast<std::uint32_t>(modf_bytes.size())};
      auto const doodads = placement_records<ENTRY_MDDF>(mddf_bytes, new_mddf, "OBJ0 MDDF");
      auto const map_objects = placement_records<ENTRY_MODF>(modf_bytes, new_modf, "OBJ0 MODF");
      auto const old_doodads = placement_records<ENTRY_MDDF>(obj1, *mldd_chunk, "OBJ1 MLDD");
      auto const old_doodad_extents = placement_records<LodExtent>(obj1, *mldx_chunk, "OBJ1 MLDX");
      // Shipped Shadowlands OBJ1 files can omit the optional per-doodad LOD chunk.
      auto const old_doodad_lod = mldl_chunk
        ? placement_records<std::uint32_t>(obj1, *mldl_chunk, "OBJ1 MLDL")
        : std::vector<std::uint32_t>(old_doodads.size(), 0);
      auto const old_doodad_indices = mdli_chunk
        ? placement_records<std::uint32_t>(obj1, *mdli_chunk, "OBJ1 MDLI")
        : std::vector<std::uint32_t>{};
      auto const old_map_objects = placement_records<LodMapObject>(obj1, *mlmd_chunk, "OBJ1 MLMD");
      auto const old_map_extents = placement_records<LodExtent>(obj1, *mlmx_chunk, "OBJ1 MLMX");
      // Older split layouts have no group table; all records form the base group.
      auto counts = mlfd_chunk
        ? placement_records<std::uint32_t>(obj1, *mlfd_chunk, "OBJ1 MLFD")
        : std::vector<std::uint32_t>(12, 0);
      if (!mlfd_chunk)
      {
        counts[3] = static_cast<std::uint32_t>(old_doodads.size());
        counts[8] = counts[9] = static_cast<std::uint32_t>(old_map_objects.size());
      }
      if (old_doodads.size() != old_doodad_extents.size()
          || old_doodads.size() != old_doodad_lod.size()
          || old_map_objects.size() != old_map_extents.size()
          || counts[3] + counts[4] != old_doodads.size()
          || counts[8] + counts[11] != old_map_objects.size()
          || counts[8] != counts[9])
        throw std::runtime_error("OBJ1 LOD placement counts do not match MLFD.");

      std::unordered_map<std::uint64_t, std::size_t> doodad_indices;
      std::unordered_map<std::uint64_t, std::size_t> map_indices;
      for (std::size_t i = 0; i < old_doodads.size(); ++i)
        doodad_indices.emplace(placement_key(old_doodads[i].nameID, old_doodads[i].uniqueID), i);
      for (std::size_t i = 0; i < old_map_objects.size(); ++i)
        map_indices.emplace(placement_key(old_map_objects[i].nameID, old_map_objects[i].uniqueID), i);

      std::vector<ENTRY_MDDF> updated_doodads;
      std::vector<LodExtent> updated_doodad_extents;
      std::vector<std::uint32_t> updated_doodad_lod;
      std::vector<std::size_t> updated_doodad_groups;
      std::array<std::uint32_t, 2> doodad_groups{};
      for (auto const& placement : doodads)
      {
        auto entry = placement;
        LodExtent extent{};
        std::uint32_t lod = 0;
        std::size_t group = 0;
        if (auto const it = doodad_indices.find(placement_key(placement.nameID, placement.uniqueID));
            it != doodad_indices.end())
        {
          auto const index = it->second;
          auto const& old = old_doodads[index];
          entry.flags = old.flags;
          auto const unchanged = std::equal(std::begin(old.pos), std::end(old.pos),
                                            std::begin(placement.pos))
            && std::equal(std::begin(old.rot), std::end(old.rot),
                          std::begin(placement.rot))
            && old.scale == placement.scale;
          if (unchanged)
            extent = old_doodad_extents[index];
          else
          {
            auto const changed_rotation = !std::equal(std::begin(old.rot), std::end(old.rot),
                                                      std::begin(placement.rot));
            extent = moved_extent(old_doodad_extents[index], old.pos, placement.pos,
                                  static_cast<float>(placement.scale) / std::max(1u, static_cast<unsigned>(old.scale)),
                                  changed_rotation);
          }
          lod = old_doodad_lod[index];
          group = index < counts[3] ? 0 : 1;
        }
        else
        {
          entry.flags = 0x60;
          extent = new_doodad_extent(placement);
          auto const exemplar = std::find_if(old_doodads.begin(), old_doodads.end(),
            [&](ENTRY_MDDF const& old) { return old.nameID == placement.nameID; });
          if (exemplar != old_doodads.end())
          {
            auto const index = static_cast<std::size_t>(exemplar - old_doodads.begin());
            entry.flags = exemplar->flags;
            extent = moved_extent(old_doodad_extents[index], exemplar->pos, placement.pos,
                                  static_cast<float>(placement.scale) / std::max(1u, static_cast<unsigned>(exemplar->scale)),
                                  true);
            lod = old_doodad_lod[index];
          }
        }
        updated_doodads.push_back(entry);
        updated_doodad_extents.push_back(extent);
        updated_doodad_lod.push_back(lod);
        updated_doodad_groups.push_back(group);
        ++doodad_groups[group];
      }

      std::vector<LodMapObject> updated_map_objects;
      std::vector<LodExtent> updated_map_extents;
      std::vector<std::size_t> updated_map_groups;
      std::array<std::uint32_t, 2> map_groups{};
      for (auto const& placement : map_objects)
      {
        LodMapObject entry{};
        std::memcpy(&entry, &placement, 32);
        std::memcpy(reinterpret_cast<std::uint8_t*>(&entry) + 32,
                    reinterpret_cast<std::uint8_t const*>(&placement) + 56, 8);
        auto extent = map_object_extent(placement);
        std::size_t group = 0;
        if (auto const it = map_indices.find(placement_key(placement.nameID, placement.uniqueID));
            it != map_indices.end())
        {
          auto const index = it->second;
          group = index < counts[8] ? 0 : 1;
          // Keep shipped bounds byte-for-byte when the placement has not moved.
          if (std::memcmp(&entry, &old_map_objects[index], sizeof(entry)) == 0)
            extent = old_map_extents[index];
        }
        updated_map_objects.push_back(entry);
        updated_map_extents.push_back(extent);
        updated_map_groups.push_back(group);
        ++map_groups[group];
      }
      counts[3] = doodad_groups[0];
      counts[4] = doodad_groups[1];
      if (counts[1])
        counts[1] = doodad_groups[0];
      counts[8] = map_groups[0];
      counts[9] = map_groups[0];
      counts[11] = map_groups[1];

      auto group_doodads = [&](std::size_t group)
      {
        std::vector<ENTRY_MDDF> entries;
        std::vector<LodExtent> extents;
        std::vector<std::uint32_t> lod_values;
        std::vector<std::size_t> indices;
        for (std::size_t i = 0; i < updated_doodads.size(); ++i)
          if (updated_doodad_groups[i] == group)
            indices.push_back(i);
        std::stable_sort(indices.begin(), indices.end(), [&](std::size_t lhs, std::size_t rhs)
        {
          return updated_doodad_extents[lhs].radius > updated_doodad_extents[rhs].radius;
        });
        for (auto const index : indices)
        {
          entries.push_back(updated_doodads[index]);
          extents.push_back(updated_doodad_extents[index]);
          lod_values.push_back(updated_doodad_lod[index]);
        }
        return std::make_tuple(std::move(entries), std::move(extents), std::move(lod_values));
      };
      auto [base_doodads, base_extents, base_lod] = group_doodads(0);
      auto [other_doodads, other_extents, other_lod] = group_doodads(1);
      base_doodads.insert(base_doodads.end(), other_doodads.begin(), other_doodads.end());
      base_extents.insert(base_extents.end(), other_extents.begin(), other_extents.end());
      base_lod.insert(base_lod.end(), other_lod.begin(), other_lod.end());
      updated_doodads = std::move(base_doodads);
      updated_doodad_extents = std::move(base_extents);
      updated_doodad_lod = std::move(base_lod);

      std::vector<std::uint32_t> updated_doodad_indices;
      if (mdli_chunk)
      {
        updated_doodad_indices.reserve(updated_doodads.size());
        for (auto const& entry : updated_doodads)
        {
          auto const it = doodad_indices.find(placement_key(entry.nameID, entry.uniqueID));
          updated_doodad_indices.push_back(
            it != doodad_indices.end() && it->second < old_doodad_indices.size()
              ? old_doodad_indices[it->second]
              : 0xFFFFFFFFu);
        }
      }

      auto group_map_objects = [&](std::size_t group)
      {
        std::vector<LodMapObject> entries;
        std::vector<LodExtent> extents;
        std::vector<std::size_t> indices;
        for (std::size_t i = 0; i < updated_map_objects.size(); ++i)
          if (updated_map_groups[i] == group)
            indices.push_back(i);
        std::stable_sort(indices.begin(), indices.end(), [&](std::size_t lhs, std::size_t rhs)
        {
          return updated_map_extents[lhs].radius > updated_map_extents[rhs].radius;
        });
        for (auto const index : indices)
        {
          entries.push_back(updated_map_objects[index]);
          extents.push_back(updated_map_extents[index]);
        }
        return std::make_pair(std::move(entries), std::move(extents));
      };
      auto [base_map_objects, base_map_extents] = group_map_objects(0);
      auto [other_map_objects, other_map_extents] = group_map_objects(1);
      base_map_objects.insert(base_map_objects.end(), other_map_objects.begin(), other_map_objects.end());
      base_map_extents.insert(base_map_extents.end(), other_map_extents.begin(), other_map_extents.end());
      updated_map_objects = std::move(base_map_objects);
      updated_map_extents = std::move(base_map_extents);

      auto output = replace_top_level_chunk(obj1, fourcc('M','L','D','D'),
        make_chunk(fourcc('M','L','D','D'), record_bytes(updated_doodads)));
      output = replace_top_level_chunk(output, fourcc('M','L','D','X'),
        make_chunk(fourcc('M','L','D','X'), record_bytes(updated_doodad_extents)));
      if (mldl_chunk)
        output = replace_top_level_chunk(output, fourcc('M','L','D','L'),
          make_chunk(fourcc('M','L','D','L'), record_bytes(updated_doodad_lod)));
      if (mdli_chunk)
        output = replace_top_level_chunk(output, fourcc('M','D','L','I'),
          make_chunk(fourcc('M','D','L','I'), record_bytes(updated_doodad_indices)));
      output = replace_top_level_chunk(output, fourcc('M','L','M','D'),
        make_chunk(fourcc('M','L','M','D'), record_bytes(updated_map_objects)));
      output = replace_top_level_chunk(output, fourcc('M','L','M','X'),
        make_chunk(fourcc('M','L','M','X'), record_bytes(updated_map_extents)));
      if (mlfd_chunk)
        output = replace_top_level_chunk(output, fourcc('M','L','F','D'),
          make_chunk(fourcc('M','L','F','D'), record_bytes(counts)));
      return output;
    }

    void validate_tex0_alpha_layout(std::vector<std::uint8_t> const& tex0)
    {
      auto const chunks = parse_chunks(tex0, "saved TEX0");
      auto const* mdid = find_chunk(chunks, fourcc('M','D','I','D'));
      if (!mdid || mdid->size % sizeof(std::uint32_t) != 0)
        throw std::runtime_error("Saved TEX0 has an invalid MDID chunk.");
      auto const texture_count = mdid->size / sizeof(std::uint32_t);

      std::size_t mcnk_count = 0;
      for (auto const& chunk : chunks)
      {
        if (chunk.magic != fourcc('M','C','N','K'))
          continue;
        ++mcnk_count;

        auto const subchunks = parse_chunks(
          tex0, chunk.payload, chunk.payload + chunk.size, "saved TEX0 MCNK");
        auto const* mcly = find_chunk(subchunks, fourcc('M','C','L','Y'));
        auto const* mcal = find_chunk(subchunks, fourcc('M','C','A','L'));
        if (!mcly || mcly->size % sizeof(ENTRY_MCLY) != 0)
          throw std::runtime_error("Saved TEX0 MCNK has an invalid MCLY chunk.");

        auto const layer_count = mcly->size / sizeof(ENTRY_MCLY);
        std::vector<ENTRY_MCLY> layers(layer_count);
        if (mcly->size)
          std::memcpy(layers.data(), tex0.data() + mcly->payload, mcly->size);

        for (std::size_t layer_index = 0; layer_index < layers.size(); ++layer_index)
        {
          auto const& layer = layers[layer_index];
          if (layer.textureID >= texture_count)
            throw std::runtime_error("Saved TEX0 MCLY references a texture outside MDID.");
          if (layer_index == 0 || !(layer.flags & FLAG_USE_ALPHA))
            continue;
          if (!mcal || layer.ofsAlpha > mcal->size)
            throw std::runtime_error("Saved TEX0 MCLY references an invalid MCAL offset.");

          auto alpha_end = mcal->size;
          for (std::size_t next = layer_index + 1; next < layers.size(); ++next)
          {
            if ((layers[next].flags & FLAG_USE_ALPHA)
                && layers[next].ofsAlpha >= layer.ofsAlpha)
            {
              alpha_end = layers[next].ofsAlpha;
              break;
            }
          }
          if (alpha_end < layer.ofsAlpha)
            throw std::runtime_error("Saved TEX0 alpha maps overlap.");

          auto const alpha_size = alpha_end - layer.ofsAlpha;
          if (!(layer.flags & FLAG_ALPHA_COMPRESSED) && alpha_size != 4096)
          {
            std::ostringstream message;
            message << "Saved TEX0 has an uncompressed alpha map with "
                    << alpha_size << " bytes instead of 4096.";
            throw std::runtime_error(message.str());
          }
        }
      }
      if (mcnk_count != 256)
        throw std::runtime_error("Saved TEX0 does not contain exactly 256 MCNK chunks.");
    }

    std::string indexed_path(std::vector<std::uint8_t> const& legacy,
                             ChunkView const* offsets_chunk,
                             ChunkView const* strings_chunk,
                             std::uint32_t index)
    {
      if (!offsets_chunk || !strings_chunk || index >= offsets_chunk->size / 4)
        throw std::runtime_error("Legacy ADT placement references an invalid filename index.");
      std::uint32_t offset = 0;
      std::memcpy(&offset, legacy.data() + offsets_chunk->payload + index * 4, 4);
      if (offset >= strings_chunk->size)
        throw std::runtime_error("Legacy ADT placement references an invalid filename offset.");
      auto const* value = reinterpret_cast<char const*>(legacy.data() + strings_chunk->payload + offset);
      auto const remaining = strings_chunk->size - offset;
      auto const length = strnlen(value, remaining);
      if (length == remaining)
        throw std::runtime_error("Legacy ADT placement filename is unterminated.");
      return {value, length};
    }

    template<typename Entry>
    std::vector<std::uint8_t> modern_placements(std::vector<std::uint8_t> const& legacy,
                                                std::uint32_t placement_magic,
                                                std::uint32_t offsets_magic,
                                                std::uint32_t strings_magic,
                                                std::vector<std::uint8_t> const& original_obj0)
    {
      auto const chunks = parse_chunks(legacy, "legacy ADT");
      auto const* placements = find_chunk(chunks, placement_magic);
      if (!placements)
        return {};
      if (placements->size % sizeof(Entry) != 0)
        throw std::runtime_error("Legacy ADT placement chunk has an invalid size.");
      auto const* offsets = find_chunk(chunks, offsets_magic);
      auto const* strings = find_chunk(chunks, strings_magic);
      auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
      auto* listfile = client_data->listfile();

      std::unordered_map<std::uint64_t, std::uint16_t> original_flags;
      if constexpr (std::is_same_v<Entry, ENTRY_MDDF>)
      {
        auto const original_chunks = parse_chunks(original_obj0, "original OBJ0");
        if (auto const* original = find_chunk(original_chunks, placement_magic))
        {
          if (original->size % sizeof(Entry) != 0)
            throw std::runtime_error("Original OBJ0 MDDF chunk has an invalid size.");
          for (std::size_t i = 0; i < original->size / sizeof(Entry); ++i)
          {
            Entry entry{};
            std::memcpy(&entry, original_obj0.data() + original->payload + i * sizeof(Entry), sizeof(Entry));
            auto const key = (static_cast<std::uint64_t>(entry.uniqueID) << 32) | entry.nameID;
            original_flags.emplace(key, entry.flags);
          }
        }
      }

      std::vector<std::uint8_t> payload(placements->size);
      for (std::size_t i = 0; i < placements->size / sizeof(Entry); ++i)
      {
        Entry entry{};
        std::memcpy(&entry, legacy.data() + placements->payload + i * sizeof(Entry), sizeof(Entry));
        auto const path = indexed_path(legacy, offsets, strings, entry.nameID);
        auto const id = listfile->getFileDataID(path);
        if (!id)
          throw std::runtime_error("No FileDataID found for modern ADT placement: " + path);
        entry.nameID = id;
        if constexpr (std::is_same_v<Entry, ENTRY_MDDF>)
        {
          auto const key = (static_cast<std::uint64_t>(entry.uniqueID) << 32) | entry.nameID;
          if (auto const it = original_flags.find(key); it != original_flags.end())
            entry.flags = it->second;
          // Retail OBJ0 stores FileDataIDs without this flag; Shadowlands uses it.
          if (client_data->version() == BlizzardArchive::ClientVersion::RETAIL)
            entry.flags &= ~static_cast<std::uint16_t>(0x40);
          else
            entry.flags |= 0x40;
        }
        else if constexpr (std::is_same_v<Entry, ENTRY_MODF>)
        {
          // Existing modern OBJ0 WMO placements use these bits with FileDataIDs.
          entry.flags |= 0x000C;
        }
        std::memcpy(payload.data() + i * sizeof(Entry), &entry, sizeof(Entry));
      }
      return payload;
    }

    void save_parts(std::vector<std::pair<std::string, std::vector<std::uint8_t>>> const& parts)
    {
      struct PendingPart
      {
        QString path;
        bool existed;
        QByteArray original;
        std::unique_ptr<QSaveFile> file;
      };
      auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
      std::vector<PendingPart> pending;
      pending.reserve(parts.size());
      for (auto const& [logical_path, bytes] : parts)
      {
        BlizzardArchive::Listfile::FileKey key(logical_path);
        auto const disk_path = client_data->getDiskPath(key);
        std::filesystem::create_directories(std::filesystem::path(disk_path).parent_path());
        PendingPart part{QString::fromStdString(disk_path), false, {}, nullptr};
        part.existed = QFile::exists(part.path);
        if (part.existed)
        {
          QFile original(part.path);
          if (!original.open(QIODevice::ReadOnly))
            throw std::runtime_error("Could not read previous modern ADT part: " + disk_path);
          part.original = original.readAll();
          if (original.error() != QFileDevice::NoError)
            throw std::runtime_error("Could not read previous modern ADT part: " + disk_path);
        }
        part.file = std::make_unique<QSaveFile>(part.path);
        if (!part.file->open(QIODevice::WriteOnly)
            || part.file->write(reinterpret_cast<char const*>(bytes.data()),
                                 static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()))
          throw std::runtime_error("Could not stage modern ADT part: " + disk_path);
        pending.push_back(std::move(part));
      }

      for (std::size_t committed = 0; committed < pending.size(); ++committed)
      {
        if (pending[committed].file->commit())
          continue;
        std::string message = "Could not commit modern ADT part: " + pending[committed].path.toStdString();
        // A tile spans several files. Restore already committed overrides on failure.
        for (std::size_t i = 0; i < committed; ++i)
        {
          auto const& part = pending[i];
          bool restored = false;
          if (part.existed)
          {
            QSaveFile restore(part.path);
            restored = restore.open(QIODevice::WriteOnly)
              && restore.write(part.original) == part.original.size() && restore.commit();
          }
          else
            restored = QFile::remove(part.path);
          if (!restored)
            message += "; rollback failed for " + part.path.toStdString();
        }
        throw std::runtime_error(message);
      }
    }
  }

  bool ShadowlandsADTBackingStore::has(ShadowlandsADTPart part) const noexcept
  {
    return parts[part_index(part)].present;
  }

  ShadowlandsADTRawPart const& ShadowlandsADTBackingStore::part(ShadowlandsADTPart part) const noexcept
  {
    return parts[part_index(part)];
  }

  ShadowlandsADTRawPart& ShadowlandsADTBackingStore::part(ShadowlandsADTPart part) noexcept
  {
    return parts[part_index(part)];
  }

  ShadowlandsADTSource ShadowlandsADTCodec::makeSource(std::string const& root_path)
  {
    ShadowlandsADTSource source;
    source.root_path = root_path;
    source.paths[part_index(ShadowlandsADTPart::Root)] = root_path;
    source.paths[part_index(ShadowlandsADTPart::Tex0)] = split_path(root_path, "_tex0");
    source.paths[part_index(ShadowlandsADTPart::Obj0)] = split_path(root_path, "_obj0");
    source.paths[part_index(ShadowlandsADTPart::Obj1)] = split_path(root_path, "_obj1");
    source.paths[part_index(ShadowlandsADTPart::Lod)] = split_path(root_path, "_lod");
    return source;
  }

  ShadowlandsADTBackingStore ShadowlandsADTCodec::loadBackingStore(std::string const& root_path)
  {
    ShadowlandsADTBackingStore backing;
    backing.source = makeSource(root_path);

    backing.parts[part_index(ShadowlandsADTPart::Root)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Root)], true);
    backing.parts[part_index(ShadowlandsADTPart::Tex0)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Tex0)], true);
    backing.parts[part_index(ShadowlandsADTPart::Obj0)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Obj0)], true);

    // These are not required by every map/tile.  They are nevertheless loaded
    // when available so future native writers can preserve their contents.
    backing.parts[part_index(ShadowlandsADTPart::Obj1)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Obj1)], false);
    backing.parts[part_index(ShadowlandsADTPart::Lod)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Lod)], false);

    return backing;
  }

  ShadowlandsADTBackingStore ShadowlandsADTCodec::createBackingStore(
    std::string const& root_path, std::vector<std::uint8_t> const& legacy_adt)
  {
    auto const chunks = parse_chunks(legacy_adt, "new legacy ADT");
    auto const* version = find_chunk(chunks, fourcc('M','V','E','R'));
    if (!version || version->size != sizeof(std::uint32_t))
      throw std::runtime_error("New ADT has no valid MVER chunk.");

    ShadowlandsADTBackingStore backing;
    backing.source = makeSource(root_path);
    for (auto part : {ShadowlandsADTPart::Root, ShadowlandsADTPart::Tex0, ShadowlandsADTPart::Obj0})
    {
      backing.part(part).present = true;
      backing.part(part).bytes = chunk_bytes(legacy_adt, *version);
    }
    auto& root = backing.part(ShadowlandsADTPart::Root).bytes;
    auto& tex0 = backing.part(ShadowlandsADTPart::Tex0).bytes;
    auto& obj0 = backing.part(ShadowlandsADTPart::Obj0).bytes;
    auto append = [](auto& destination, auto const& bytes)
    {
      destination.insert(destination.end(), bytes.begin(), bytes.end());
    };
    append(root, make_chunk(fourcc('M','H','D','R'), std::vector<std::uint8_t>(sizeof(MHDR), 0)));
    append(tex0, make_chunk(fourcc('M','D','I','D'), {}));
    append(obj0, make_chunk(fourcc('M','D','D','F'), {}));
    append(obj0, make_chunk(fourcc('M','O','D','F'), {}));

    std::size_t count = 0;
    for (auto const& chunk : chunks)
    {
      if (chunk.magic != fourcc('M','C','N','K'))
        continue;
      if (chunk.size < sizeof(MapChunkHeader))
        throw std::runtime_error("New ADT has a truncated MCNK header.");
      MapChunkHeader header{};
      std::memcpy(&header, legacy_adt.data() + chunk.payload, sizeof(header));
      header.nLayers = header.nDoodadRefs = header.nMapObjRefs = 0;
      header.ofsHeight = header.ofsNormal = header.ofsLayer = header.ofsRefs = 0;
      header.ofsAlpha = header.sizeAlpha = header.ofsShadow = header.sizeShadow = 0;
      header.ofsSndEmitters = header.nSndEmitters = header.ofsLiquid = header.sizeLiquid = 0;
      header.ofsMCCV = 0;
      std::vector<std::uint8_t> payload(sizeof(header));
      std::memcpy(payload.data(), &header, sizeof(header));
      append(root, make_chunk(fourcc('M','C','N','K'), payload));
      append(tex0, make_chunk(fourcc('M','C','N','K'), {}));
      append(obj0, make_chunk(fourcc('M','C','N','K'), {}));
      ++count;
    }
    if (count != 256)
      throw std::runtime_error("New ADT must contain exactly 256 MCNK chunks.");
    return backing;
  }

  void ShadowlandsADTCodec::saveFromLegacy(
    ShadowlandsADTBackingStore const& backing,
    std::vector<std::uint8_t> const& legacy_adt)
  {
    auto root = rewrite_mcnks(backing.part(ShadowlandsADTPart::Root).bytes, legacy_adt, true);
    auto tex0 = rewrite_mcnks(backing.part(ShadowlandsADTPart::Tex0).bytes, legacy_adt, false);
    auto obj0 = backing.part(ShadowlandsADTPart::Obj0).bytes;

    auto const legacy_chunks = parse_chunks(legacy_adt, "legacy ADT");
    for (auto const magic : {fourcc('M','H','2','O'), fourcc('M','F','B','O')})
    {
      if (auto const* replacement = find_chunk(legacy_chunks, magic))
        root = replace_top_level_chunk(root, magic, chunk_bytes(legacy_adt, *replacement));
    }

    auto const texture_paths = legacy_string_table(legacy_adt, fourcc('M','T','E','X'));
    auto const texture_ids = resolve_file_ids(texture_paths);
    tex0 = replace_top_level_chunk(
      tex0, fourcc('M','D','I','D'), make_chunk(fourcc('M','D','I','D'), uint32_payload(texture_ids)));

    auto const mddf = modern_placements<ENTRY_MDDF>(
      legacy_adt, fourcc('M','D','D','F'), fourcc('M','M','I','D'), fourcc('M','M','D','X'), obj0);
    auto const modf = modern_placements<ENTRY_MODF>(
      legacy_adt, fourcc('M','O','D','F'), fourcc('M','W','I','D'), fourcc('M','W','M','O'), obj0);
    std::vector<std::uint8_t> obj1;
    if (backing.has(ShadowlandsADTPart::Obj1))
      obj1 = rewrite_obj1(backing.part(ShadowlandsADTPart::Obj1).bytes, mddf, modf);
    obj0 = replace_top_level_chunk(
      obj0, fourcc('M','D','D','F'), make_chunk(fourcc('M','D','D','F'), mddf));
    obj0 = replace_top_level_chunk(
      obj0, fourcc('M','O','D','F'), make_chunk(fourcc('M','O','D','F'), modf));
    obj0 = rewrite_obj0_refs(obj0, legacy_adt,
                             mddf.size() / sizeof(ENTRY_MDDF),
                             modf.size() / sizeof(ENTRY_MODF));

    validate_tex0_alpha_layout(tex0);

    // Stage the complete tile before replacing any project override.
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> parts;
    parts.emplace_back(backing.source.paths[part_index(ShadowlandsADTPart::Root)], std::move(root));
    parts.emplace_back(backing.source.paths[part_index(ShadowlandsADTPart::Tex0)], std::move(tex0));
    parts.emplace_back(backing.source.paths[part_index(ShadowlandsADTPart::Obj0)], std::move(obj0));
    if (backing.has(ShadowlandsADTPart::Obj1))
      parts.emplace_back(backing.source.paths[part_index(ShadowlandsADTPart::Obj1)], std::move(obj1));
    if (backing.has(ShadowlandsADTPart::Lod))
      parts.emplace_back(backing.source.paths[part_index(ShadowlandsADTPart::Lod)],
                         backing.part(ShadowlandsADTPart::Lod).bytes);
    save_parts(parts);

    Log << "[ModernADT][Save] Saved split tile ROOT/TEX0/OBJ0/OBJ1/LOD to the project override: "
        << backing.source.root_path << std::endl;
  }
}
