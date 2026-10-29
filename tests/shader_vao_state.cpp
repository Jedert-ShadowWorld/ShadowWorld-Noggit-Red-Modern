#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
  void check(bool ok, char const* message)
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
  { std::cout << "SKIP: OpenGL 4.1 offscreen context unavailable\n"; return 77; }
  auto* f = context.versionFunctions<QOpenGLFunctions_4_1_Core>();
  if (!f || !f->initializeOpenGLFunctions())
  { std::cout << "SKIP: OpenGL 4.1 core functions unavailable\n"; return 77; }
  OpenGL::context::scoped_setter current(gl, &context);
  GLuint vaos[2]{}, buffer = 0;
  gl.genVertexArrays(2, vaos);
  GLint max_attributes = 0;
  gl.getIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attributes);
  GLuint const attribute = vaos[0];
  if (!attribute || attribute >= static_cast<GLuint>(max_attributes))
  {
    f->glDeleteVertexArrays(2, vaos);
    std::cout << "SKIP: Generated VAO names cannot reproduce an attribute-index collision\n";
    return 77;
  }
  try
  {
    OpenGL::program program({
      OpenGL::shader(GL_VERTEX_SHADER, "#version 410 core\nlayout(location="
          + std::to_string(attribute) + ") in vec2 position;\n"
          "void main(){gl_Position=vec4(position,0,1);}"),
      OpenGL::shader(GL_FRAGMENT_SHADER,
          "#version 410 core\nout vec4 color;void main(){color=vec4(0,1,0,1);}")});
    float const vertices[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
    gl.genBuffers(1, &buffer);
    gl.bindBuffer(GL_ARRAY_BUFFER, buffer);
    gl.bufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    gl.bindVertexArray(vaos[0]);
    {
      OpenGL::Scoped::use_program shader(program);
      shader.attrib("position", buffer, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    GLint enabled = 0, bound = 0, active_program = -1;
    f->glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
    check(enabled == GL_TRUE, "Shader scope disabled a cached VAO attribute matching its object name");
    gl.getIntegerv(GL_VERTEX_ARRAY_BINDING, &bound);
    check(static_cast<GLuint>(bound) == vaos[0], "Shader scope changed the current VAO");
    gl.getIntegerv(GL_CURRENT_PROGRAM, &active_program);
    check(active_program == 0, "Shader scope did not restore the previous program");

    // Set up another mesh, then restore a different cached VAO before scope exit.
    {
      OpenGL::Scoped::use_program shader(program);
      gl.bindVertexArray(vaos[1]);
      shader.attrib("position", buffer, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
      gl.bindVertexArray(vaos[0]);
    }
    f->glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
    check(enabled == GL_TRUE, "Shader scope modified an unrelated current VAO");
    gl.bindVertexArray(vaos[1]);
    f->glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
    check(enabled == GL_TRUE, "Second cached VAO lost its attribute enablement");

    QOpenGLFramebufferObject target(8, 8);
    check(target.isValid() && target.bind(), "Cannot create the regression framebuffer");
    f->glViewport(0, 0, 8, 8);
    for (auto vao : vaos)
    {
      f->glClearColor(0, 0, 0, 1);
      f->glClear(GL_COLOR_BUFFER_BIT);
      gl.bindVertexArray(vao);
      {
        OpenGL::Scoped::use_program shader(program);
        // Reuse cached layout, exactly as subsequent model/WMO frames do.
        f->glDrawArrays(GL_TRIANGLES, 0, 3);
      }
      unsigned char pixel[4]{};
      f->glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
      check(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0,
          "Cached VAO failed to render after its setup shader scope ended");
    }
    check(f->glGetError() == GL_NO_ERROR, "OpenGL error during shader/VAO regression");
    target.release();
    std::cout << "PASS: production use_program preserves both cached VAOs and rendered pixels; "
              << reinterpret_cast<char const*>(f->glGetString(GL_RENDERER)) << '\n';
  }
  catch (std::exception const& error)
  {
    std::cerr << error.what() << '\n';
    gl.deleteBuffers(1, &buffer);
    f->glDeleteVertexArrays(2, vaos);
    return 1;
  }
  gl.deleteBuffers(1, &buffer);
  f->glDeleteVertexArrays(2, vaos);
  return 0;
}
