// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <string>
#include <Exception.hpp>
#include <ClientFile.hpp>
#include <Listfile.hpp>
#include <noggit/AsyncLoader.h>
#include <noggit/AsyncObject.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/errorHandling.h>
#include <noggit/Log.h>
#include <noggit/project/CurrentProject.hpp>
#include <util/exception_to_string.hpp>

#include <QtCore/QSettings>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <list>
#include <sstream>

namespace
{
  std::string async_object_name(AsyncObject const* object)
  {
    if (!object)
      return "<null>";

    auto const& file_key = object->file_key();
    return file_key.hasFilepath() ? file_key.filepath() : std::to_string(file_key.fileDataID());
  }

  bool string_ends_with(std::string const& value, std::string const& suffix)
  {
    return value.size() >= suffix.size()
      && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  bool is_root_adt_path(std::string const& path)
  {
    if (!string_ends_with(path, ".adt"))
      return false;

    return !string_ends_with(path, "_tex0.adt")
      && !string_ends_with(path, "_obj0.adt")
      && !string_ends_with(path, "_obj1.adt")
      && !string_ends_with(path, "_lod.adt");
  }

  std::string split_adt_path(std::string const& root_path, std::string const& suffix)
  {
    auto const extension = root_path.rfind(".adt");
    if (extension == std::string::npos)
      return root_path + suffix;

    return root_path.substr(0, extension) + suffix + ".adt";
  }

  std::string fourcc_name(std::uint32_t value)
  {
    std::string result(4, '.');
    for (std::size_t i = 0; i < 4; ++i)
    {
      auto const c = static_cast<unsigned char>((value >> (i * 8)) & 0xFFu);
      result[i] = c >= 32 && c <= 126 ? static_cast<char>(c) : '.';
    }
    return result;
  }

  struct ModernAdtScanResult
  {
    bool opened = false;
    bool structurally_valid = false;
    std::size_t mcnk_count = 0;
  };

  void scan_mcnk_payload(char const* payload,
                         std::size_t payload_size,
                         bool has_header,
                         std::size_t chunk_index,
                         std::string const& label)
  {
    constexpr std::size_t mcnk_header_size = 128;
    std::size_t pos = has_header ? mcnk_header_size : 0;

    if (pos > payload_size)
    {
      LogError << "[ModernADT] " << label << " MCNK[" << chunk_index
               << "] is smaller than its 128-byte header (size=" << payload_size << ")."
               << std::endl;
      return;
    }

    std::ostringstream subchunks;
    std::size_t subchunk_count = 0;

    while (pos + 8 <= payload_size)
    {
      std::uint32_t magic = 0;
      std::uint32_t declared_size = 0;
      std::memcpy(&magic, payload + pos, sizeof(magic));
      std::memcpy(&declared_size, payload + pos + 4, sizeof(declared_size));

      auto const data_pos = pos + 8;
      auto const remaining = payload_size - data_pos;
      if (declared_size > remaining)
      {
        LogError << "[ModernADT] " << label << " MCNK[" << chunk_index << "] subchunk "
                 << fourcc_name(magic) << " declares " << declared_size
                 << " bytes with only " << remaining << " remaining (offset=" << pos << ")."
                 << std::endl;
        return;
      }

      if (subchunk_count != 0)
        subchunks << ' ';
      subchunks << fourcc_name(magic) << '(' << declared_size << ')';
      ++subchunk_count;

      pos = data_pos + declared_size;
    }

    if (pos != payload_size)
    {
      LogError << "[ModernADT] " << label << " MCNK[" << chunk_index << "] leaves "
               << (payload_size - pos) << " trailing byte(s) after subchunk scan."
               << std::endl;
    }

    LogDebug << "[ModernADT] " << label << " MCNK[" << chunk_index << "] size="
             << payload_size << " subchunks=" << subchunk_count << " :: "
             << subchunks.str() << std::endl;
  }

  ModernAdtScanResult scan_modern_adt_file(std::string const& path,
                                           std::string const& label,
                                           bool mcnk_has_header)
  {
    ModernAdtScanResult result;

    auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
    try
    {
      BlizzardArchive::Listfile::FileKey key(path);
      BlizzardArchive::ClientFile file(key, client_data);
      result.opened = true;

      auto const* data = file.getBuffer();
      auto const file_size = file.getSize();
      std::size_t pos = 0;
      bool valid = true;

      LogDebug << "[ModernADT] Scanning " << label << " file '" << path
               << "' size=" << file_size << std::endl;

      while (pos + 8 <= file_size)
      {
        std::uint32_t magic = 0;
        std::uint32_t declared_size = 0;
        std::memcpy(&magic, data + pos, sizeof(magic));
        std::memcpy(&declared_size, data + pos + 4, sizeof(declared_size));

        auto const payload_pos = pos + 8;
        auto const remaining = file_size - payload_pos;
        if (declared_size > remaining)
        {
          LogError << "[ModernADT] " << label << " top-level chunk " << fourcc_name(magic)
                   << " at offset " << pos << " declares " << declared_size
                   << " bytes with only " << remaining << " remaining."
                   << std::endl;
          valid = false;
          break;
        }

        if (magic == 0x4B4E434Du) // MCNK in little-endian byte order
        {
          scan_mcnk_payload(data + payload_pos,
                            declared_size,
                            mcnk_has_header,
                            result.mcnk_count,
                            label);
          ++result.mcnk_count;
        }
        else
        {
          LogDebug << "[ModernADT] " << label << " chunk " << fourcc_name(magic)
                   << " offset=" << pos << " size=" << declared_size << std::endl;
        }

        pos = payload_pos + declared_size;
      }

      if (pos != file_size)
      {
        LogError << "[ModernADT] " << label << " scan stopped at " << pos
                 << " of " << file_size << " bytes."
                 << std::endl;
        valid = false;
      }

      result.structurally_valid = valid;
      LogDebug << "[ModernADT] " << label << " summary: MCNK=" << result.mcnk_count
               << " structurally_valid=" << (result.structurally_valid ? "yes" : "no")
               << std::endl;
    }
    catch (std::exception const& e)
    {
      LogError << "[ModernADT] Unable to open/scan " << label << " file '" << path
               << "': " << e.what() << std::endl;
    }
    catch (...)
    {
      LogError << "[ModernADT] Unable to open/scan " << label << " file '" << path
               << "': unknown error." << std::endl;
    }

    return result;
  }

  bool run_shadowlands_split_adt_diagnostics(std::string const& root_path)
  {
    auto* project = Noggit::Project::CurrentProject::get();
    if (project->projectVersion != Noggit::Project::ProjectVersion::SL)
      return false;

    if (!is_root_adt_path(root_path))
      return false;

    LogDebug << "[ModernADT] Shadowlands split-ADT diagnostic path engaged for '"
             << root_path << "'. Legacy WotLK MapTile parsing will be skipped for this tile."
             << std::endl;

    auto const root = scan_modern_adt_file(root_path, "ROOT", true);
    auto const tex0 = scan_modern_adt_file(split_adt_path(root_path, "_tex0"), "TEX0", false);
    auto const obj0 = scan_modern_adt_file(split_adt_path(root_path, "_obj0"), "OBJ0", false);

    bool const complete = root.opened && tex0.opened && obj0.opened
      && root.structurally_valid && tex0.structurally_valid && obj0.structurally_valid
      && root.mcnk_count == 256 && tex0.mcnk_count == 256 && obj0.mcnk_count == 256;

    LogDebug << "[ModernADT] Tile summary ROOT=" << root.mcnk_count
             << " TEX0=" << tex0.mcnk_count
             << " OBJ0=" << obj0.mcnk_count
             << " verified_256x3=" << (complete ? "yes" : "no")
             << std::endl;

    if (!complete)
    {
      LogError << "[ModernADT] Split ADT diagnostic scan is incomplete. "
               << "Use the per-file/per-chunk diagnostics above to locate the first layout mismatch."
               << std::endl;
    }

    return true;
  }
}

AsyncLoader* AsyncLoader::instance;

void AsyncLoader::setup(int threads)
{
  // make sure there's always at least one thread otherwise nothing can load
  instance = new AsyncLoader(std::max(1, threads));
}

bool AsyncLoader::is_loading()
{
  std::lock_guard<std::mutex> const lock (_guard);
  return !_currently_loading.empty();
}

void AsyncLoader::process()
{
  AsyncObject* object = nullptr;
  std::string object_name;

  QSettings settings;
  bool additional_log = settings.value("additional_file_loading_log", true).toBool();

  while (!_stop)
  {
    {
      std::unique_lock<std::mutex> lock (_guard);

      _state_changed.wait
      ( lock
      , [&]
        {
          return !!_stop || std::any_of ( _to_load.begin(), _to_load.end()
                                        , [](auto const& to_load) { return !to_load.empty(); }
                                        );
        }
      );

      if (_stop)
      {
        return;
      }

      for (auto& to_load : _to_load)
      {
        if (to_load.empty())
        {
          continue;
        }

        object = to_load.front();
        _currently_loading.emplace_back (object);
        to_load.pop_front();
        object_name = async_object_name(object);

        break;
      }
    }

    try
    {
      if (additional_log)
      {
        std::lock_guard<std::mutex> const lock(_guard);
        LogDebug << "Loading file '" << object_name << "'" << std::endl;
      }

      if (run_shadowlands_split_adt_diagnostics(object_name))
      {
        std::lock_guard<std::mutex> const lock(_guard);
        LogDebug << "[ModernADT] Diagnostic scan completed for '" << object_name
                 << "'. Marking the tile load as failed intentionally so no legacy MapChunk/render path consumes split modern data."
                 << std::endl;

        if (object->is_required_when_saving())
          _important_object_failed_loading = true;

        _currently_loading.remove(object);
        object->error_on_loading();
        _state_changed.notify_all();
        continue;
      }

      object->finishLoading();

      if (additional_log)
      {
        std::lock_guard<std::mutex> const lock(_guard);
        LogDebug << "Loaded  file '" << object_name << "'" << std::endl;
      }

      {
        std::lock_guard<std::mutex> const lock (_guard);
        _currently_loading.remove (object);
        _state_changed.notify_all();
      }
    }
    catch (BlizzardArchive::Exceptions::FileReadFailedError const& e)
    {
      std::lock_guard<std::mutex> const lock(_guard);

      LogError << "Caught file read error while loading '" << object_name << "': " << e.what() << std::endl;

      if (object->is_required_when_saving())
      {
        _important_object_failed_loading = true;
      }

      _currently_loading.remove(object);
      object->error_on_loading();
      _state_changed.notify_all();
    }
    catch (...)
    {
      std::lock_guard<std::mutex> const lock(_guard);

      std::string const reason{ util::exception_to_string(std::current_exception()) };
      LogError << "Caught unknown error while loading '" << object_name << "': " << reason <<  std::endl;

      if (object->is_required_when_saving())
      {
        _important_object_failed_loading = true;
      }

      _currently_loading.remove(object);
      object->error_on_loading();
      _state_changed.notify_all();
    }
  }
}

void AsyncLoader::queue_for_load (AsyncObject* object)
{
  std::lock_guard<std::mutex> const lock (_guard);
  _to_load[(size_t)object->loading_priority()].push_back (object);
  _state_changed.notify_one();
}

void AsyncLoader::ensure_deletable (AsyncObject* object)
{
  std::unique_lock<std::mutex> lock (_guard);
  _state_changed.wait
  ( lock
  , [&]
    {
      auto& to_load = _to_load[(size_t)object->loading_priority()];
      auto const& it = std::find (to_load.begin(), to_load.end(), object);

      // don't load it if it's just to delete it afterward
      if (it != to_load.end())
      {
        to_load.erase(it);
        return true;
      }
      else
      {
        return std::find (_currently_loading.begin(), _currently_loading.end(), object) == _currently_loading.end();
      }
    }
  );
}

AsyncLoader::AsyncLoader(int numThreads)
  : _stop (false)
{
  // use half of the available threads
  // unsigned int maxThreads = std::thread::hardware_concurrency() / 2;
  // numThreads = maxThreads > numThreads ? maxThreads : numThreads;

  for (int i = 0; i < numThreads; ++i)
  {
    _threads.emplace_back (&AsyncLoader::process, this);
  }
}

AsyncLoader::~AsyncLoader()
{
  {
    std::unique_lock<std::mutex> lock(_guard);
    _stop = true;
  }
  _state_changed.notify_all();

  for (auto& thread : _threads)
  {
    thread.join();
  }
}

bool AsyncLoader::important_object_failed_loading() const
{
  return _important_object_failed_loading;
}

void AsyncLoader::reset_object_fail()
{
  _important_object_failed_loading = false;
}
