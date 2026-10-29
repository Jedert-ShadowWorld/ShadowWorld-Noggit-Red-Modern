#include <opengl/scoped.hpp>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <iostream>
#include <stdexcept>

void check(bool value, char const* message)
{
  if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
  QGuiApplication app(argc, argv);
  QSurfaceFormat format;
  format.setVersion(4, 1);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context;
  context.setFormat(format);
  if (!context.create()) return 77;
  QOffscreenSurface surface;
  surface.setFormat(context.format());
  surface.create();
  if (!context.makeCurrent(&surface)) return 77;
  auto* functions = context.versionFunctions<QOpenGLFunctions_4_1_Core>();
  if (!functions || !functions->initializeOpenGLFunctions()) return 77;
  OpenGL::context::scoped_setter current(gl, &context);
  GLuint buffers[2], textures[16];
  gl.genBuffers(2, buffers);
  gl.genTextures(16, textures);
  check(textures[15] != buffers[0], "Fixture must distinguish texture names from buffer names");
  gl.activeTexture(GL_TEXTURE0);
  gl.bindTexture(GL_TEXTURE_BUFFER, textures[15]);
  gl.bindBuffer(GL_TEXTURE_BUFFER, buffers[0]);
  float const initial[4] = {1.f, 2.f, 3.f, 4.f};
  gl.bufferData(GL_TEXTURE_BUFFER, sizeof(initial), initial, GL_DYNAMIC_DRAW);
  gl.bindBuffer(GL_TEXTURE_BUFFER, buffers[1]);
  gl.bufferData(GL_TEXTURE_BUFFER, sizeof(initial), initial, GL_DYNAMIC_DRAW);
  gl.texBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, buffers[1]);
  gl.bindBuffer(GL_TEXTURE_BUFFER, buffers[0]);
  {
    OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> binding(buffers[1]);
    GLint active = 0;
    gl.getIntegerv(GL_TEXTURE_BUFFER_BINDING, &active);
    check(static_cast<GLuint>(active) == buffers[1], "Bone buffer was not bound");
    float const pose[4] = {10.f, 20.f, 30.f, 40.f};
    gl.bufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(pose), pose);
  }
  GLint restored = 0;
  gl.getIntegerv(GL_TEXTURE_BUFFER_BINDING, &restored);
  check(static_cast<GLuint>(restored) == buffers[0], "Restored texture object instead of buffer object");
  GLint texture = 0;
  gl.getIntegerv(GL_TEXTURE_BINDING_BUFFER, &texture);
  check(static_cast<GLuint>(texture) == textures[15], "Bone buffer update changed texture binding");
  float values[4]{};
  functions->glGetBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(values), values);
  check(values[0] == 1.f && values[3] == 4.f, "Unrelated buffer data was modified");
  {
    OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> binding(buffers[1]);
    functions->glGetBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(values), values);
    check(values[0] == 10.f && values[3] == 40.f, "Pose was not uploaded to the intended bone buffer");
  }
  check(functions->glGetError() == GL_NO_ERROR, "OpenGL error during bone buffer update");
  gl.deleteTextures(16, textures);
  gl.deleteBuffers(2, buffers);
  std::cout << "Texture-buffer binding and matrix update isolation verified on OpenGL "
            << context.format().majorVersion() << '.' << context.format().minorVersion() << '\n';
}
