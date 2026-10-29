#include <noggit/rendering/MapPreviewQuality.hpp>
#include <noggit/rendering/SurfaceDecalPreview.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

using Noggit::Rendering::MapPreviewQuality;
using Noggit::Rendering::ModelPreviewClock;

namespace
{
  void require(bool ok, char const* message)
  {
    if (!ok) throw std::runtime_error(message);
  }

  void requireDistances(MapPreviewQuality const& quality, float scene, float ground)
  {
    require(quality.modelDistance == scene && quality.doodadDistance == scene
        && quality.detailDistance == ground && quality.particleDistance == scene,
        "Quality must preserve the artist's placement and ground-effect visibility distances");
  }

  void requireFullQuality(MapPreviewQuality const& quality)
  {
    require(quality.skinLod == 0 && quality.textureLodBias == 0.f
        && quality.animationFps == 0 && quality.particles,
        "Exports and other clients must retain full rendering regardless of Retail preferences");
  }
}

int main()
{
  for (int preset : {-1, 0, 1, 2, 3, 4, 999})
  {
    for (int override : {-1, 0, 1, 2, 3, 4})
    {
      for (auto flags : {std::pair{false, false}, std::pair{false, true}, std::pair{true, true}})
      {
        auto quality = MapPreviewQuality::resolve(flags.first, flags.second, preset, 2000, 300, override);
        requireDistances(quality, 2000, 300);
        requireFullQuality(quality);
      }
      for (auto distances : {std::pair{2000.f, 300.f}, std::pair{50.f, 25.f}, std::pair{0.f, 0.f}})
        requireDistances(MapPreviewQuality::resolve(true, false, preset, distances.first, distances.second, override),
                         distances.first, distances.second);
    }
  }

  auto low = MapPreviewQuality::resolve(true, false, 0, 2000, 300);
  auto medium = MapPreviewQuality::resolve(true, false, 1, 2000, 300);
  auto high = MapPreviewQuality::resolve(true, false, 2, 2000, 300);
  auto ultra = MapPreviewQuality::resolve(true, false, 3, 2000, 300);
  require(low.skinLod == 2 && medium.skinLod == 1 && high.skinLod == 0 && ultra.skinLod == 0,
          "Placement and review presets must request the intended authored mesh detail");
  require(low.textureLodBias > medium.textureLodBias && medium.textureLodBias > high.textureLodBias
      && high.textureLodBias == ultra.textureLodBias && ultra.textureLodBias == 0,
          "Texture detail must improve without altering the source image");
  require(low.animationFps == 15 && medium.animationFps == 30 && high.animationFps == 60 && ultra.animationFps == 0,
          "Animation sampling follows the selected CPU budget");
  require(!low.particles && medium.particles && high.particles && ultra.particles,
          "Low quality disables effects, never model placements");
  for (int preset : {-1, 4, 999})
    requireFullQuality(MapPreviewQuality::resolve(true, false, preset, 2000, 300));
  require(MapPreviewQuality::resolve(true, false, 0, 2000, 300, 1).skinLod == 0,
          "Artists can inspect primary geometry while retaining the low animation budget");
  require(MapPreviewQuality::resolve(true, false, 3, 2000, 300, 3).skinLod == 2,
          "Artists can inspect lowest authored geometry independently of texture quality");

  ModelPreviewClock clock;
  require(clock.update(1000, 0, true, 0) == 0, "Newly loaded models need a defined first pose");
  require(clock.update(1123, 0, true, 0) == 123, "Running animation advances with elapsed time");
  require(clock.update(1200, 1, true, 0) == 123, "Pause must retain the displayed pose");
  require(clock.update(9000, 1, true, 0) == 123, "Paused animations must not drift");
  require(clock.update(10000, 0, true, 0) == 123, "Resume must not jump across the paused interval");
  require(clock.update(10010, 0, true, 0) == 133, "Resumed animation advances normally");
  require(clock.update(11000, 0, false, 0) == 133, "Existing animation toggle also pauses the model clock");
  require(clock.update(20000, 0, true, 0) == 133, "Re-enabling animation must not cause a catch-up burst");
  require(clock.update(20010, 0, true, 0) == 143, "Animation can continue after the existing toggle");
  require(clock.update(21000, 2, true, 0) == 0, "Off returns a stable initial pose, unlike Pause");
  require(clock.update(30000, 2, true, 0) == 0, "Off must remain stable");
  require(clock.update(31000, 0, true, 0) == 0, "Restart after Off begins without a jump");
  require(clock.update(31050, 0, true, 0) == 50, "Restart after Off advances from the initial pose");
  require(clock.update(100, 0, true, 0) == 50, "A reset of the source clock must never reverse a model pose");
  require(clock.update(std::numeric_limits<double>::quiet_NaN(), 0, true, 0) == 50,
          "Invalid source times must not poison model animation state");
  require(clock.update(150, 0, true, 0) == 100, "Clock recovers after invalid source time");

  ModelPreviewClock sampled;
  sampled.update(0, 0, true, 15);
  require(sampled.update(50, 0, true, 15) == 0, "Low quality holds poses between sampling boundaries");
  require(sampled.update(75, 0, true, 15) == 66, "Low quality samples a new pose at its next boundary");
  require(sampled.update(80, 0, true, 0) == 80, "Switching to Ultra retains elapsed animation time");
  ModelPreviewClock pausedQualitySwitch;
  pausedQualitySwitch.update(0, 0, true, 15);
  require(pausedQualitySwitch.update(75, 0, true, 15) == 66, "Prepare a sampled low-quality pose");
  require(pausedQualitySwitch.update(80, 1, true, 0) == 66,
          "Pausing and changing quality must retain the displayed pose, not resample it");
  require(pausedQualitySwitch.update(1000, 1, true, 60) == 66,
          "Changing quality during a pause must not move the model");
  require(pausedQualitySwitch.update(2000, 0, true, 0) == 66,
          "The first resumed frame retains the held pose even after a quality change");
  ModelPreviewClock initiallyPaused;
  require(initiallyPaused.update(9000, 1, true, 0) == 0, "Opening a paused view still has a defined first pose");

  using Noggit::Rendering::hasDecalBasename;
  using Noggit::Rendering::isSurfaceDecal;
  using Noggit::Rendering::surfaceDecalMode;
  require(hasDecalBasename("world/detail/Stone_Decal_01.m2")
      && hasDecalBasename("WORLD\\DETAIL\\STONE_DECAL_01.M2")
      && hasDecalBasename("dirtdecal.blp"),
          "Explicit decal asset basenames are recognized independent of path separator or case");
  require(!hasDecalBasename("world/decal/tree.m2")
      && !hasDecalBasename("world/detail/stone_nodecal.m2")
      && !hasDecalBasename("WORLD\\DETAIL\\STONE_NODECAL.M2")
      && !hasDecalBasename("world/decal/")
      && !hasDecalBasename("filedataid:123456")
      && !hasDecalBasename(""),
          "Directory names, explicit nodecal variants and unresolved IDs do not identify decal meshes");
  for (unsigned flags : {0u, 1u, 2u, 0x10u, 0x40u, 0x80u})
    require(!isSurfaceDecal(static_cast<std::uint8_t>(flags), "world/tree.m2", 1, "leaf_alpha.blp"),
            "Ordinary transparent materials and unrelated flags must never hide as decals");
  require(isSurfaceDecal(0x14, "world/footprints.m2", 1, "footprints.blp")
      && isSurfaceDecal(0x20, "filedataid:123456", 1, ""),
          "Projected batch flags identify decals even when no basename is available");
  require(isSurfaceDecal(0x80, "world/ground_cracks_decal.m2", 2, ""),
          "Explicit decal models may contain multiple texture stages");
  require(isSurfaceDecal(0, "world/plane.m2", 1, "world/cracks_decal.blp")
      && !isSurfaceDecal(0, "world/plane.m2", 2, "world/cracks_decal.blp"),
          "Texture-name classification must never hide a combined base/decal material");
  require(!isSurfaceDecal(0x14, "world/ground.m2", 1, "cracks_decal.blp", true)
      && !isSurfaceDecal(0, "world/ground.m2", 2, "cracks_decal.blp", true)
      && isSurfaceDecal(0, "world/cracks_decal.m2", 2, "", true),
          "Layered base geometry remains visible unless the whole model is explicitly a decal");
  for (int setting : {-1, 0, 1, 2, 3, 99})
  {
    require(surfaceDecalMode(false, false, setting) == 0
        && surfaceDecalMode(false, true, setting) == 0
        && surfaceDecalMode(true, true, setting) == 0,
            "Retail decal preferences must not affect other clients or exported images");
    require(surfaceDecalMode(true, false, setting) == (setting >= 0 && setting <= 2 ? setting : 0),
            "Invalid decal preferences must retain full display");
  }
  return 0;
}
