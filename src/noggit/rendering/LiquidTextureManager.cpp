// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "LiquidTextureManager.hpp"
#include "opengl/context.inl"
#include "noggit/DBC.h"
#include "noggit/application/NoggitApplication.hpp"
#include <noggit/TextureManager.h>
#include <noggit/Log.h>

using namespace Noggit::Rendering;

LiquidTextureManager::LiquidTextureManager(Noggit::NoggitRenderContext context)
  : _context(context)
{
}

void LiquidTextureManager::upload()
{
  if (_uploaded)
    return;

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

  // Shadowlands MH2O is now parsed independently from the legacy LiquidType
  // database.  The old lake_a texture sequence does exist in 9.2.7, but the
  // modern fallback renderer does not have the legacy material/shader metadata
  // that makes those frames meaningful.  Sampling them with the bootstrap
  // profile produces an almost-black, oil-like surface.  Keep a deterministic
  // blue bootstrap material until the DB2 liquid material bridge is available.
  if (_texture_frames_map.empty())
  {
    constexpr unsigned fallback_liquid_id = 5;
    constexpr unsigned fallback_frames = 4;

    GLuint array = 0;
    gl.genTextures(1, &array);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);

    // Four subtly different blue frames.  This stays visibly water-like and
    // exercises the animated-array path without depending on legacy shader
    // semantics.  Geometry, MH2O masks and heights remain untouched.
    const unsigned char pixels[fallback_frames][4] =
    {
      { 72, 138, 186, 190 },
      { 78, 146, 194, 190 },
      { 66, 132, 182, 190 },
      { 75, 142, 191, 190 }
    };

    gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, fallback_frames,
                  0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    _texture_frames_map[fallback_liquid_id] =
      std::make_tuple(array, glm::vec2(1.f, 0.f), 0, fallback_frames);

    LogDebug << "[ModernADT][WaterRender] Legacy LiquidType profiles are unavailable; "
             << "installed safe animated procedural water profile id=" << fallback_liquid_id
             << " with " << fallback_frames
             << " blue frame(s). Legacy lake_a is intentionally disabled for modern MH2O."
             << std::endl;
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
