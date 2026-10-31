#include <noggit/LoadFailures.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>
void check(bool condition) { if (!condition) throw std::runtime_error("Failure lifecycle regression"); }
int main()
{
  LoadFailures failures;
  int first, second, texture;
  failures.record(&first, {"a.m2", "Missing skin", true});
  failures.record(&second, {"b.wmo", "Invalid group", true});
  failures.record(&texture, {"t.blp", "Missing texture", false});
  check(failures.required().size() == 2);
  failures.forget(&first);
  auto active = failures.required();
  check(active.size() == 1 && active[0].asset == "b.wmo" && active[0].reason == "Invalid group");
  // Taking a snapshot (including after a save) must not acknowledge the failure.
  check(failures.required().size() == 1);
  failures.record(&second, {"b.wmo", "New attempt failed", true});
  check(failures.required()[0].reason == "New attempt failed");
  std::thread worker([&] {
    for (int i=0;i<1000;++i) {
      failures.record(&first, {"a.m2", "Concurrent failure", true});
      failures.forget(&first);
    }
  });
  for (int i=0;i<1000;++i) check(!failures.required().empty());
  worker.join();
  failures.forget(&second);
  check(failures.required().empty());
  std::cout << "PASS failure lifetime, filtering, retry, snapshots and concurrent access\n";
}
