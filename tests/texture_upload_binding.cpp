#include <opengl/ScopedTextureArrayBinding.hpp>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_1_Core>
#include <array>
#include <iostream>
#include <stdexcept>

namespace
{
  struct GLAdapter
  {
    QOpenGLFunctions_4_1_Core& functions;
    void getIntegerv(GLenum name, GLint* value) { functions.glGetIntegerv(name, value); }
    void bindTexture(GLenum target, GLuint texture) { functions.glBindTexture(target, texture); }
  };
  void require(bool ok, char const* message)
  { if (!ok) throw std::runtime_error(message); }
}

int main(int argc, char** argv)
{
  QGuiApplication app(argc, argv);
  QSurfaceFormat format;
  format.setVersion(4, 1);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context;
  context.setFormat(format);
  QOffscreenSurface surface;
  surface.setFormat(format);
  surface.create();
  if (!context.create() || !surface.isValid() || !context.makeCurrent(&surface))
  {
    std::cout << "SKIP: OpenGL 4.1 offscreen context unavailable\n";
    return 77;
  }
  auto* f = context.versionFunctions<QOpenGLFunctions_4_1_Core>();
  if (!f || !f->initializeOpenGLFunctions())
  {
    std::cout << "SKIP: OpenGL 4.1 core functions unavailable\n";
    return 77;
  }
  GLAdapter gl{*f};
  std::array<GLuint, 2> textures{};
  f->glGenTextures(2, textures.data());
  std::array<unsigned char, 4> const first{255, 0, 0, 255}, second{0, 255, 0, 255};
  auto upload = [&](GLuint id, auto const& pixel)
  {
    f->glBindTexture(GL_TEXTURE_2D_ARRAY, id);
    f->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    f->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
  };
  auto read = [&]
  {
    std::array<unsigned char, 4> result{};
    f->glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
    return result;
  };
  try
  {
    // prepareDraw binds material slot 2 before a newly loaded slot 1 uploads.
    f->glActiveTexture(GL_TEXTURE2);
    upload(textures[1], second);
    upload(textures[0], first);
    require(read() == first, "The unguarded upload did not reproduce slot 2 clobbering");
    f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[1]);
    {
      OpenGL::Scoped::texture_array_binding_restore const restore(gl);
      upload(textures[0], first);
    }
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[0]);
    require(read() == first, "Material slot 1 lost its uploaded pixels");
    f->glActiveTexture(GL_TEXTURE2);
    require(read() == second, "Material slot 2 was replaced by the slot 1 upload");
    try
    {
      OpenGL::Scoped::texture_array_binding_restore const restore(gl);
      f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[0]);
      throw std::runtime_error("interrupted upload");
    }
    catch (std::runtime_error const&) {}
    require(read() == second, "An interrupted upload failed to restore the material binding");
    require(f->glGetError() == GL_NO_ERROR, "OpenGL error during texture upload regression");
    std::cout << "PASS: real OpenGL material slot pixels preserved, including interrupted upload; "
              << reinterpret_cast<char const*>(f->glGetString(GL_RENDERER)) << '\n';
  }
  catch (std::exception const& error)
  {
    std::cerr << error.what() << '\n';
    f->glDeleteTextures(2, textures.data());
    return 1;
  }
  f->glDeleteTextures(2, textures.data());
  return 0;
}
