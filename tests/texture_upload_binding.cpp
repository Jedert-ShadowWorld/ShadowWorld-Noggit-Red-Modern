#include <opengl/ScopedTextureArrayBinding.hpp>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QOpenGLFunctions_4_1_Core>
#include <algorithm>
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
    GLboolean isTexture(GLuint texture) { return functions.glIsTexture(texture); }
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
  std::array<GLuint, 5> textures{};
  f->glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
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

    // Several terrain BLPs now occupy slices of one compatible array page.
    // Upload/reuse a middle slice without changing either live neighbour.
    std::array<std::array<unsigned char, 4>, 3> const colors{
      first, second, std::array<unsigned char, 4>{0, 0, 255, 255}};
    {
      OpenGL::Scoped::texture_array_binding_restore const restore(gl);
      f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[2]);
      f->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 4, 4, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      f->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
      for (unsigned layer = 0; layer < colors.size(); ++layer)
      {
        std::array<unsigned char, 4 * 4 * 4> pixels{};
        for (unsigned i = 0; i < 16; ++i)
          std::copy(colors[layer].begin(), colors[layer].end(), pixels.begin() + i * 4);
        f->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, 4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      }
      std::array<unsigned char, 3 * 4 * 4 * 4> pixels{};
      f->glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      for (unsigned layer = 0; layer < colors.size(); ++layer)
        for (unsigned i = 0; i < 16; ++i)
          require(std::equal(colors[layer].begin(), colors[layer].end(), pixels.begin() + (layer * 16 + i) * 4),
                  "Shared uncompressed texture array changed a sibling slice");
      std::array<unsigned char, 4 * 4 * 4> replacement{};
      replacement.fill(255);
      f->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, replacement.data());
      f->glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      for (unsigned i = 0; i < 16; ++i)
      {
        require(std::equal(first.begin(), first.end(), pixels.begin() + i * 4)
          && std::equal(colors[2].begin(), colors[2].end(), pixels.begin() + (32 + i) * 4),
          "Reusing a texture array slice overwrote a live sibling");
      }
    }
    require(read() == second, "Shared array upload clobbered the previous material binding");

    if (!context.hasExtension(QByteArrayLiteral("GL_EXT_texture_compression_s3tc")))
    {
      std::cout << "SKIP: S3TC array upload unsupported by this context\n";
      f->glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
      return 77;
    }
    {
      OpenGL::Scoped::texture_array_binding_restore const restore(gl);
      // A 4x4 DXT1 slice is one eight-byte block. The allocation size includes
      // all three slices; each subsequent upload supplies only its own block.
      std::array<std::array<unsigned char, 8>, 3> const blocks{{
        {{0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0}},
        {{0xE0, 0x07, 0xE0, 0x07, 0, 0, 0, 0}},
        {{0x1F, 0x00, 0x1F, 0x00, 0, 0, 0, 0}}}};
      f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[3]);
      f->glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
        4, 4, 3, 0, 24, nullptr);
      f->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
      for (unsigned layer = 0; layer < blocks.size(); ++layer)
        f->glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, 4, 4, 1,
          GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 8, blocks[layer].data());
      std::array<unsigned char, 24> compressed{};
      f->glGetCompressedTexImage(GL_TEXTURE_2D_ARRAY, 0, compressed.data());
      for (unsigned layer = 0; layer < blocks.size(); ++layer)
        require(std::equal(blocks[layer].begin(), blocks[layer].end(), compressed.begin() + layer * 8),
                "Shared compressed array changed another slice's blocks");
      // Sample every layer as the renderer does. Some drivers' glGetTexImage
      // compressed-array decompression only returns layer zero; compressed
      // byte readback above still verifies the complete stored payload.
      QOpenGLShaderProgram program;
      require(program.addShaderFromSourceCode(QOpenGLShader::Vertex,
        "#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0.,1.);}"), "Texture test vertex shader failed");
      require(program.addShaderFromSourceCode(QOpenGLShader::Fragment,
        "#version 410 core\nuniform sampler2DArray source;uniform int layer;out vec4 color;void main(){color=texelFetch(source,ivec3(ivec2(gl_FragCoord.xy),layer),0);}"), "Texture test fragment shader failed");
      require(program.link() && program.bind(), "Texture test shader link failed");
      program.setUniformValue("source", 2);
      GLuint framebuffer=0, target=0, vao=0;
      f->glGenFramebuffers(1, &framebuffer);
      f->glGenTextures(1, &target);
      f->glBindTexture(GL_TEXTURE_2D, target);
      f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      f->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
      require(f->glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE, "Texture test framebuffer incomplete");
      f->glGenVertexArrays(1, &vao);
      f->glBindVertexArray(vao);
      f->glViewport(0, 0, 4, 4);
      for (unsigned layer=0; layer<colors.size(); ++layer)
      {
        program.setUniformValue("layer", int(layer));
        f->glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<unsigned char, 64> pixels{};
        f->glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (unsigned i=0; i<16; ++i)
          require(std::equal(colors[layer].begin(), colors[layer].end(), pixels.begin()+i*4), "Shared compressed array sampled the wrong slice");
      }
      f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
      f->glBindVertexArray(0);
      program.release();
      f->glDeleteVertexArrays(1, &vao);
      f->glDeleteFramebuffers(1, &framebuffer);
      f->glDeleteTextures(1, &target);
    }
    require(read() == second, "Compressed array upload clobbered the previous material binding");

    upload(textures[4], first);
    {
      OpenGL::Scoped::texture_array_binding_restore const restore(gl);
      f->glDeleteTextures(1, &textures[4]);
      textures[4] = 0;
      f->glBindTexture(GL_TEXTURE_2D_ARRAY, textures[2]);
    }
    GLint binding = -1;
    f->glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &binding);
    require(binding == 0, "Binding restoration revived a retired texture page");
    require(f->glGetError() == GL_NO_ERROR, "OpenGL error during texture upload regression");
    std::cout << "PASS: real OpenGL material bindings and three compressed/uncompressed slices preserved, including retired binding; "
              << reinterpret_cast<char const*>(f->glGetString(GL_RENDERER)) << '\n';
  }
  catch (std::exception const& error)
  {
    std::cerr << error.what() << '\n';
    f->glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    return 1;
  }
  f->glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
  return 0;
}
