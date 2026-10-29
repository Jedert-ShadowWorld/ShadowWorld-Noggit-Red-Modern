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

Some test executables accept optional file or directory arguments for
read-only checks against local client data. CTest passes only repository
paths (shader sources), so registered tests depend only on the checkout.

Other test sources in this directory are not registered in `CMakeLists.txt`.
