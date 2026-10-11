# Texture-only OBJ1 save preservation

The Retail 12.1.0.69933 test on map 3075 failed to save tiles 34_34,
35_34, 37_34, 35_35 and 36_36 with "OBJ1 LOD placement counts do not
match MLFD". Some other tiles were saved successfully.

The save chain now compares the serialized OBJ0 M2 and WMO placements with
the source placements, ignoring serialization order but comparing all record
bytes. If they are unchanged, it preserves the complete source OBJ1 without
reinterpreting its LOD layout. Changed placements still enter the existing
validated rewrite path; unsupported layouts are not silently accepted.

Verification:

- Full Noggit target build succeeded.
- The original CASC OBJ0/OBJ1 pair for 3075_35_34 reproduces the old failure.
- The same real fixture preserves OBJ1 byte-for-byte with unchanged or reordered
  placements, and rejects a moved M2 in the unsupported layout.
- All eight existing OBJ1 fixtures pass their four editing scenarios.
- Malformed MLDL rejection and staged-save rollback tests still pass.
- The separate noggit-mtxp-test.exe has been updated; user map files were not edited.

Still required: in-app painting, saving and reopening the affected tile with
the new build. The previous session logged Modern Features Disabled, so it
does not establish that the MTXP height-rendering path was enabled.

## Follow-up after the first retry

The real editor retry still failed: its serialization differed from a raw
OBJ0 copy. The 3075_35_34 source has 482 MDDF flags of 0x40 and 110 of 0x240;
the Retail conversion incorrectly cleared 0x40 even for existing placements.
Existing M2 flags now remain exact. A fixture test runs the production
modern_placements conversion with a deterministic FileDataID resolver and
verifies all 592 source records byte-for-byte.

WMO comparisons now exclude derived bounds while retaining identity, transform,
scale, flags and set checks. When placements are unchanged, the complete
original OBJ0 (including bounds and references) and OBJ1 are kept together.
Tests cover altered derived bounds and rejection of actual WMO scale changes.
Modern WMO scale loading is independent of the optional rendering toggle.

The user reports Modern Features is enabled for the new retry; the earlier
disabled setting is not evidence about this session. The new build is ready
at C:/NoggitBuild/Shadow-Noggit/bin/Release/noggit.exe. Final in-app save/reopen
verification is still pending; no user map files were modified by this fix.

## Rotation normalization follow-up

The next real retry ran the updated code but still marked five tiles as having
changed placements. ModelInstance and WMOInstance normalize every input angle
to [-180, 180); the byte comparison treated equivalent angles as changes.
The real 3075_35_34 fixture contains 292 angles changed by this normalization.
An editor-normalization regression reproduced the same MLFD failure before
the fix and now passes. Placement comparison normalizes both rotations while
preserving all original OBJ0/OBJ1 bytes for equivalent transforms. A real
one-degree M2 or WMO edit still compares as changed. Existing movement, scale,
malformed LOD and eight-fixture editing tests remain passing.

A separate noggit-savefix.exe build avoids replacing or closing the user's
currently running Noggit with unsaved paint. In-app save/reopen remains pending.

## 37_34 rotation boundary

The recovered build's real retry failed for 3075_37_34. A fresh read-only CASC
capture reproduced the failure in the production-code regression: UID 70138693
has a rotation just below 180 degrees. Float rounding in the editor's angle
normalization crosses the boundary again when applied a second time. Comparing
two normalized values therefore incorrectly classified the loaded placement
as an edit. Comparison now also accepts the exact one-step editor result,
without a broad tolerance or changing original placement bytes.

The real 37_34 and 35_34 fixtures now preserve OBJ1 after normalization.
Boundary regression cases pass, while actual 0.01-degree rotation edits still
compare as changed. Eight OBJ1 fixtures and malformed LOD rejection also pass.
No project map files were manually saved or patched. The project author
confirmed the in-app painting/save retry succeeds with the boundary fix.
