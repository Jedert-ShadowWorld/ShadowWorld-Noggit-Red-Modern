// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <QGuiApplication>
#include <QFile>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_1_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

int main(int argc, char** argv)
{
  QGuiApplication application(argc, argv);
  QSurfaceFormat format;
  format.setVersion(4, 1);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context;
  context.setFormat(format);
  QOffscreenSurface surface;
  surface.setFormat(format);
  surface.create();
  if (!context.create() || !context.makeCurrent(&surface))
  {
    std::cerr << "FAIL: Cannot create OpenGL 4.1 context.\n";
    return 1;
  }
  QOpenGLShaderProgram program;
  if (argc != 2)
  {
    std::cerr << "Usage: modern-terrain-shader-test <shader-directory>\n";
    return 1;
  }
  QString const path = QString::fromLocal8Bit(argv[1]) + "/";
  auto read_shader = [&](QString const& name)
  {
    QFile file(path + name);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
      return QByteArray{};
    auto source = file.readAll();
    auto const version = source.indexOf("#version");
    return version >= 0 ? source.mid(version) : source;
  };
  if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, read_shader("terrain_vert.glsl"))
      || !program.addShaderFromSourceCode(QOpenGLShader::Fragment, read_shader("terrain_frag.glsl"))
      || !program.link())
  {
    std::cerr << "FAIL: " << program.log().toStdString() << '\n';
    return 1;
  }
  auto* gl = context.versionFunctions<QOpenGLFunctions_4_1_Core>();
  gl->initializeOpenGLFunctions();
  auto const block = gl->glGetUniformBlockIndex(program.programId(), "chunk_instances");
  GLint size = 0;
  gl->glGetActiveUniformBlockiv(program.programId(), block, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
  if (size != 240 * 256)
  {
    std::cerr << "FAIL: Terrain uniform layout size " << size << '\n';
    return 1;
  }
  std::cout << "PASS: Terrain shaders compile/link on " << gl->glGetString(GL_RENDERER)
            << "; uniform block matches 240-byte CPU stride.\n";

  // Keep the real blend function, isolating it from lighting and terrain geometry.
  auto fragment = read_shader("terrain_frag.glsl");
  fragment.truncate(fragment.indexOf("void main()"));
  fragment += "void main() { out_color = mists_texture_blend(); }\n";
  QOpenGLShaderProgram blend;
  char const* vertex = R"(#version 410 core
out vec2 vary_texcoord, vary_t0_uv, vary_t1_uv, vary_t2_uv, vary_t3_uv;
flat out int instanceID;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  vary_texcoord = vary_t0_uv = vary_t1_uv = vary_t2_uv = vary_t3_uv = vec2(0.5);
  instanceID = 0;
}
)";
  if (!blend.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex)
      || !blend.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !blend.link())
  {
    std::cerr << "FAIL: blend fixture " << blend.log().toStdString() << '\n';
    return 1;
  }
  blend.bind();
  auto upload_texture = [&](int unit, int depth, float const* pixels)
  {
    GLuint texture;
    gl->glGenTextures(1, &texture);
    gl->glActiveTexture(GL_TEXTURE0 + unit);
    gl->glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA32F, 1, 1, depth, 0, GL_RGBA, GL_FLOAT, pixels);
    return texture;
  };
  float const diffuse[] = {1,0,0,1, 0,1,0,1};
  float const heights[] = {0,0,0,0, 0,0,0,1};
  float const alpha[] = {0.25f,0,0,1};
  auto const diffuse_texture = upload_texture(0, 2, diffuse);
  auto const height_texture = upload_texture(1, 2, heights);
  auto const alpha_texture = upload_texture(2, 1, alpha);
  int samplers[11] = {0, 1};
  blend.setUniformValueArray("textures", samplers, 11);
  blend.setUniformValue("alphamap", 2);

  std::array<std::array<std::uint32_t, 60>, 256> instances{};
  auto set_float = [&](int word, float value) { std::memcpy(&instances[0][word], &value, 4); };
  instances[0][4] = 0; instances[0][5] = 1; // Diffuse array layers.
  instances[0][10] = 2; // Two terrain layers.
  instances[0][32] = instances[0][33] = 1; // Height sampler.
  instances[0][36] = 1; instances[0][37] = 0; // Independent, reversed height layers.
  for (int i = 0; i < 4; ++i) set_float(48 + i, 1.0f);
  GLuint buffer, vao;
  gl->glGenBuffers(1, &buffer);
  gl->glBindBuffer(GL_UNIFORM_BUFFER, buffer);
  gl->glBufferData(GL_UNIFORM_BUFFER, sizeof(instances), instances.data(), GL_DYNAMIC_DRAW);
  gl->glUniformBlockBinding(blend.programId(), gl->glGetUniformBlockIndex(blend.programId(), "chunk_instances"), 0);
  gl->glBindBufferBase(GL_UNIFORM_BUFFER, 0, buffer);
  gl->glGenVertexArrays(1, &vao);
  gl->glBindVertexArray(vao);
  QOpenGLFramebufferObjectFormat framebuffer_format;
  framebuffer_format.setInternalTextureFormat(GL_RGBA32F);
  QOpenGLFramebufferObject framebuffer(2, 2, framebuffer_format);
  framebuffer.bind();
  gl->glViewport(0, 0, 2, 2);
  auto check_pixel = [&](float red, float green)
  {
    gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(instances), instances.data());
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    std::array<float, 4> pixel{};
    gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, pixel.data());
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1])
      && std::abs(pixel[0] - red) < 0.002f && std::abs(pixel[1] - green) < 0.002f;
  };
  bool const fallback_ok = check_pixel(0.75f, 0.25f);
  set_float(44, 1.0f); set_float(45, 1.0f);
  set_float(48, 0.0f); set_float(49, 0.0f);
  bool const height_ok = check_pixel(1.0f, 0.0f);
  set_float(44, 0.0f); set_float(45, 0.0f);
  set_float(50, 0.0f); set_float(51, 0.0f);
  bool const zero_ok = check_pixel(0.75f, 0.25f);
  framebuffer.release();
  gl->glDeleteTextures(1, &diffuse_texture);
  gl->glDeleteTextures(1, &height_texture);
  gl->glDeleteTextures(1, &alpha_texture);
  gl->glDeleteBuffers(1, &buffer);
  gl->glDeleteVertexArrays(1, &vao);
  if (!fallback_ok || !height_ok || !zero_ok)
  {
    std::cerr << "FAIL: GPU pixels: alpha=" << fallback_ok << " height=" << height_ok
              << " zero=" << zero_ok << '\n';
    return 1;
  }
  std::cout << "PASS: GPU pixels verify alpha fallback, independent MHID layers and zero-height safety.\n";
  return 0;
}
