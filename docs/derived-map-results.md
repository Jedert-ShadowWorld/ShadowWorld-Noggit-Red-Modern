# Retail derived map results

For Retail projects, Noggit tracks derived map data independently of source
saves: diffuse and normal map textures, terrain LOD heights and holes, liquid
LOD, WDL heights and terrain occlusion. Rendering uses Noggit's terrain renderer;
files are written by the ADT writer. Other clients keep their existing behavior.

## Saving a map

**Save current tile**, **Save changed tiles** and **Save all tiles** save the
ADTs and dirty database files before any optional generation. By default,
generated LOD, map textures, WDL and occlusion files are retained and their
pending flags persist. Placement references and required OBJ1 placement metadata
are still saved. The source save is reported as complete and the undo history
cleared; intentionally deferred results are not a failed save.

**Settings > Retail > Update LOD, map textures and heightmap after saving**
enables automatic generation (off by default, key
`retail/generate_derived_on_save`). It schedules only the saved tiles on the
existing terrain export queue, after source saving succeeds. It does not expand
the scope to all project tiles or other pending tiles. The normal scene pauses
while the queue runs. Failure or cancellation leaves results pending and does not
undo the completed source save or skip database writes. A new save must wait for
the active export to finish or be cancelled.

With automatic generation disabled, use **Update project LOD + map textures +
heightmap** before testing or publishing the map. Tiles with unsaved changes
must be saved before their derived results are generated.

**Save all tiles** covers loaded tiles. A regular save does not process pending
tiles outside the saved set; use the Minimap Editor controls for those.

Bulk/import code that writes a tile directly retains its existing LOD-rebuild
policy and marks other results as pending without rendering. An explicit export
processes them. The editor's ordinary source saves pass a deferred policy to the
codec rather than reading UI settings inside the file writer.

Before the first deferred save, a coherent source ROOT/LOD pair is staged in
`.noggit/lod-reference/` if no retained reference files exist. This preserves a
fully custom map's baseline even when the client archives contain no original.
It validates the pair without regenerating LOD. Existing reference files are
not replaced by this capture; an incoherent pair leaves generation pending and
can still be repaired using a reference folder.

The Minimap Editor offers **Both map textures: selected tiles** and
**Both map textures: changed / pending tiles** for Retail projects. Both
require saved terrain.

| Output | Export profile |
| --- | --- |
| `world/maptextures/<map>/<map>_<x>_<y>.blp` | 512×512, DXT5, mipmaps |
| `world/maptextures/<map>/<map>_<x>_<y>_n.blp` | 256×256, DXT1, mipmaps |

Normal maps encode terrain orientation. Objects and water are excluded from
both images. Map textures do not update the minimap MD5 lookup. Files under
`textures/maptextures/` are neither migrated nor deleted.

Retail map-texture rendering prepares only the target tile and the neighbour
needed by the slightly tilted projection. Both must have ready terrain textures
before the image is accepted; unrelated loaded tiles are not uploaded or drawn
for each image. Normal viewport rendering and ordinary minimap rendering,
including legacy clients, retain their existing tile selection.

The captured RGBA image is passed directly to the existing BLP encoder, avoiding
an intermediate PNG encode/decode. Synthetic fixtures compare complete BLP bytes
against the PNG path, including alpha, mipmaps and padded image rows. This checks
encoding equivalence, not total export time or game-client appearance.

Terrain preview and map-texture export use the same layer metadata. Retail
layers above the first four now retain their MTXP UV scale and world phase
when modern features are enabled; other clients keep their previous UV path.
Animation metadata is
refreshed when layers move or disappear, and disabled animation cannot read
undefined direction data. These changes do not rewrite the source textures.

Retail terrain with modern features enabled uses one height-based blend across
all active layers, up to 16.
The MCAL weights, MHID alpha and MTXP scale/offset participate in the same
maximum and normalization calculation. Moving a material between the fourth
and fifth slots no longer changes its blending rule. The calculation was
cross-checked against the local wow.export 0.2.19
[ADT shader](https://github.com/Kruithne/wow.export/blob/c2fd7bde36a712be78a5da896c995b84fbfa2545/src/shaders/adt.fragment.shader).
Noggit retains its neutral-height and zero-total fallbacks and legacy alpha path.
The renderer selects this behavior using the actual client version, independently
of the shared `modern_features` setting. Non-Retail clients with that setting
enabled keep the previous four-layer height blend followed by ordinary alpha
blending for additional layers; their additional height maps are not uploaded.
General resource-lifetime and binding-safety fixes remain shared.

Retail textures share array pages only when compression, dimensions and mip
counts match; no resizing or recompression is involved. Pages hold at most 16
slices, bounded by a 16 MiB mip-chain budget unless a single texture is already
larger. Released slices can be reused without deleting live neighbours. Legacy
clients retain single-slice allocations, including after switching away from
a Retail project while old array pages still exist. The terrain draw still supports 13
distinct array bindings per chunk, with three other fragment samplers reserved
for alpha, shadows and the brush. A chunk exceeding that capacity is reported
as unsupported and is not exported with a partial texture mapping.

Explicit paired export renders one texture pair per frame and can be cancelled
between jobs. Both images are first rendered into a private temporary directory
under `.noggit/derived-staging/`. Both destination `QSaveFile`s are prepared
before publication; a failed second commit restores the previous first file.
Pending state is cleared only after both commits succeed. This handles reported
write errors, but is not a crash-atomic transaction across two files.

## Updating project terrain

For the open Retail map, **Tools > Update project LOD + map textures +
heightmap...** runs the same job queue as **Minimap Editor > Generate >
Update LOD + map textures + heightmap** with **Project tiles (edited areas)**.
Save terrain edits first; the confirmation shows the number of tiles. The panel
also offers selected tiles and the whole map.

Project scope includes tiles with ROOT, TEX0, OBJ0 or OBJ1 files in the
project, with or without pending markers. LOD and map texture files do not
select a tile, and neither do OBJ-only neighbour files recorded as UID-repair
dependencies (see below). Scope is derived from project files, not from a
comparison with the client data, so earlier full-map outputs can enlarge it.

The operation reads saved terrain, rebuilds terrain and liquid LOD, generates
both map textures, then updates WDL heights and terrain occlusion for successful
terrain jobs.
An unsuccessful terrain job blocks its dependent texture pair; the log names
the tile and reason. An explicit texture-only export remains independent.
ROOT/TEX0/OBJ0 are not rewritten; OBJ1 water heights may be updated. Progress
and cancellation are shown in the status bar; failed and cancelled work stays
pending. Rendering runs on the render thread.

The log records the start and finish of each terrain export job, including the
tile, LOD/map-texture/WDL stage and elapsed time. These markers identify the last
active stage in an interrupted session without logging every terrain chunk.

During the Retail queue, normal scene rendering is paused. The frame timer
continues advancing jobs, and OpenGL still renders exported textures into an
offscreen framebuffer. The previous widget update behavior is restored on
completion, error or cancellation, and switching tools does not stall the queue.
The paused interval is excluded from the next scene tick and FPS samples.

Generated files reach the game through the project's normal patch workflow.
A successful export does not validate the installed patch.

## Pending results

`<project>/.noggit/derived-maps/<map>.json` stores tile coordinates and flags:
diffuse=1, normal=2, LOD=4, WDL=8. All four are marked before each Retail tile
save, including direct bulk/import saves and object-only changes. Explicit
selected export marks both textures. WDL is cleared only after the WDL file is
written and the referenced terrain occlusion table is updated by the export
queue. A source save alone does not retry pending derived work. Tiles with
unsaved edits or unsuccessful LOD updates stay pending.

Flag 16 is provenance, not pending work: the tile's only project files are
OBJ0/OBJ1 written by a project-scope UID repair of a neighbour. It is recorded
before those files are written and removed on rollback or the next tile save.

State writes use a lock and atomic replacement. A failed state write aborts the
tile save. Corrupt or unknown state is reported and left unchanged. Paired
export and automatic queued generation clear both texture flags only after both files are
written; single-output controls clear only their own flag. A committed LOD
update clears only LOD. Flags persist across restarts and are independent of
the unsaved-ADT flag. External file changes are not detected.

## Terrain LOD

`TerrainLodUpdater` validates chunk ranges, root grid/MCVT data, skirt indices
and finite heights. It scores four orientations against the reference ROOT/LOD
pair and requires 50% agreement and a five-percentage-point margin, unless the
alternatives produce heights within 0.001 world units and identical hole masks.
The equivalent flat case does not establish a unique orientation. Unchanged
equivalent terrain retains its authored MLVH bytes. It updates MLVH and the
MLHD minimum/maximum heights.

All four LOD levels are checked against the saved ROOT hole mask. Triangles
overlapping active holes are removed. Their repair patch expands across
non-cell-aligned edges until it can join the existing mesh on whole-cell
boundaries, using the fixed vertex grid. Only uncovered solid cells receive
replacement triangles at current heights. Thus newly opened and partially
closed holes retain their current masks. Unsupported open boundaries are
rejected. Node edges are subdivided to avoid cracks; MLND and MLLL ranges are
rebuilt. Exposed tile borders get skirts on levels that use them, and skirts
over newly opened border holes are removed. Repeated saves without changes
add no triangles.

Height edits also refine triangles whose vertical interpolation error at the
terrain's authored grid/centre samples exceeds 8, 4, 2 or 1 world units on the
four levels, from coarse to fine. The existing terrain vertex pool is reused;
edge subdivisions propagate across collinear node edges at the same level,
and tile-border skirts are subdivided with their top edges. Each level has at
most 16 refinement passes and a triangle budget of
`min(65536, max(16384, 4 * originalTriangleCount))`. Exceeding a limit leaves
LOD pending. These are sample-error bounds, not a continuous-surface or
game-client visual guarantee.

The terrain export queue checks this error even on already height-updated LOD,
so earlier sparse results can be repaired. Ordinary source saves keep the
existing LOD unchanged. Repeated successful rebuilds
do not continue adding triangles. Hole repair can increase the number of
triangles locally; each level remains limited to 65,536 terrain triangles.

Explicit and queued generation stage LOD with its reference and updated OBJ1
water data. Direct callers that request an immediate rebuild stage it with the
ROOT/TEX0/OBJ parts. The split-ADT writer attempts rollback on commit failure;
these multi-file writes are not crash-atomic.

Liquid LOD (MLLN/MLLI/MLLV meshes, MLLD depth and visibility) is rebuilt from
MH2O, including added, removed, raised and partial layers. Flat water is merged
into rectangles; sloped water keeps its vertex samples. MLHD liquid bounds are
recalculated. The Retail MH2O reader keeps LiquidObject references, resolves
their vertex layout through the client's LiquidMaterial table and uses the full
8×8 rectangle for them. Partial-layer bounds include all four corners of each
active cell.

Instances with explicit vertex formats take their LiquidObject from the
reference ROOT at that cell, then from the nearest matching reference LOD
surface. Newly painted types use a LiquidObject of that type from the client
database. A missing material or object definition leaves LOD pending with a
specific error.

OBJ1 MLDL water heights are updated for flagged doodads whose origins lie in
the tile. References owned by a neighbour keep their value; save the owning
tile after changing water there. MLHD object height bounds are taken from OBJ1.
Placement order, UIDs, dictionaries, batches and LOD memberships are not
changed. Water inside a WMO belongs to that asset and is not converted.

## Limitations

- Missing templates and uncertain orientation leave LOD
  pending; the ADT save can still succeed.
- Closures with a non-cell-aligned mesh boundary or an unsupported quadtree
  layout stay pending instead of approximating the cut.
- Changed legacy MCLQ and PBR MALP/MASD liquid are not rebuilt.
- New tile templates are not generated.
- The heightmap step updates existing WDL MARE/MAHO records of affected tiles and
  preserves other tiles and chunks. MAHO marks only wholly empty terrain chunks;
  partial high-resolution holes do not remove their remaining solid surface.
  A missing MAHO is valid for an empty hole mask; adding a required MAHO or a
  missing MARE remains unsupported and leaves WDL pending.

### Terrain occlusion after sculpting

The heightmap step also reads the `_occ.wdt` satellite referenced by MPHD.
An original occlusion surface can remain above lowered project terrain and
hide visible ground as the camera moves, even when ROOT and LOD are correct.
For selected tiles only, Noggit lowers the existing 17x17 MAOH grid below the
minimum of each adjacent terrain chunk. This includes interior vertices, rounds
down negative heights, and lowers quads containing holes to the signed 16-bit
minimum. Existing lower occluders are preserved. This conservative repair may
reduce culling efficiency locally; it never raises an occluder or changes visible
terrain. Noggit's scene preview does not use this client occlusion surface.

MAOI, other tiles and unknown chunks remain byte-identical. ROOT WDT references
and MAID are preserved. A zero MPHD occlusion FileDataID or an absent tile entry
needs no new occluder; malformed or unsupported referenced data leaves the
heightmap step pending. The supported Retail MAOH record is 578 bytes (17x17),
not the WDL MARE's 1090 bytes. The occlusion table is committed before WDL, each
with atomic file replacement; a failed write leaves the step pending for retry.
The two files are not a crash-atomic transaction. Include the generated
`world/maps/<map>/<map>_occ.wdt` in the normal client patch workflow.

The MLLD writer targets the Retail 12.1.5 layout: 12-byte header, big-endian
RLE control words, 128×128 depth bytes and 2,048 visibility bytes. See the
[ADT format reference](https://wowdev.wiki/ADT/v18#MLLD_.28Legion.2B.29) and the
[TrinityCore MH2O reader](https://github.com/TrinityCore/TrinityCore/blob/master/src/tools/map_extractor/adt.h).

### Original ROOT/LOD references

Layout detection needs the ROOT that corresponds to the retained LOD, before
terrain edits. Pairing a modified ROOT with an old LOD can reject valid layouts
or conceal topology changes. The current project ROOT is not automatically
promoted to an original reference.

`.noggit/lod-reference/` retains inspected ROOT/LOD pairs in the same staged save
as the output. Per-tile JSON records the logical ROOT path, SHA-256 of both
files, source and candidate layout mask. Only a unique orientation is recorded
as such. Older pairs without JSON are inspected before use; incomplete pairs,
hash mismatches and insufficient layout confidence are rejected.

For imported projects, **Tools > Choose LOD reference folder...** selects the
directory directly containing the original `<map>_<x>_<y>.adt` and
`<map>_<x>_<y>_lod.adt`. The path is private project metadata in
`.noggit/lod-reference/<map>.json`, not an application-wide setting. Source files
are read only. Client archive pairs are another fallback, read without project
overrides. An ordinary modified map does not require a separate original folder
when the archive pair is compatible.

Each candidate must pass ROOT/LOD inspection. If its LOD differs from the
current file, calibration additionally requires the same vertex count and exact
MLVI/MLND/MLLL/MLSI indexing, version and opaque header fields. Only the six
derived vertical bounds may differ in MLHD. A temporary copy of the current LOD
receives reference ROOT heights to establish orientation; the updater then
applies current project heights, holes and liquids. Current unknown chunks and
liquid data are not replaced by the archive file. A structural mismatch is
rejected rather than substituted. A failed calibrated update retains neither
the temporary heights nor an invalid reference pair.

A failed guess is never saved as a baseline. A verified baseline is retained
when later topology or liquid validation rejects an ordinary ADT save; explicit
rebuild failures write no LOD/reference/OBJ1 files. A successful update refreshes
the pair. Include this directory in project backups.

## Regression checks

Standalone tests build without the full editor dependency tree:

```text
cmake -S tests -B build/derived-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/derived-tests --config Release
ctest --test-dir build/derived-tests -C Release --output-on-failure
```

On Windows, use a Visual Studio developer shell. Qt5 Core/Widgets must be found
through `CMAKE_PREFIX_PATH` to build the state and UI tests.

LOD tests cover orientations, skirts, bounds, preserved chunks, malformed input,
ambiguity, hole closure on all four levels, partial closures, node/tile
boundaries, skirt extension, idempotence and malformed liquid. Liquid tests cover
vertex layouts 0–5, LiquidObject rectangles, partial masks, sloped and stacked
surfaces, deletion, bounds, RLE boundaries, object-water heights and
preservation of unrelated object data. `retail_liquid_lod` also validates a LOD
file passed on the command line. State tests cover independent
flags/maps/tiles, persistence, locking, malformed JSON, write failures, project
tile selection and texture-pair state. UID tests cover unchanged unrelated
bytes, reserved-ID collisions, idempotence and minimal neighbour OBJ reference
updates. An optional read-only fixture comparison takes reference-map,
modified-map and expected-LOD directories; the default tests need no map
fixtures.

Reference fixtures additionally check retained metadata/hash failures, incomplete
pairs, archive versus project selection and explicit configuration recovery.
They exercise the real update/catch path on a Windows thread with a reserved
256 KiB stack, including successful updates, rejected updates and calibration.
Height grids and liquid-cell lookup tables are heap allocated; failure handling
reuses the verified inspection instead of recursively inspecting its baseline.
Refinement checks exercise moved terrain, shared node edges and skirts, bounded
error and budgets, forced repair of coherent but sparse LOD, idempotence and the
16-bit vertex limit. Local real-map checks must use a separate output directory;
they are not a reason to commit game assets into these tests.

Rendering, saved texture appearance, save/reopen behavior and in-game distance
transitions must be checked with the matching Retail client.

Retail export restores the editor framebuffer and viewport after each job.
Failed jobs are logged with tile coordinates.
The extracted queue fixture covers scene pause/restoration, tool switching,
completion, cancellation and failures. It uses a recording renderer and does
not prove real GPU stability or saved texture appearance.
