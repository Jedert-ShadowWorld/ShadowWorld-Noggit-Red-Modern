// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/async_priority.hpp>
#include <Listfile.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>

class AsyncObject
{
private: 
  std::atomic<bool> _loading_failed = {false};
  mutable std::mutex _modern_attachment_mutex;
  mutable std::condition_variable _modern_attachment_changed;
  mutable unsigned char _modern_water_state = 0;
  mutable unsigned char _modern_objects_state = 0;

protected:
  std::atomic<bool> finished = {false};
  std::mutex _mutex;
  std::condition_variable _state_changed;

  BlizzardArchive::Listfile::FileKey _file_key;

  AsyncObject(BlizzardArchive::Listfile::FileKey file_key);

  // Publish payload readiness under the same mutex used by wait_until_loaded.
  // This does not replace AsyncLoader::ensure_deletable: the worker may still
  // be using the object after its payload is ready.
  void complete_loading(bool failed = false);
  void reset_loading();

public:

  [[nodiscard]]
  BlizzardArchive::Listfile::FileKey const& file_key() const;

  AsyncObject() = delete;
  virtual ~AsyncObject();

  [[nodiscard]]
  virtual bool finishedLoading() const;

  [[nodiscard]]
  bool loading_failed() const;

  void wait_until_loaded();

  void error_on_loading();

  [[nodiscard]]
  virtual bool is_required_when_saving() const;

  [[nodiscard]]
  virtual async_priority loading_priority() const;

  bool begin_modern_attachment(bool water) const;
  void finish_modern_attachment(bool water) const;

  virtual void finishLoading() = 0;
  virtual void waitForChildrenLoaded() = 0;
};
