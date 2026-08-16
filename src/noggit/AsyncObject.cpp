// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncObject.h>
#include <noggit/Log.h>
#include <noggit/MapTile.h>
#include <noggit/application/NoggitApplication.hpp>

#include <ClientFile.hpp>

#include <cstdint>
#include <cstring>
#include <mutex>
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

  // The modern split-ADT loader currently owns ROOT parsing in MapTileModern.cpp,
  // while the legacy TileWater implementation already understands MH2O itself.
  // Until the modern loader is split into dedicated terrain/water/object phases,
  // lazily attach ROOT MH2O once the reconstructed MapTile is fully available.
  // This keeps water parsing out of the AsyncLoader worker pool (important for
  // parallel tile streaming) and lets us reuse the proven MH2O reader unchanged.
  void load_modern_mh2o_if_needed(AsyncObject const* object)
  {
    auto* tile = dynamic_cast<MapTile*>(const_cast<AsyncObject*>(object));
    if (!tile || !object->file_key().hasFilepath())
      return;

    auto const& path = object->file_key().filepath();
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".adt") != 0)
      return;

    // finishedLoading() can be queried many times per frame. Ensure each tile is
    // inspected at most once, including dry tiles which contain no MH2O data.
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

      // Legacy monolithic ADTs are already handled by MapTile::finishLoading().
      // Our Shadowlands ROOTs have the 256 MCNKs directly and no MCIN table.
      if (has_legacy_mcin || mcnk_count != 256 || !mh2o_payload || !mh2o_size)
        return;

      constexpr std::size_t mh2o_header_table_size = 256 * sizeof(MH2O_Header);
      if (mh2o_size < mh2o_header_table_size)
      {
        LogError << "[ModernADT][Water] MH2O chunk is too small on tile "
                 << tile->index.x << ',' << tile->index.z << ": " << mh2o_size
                 << " bytes (need at least " << mh2o_header_table_size << ")."
                 << std::endl;
        return;
      }

      file.seek(mh2o_payload);
      tile->Water.readFromFile(file, mh2o_payload);

      LogDebug << "[ModernADT][Water] Loaded ROOT MH2O for tile "
               << tile->index.x << ',' << tile->index.z
               << " (" << mh2o_size << " bytes)." << std::endl;
    }
    catch (...)
    {
      // Water is an incremental modern-client feature. Never invalidate terrain
      // that has already loaded successfully just because one MH2O needs more RE.
      LogError << "[ModernADT][Water] Failed to attach MH2O for '"
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
    load_modern_mh2o_if_needed(this);
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
  {
    return;
  }

  // Terrain texture references can be created from inside an AsyncLoader worker
  // while modern split ADTs are being streamed in parallel. Blocking that worker
  // until the BLP finishes can starve the same loader pool that must perform the
  // queued BLP load. Let BLP references remain asynchronous; render code already
  // skips not-yet-ready textures/tiles until their AsyncObject finishes.
  if (_file_key.hasFilepath())
  {
    auto const& path = _file_key.filepath();
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".blp") == 0)
    {
      return;
    }
  }

  std::unique_lock<std::mutex> lock(_mutex);

  _state_changed.wait
  (lock
    , [&]
    {
      return finished.load();
    }
  );
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
