# Retail model loading and preview

The asset-format and material features below apply to Retail projects and
target the Retail 12.1.5.70448 format. wow.export is not required at runtime.
WotLK, Shadowlands and Forever keep their material selection, animation and
preview behavior. Additional WMO UV, weight and blend-alpha streams are
allocated and uploaded only for Retail; FileDataID support alone does not
enable them.

Shared resource-lifetime changes apply to all clients: cached VAO attributes
and index bindings are preserved, the texture-buffer binding query is correct,
M2 resource keys survive context reloads, and deleted texture-array entries are
cleared. They do not select Retail shaders.

## Asynchronous texture loading

WMO group upload checks only the texture slots referenced by its materials.
Groups whose textures are still decoding keep their CPU geometry and retry on a
later frame through the existing asynchronous loader. Failed loads stay failed
until a context reset. The GPU batch table is built only after the referenced
textures are ready. OpenGL work stays on the render thread.

Material slot counts and extended batch material indices use shared helpers in
loading and rendering. Batch construction uses fixed-size scratch storage and
splits draws when blend modes differ; incompatible materials are not merged.

## Materials and textures

Retail WMO materials support up to nine texture slots. Shader 23 uses four MOTV
sets and MOC2 weights for height-based layer blending. MOCV lighting and
material blend alpha are kept separate, including groups with a blend-only
color stream. Missing environment textures are handled explicitly. Supported
compositions: 7, 8, 9, 11, 12, 13, 15, 17, 18, 19, 20 and 23.

Retail M2 keeps authored batches instead of applying the legacy layer reducer.
Supported high-bit shader IDs are 0-17, 21, 23, 24, 26-28, 30 and 32-34, with up
to three texture stages, per-stage UV animation, emission and edge fade where
implemented. Unsupported high-bit IDs are reported. Unlit materials bypass
world lighting. Retail BLP2 encoding 3 is decoded as BGRA with validated mip
ranges; palette and DXT decoding are unchanged.

## Asset references and geometry

WMO groups use the root GFID table when present. The first group-count entries
are primary geometry. Lower-detail groups do not replace the geometry used for
picking, placement bounds or tile references. Roots without GFID use named
groups. Malformed tables and missing primary groups produce loading errors.

Retail SKIN triangle starts include the high bits in the section level.
Render-pass offsets are 32-bit. Index ranges, lookup ranges and resolved model
vertex references are checked independently.

## Skeletons and animation

Retail M2 keeps its FileDataID and uses SFID skins. SKID opens the external
SKEL, following bounded SKPD parent references. Parent bones define the
hierarchy; child animation sequences override matching IDs/sub-IDs.
Per-sequence interpolation and global-loop parameters are kept. Existing M2
global-loop indices and sequence order remain valid for non-bone tracks.

AFID references resolve raw animation data or chunked AFM2/AFSB payloads. Bone
payloads are loaded on demand while preparing nonempty tracks and shared across
aliases and repeated ranges. Loading a model prepares all its referenced
animations; there is no playback-time streaming. Cycles, invalid offsets and
incompatible bone hierarchies produce loading errors.

SKA1/AFSA attachment data is parsed and validated but not rendered; attachment
and equipment animation are not supported. Source geometry and collision
bounds are preserved.

## Map preview quality

Choose a quality preset in **Settings > Retail preview** and save settings, or
use the map's **View > Model preview** menu. **F11** pauses or resumes model
animation in Retail. Ultra is the default. Model quality does not change the
scene's model, WMO doodad and ground-effect view distances.

| Preset | M2 mesh | Texture sampling | Animation sampling | Particles/ribbons |
| --- | --- | --- | --- | --- |
| Low | Lowest available authored LOD | Two mip levels coarser | 15 updates/s | Hidden |
| Medium | First available authored LOD | One mip level coarser | 30 updates/s | Visible |
| High | Primary geometry | Normal | 60 updates/s | Visible |
| Ultra | Primary geometry | Normal | Every rendered frame | Visible |

The animation sampling limit is independent of the viewport frame rate.
Camera-facing billboards still update their orientation when rendered. Savings
depend on the assets: models without usable authored LODs keep their primary
mesh. WMO geometry is always primary; M2 doodads inside WMOs use the model
setting. Texture sampling applies to both M2 and WMO materials.

The mesh selector can override the preset to show the primary mesh, first
authored LOD or lowest authored LOD. Retail SFID entries after `nViews` are the
geometric LOD skins; the initial views are not treated as lower-detail meshes.
Optional skins are validated independently and fall back to primary geometry
when unavailable or incompatible. Primary indices and render passes remain
available for picking and editing tools.

Animation playback has **Running**, **Paused** and **Off** modes. Paused holds
the current pose and existing particles/ribbons; resuming does not skip ahead.
Off returns models to their initial pose and hides particles/ribbons. Models
loaded while paused receive a pose at the held animation time. Water, sky and
terrain clocks are not affected.

**Surface decals (identified)** has **Full**, **Reduced texture detail** and
**Hidden** modes. Reduced uses coarser texture sampling for recognized M2 decal
passes; Hidden omits those passes from the viewport. Recognition uses projected
batch flags or explicit decal asset basenames, including names resolved from
FileDataIDs. A decal texture name identifies only a single-texture pass.
Basename checks ignore directory names and `nodecal` variants; alpha blending
alone does not identify a decal. Materials combining base geometry with extra
layers are kept unless the whole model is named as a decal. Unrecognized decals
stay visible, and decals baked into terrain textures or WMO materials are not
controlled. Exports keep decals; hidden decal placements remain saved and
selectable.

These are viewport settings. They do not change source geometry, UID repair,
placement bounds, tile references or saved ADTs. Minimap and map texture
exports ignore the presets. Texture mip bias does not free allocated textures,
and preview quality does not reduce the CPU memory needed for full asset
loading and validation.

## Limitations and validation

WMO modes 10/14 and parallax mode 22 use diffuse fallbacks. Exact specular,
material clamp flags, global transparent-object sorting and shader 23 ambient
occlusion are not fully reproduced. Terrain projection is a preview
approximation. Particle and ribbon rendering uses the existing implementation,
not modern effects.

The [standalone regressions](../tests/README.md) cover delayed texture
readiness, context restoration, unused and failed texture slots, material
compositions, vertex-color roles, high-bit M2 stages, skeleton/animation ranges
and preview isolation. WMO lifecycle checks also cover WotLK, Shadowlands and
Forever. OpenGL checks use production GLSL and inspect pixels or buffer state.
They do not establish full rendering or performance parity with the client.

For visual checks, compare a cold load and a warm redraw of layered buildings,
foliage, ships and alpha-key materials, including after context reload,
copy/paste and movement. Check the other clients separately and compare frame
times from the same camera position. Minimap and map texture checks must use
the export path, which ignores viewport quality settings.

Map serialization and UID handling are documented in
[derived map results](derived-map-results.md) and [UID repair](uid-repair.md).
Model and texture asset files are not rewritten.

## Reference and attribution

Format and material handling use wow.export as a source reference. Adapted parts
retain its [MIT license notice](third-party/wow.export-MIT.txt).
