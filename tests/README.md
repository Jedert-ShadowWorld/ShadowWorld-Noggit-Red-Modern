# Standalone regression checks

These tests cover format parsing and writing, map/UID preservation and renderer
resource lifetimes. They use synthetic in-repo fixtures and temporary
directories, and do not require a client installation. They are built
separately from the main application.

Configure with a C++20 compiler and CMake 3.16 or newer:

```sh
cmake -S tests -B build/regressions -DCMAKE_BUILD_TYPE=Release
cmake --build build/regressions --config Release --parallel
ctest --test-dir build/regressions -C Release --output-on-failure
```

Set `CMAKE_PREFIX_PATH` to a Qt 5 SDK to enable the Qt and OpenGL tests, and
make its runtime libraries available to the test process. Without Qt these
tests are not configured. OpenGL tests need an OpenGL 4.1 core context and
exit with code 77 (reported as skipped) when none is available.

On Windows, the `tools/prepare-*-regression.ps1` scripts generate four tests
from current production sources (WMO upload and context restoration, M2
texture key lifetimes, object clipboard copying, OBJ1 serialization) combined
with test doubles. Generated sources are written to the build directory. Other
platforms omit these four tests.

With Qt, a fifth Windows fixture exercises the actual Retail LOD reference
resolver, update/catch path and configuration methods. It uses temporary files
and an archive gateway double to check structural calibration, retained hashes,
fallback selection, legacy references and recovery of invalid configuration.
Successful and rejected updates run on a thread with a reserved 256 KiB stack.
It does not open CASC. A sixth extracted fixture checks the actual Retail export
queue with a recording renderer: scene pause/restoration, tool switching,
completion, cancellation and failed jobs.

Terrain LOD checks include near-flat float equivalence, strict hole masks,
reference rejection, new-hole cutting, closure, refinement and idempotence.
WDL checks cover selected MARE/MAHO writes,
whole-chunk versus partial holes and finite Int16 height conversion. The paired
texture state test uses real temporary files and injects a second-commit failure
to verify rollback. These checks do not replace an editor or game-client run.

Some test executables accept optional file or directory arguments for
read-only checks against local client data. CTest passes only repository
paths (shader sources), so registered tests depend only on the checkout.

Other test sources in this directory are not registered in `CMakeLists.txt`.
