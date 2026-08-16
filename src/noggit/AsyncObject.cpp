// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncObject.h>
#include <noggit/Log.h>
#include <noggit/MapTile.h>
#include <noggit/application/NoggitApplication.hpp>

#include <ClientFile.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>

namespace
{
  constexpr std::uint32_t on_disk_fourcc(char a, char b, char c, char d)
  {
    return (static_cast<std::uint32_t>(a) << 24)
         | (static_cast<std::uint32_t>(b) << 16)
         | (static_cast<std::uint32_t>(c) << 8)
         | static_cast<std::uint32_t>(d);
  }

  std::uint16_t read_u16(char const* data, std::size_t offset)
  {
    std::uint16_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
  }

  std::uint32_t read_u32(char const* data, std::size_t offset)
  {
    std::uint32_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
  }

  float read_f32(char const* data, std::size_t offset)
  {
    float value = 0.0f;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
  }

  std::string hex_dump(char const* data, std::size_t size, std::size_t max_bytes)
  {
    std::ostringstream out;
    auto const count = std::min(size, max_bytes);
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < count; ++i)
    {
      if (i && (i % 16) == 0)
        out << " | ";
      out << std::setw(2)
          << static_cast<unsigned>(static_cast<unsigned char>(data[i]));
      if ((i % 16) != 15 && i + 1 < count)
        out << ' ';
    }
    return out.str();
  }

  // Validate the modern ROOT MH2O, keep a small amount of diagnostics, then
  // feed it through TileWater. liquid_layer now adapts the modern second uint16
  // metadata/reference field into Noggit's legacy vertex-format enum.
  void inspect_and_load_modern_mh2o_if_needed(AsyncObject const* object)
  {
    auto* tile = dynamic_cast<MapTile*>(const_cast<AsyncObject*>(object));
    if (!tile || !object->file_key().hasFilepath())
      return;

    auto const& path = object->file_key().filepath();
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".adt") != 0)
      return;

    static std::mutex processed_guard;
    static std::unordered_set<std::string> processed_tiles;
    {
      std::lock_guard<std::mutex> lock(processed_guard);
      if (!processed_tiles.emplace(path).second)
        return;
    }

    try
    {
      BlizzardArchive::ClientFile file(
        object->file_key(),
        Noggit::Application::NoggitApplication::instance()->clientData());

      auto const* data = file.getBuffer();
      auto const file_size = file.getSize();

      bool has_legacy_mcin = false;
      std::size_t mcnk_count = 0;
      std::size_t mh2o_payload = 0;
      std::uint32_t mh2o_size = 0;

      std::size_t pos = 0;
      while (pos + 8 <= file_size)
      {
        std::uint32_t magic = 0;
        std::uint32_t size = 0;
        std::memcpy(&magic, data + pos, sizeof(magic));
        std::memcpy(&size, data + pos + 4, sizeof(size));

        auto const payload = pos + 8;
        if (size > file_size - payload)
          return;

        if (magic == on_disk_fourcc('M', 'C', 'I', 'N'))
          has_legacy_mcin = true;
        else if (magic == on_disk_fourcc('M', 'C', 'N', 'K'))
          ++mcnk_count;
        else if (magic == on_disk_fourcc('M', 'H', '2', 'O'))
        {
          mh2o_payload = payload;
          mh2o_size = size;
        }

        pos = payload + size;
      }

      if (has_legacy_mcin || mcnk_count != 256 || !mh2o_payload || !mh2o_size)
        return;

      constexpr std::size_t header_size = 12;
      constexpr std::size_t header_table_size = 256 * header_size;
      constexpr std::size_t classic_info_size = 24;

      if (mh2o_size < header_table_size)
      {
        LogError << "[ModernADT][WaterAdapter] MH2O too small on tile "
                 << tile->index.x << ',' << tile->index.z << ": " << mh2o_size
                 << " bytes." << std::endl;
        return;
      }

      auto const* mh2o = data + mh2o_payload;
      std::size_t wet_chunks = 0;
      std::size_t layer_records = 0;
      std::size_t plausible_headers = 0;
      std::size_t emitted_payloads = 0;
      std::set<std::uint16_t> field02_values;
      std::set<std::uint16_t> payload_fields_emitted;

      for (std::size_t i = 0; i < 256; ++i)
      {
        auto const base = i * header_size;
        auto const ofs_information = read_u32(mh2o, base);
        auto const layer_count = read_u32(mh2o, base + 4);
        auto const ofs_attributes = read_u32(mh2o, base + 8);

        if (!ofs_information && !layer_count && !ofs_attributes)
          continue;

        ++wet_chunks;
        bool const information_in_range = ofs_information < mh2o_size;
        bool const attributes_in_range = !ofs_attributes || ofs_attributes < mh2o_size;
        bool const count_sane = layer_count > 0 && layer_count <= 64;
        bool const information_table_fits = information_in_range && count_sane &&
          (static_cast<std::size_t>(ofs_information) +
             static_cast<std::size_t>(layer_count) * classic_info_size <= mh2o_size);

        if (!(information_in_range && attributes_in_range && count_sane && information_table_fits))
        {
          LogError << "[ModernADT][WaterAdapter] implausible header tile="
                   << tile->index.x << ',' << tile->index.z
                   << " chunk=" << i
                   << " ofsInfo=" << ofs_information
                   << " layers=" << layer_count
                   << " ofsAttrs=" << ofs_attributes << std::endl;
          continue;
        }

        ++plausible_headers;
        layer_records += layer_count;

        for (std::size_t layer = 0; layer < layer_count; ++layer)
        {
          auto const info_offset = static_cast<std::size_t>(ofs_information) + layer * classic_info_size;
          auto const* info = mh2o + info_offset;

          auto const liquid_id = read_u16(info, 0);
          auto const field_02 = read_u16(info, 2);
          auto const min_height = read_f32(info, 4);
          auto const max_height = read_f32(info, 8);
          auto const width = static_cast<unsigned>(static_cast<unsigned char>(info[14]));
          auto const height = static_cast<unsigned>(static_cast<unsigned char>(info[15]));
          auto const ofs_vertex = read_u32(info, 20);

          field02_values.insert(field_02);

          if (ofs_vertex && ofs_vertex < mh2o_size &&
              payload_fields_emitted.insert(field_02).second && emitted_payloads < 8)
          {
            auto const vertex_count = static_cast<std::size_t>(width + 1) *
                                      static_cast<std::size_t>(height + 1);
            auto const available = static_cast<std::size_t>(mh2o_size - ofs_vertex);
            auto const dump_bytes = std::min<std::size_t>(available, 64);

            LogDebug << "[ModernADT][WaterAdapter] sample tile="
                     << tile->index.x << ',' << tile->index.z
                     << " liquidId=" << liquid_id
                     << " metadata=0x" << std::hex << field_02 << std::dec
                     << " min=" << min_height << " max=" << max_height
                     << " vertexCount=" << vertex_count
                     << " first" << dump_bytes << "={"
                     << hex_dump(mh2o + ofs_vertex, available, dump_bytes) << '}'
                     << std::endl;
            ++emitted_payloads;
          }
        }
      }

      if (!wet_chunks)
        return;

      if (plausible_headers != wet_chunks)
      {
        LogError << "[ModernADT][WaterAdapter] Refusing MH2O on tile "
                 << tile->index.x << ',' << tile->index.z
                 << ": only " << plausible_headers << '/' << wet_chunks
                 << " wet headers validated." << std::endl;
        return;
      }

      std::ostringstream fields;
      bool first = true;
      for (auto const value : field02_values)
      {
        if (!first)
          fields << ',';
        fields << "0x" << std::hex << value << std::dec;
        first = false;
      }

      LogDebug << "[ModernADT][WaterAdapter] Validated tile="
               << tile->index.x << ',' << tile->index.z
               << " wetChunks=" << wet_chunks
               << " layerRecords=" << layer_records
               << " metadataValues={" << fields.str() << "}"
               << " MH2O=" << mh2o_size << " bytes; loading adapted water."
               << std::endl;

      file.seek(mh2o_payload);
      tile->Water.readFromFile(file, mh2o_payload);

      LogDebug << "[ModernADT][WaterAdapter] Loaded adapted MH2O water for tile "
               << tile->index.x << ',' << tile->index.z << '.' << std::endl;
    }
    catch (std::exception const& e)
    {
      LogError << "[ModernADT][WaterAdapter] Failed to load MH2O for '"
               << path << "': " << e.what() << ". Terrain remains usable." << std::endl;
    }
    catch (...)
    {
      LogError << "[ModernADT][WaterAdapter] Failed to load MH2O for '"
               << path << "' with an unknown exception. Terrain remains usable."
               << std::endl;
    }
  }
}

AsyncObject::AsyncObject(BlizzardArchive::Listfile::FileKey file_key) : _file_key(std::move(file_key)) {}

[[nodiscard]]
BlizzardArchive::Listfile::FileKey const& AsyncObject::file_key() const
{
  return _file_key;
}

[[nodiscard]]
bool AsyncObject::finishedLoading() const
{
  bool const done = finished.load();
  if (done)
    inspect_and_load_modern_mh2o_if_needed(this);
  return done;
}

[[nodiscard]]
bool AsyncObject::loading_failed() const
{
  return _loading_failed;
}

void AsyncObject::wait_until_loaded()
{
  if (finished.load())
    return;

  if (_file_key.hasFilepath())
  {
    auto const& path = _file_key.filepath();
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".blp") == 0)
      return;
  }

  std::unique_lock<std::mutex> lock(_mutex);
  _state_changed.wait(lock, [&] { return finished.load(); });
}

void AsyncObject::error_on_loading()
{
  LogError << "File " << (_file_key.hasFilepath() ? _file_key.filepath() : std::to_string(_file_key.fileDataID()))
           << " could not be loaded" << std::endl;
  _loading_failed = true;
  finished = true;
  _state_changed.notify_all();
}

[[nodiscard]]
bool AsyncObject::is_required_when_saving() const
{
  return false;
}

[[nodiscard]]
async_priority AsyncObject::loading_priority() const
{
  return async_priority::medium;
}
