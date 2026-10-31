// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace Noggit::Rendering
{
  // Viewport-only policy. Never use this to load placements, compute bounds or save assets.
  struct MapPreviewQuality
  {
    float modelDistance;
    float doodadDistance;
    float detailDistance;
    float particleDistance;
    int skinLod = 0; // 0: primary, 1: first authored LOD, 2: lowest authored LOD
    float textureLodBias = 0.f;
    int animationFps = 0;
    bool particles = true;

    static MapPreviewQuality resolve(bool retail, bool exporting, int preset,
                                     float sceneDistance, float grassDistance, int lodOverride = 0)
    {
      MapPreviewQuality result{sceneDistance, sceneDistance, grassDistance, sceneDistance};
      if (!retail || exporting)
        return result;
      switch (preset)
      {
        case 0: result.skinLod = 2; result.textureLodBias = 2.f; result.animationFps = 15; result.particles = false; break;
        case 1: result.skinLod = 1; result.textureLodBias = 1.f; result.animationFps = 30; break;
        case 2: result.animationFps = 60; break;
        default: break; // Ultra also handles invalid or missing preferences.
      }
      if (lodOverride >= 1 && lodOverride <= 3)
        result.skinLod = lodOverride - 1;
      return result;
    }
  };

  // Separate from world time: pausing models must not pause water, sky or terrain.
  class ModelPreviewClock
  {
  public:
    int update(double sourceMs, int mode, bool enabled, int fps)
    {
      if (!std::isfinite(sourceMs))
        return _lastSample;
      double const delta = _initialized ? std::max(0.0, sourceMs - _sourceMs) : 0.0;
      _sourceMs = sourceMs;
      _initialized = true;
      bool const running = mode != 1 && mode != 2 && enabled;
      if (mode == 2)
        _elapsedMs = 0.0;
      else if (running && _wasRunning)
        _elapsedMs += delta;
      if (mode == 2)
        _lastSample = 0;
      else if (running && _wasRunning)
        _lastSample = sample(fps);
      _wasRunning = running;
      return _lastSample;
    }

  private:
    int sample(int fps) const
    {
      double value = _elapsedMs;
      if (fps > 0)
        value = std::floor(value * fps / 1000.0) * 1000.0 / fps;
      return static_cast<int>(std::fmod(value, static_cast<double>(std::numeric_limits<int>::max())));
    }
    double _sourceMs = 0.0;
    double _elapsedMs = 0.0;
    bool _initialized = false;
    bool _wasRunning = false;
    int _lastSample = 0;
  };
}
