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
      // TODO: why even try-catching there? empty string? BARE_EXCEPT_INV
      try
      {
        std::string db_string_template = record.getString(LiquidTypeDB::TextureFilenames);
        filename = db_string_template.substr(0, db_string_template.length() - 6);
      }
      catch (...) // fallback for malformed DBC
      {
        filename = "XTextures\\river\\lake_a.";
      }

    }

    GLuint array = 0;
    gl.genTextures(1, &array);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);

    // init 2D texture array
    // loading a texture is required to get its dimensions and format
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

      // error checking
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

      // use the first frame, the texture will end-up non-animated or skipping certain frames,
      // but that avoids OpenGL errors.
      tex.uploadToArray(j);
    }

    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, mip_level - 3);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    _texture_frames_map[liquid_type_id] = std::make_tuple(array, anim, type, n_frames);
  }

  // Modern clients may not provide the legacy LiquidType.dbc that Noggit's
  // liquid renderer normally uses to build its material/texture profile map.
  // Prefer a real animated client water sequence when available; this gives
  // modern MH2O useful visual feedback before the DB2 liquid-material bridge
  // is implemented.  Keep the old 1x1 procedural pixel only as a last resort.
  if (_texture_frames_map.empty())
  {
    constexpr unsigned fallback_liquid_id = 5;
    constexpr unsigned max_fallback_frames = 30;
    const std::string filename = "XTextures\\river\\lake_a.";
    auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();

    bool installed_animated_fallback = false;

    if (client_data && client_data->exists(filename + "1.blp"))
    {
      try
      {
        unsigned n_frames = 0;
        while (n_frames < max_fallback_frames &&
               client_data->exists(filename + std::to_string(n_frames + 1) + ".blp"))
        {
          ++n_frames;
        }

        if (n_frames > 0)
        {
          blp_texture first_frame(filename + "1.blp", _context);
          first_frame.finishLoading();

          GLuint array = 0;
          gl.genTextures(1, &array);
          gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);

          int width = first_frame.width();
          int height = first_frame.height();
          const unsigned mip_level = first_frame.mip_level();
          const bool is_uncompressed = !first_frame.compression_format();

          if (is_uncompressed)
          {
            for (unsigned mip = 0; mip < mip_level; ++mip)
            {
              gl.texImage3D(GL_TEXTURE_2D_ARRAY, mip, GL_RGBA8, width, height, n_frames,
                            0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
              width = std::max(width >> 1, 1);
              height = std::max(height >> 1, 1);
            }
          }
          else
          {
            for (unsigned mip = 0; mip < mip_level; ++mip)
            {
              gl.compressedTexImage3D(GL_TEXTURE_2D_ARRAY, mip,
                                      first_frame.compression_format().value(),
                                      width, height, n_frames, 0,
                                      static_cast<GLsizei>(first_frame.compressed_data()[mip].size() * n_frames),
                                      nullptr);
              width = std::max(width >> 1, 1);
              height = std::max(height >> 1, 1);
            }
          }

          for (unsigned frame = 0; frame < n_frames; ++frame)
          {
            blp_texture tex_frame(filename + std::to_string(frame + 1) + ".blp", _context);
            tex_frame.finishLoading();

            if (tex_frame.height() == first_frame.height() &&
                tex_frame.width() == first_frame.width() &&
                tex_frame.compression_format() == first_frame.compression_format() &&
                tex_frame.mip_level() == first_frame.mip_level())
            {
              tex_frame.uploadToArray(frame);
            }
            else
            {
              first_frame.uploadToArray(frame);
            }
          }

          gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,
                           mip_level > 3 ? static_cast<GLint>(mip_level - 3) : 0);
          gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER,
                           mip_level > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
          gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

          _texture_frames_map[fallback_liquid_id] =
            std::make_tuple(array, glm::vec2(1.f, 0.f), 0, n_frames);
          installed_animated_fallback = true;

          LogDebug << "[ModernADT][WaterRender] Legacy LiquidType profiles are unavailable; "
                   << "using animated client fallback '" << filename
                   << "' with " << n_frames << " frame(s) for modern MH2O."
                   << std::endl;
        }
      }
      catch (...)
      {
        LogError << "[ModernADT][WaterRender] Failed to build animated client water fallback; "
                 << "falling back to procedural 1x1 water." << std::endl;
      }
    }

    if (!installed_animated_fallback)
    {
      GLuint array = 0;
      gl.genTextures(1, &array);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);

      const unsigned char pixel[4] = { 72, 138, 186, 190 };
      gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1,
                    0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
      gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
      gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

      _texture_frames_map[fallback_liquid_id] =
        std::make_tuple(array, glm::vec2(0.f, 0.f), 0, 1u);

      LogDebug << "[ModernADT][WaterRender] Legacy LiquidType profiles are unavailable and "
               << "no animated client fallback was found; installed procedural fallback profile id="
               << fallback_liquid_id << " texture=1x1x1." << std::endl;
    }
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
