# Retail UID repair

In a Retail project, choose **Review and fix UIDs...**. Noggit prepares the repair
in memory and opens a searchable table of asset/FileDataID, source tile, old UID
and problem. No map files or UID counters change before confirmation. Cancel,
Escape and closing the window return to map selection without applying the plan.
Other project targets keep their existing UID repair workflow.

The Retail dialog defaults to **Project tiles only (edited areas)**. This scope
includes tiles with ROOT, TEX0, OBJ0 or OBJ1 overrides under the current project's
`world/maps/<map>/` directory, including edits from previous sessions. LOD files
and maptextures alone do not select a tile. Existing valid UIDs are retained;
collisions receive unused IDs above the map's maximum. Client-only object files
are read to reserve their UIDs and check shared references.
Unrelated client-only model assets are not loaded or proposed for removal.

If a selected placement crosses a tile boundary, only the necessary neighbour
OBJ0/OBJ1 files are added or updated. Their unrelated placements, instance data
and chunk membership are preserved; their model assets are not loaded. This does
not recursively select the neighbour's objects or write ROOT/TEX files. Changed
neighbour files are listed in the review and backup report before applying the
repair. Neighbour OBJ files that did not exist in the project before are recorded
as dependencies in `.noggit/derived-maps/<map>.json`, so a later project-only
repair or terrain update does not treat those neighbours as edited tiles.
Missing required map data still blocks preflight.
Whole-map mode renumbers and redistributes all placements.
For an unedited client map, use **Get max UID**.

Scope comes from existing project files, not from a comparison with a pristine
client. Files produced by an earlier whole-map repair therefore count as project
tiles too and are not deleted automatically. To narrow the scope, restore the
intended edit set from a backup.

## Reviewing and applying repairs

**Remove listed placements and repair** removes placements whose asset cannot
be read, whose bounds/transform are invalid, or whose bounds do not intersect
any available terrain tile. Orphan OBJ1 placements are listed too. Valid
placements receive consistent UIDs, transformed bounds, tile coverage and MCNK
references. Missing terrain, required OBJ0 files or unsupported structures block
confirmation.

M2 bounds come from checked MD20/MD21 headers; WMO bounds combine MOHD and MOGI
boxes. This validates root model metadata, not every mesh, texture, skin or WMO
group dependency. Placement flags and instance options are retained. Distinct
old UIDs remain distinct even for co-located instances; shared copies of one
placement receive the same UID.

OBJ0 uses FileDataIDs and rebuilt MCRD/MCRW references. Full-map repair collects
instance data from every source tile before redistributing placements:

- MLMB, MLDB and MLDL values travel with their instance, including when the
  destination previously omitted these tables.
- WMO identities compare the actual selected MWDS sets. MWDR/MWDS dictionaries
  are rebuilt and shared selectors deduplicated separately in OBJ0 and OBJ1.
- MDLI references are resolved to their opaque 32-byte MDLD definitions.
  The destination stores those definitions and assigns new local indices;
  missing definitions retain the `0xffffffff` sentinel.
- OBJ1 retains its own loading flags and membership in each of the three MLFD
  levels. Bounds and radius ordering are rebuilt within membership groups.
  Existing destination copies retain their local metadata. New copies use
  the matching source instance.
- Primary WMOs that have no LOD placement anywhere remain primary-only.

When optional byte tables become necessary, absent M2/WMO values use 64/128,
respectively, matching shared instances in Retail tiles with and without
MLDB/MLMB. Existing values are preserved without interpretation.

Ordinary saves after repair retain mixed LOD memberships and instance data when
editing existing placements. Retail WMOs retain the selected doodad-set values
and their source OBJ0/OBJ1 metadata in the editor. Adjacent tiles can use different
MWDR indices for the same placement; ordinary saves rebuild each tile's local
dictionary from those values. They preserve destination metadata for existing
placements and carry source metadata when a WMO is moved or copied to another
tile. Clipboard paste and add/remove undo retain that provenance. Rendering
combines the selected MODS sets with the default set without changing the saved
selection. An explicit single-set edit updates OBJ0 and OBJ1 consistently.
When placements and semantic selections are unchanged, both object files retain
their original placement order, bounds, angles and chunk references. A different
in-memory order or an equivalent tile-local selector does not trigger a rewrite.

A newly placed WMO without authored object LOD metadata remains primary-only.
Unavailable or invalid source LOD metadata blocks a transfer that would otherwise
lose it. New M2 placements that need unavailable per-instance metadata remain
guarded, as do WMO transfers with authored LOD into a tile without an OBJ1 template.
Full-map repair has access to the complete source set for redistribution.
Counts, MDLI indices and MLFD/MWDR ranges are checked before writing, including
saves with unchanged objects.

Tables whose sizes do not match their placements must be restored from the
original object files; resizing them cannot restore their association with instances.
Other unknown chunks are retained. CASC files and root WDT are not written,
and absent terrain is not synthesized.

## Backups and failure handling

After confirmation, changed original ADTs are copied to
`uid-repair/<UTC timestamp>-<id>/world/maps/...` inside the project. `report.txt`
lists removals, source tiles, old UIDs, reasons and backup origins. Original ADTs
preserve deleted placement records and transforms for recovery. Backups and the
report must be written before the first map override is replaced.
`uid-mapping.csv` records old and new UIDs for the surviving placements; one old
UID can map to several new UIDs when it was reused by distinct placements.

Examined object files and model asset states are checked again after review.
Changes abort the operation. Each output uses `QSaveFile`; a later failure
triggers restoration of previously written files, or removal of new overrides.
Restoration failures and report paths are logged. This is not a crash-atomic
transaction across every ADT and the optional UID database. Do not edit the same
project concurrently.

Preflight failures appear in the review window with confirmation disabled.
Review runs outside `initializeGL`/`paintGL`; rendering waits until it finishes.
Failure stops initialization instead of retrying on each paint.

## Loading errors after repair

Whole-map repair covers terrain tiles declared by the map WDT, including
archive-only tiles; project-only repair limits writes as described above.
A successful repair does not establish that every dependent
skin, WMO group or texture can be rendered.

The Retail save warning lists active failed objects and loader exceptions in
**Details**. Cancel is the default; **Save anyway** explicitly continues despite
those failures. A load failure alone does not establish a collision defect.
Saving does not clear unresolved load failures. Failure entries expire when the
associated loaded objects are destroyed.

Retail loader failures are appended and flushed to
`logs/load-errors-<UTC timestamp>-<pid>.jsonl` in the application working directory.
The report includes the asset, reason, timestamp and whether the object is
required for saving. Report write failures are logged; the in-memory Details
list remains available. Startup retains the preceding `log.txt` in memory until
the first project selection. If that project is Retail, the snapshot is saved to
`logs/session-*.log`; archival failure appends it to the current log. Selecting
another project type first releases the snapshot without archiving it, so a
later switch to Retail cannot archive that discarded session. Other project
targets keep their existing logging behavior. Archives are not automatically
deleted.

Retail ground-effect models use the FileDataID stored in GroundEffectDoodad;
legacy filename records still resolve under `world/nodxt/detail/`. The same
resolution is used by terrain detail doodads and their preview. Retail M2 models
retain the numeric asset key and use their SFID reference for the first skin
when present, without requiring a filename in the listfile.

Retail WMO groups containing only MOPL planes are accepted when the group flags,
zero render-batch counts and complete plane payload identify that layout.
Missing mesh chunks in other groups remain loading errors. These loader cases
do not require deleting placements or running Fix UID again.

## Regression checks

The standalone suite in `tests/` covers placement parsing and identity, geometry,
missing-asset proposals, cross-tile dictionaries, co-located instances with
different UIDs, mixed/overlapping LOD ranges, subsequent saves, review
acceptance/cancellation/filtering, failure tracking and log preservation:

```text
cmake -S tests -B build/derived-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/derived-tests --config Release
ctest --test-dir build/derived-tests -C Release --output-on-failure
```

Qt5 Core/Widgets are required for the state and UI checks. See the
[standalone test instructions](../tests/README.md) for platform requirements.
These checks do not cover rendering. Review/cancel, repair, save/reopen, model
edits across tile boundaries and in-game culling/collision require testing with
the matching Retail client.
