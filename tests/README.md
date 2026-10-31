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

On Windows, the `tools/prepare-*-regression.ps1` scripts extract current
production methods and combine them with test doubles. Generated sources stay
in the build directory; other platforms omit these extracted fixtures. They
cover WMO upload/context restoration, M2 texture lifetimes, object clipboard
copying and OBJ1 serialization, plus:

| Test | Coverage |
| --- | --- |
| `texture_paint` | Layer weights and brush footprint, no-op dirty state, and replacement after layer cleanup changes indices. |
| `async_completion` | Completion publication under the predicate mutex and repeated success/failure wakeups, including FileDataID-only resources. |
| `tile_removal` | Queue removal without loading tiles, draining additions before the instance-storage lock, and retaining instances when an affected tile fails to load. |

`wmo_render_lifecycle` also exercises texture-array retirement, context generation
guards against reused GL names, and the actual terrain sampler/pruning methods.
It checks sixteen diffuse/height pairs in shared pages, slice retirement and
reuse, upload rollback with retained CPU data, and binding reuse after pruning.
Other cases cover draw-call coverage, explicit sampler overflow and recovery,
deferred binding updates across skipped frames, and refreshed layer animation
metadata. Its recording GL double checks resource ownership; it is not a driver
stress test. `texture_upload_binding` checks actual RGBA/DXT uploads, surviving
sibling slices and GL binding restoration on a GPU.

WMO checks also exercise the production MOCV decoder/fixup and preserve the
legacy lighting path. Synthetic ambient records cover MAVG/MAVD parsing,
channel order, directional colors and active doodad-set selection. The
`wmo_retail_material` GPU fixture distinguishes both MOCV alpha streams,
checks absent second texture and optional emission/environment/height stages, and verifies
Retail indoor ambient interpolation without changing legacy or unlit output.

With Qt, `terrain_lod_reference` exercises the actual Retail LOD reference
resolver, update/catch path and configuration methods. It uses temporary files
and an archive gateway double to check structural calibration, retained hashes,
fallback selection, legacy references and recovery of invalid configuration.
It also checks that a deferred save retains a custom map's original coherent
pair and that a later rebuild succeeds without client archives. Successful and
rejected updates run on a thread with a reserved 256 KiB stack. It does not open
CASC. `terrain_export_lifecycle` uses the actual Retail queue with a recording
renderer: scene pause/restoration, tool switching, cancellation, failed jobs and
automatic generation limited to the saved tile set. This fixture does not draw
terrain on a GPU.

`modern_terrain_shader` compiles the production shaders and checks their uniform
layout, then renders the actual blend functions into a framebuffer. Cases cover
independent height-texture indices, finite fallback weights and global height
blending across eight and sixteen layers. Slot permutations include the
fourth/fifth boundary with MCAL/MTXP values from the reported leaf seam, UV
scaling and world phase. Transform feedback checks the production vertex
animation path and all 64 Retail hole cells, including both packed mask words,
shared corner preservation and unchanged legacy hole vertices at both LODs.
Raster coverage checks the original 145-vertex terrain grid and all four fan
triangles per cell, including bits 31/32 and neighboring solid cells.
Framebuffer checks cover extra-layer animation, signed diffuse
slice indices/specular flags, and the thirteenth sampler. These GPU fixtures use
synthetic textures; they do not reproduce a complete client scene.
They also exercise the production Retail/modern-features dispatch: non-Retail
keeps its original four-layer height blend and unscaled extra-layer UVs, while
disabling modern features selects ordinary alpha blending for either client.

`terrain_holes` checks exact 8x8 source masks through no-edit preview, coarse
hole edits and undo, saved-baseline transitions for clear/save/undo and repaint,
plus expansion of every possible legacy 4x4 mask. These
checks do not modify ADTs or establish complete map rendering in the editor.

`map_texture_encoding` compares complete BLP outputs from the PNG and direct
RGBA paths for 512-pixel DXT5 diffuse and 256-pixel DXT1 normal fixtures, with and
without mipmaps and with padded rows. Invalid buffers are rejected. Its optional
`--benchmark` argument reports alternating PNG/RGBA encoding samples after
warmup; CTest runs correctness checks only. These timings exclude terrain
loading, rendering, GPU readback and file publication.

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
