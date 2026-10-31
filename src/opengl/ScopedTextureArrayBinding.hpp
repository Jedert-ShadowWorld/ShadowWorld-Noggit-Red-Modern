// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <QtGui/qopengl.h>

namespace OpenGL::Scoped
{
  // Uploads use the currently active unit. Keep its material binding intact.
  template<typename GL>
  class texture_array_binding_restore
  {
  public:
    explicit texture_array_binding_restore(GL& context, bool enabled = true)
      : _context(context), _enabled(enabled)
    {
      if (_enabled)
        _context.getIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &_previous);
    }
    ~texture_array_binding_restore()
    {
      if (_enabled)
        // Allocating a new page may retire the last slice of the old binding.
        _context.bindTexture(GL_TEXTURE_2D_ARRAY,
          _previous && _context.isTexture(static_cast<GLuint>(_previous)) ? static_cast<GLuint>(_previous) : 0);
    }
    texture_array_binding_restore(texture_array_binding_restore const&) = delete;
    texture_array_binding_restore& operator=(texture_array_binding_restore const&) = delete;
  private:
    GL& _context;
    bool _enabled;
    GLint _previous = 0;
  };
}
