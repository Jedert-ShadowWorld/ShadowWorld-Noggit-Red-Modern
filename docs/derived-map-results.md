# Retail derived map results

For Retail projects, Noggit regenerates derived map data after ADT saves:
diffuse and normal map textures, terrain LOD heights and holes, liquid LOD and
WDL heights. Rendering uses Noggit's terrain renderer; files are written by the
ADT writer. Other clients keep their existing behavior.

## Saving a map

**Save current tile**, **Save changed tiles** and **Save all tiles** save the
ADTs, then generate both map textures for the saved tiles only. The map is
reported as saved, and the undo history cleared, after generation finishes. A
texture failure reports an incomplete save and keeps the tiles pending; the
ADTs may already be written. Tiles with unsaved changes must be saved before
their textures are generated.

**Save all tiles** covers loaded tiles. A regular save does not process pending
tiles outside the saved set; use the Minimap Editor controls for those.

Bulk/import code that writes a tile directly marks its results as pending
without rendering. Saving the tile again or an explicit export processes them.

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

Explicit paired export renders one texture pair per frame and can be cancelled
between jobs. Each file is written with `QSaveFile`; pending state is cleared
only after both files of a pair succeed. A failed pair can leave one new and
one old file; both stay pending.

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
both map textures, then updates WDL heights for successful terrain jobs.
ROOT/TEX0/OBJ0 are not rewritten; OBJ1 water heights may be updated. Progress
and cancellation are shown in the status bar; failed and cancelled work stays
pending. Rendering runs on the render thread.

Generated files reach the game through the project's normal patch workflow.
A successful export does not validate the installed patch.

## Pending results

`<project>/.noggit/derived-maps/<map>.json` stores tile coordinates and flags:
diffuse=1, normal=2, LOD=4, WDL=8. All four are marked before each Retail tile
save, including direct bulk/import saves and object-only changes. Explicit
selected export marks both textures. WDL is cleared only after the WDL file is
written; the next save writes WDL heights for every tile still pending,
reloading them from the saved ADTs. Tiles with unsaved edits stay pending.

Flag 16 is provenance, not pending work: the tile's only project files are
OBJ0/OBJ1 written by a project-scope UID repair of a neighbour. It is recorded
before those files are written and removed on rollback or the next tile save.

State writes use a lock and atomic replacement. A failed state write aborts the
tile save. Corrupt or unknown state is reported and left unchanged. Paired
export and automatic save clear both texture flags only after both files are
written; single-output controls clear only their own flag. A committed LOD
update clears only LOD. Flags persist across restarts and are independent of
the unsaved-ADT flag. External file changes are not detected.

## Terrain LOD

`TerrainLodUpdater` validates chunk ranges, root grid/MCVT data, skirt indices
and finite heights. It scores four orientations against the reference ROOT/LOD
pair and requires 50% agreement and a five-percentage-point margin, unless the
alternatives produce identical heights and hole masks. It updates MLVH and the
MLHD minimum/maximum heights.

All four LOD levels are checked against the saved ROOT hole mask. LOD openings
that are solid in ROOT receive terrain triangles at current heights; partially
closed openings keep their remaining open cells. Existing triangles are kept
except where hole boundaries and adjacent node edges are subdivided to avoid
cracks. MLND and MLLL ranges are rebuilt, and newly exposed tile borders get
skirts on levels that use them. Repeated saves without changes add no
triangles.

Updated LOD is staged with the ROOT/TEX0/OBJ parts. The split-ADT writer
attempts rollback on commit failure; the tile save is not crash-atomic.

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

- Missing templates, uncertain orientation and newly opened holes leave LOD
  pending; the ADT save can still succeed.
- Closures with a non-cell-aligned mesh boundary or an unsupported quadtree
  layout stay pending instead of approximating the cut.
- Changed legacy MCLQ and PBR MALP/MASD liquid are not rebuilt.
- New tile templates and newly cut holes are not generated.
- The heightmap step updates existing WDL MARE records of the affected tiles and
  preserves other tiles and chunks. Missing or unsupported WDL templates leave
  the operation incomplete.

The MLLD writer targets the Retail 12.1.5 layout: 12-byte header, big-endian
RLE control words, 128×128 depth bytes and 2,048 visibility bytes. See the
[ADT format reference](https://wowdev.wiki/ADT/v18#MLLD_.28Legion.2B.29) and the
[TrinityCore MH2O reader](https://github.com/TrinityCore/TrinityCore/blob/master/src/tools/map_extractor/adt.h).

After an unsupported update, `.noggit/lod-reference/` keeps the last consistent
ROOT/LOD reference pair in the same staged save. Later saves use that pair, so
unsupported edits are not marked as updated. A successful update refreshes the
pair. Missing or inconsistent references leave LOD pending; include this
directory in project backups.

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

Rendering, saved texture appearance, save/reopen behavior and in-game distance
transitions must be checked with the matching Retail client.

Retail export restores the editor framebuffer and viewport after each job.
Failed jobs are logged with tile coordinates.
