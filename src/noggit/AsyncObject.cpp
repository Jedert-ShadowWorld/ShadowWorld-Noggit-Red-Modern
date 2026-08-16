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

  std::uint32_t read_u32(char const* data, std::size_t offset)
  {
    std::uint32_t value = 0;
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

  // Diagnostic-only modern MH2O pass. Do not feed the chunk to the legacy
  // TileWater parser yet: current Shadowlands data produces invalid LiquidType
  // values, so first identify the exact header/layer layout from raw offsets.
  void inspect_modern_mh2o_if_needed(AsyncObject const* object)
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

      auto const* mh2o = data + mh2o_payload;
      LogDebug << "[ModernADT][WaterDiag] tile=" << tile->index.x << ',' << tile->index.z
               << " MH2O payload=" << mh2o_payload << " size=" << mh2o_size
               << " first256={" << hex_dump(mh2o, mh2o_size, 256) << '}'
               << std::endl;

      // Classic MH2O uses 256 x 12-byte headers:
      //   uint32 ofsLiquid; uint32 layerCount; uint32 ofsAttributes.
      // Log every non-empty candidate header, but do not trust it yet. Offsets
      // are checked against this MH2O payload so bad interpretations stand out.
      constexpr std::size_t candidate_header_size = 12;
      constexpr std::size_t candidate_table_size = 256 * candidate_header_size;
      if (mh2o_size >= candidate_table_size)
      {
        std::size_t nonzero_headers = 0;
        std::size_t plausible_headers = 0;
        for (std::size_t i = 0; i < 256; ++i)
        {
          auto const base = i * candidate_header_size;
          auto const ofs_liquid = read_u32(mh2o, base);
          auto const layer_count = read_u32(mh2o, base + 4);
          auto const ofs_attributes = read_u32(mh2o, base + 8);

          if (!ofs_liquid && !layer_count && !ofs_attributes)
            continue;

          ++nonzero_headers;
          bool const liquid_in_range = !ofs_liquid || ofs_liquid < mh2o_size;
          bool const attrs_in_range = !ofs_attributes || ofs_attributes < mh2o_size;
          bool const count_sane = layer_count <= 64;
          bool const plausible = liquid_in_range && attrs_in_range && count_sane;
          if (plausible)
            ++plausible_headers;

          if (nonzero_headers <= 32)
          {
            LogDebug << "[ModernADT][WaterDiag] headerCandidate chunk=" << i
                     << " grid=" << (i / 16) << ',' << (i % 16)
                     << " ofsLiquid=" << ofs_liquid
                     << " layers=" << layer_count
                     << " ofsAttrs=" << ofs_attributes
                     << " liquidInRange=" << liquid_in_range
                     << " attrsInRange=" << attrs_in_range
                     << " countSane=" << count_sane
                     << std::endl;

            if (ofs_liquid && ofs_liquid < mh2o_size)
            {
              auto const available = static_cast<std::size_t>(mh2o_size - ofs_liquid);
              LogDebug << "[ModernADT][WaterDiag] layerBytes chunk=" << i
                       << " @" << ofs_liquid << " first64={"
                       << hex_dump(mh2o + ofs_liquid, available, 64) << '}'
                       << std::endl;
            }
          }
        }

        LogDebug << "[ModernADT][WaterDiag] candidateSummary tile="
                 << tile->index.x << ',' << tile->index.z
                 << " nonzero=" << nonzero_headers
                 << " plausible=" << plausible_headers
                 << "/256 using 12-byte classic header hypothesis."
                 << std::endl;
      }

      LogDebug << "[ModernADT][WaterDiag] Legacy TileWater parsing intentionally skipped for tile "
               << tile->index.x << ',' << tile->index.z
               << " until the Shadowlands MH2O layout is confirmed."
               << std::endl;
    }
    catch (...)
    {
      LogError << "[ModernADT][WaterDiag] Failed to inspect MH2O for '"
               << path << "'. Terrain remains usable." << std::endl;
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
    inspect_modern_mh2o_if_needed(this);
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
