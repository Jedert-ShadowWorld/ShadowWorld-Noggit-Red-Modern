// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Model.h>
#include <noggit/ModelManager.h>
#include <noggit/TextureManager.h>
#include <noggit/WMO.h>

// The manager multimaps are defined together in this translation unit so their
// destruction order at exit is well-defined (reverse of definition order).
// WMOs hold model and texture references, and models hold texture references,
// so textures must be destroyed last: any object still alive during static
// teardown releases its references into a manager that is still alive instead
// of a destroyed one (previously a source of secondary access violations
// masking the original crash whenever the SIGSEGV handler called exit()).

decltype (TextureManager::_) TextureManager::_;
decltype (ModelManager::_) ModelManager::_;
decltype (WMOManager::_) WMOManager::_;
