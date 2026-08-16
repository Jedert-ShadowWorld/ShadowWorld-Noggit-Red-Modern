// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncObject.h>
#include <noggit/Log.h>
#include <noggit/MapTile.h>
#include <noggit/application/NoggitApplication.hpp>

#include <ClientFile.hpp>

#include <condition_variable>
#include <cstdint>
#include <cstring>
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

  // Water is currently attached after MapTile's terrain loader sets finished=true.
  // finishedLoading() can be queried from several threads, so the old one-shot set
  // allowed a second caller to observe/use the tile while the first caller was still
  // mutating TileWater. Keep a per-path loading/completed state and make every caller
  // wait until the MH2O attachment has fully completed before reporting the tile ready.
  void load_modern_mh2o_before_exposing_tile(AsyncObject const* object)
  {
    auto* tile = dynamic_cast<MapTile*>(const_cast<AsyncObject*>(object));
    if (!tile || !object->file_key().hasFilepath())
      return;

    auto const& path = object->file_key().filepath();
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".adt") != 0)
      return;

    static std::mutex state_mutex;
    static std::condition_variable state_changed;
    static std::unordered_set<std::string> loading;
    static std::unordered_set<std::string> completed;

    {
      std::unique_lock<std::mutex> lock(state_mutex);
      if (completed.count(path))
        return;

      if (loading.count(path))
      {
        state_changed.wait(lock, [&] { return completed.count(path) != 0; });
        return;
      }

      loading.insert(path);
    }

    auto finish_state = [&]
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      loading.erase(path);
      completed.insert(path);
      state_changed.notify_all();
    };

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
        {
          finish_state();
          return;
        }

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
      {
        finish_state();
        return;
      }

      constexpr std::size_t header_size = 12;
      constexpr std::size_t header_table_size = 256 * header_size;
      constexpr std::size_t info_size = 24;

      if (mh2o_size < header_table_size)
      {
        LogError << "[ModernADT][WaterAdapter] MH2O too small on tile "
                 << tile->index.x << ',' << tile->index.z << ": " << mh2o_size
                 << " bytes." << std::endl;
        finish_state();
        return;
      }

      auto const* mh2o = data + mh2o_payload;
      std::size_t wet_chunks = 0;
      std::size_t plausible_headers = 0;
      std::size_t layer_records = 0;
      std::set<std::uint16_t> metadata_values;

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
          static_cast<std::size_t>(ofs_information) +
            static_cast<std::size_t>(layer_count) * info_size <= mh2o_size;

        if (!(information_in_range && attributes_in_range && count_sane && information_table_fits))
          continue;

        ++plausible_headers;
        layer_records += layer_count;
        for (std::size_t layer = 0; layer < layer_count; ++layer)
        {
          auto const info_offset = static_cast<std::size_t>(ofs_information) + layer * info_size;
          metadata_values.insert(read_u16(mh2o + info_offset, 2));
        }
      }

      if (!wet_chunks || plausible_headers != wet_chunks)
      {
        if (wet_chunks)
          LogError << "[ModernADT][WaterAdapter] Refusing MH2O on tile "
                   << tile->index.x << ',' << tile->index.z << ": only "
                   << plausible_headers << '/' << wet_chunks << " headers validated."
                   << std::endl;
        finish_state();
        return;
      }

      std::ostringstream fields;
      bool first = true;
      for (auto const value : metadata_values)
      {
        if (!first)
          fields << ',';
        fields << "0x" << std::hex << value << std::dec;
        first = false;
      }

      LogDebug << "[ModernADT][WaterAdapter] Attaching synchronized water tile="
               << tile->index.x << ',' << tile->index.z
               << " wetChunks=" << wet_chunks
               << " layerRecords=" << layer_records
               << " metadataValues={" << fields.str() << "}." << std::endl;

      file.seek(mh2o_payload);
      tile->Water.readFromFile(file, mh2o_payload);

      LogDebug << "[ModernADT][WaterAdapter] Synchronized MH2O attachment complete for tile "
               << tile->index.x << ',' << tile->index.z << '.' << std::endl;
    }
    catch (std::exception const& e)
    {
      LogError << "[ModernADT][WaterAdapter] Failed to attach MH2O for '"
               << path << "': " << e.what() << ". Terrain remains usable." << std::endl;
    }
    catch (...)
    {
      LogError << "[ModernADT][WaterAdapter] Failed to attach MH2O for '"
               << path << "' with an unknown exception. Terrain remains usable."
               << std::endl;
    }

    finish_state();
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
    load_modern_mh2o_before_exposing_tile(this);
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
