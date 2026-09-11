// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "LiquidTextureManager.hpp"
#include "opengl/context.inl"
#include "noggit/DBC.h"
#include "noggit/application/NoggitApplication.hpp"
#include <noggit/TextureManager.h>
#include <noggit/Log.h>

#include <vector>

using namespace Noggit::Rendering;

LiquidTextureManager::LiquidTextureManager(Noggit::NoggitRenderContext context)
  : _context(context)
{
}

void LiquidTextureManager::upload()
{
  if (_uploaded)
    return;

  unsigned skipped_invalid_profiles = 0;
  for (int i = 0; i < gLiquidTypeDB.getRecordCount(); ++i)
  {
    const DBCFile::Record record = gLiquidTypeDB.getRecord(i);
    unsigned liquid_type_id = record.getInt(LiquidTypeDB::ID);
    int type = record.getInt(LiquidTypeDB::Type);
    glm::vec2 anim = {record.getFloat(LiquidTypeDB::AnimationX), record.getFloat(LiquidTypeDB::AnimationY)};
    int shader_type = record.getInt(LiquidTypeDB::ShaderType);

    std::string filename;

    // procedural water hack fix
    if (shader_type == 3)
    {
      filename = "XTextures\\river\\lake_a.";
      // default param for water
      anim = glm::vec2(1.f, 0.f);
    }
    else
    [[likely]]
    {
      try
      {
        std::string db_string_template = record.getString(LiquidTypeDB::TextureFilenames);
        if (db_string_template.length() <= 6)
        {
          ++skipped_invalid_profiles;
          continue;
        }
        filename = db_string_template.substr(0, db_string_template.length() - 6);
      }
      catch (...)
      {
        filename = "XTextures\\river\\lake_a.";
      }
    }

    GLuint array = 0;
    gl.genTextures(1, &array);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);

    blp_texture tex(filename + "1.blp", _context);
    tex.finishLoading();

    int width_ = tex.width();
    int height_ = tex.height();
    const unsigned mip_level = tex.mip_level();
    const bool is_uncompressed = !tex.compression_format();

    constexpr unsigned N_FRAMES = 30;

    if (is_uncompressed)
    {
      for (unsigned int j = 0; j < mip_level; ++j)
      {
        gl.texImage3D(GL_TEXTURE_2D_ARRAY, j, GL_RGBA8, width_, height_, N_FRAMES, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      nullptr);
        width_ = std::max(width_ >> 1, 1);
        height_ = std::max(height_ >> 1, 1);
      }
    }
    else
    [[likely]]
    {
      for (unsigned int j = 0; j < mip_level; ++j)
      {
        gl.compressedTexImage3D(GL_TEXTURE_2D_ARRAY, j, tex.compression_format().value(), width_, height_, N_FRAMES,
                                0, static_cast<GLsizei>(tex.compressed_data()[j].size() * N_FRAMES), nullptr);
        width_ = std::max(width_ >> 1, 1);
        height_ = std::max(height_ >> 1, 1);
      }
    }

    unsigned n_frames = 30;
    for (int j = 0; j < N_FRAMES; ++j)
    {
      if (!Noggit::Application::NoggitApplication::instance()->clientData()->exists(filename + std::to_string((j + 1)) + ".blp"))
      {
        n_frames = j;
        break;
      }

      blp_texture tex_frame(filename + std::to_string(j + 1) + ".blp", _context);
      tex_frame.finishLoading();

      if (tex_frame.height() != tex.height() || tex_frame.width() != tex.width())
        LogError << "Liquid texture resolution mismatch. Make sure all textures within a liquid type use identical format." << std::endl;
      else if (tex_frame.compression_format() != tex.compression_format())
        LogError << "Liquid texture compression mismatch. Make sure all textures within a liquid type use identical format." << std::endl;
      else if (tex_frame.mip_level() != tex.mip_level())
        LogError << "Liquid texture mip level mismatch. Make sure all textures within a liquid type use identical format." << std::endl;
      else
      [[likely]]
      {
        tex_frame.uploadToArray(j);
        continue;
      }

      tex.uploadToArray(j);
    }

    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, mip_level - 3);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    _texture_frames_map[liquid_type_id] = std::make_tuple(array, anim, type, n_frames);
  }

  if (skipped_invalid_profiles)
  {
    LogDebug << "[ModernADT][WaterRender] Skipped " << skipped_invalid_profiles
             << " LiquidType profile(s) with no usable texture filename."
             << std::endl;
  }

  // Modern clients do not expose the legacy LiquidType.dbc profiles Noggit's
  // water shader expects. Keep MH2O geometry authoritative and bootstrap the
  // shader with small, low-energy ripple textures. The special compatibility
  // types 4/5 tell liquid_frag.glsl to derive a visible water tint from the
  // current scene lighting instead of trusting missing/black River/Ocean colors.
  if (_texture_frames_map.empty())
  {
    constexpr unsigned fallback_frames = 4;
    constexpr unsigned fallback_width = 4;
    constexpr unsigned fallback_height = 4;

    auto install_modern_profile = [&](unsigned liquid_id, int liquid_type, glm::vec2 anim)
    {
      std::vector<unsigned char> pixels(
        fallback_width * fallback_height * fallback_frames * 4u, 0u);

      for (unsigned frame = 0; frame < fallback_frames; ++frame)
      {
        for (unsigned y = 0; y < fallback_height; ++y)
        {
          for (unsigned x = 0; x < fallback_width; ++x)
          {
            const unsigned pixel_index =
              (((frame * fallback_height + y) * fallback_width + x) * 4u);

            // Moving diagonal ripple with a very small additive contribution.
            const unsigned wave = (x * 3u + y * 5u + frame * 4u) % 13u;
            pixels[pixel_index + 0] = static_cast<unsigned char>(3u + wave / 3u);
            pixels[pixel_index + 1] = static_cast<unsigned char>(5u + wave / 2u);
            pixels[pixel_index + 2] = static_cast<unsigned char>(8u + wave);
            pixels[pixel_index + 3] = 255u;
          }
        }
      }

      GLuint array = 0;
      gl.genTextures(1, &array);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);
      gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8,
                    fallback_width, fallback_height, fallback_frames,
                    0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
      gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
      gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);

      _texture_frames_map[liquid_id] =
        std::make_tuple(array, anim, liquid_type, fallback_frames);
    };

    // Compatibility-only shader types:
    //   4 = modern river/lake
    //   5 = modern ocean
    // They are intentionally outside the legacy 0..3 LiquidType meanings.
    install_modern_profile(5u, 4, glm::vec2(1.0f, 0.0f));
    install_modern_profile(1u, 4, glm::vec2(0.9f, 6.0f));
    install_modern_profile(2u, 5, glm::vec2(0.7f, 12.0f));

    LogDebug << "[ModernADT][WaterRender] Legacy LiquidType profiles are unavailable; "
             << "installed scene-lit modern MH2O fallback profiles: river/lake ids={1,5} type=4, "
             << "ocean id=2 type=5, each with " << fallback_frames
             << " low-energy ripple frame(s)." << std::endl;
  }

  _uploaded = true;
}

void LiquidTextureManager::unload()
{
  for (auto& pair : _texture_frames_map)
  {
    GLuint array = std::get<0>(pair.second);
    gl.deleteTextures(1, &array);
  }

  _texture_frames_map.clear();
  _uploaded = false;
}
