// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/async_priority.hpp>
#include <noggit/LoadFailures.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <list>
#include <memory>
#include <thread>

class AsyncObject;

class AsyncLoader
{
public:
  // static AsyncLoader& instance()
  // {
  //   static AsyncLoader async_loader(3);
  //   return async_loader;
  // }

  // use regular pointer because unique_ptr was causing
  // a significant performance hit
  static AsyncLoader* instance;

  static void setup(int threads);

  //! Ownership is _not_ transferred. Call ensure_deletable to ensure 
  //! that a previously enqueued object can be destroyed.
  void queue_for_load (AsyncObject*);
  
  void ensure_deletable (AsyncObject*);

  bool is_loading();

  AsyncLoader(int numThreads);
  ~AsyncLoader();

  bool important_object_failed_loading() const;
  void reset_object_fail();
  std::vector<LoadFailure> loading_failures() const;
  void forget_failure(AsyncObject const*);
  std::string failure_report_path() const;

private:
  void process();
  void record_failure(AsyncObject const*, std::string const& reason, bool retail);

  std::mutex _guard;
  std::condition_variable _state_changed;
  std::atomic<bool> _stop;
  std::array<std::list<AsyncObject*>, (size_t)async_priority::count> _to_load;
  std::list<AsyncObject*> _currently_loading;
  std::list<std::thread> _threads;
  std::atomic<bool> _important_object_failed_loading = false;
  LoadFailures _failures;
};
