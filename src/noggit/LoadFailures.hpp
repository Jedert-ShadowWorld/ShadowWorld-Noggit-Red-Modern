#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct LoadFailure
{
  std::string asset;
  std::string reason;
  bool requiredForSaving;
};

// Keyed by the failed object; an entry is removed when that object is destroyed.
class LoadFailures
{
public:
  void record(void const* owner, LoadFailure failure)
  { std::lock_guard lock(_mutex); _entries[owner] = std::move(failure); }
  void forget(void const* owner)
  { std::lock_guard lock(_mutex); _entries.erase(owner); }
  std::vector<LoadFailure> required() const
  {
    std::lock_guard lock(_mutex);
    std::vector<LoadFailure> result;
    for (auto const& [owner, failure] : _entries)
      if (failure.requiredForSaving) result.push_back(failure);
    return result;
  }
private:
  mutable std::mutex _mutex;
  std::map<void const*, LoadFailure> _entries;
};
