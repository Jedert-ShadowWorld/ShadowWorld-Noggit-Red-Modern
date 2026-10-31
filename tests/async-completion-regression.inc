#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;
void check(bool value) { if (!value) throw std::runtime_error("Async completion regression"); }
struct FileKey
{
  std::string path;
  bool hasFilepath() const { return !path.empty(); }
  std::string const& filepath() const { return path; }
};
struct AsyncObject
{
  std::atomic<bool> finished = false;
  std::atomic<bool> _loading_failed = false;
  std::mutex _mutex;
  std::condition_variable _state_changed;
  FileKey _file_key;
  void complete_loading(bool failed = false);
  void reset_loading();
  void wait_until_loaded();
};
// ACTUAL_COMPLETION

int main()
{
  try
  {
    AsyncObject object;
    // Completion must not publish while a waiter owns its predicate mutex.
    // A notify without that lock could be lost immediately before cv.wait().
    std::unique_lock lock(object._mutex);
    std::promise<void> entered;
    auto started = entered.get_future();
    auto completion = std::async(std::launch::async, [&]
    {
      entered.set_value();
      object.complete_loading();
    });
    started.wait();
    bool blocked = completion.wait_for(100ms) == std::future_status::timeout;
    bool unpublished = !object.finished.load();
    lock.unlock();
    completion.get();
    check(blocked && unpublished && object.finished.load());

    // Exercise repeated success/error publication with waiting consumers,
    // including FileDataID-only textures (no .blp pathname shortcut).
    for (int iteration = 0; iteration < 500; ++iteration)
    {
      object.reset_loading();
      check(!object.finished && !object._loading_failed);
      auto waiter = std::async(std::launch::async, [&] { object.wait_until_loaded(); });
      bool failed = iteration % 2;
      object.complete_loading(failed);
      if (waiter.wait_for(2s) != std::future_status::ready)
      {
        // Release the broken implementation before reporting a failed test.
        object._state_changed.notify_all();
        waiter.wait();
        throw std::runtime_error("Completion notification was lost");
      }
      waiter.get();
      check(object._loading_failed == failed);
    }
    std::cout << "PASS synchronized payload completion and failure wakeup\n";
    return 0;
  }
  catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
