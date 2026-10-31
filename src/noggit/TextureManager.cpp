// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/TextureManager.h>
#include <noggit/Log.h> // LogDebug
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <ClientFile.hpp>
#include <Exception.hpp>

#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFramebufferObjectFormat>
#include <QtGui/QSurfaceFormat>
#include <opengl/ScopedTextureArrayBinding.hpp>
#include <noggit/formats/BlpBgra.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <glm/vec2.hpp>

// TextureManager storage is defined in AsyncObjectManagers.cpp to preserve
// destruction order when releasing the final texture references.

void TextureManager::report()
{
  std::string output = "Still in the Texture manager:\n";
  _.apply ( [&] (BlizzardArchive::Listfile::FileKey const& key, blp_texture const&)
            {
              output += " - " + key.stringRepr() + "\n";
            }
          );
  LogDebug << output;
}

void TextureManager::unload_all(Noggit::NoggitRenderContext context)
{
  _.context_aware_apply(
      [&] (BlizzardArchive::Listfile::FileKey const& key, blp_texture& blp_texture)
      {
          try
          {
            blp_texture.unload();
          }
          catch (std::exception const& error)
          {
            LogError << "Could not reload texture while releasing context: "
                     << key.stringRepr() << ": " << error.what() << std::endl;
          }
      }
      , context
  );

  // cleanup texture arrays
  auto& arrays_for_context = _tex_arrays[context];

  for (auto& pair : arrays_for_context)
  {
    for (auto& page : pair.second.pages)
      gl.deleteTextures(1, &page.array);
  }
  arrays_for_context.clear();
  // Synchronous CPU reload may have replaced height-map objects and retired
  // their old handles. The bulk delete above already released those arrays.
  std::lock_guard<std::mutex> lock(_retired_mutex);
  _retired_arrays[context].clear();
  ++_texture_generations[context];
}

std::uint64_t TextureManager::texture_generation(Noggit::NoggitRenderContext context)
{
  std::lock_guard<std::mutex> lock(_retired_mutex);
  return _texture_generations[context];
}

void TextureManager::retire_texture(Noggit::NoggitRenderContext context, GLuint texture,
                                  unsigned layer, std::uint64_t generation)
{
  if (!texture) return;
  std::lock_guard<std::mutex> lock(_retired_mutex);
  if (generation != _texture_generations[context]) return;
  _retired_arrays[context].push_back({texture, layer});
}

void TextureManager::release_retired_textures(Noggit::NoggitRenderContext context)
{
  // A worker may release a texture, but only its render context frees the slice.
  // Keep shared pages alive until their last texture has released its slot.
  std::vector<TexArrayAllocation> retired;
  {
    std::lock_guard<std::mutex> lock(_retired_mutex);
    retired.swap(_retired_arrays[context]);
  }
  if (retired.empty()) return;
  std::sort(retired.begin(), retired.end(), [](auto const& a, auto const& b) {
    return std::tie(a.array, a.layer) < std::tie(b.array, b.layer);
  });
  for (auto& [key, params] : _tex_arrays[context])
  {
    auto end = std::remove_if(params.pages.begin(), params.pages.end(), [&](TexArrayPage& page)
    {
      auto it = std::lower_bound(retired.begin(), retired.end(), page.array,
        [](auto const& allocation, GLuint array) { return allocation.array < array; });
      for (; it != retired.end() && it->array == page.array; ++it)
        if (it->layer < page.used.size())
          page.used[it->layer] = false;
      if (std::any_of(page.used.begin(), page.used.end(), [](bool used) { return used; }))
        return false;
      gl.deleteTextures(1, &page.array);
      return true;
    });
    params.pages.erase(end, params.pages.end());
  }
}

TexArrayAllocation TextureManager::allocate_texture_array(GLint compression, int width, int height,
    std::vector<std::size_t> const& mip_sizes, Noggit::NoggitRenderContext context)
{
  if (width <= 0 || height <= 0 || mip_sizes.empty() || mip_sizes.size() > 16)
    throw std::runtime_error("Invalid texture array dimensions or mip chain.");
  for (auto size : mip_sizes)
    if (!size || size > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
      throw std::runtime_error("Invalid texture array mip size.");
  // The caller drains retired pages before saving GL bindings. Allocation must
  // not delete a page after the binding guard has captured its name.
  auto& params = _tex_arrays[context][std::make_tuple(compression, width, height, static_cast<int>(mip_sizes.size()))];
  bool const retail = Noggit::Application::NoggitApplication::instance()->clientData()->version()
      == BlizzardArchive::ClientVersion::RETAIL;
  for (auto& page : params.pages)
  {
    // A project switch can leave live references in the same render context.
    // Non-Retail uploads must not reuse spare slices from a Retail page.
    if (!retail && page.used.size() != 1)
      continue;
    auto free = std::find(page.used.begin(), page.used.end(), false);
    if (free != page.used.end())
    {
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, page.array);
      *free = true;
      return {page.array, static_cast<unsigned>(free - page.used.begin())};
    }
  }

  // Retail layers share compatible storage, without resizing or decoding BLPs.
  // Bound unused storage on large assets, and keep legacy allocations unchanged.
  unsigned capacity = 1;
  if (retail)
  {
    constexpr std::size_t page_budget = 16u * 1024u * 1024u;
    std::size_t bytes = 0;
    for (auto size : mip_sizes)
      bytes += std::min(size, page_budget - bytes);
    GLint layer_limit = 1;
    gl.getIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &layer_limit);
    capacity = static_cast<unsigned>(std::min<std::size_t>(
      std::min(16, std::max(1, layer_limit)), std::max<std::size_t>(1, page_budget / std::max<std::size_t>(1, bytes))));
  }

  TexArrayPage page;
  page.used.resize(capacity, false);
  page.used[0] = true;
  gl.genTextures(1, &page.array);
  try
  {
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, page.array);
    for (std::size_t i = 0; i < mip_sizes.size(); ++i)
    {
      if (compression == -1)
        gl.texImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(i), GL_RGBA8, width, height,
          capacity, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      else
        gl.compressedTexImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(i), compression, width, height,
          capacity, 0, static_cast<GLsizei>(mip_sizes[i] * capacity), nullptr);
      width = std::max(width >> 1, 1);
      height = std::max(height >> 1, 1);
    }
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(mip_sizes.size()) - 1);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    params.pages.push_back(std::move(page));
  }
  catch (...)
  {
    gl.deleteTextures(1, &page.array);
    throw;
  }
  return {params.pages.back().array, 0};
}

TexArrayAllocation TextureManager::get_tex_array(int width, int height, int mip_level,
                                               Noggit::NoggitRenderContext context)
{
  if (width <= 0 || height <= 0 || mip_level <= 0 || mip_level > 16)
    throw std::runtime_error("Invalid texture dimensions or mip count.");
  std::vector<std::size_t> mip_sizes;
  int mip_width = width, mip_height = height;
  for (int i = 0; i < mip_level; ++i)
  {
    if (static_cast<std::size_t>(mip_width) > std::numeric_limits<std::size_t>::max() / 4 / mip_height)
      throw std::runtime_error("Texture mip size overflow.");
    mip_sizes.push_back(static_cast<std::size_t>(mip_width) * mip_height * 4);
    mip_width = std::max(mip_width >> 1, 1);
    mip_height = std::max(mip_height >> 1, 1);
  }
  return allocate_texture_array(-1, width, height, mip_sizes, context);
}

TexArrayAllocation TextureManager::get_tex_array(GLint compression, int width, int height, int mip_level,
    std::map<int, std::vector<uint8_t>> const& comp_data, Noggit::NoggitRenderContext context)
{
  if (mip_level <= 0 || mip_level > 16 || comp_data.size() != static_cast<std::size_t>(mip_level))
    throw std::runtime_error("Invalid compressed texture mip count.");
  std::vector<std::size_t> mip_sizes;
  for (int i = 0; i < mip_level; ++i) mip_sizes.push_back(comp_data.at(i).size());
  return allocate_texture_array(compression, width, height, mip_sizes, context);
}

#include <cstdint>
//! \todo Cross-platform syntax for packed structs.
#pragma pack(push,1)
struct BLPHeader
{
  int32_t magix;
  int32_t version;
  uint8_t attr_0_compression;
  uint8_t attr_1_alphadepth;
  uint8_t attr_2_alphatype;
  uint8_t attr_3_mipmaplevels;
  int32_t resx;
  int32_t resy;
  int32_t offsets[16];
  int32_t sizes[16];
};
#pragma pack(pop)

void blp_texture::bind()
{
  if (!finished || loading_failed())
  {
    return;
  }

  if (!_uploaded)
  {
    upload();
  }

  gl.bindTexture(GL_TEXTURE_2D_ARRAY, _texture_array);
}

void blp_texture::uploadToArray(unsigned layer)
{
  finishLoading();

  int width = _width, height = _height;

  if (!_compression_format)
  {

    for (int i = 0; i < _data.size(); ++i)
    {
      gl.texSubImage3D(GL_TEXTURE_2D_ARRAY, i, 0, 0, layer, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, _data[i].data());

      width = std::max(width >> 1, 1);
      height = std::max(height >> 1, 1);
    }

    _data.clear();

  }
  else
  {
    for (int i = 0; i < _compressed_data.size(); ++i)
    {
      gl.compressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, i, 0, 0, layer, width, height, 1, _compression_format.value(), static_cast<GLsizei>(_compressed_data[i].size()), _compressed_data[i].data());

      width = std::max(width >> 1, 1);
      height = std::max(height >> 1, 1);
    }

    _compressed_data.clear();
  }
}

void blp_texture::upload()
{
  if (!finished || loading_failed())
  {
    return;
  }

  if (_uploaded)
  {
    return;
  }

  // Drain before capturing the binding: allocation may reuse a just-deleted GL
  // name. Worker releases after this point stay queued until the next upload.
  TextureManager::release_retired_textures(_context);
  // Uploads use the active texture unit, which may hold another stage of a Retail material.
  auto* client = Noggit::Application::NoggitApplication::instance()->clientData();
  bool const preserve_binding = client->version() == BlizzardArchive::ClientVersion::RETAIL;
  OpenGL::Scoped::texture_array_binding_restore const restore_binding(gl, preserve_binding);

  int width = _width, height = _height;

  _texture_generation = TextureManager::texture_generation(_context);

  try
  {
    if (!_compression_format)
    {
      auto const allocation = TextureManager::get_tex_array(_width, _height, static_cast<int>(_data.size()), _context);
      _texture_array = allocation.array;
      _array_index = static_cast<int>(allocation.layer);

      for (int i = 0; i < _data.size(); ++i)
      {
        gl.texSubImage3D(GL_TEXTURE_2D_ARRAY, i, 0, 0, _array_index, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, _data[i].data());

        width = std::max(width >> 1, 1);
        height = std::max(height >> 1, 1);
      }

      _data.clear();
    }
    else
    {
      auto const allocation = TextureManager::get_tex_array(_compression_format.value(), _width, _height, static_cast<int>(_compressed_data.size()), _compressed_data, _context);
      _texture_array = allocation.array;
      _array_index = static_cast<int>(allocation.layer);

      for (int i = 0; i < _compressed_data.size(); ++i)
      {
        gl.compressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, i, 0, 0, _array_index, width, height, 1, _compression_format.value(), static_cast<GLsizei>(_compressed_data[i].size()), _compressed_data[i].data());

        width = std::max(width >> 1, 1);
        height = std::max(height >> 1, 1);
      }
      _compressed_data.clear();
    }
  }
  catch (...)
  {
    // Keep CPU mip data for a retry, but return its reserved GPU slice first.
    TextureManager::retire_texture(_context, _texture_array, static_cast<unsigned>(_array_index), _texture_generation);
    _texture_array = 0;
    _array_index = -1;
    throw;
  }

  _uploaded = true;
}

void blp_texture::unload()
{
  _uploaded = false;
  reset_loading();
  _texture_array = 0;
  _array_index = -1;
  try
  {
    if (hasHeightMap() && heightMap)
      heightMap->unload();
    _compression_format.reset();
    _data.clear();
    _compressed_data.clear();

    // load data back from file. pretty sad. maybe keep it after loading?
    finishLoading();
  }
  catch (...)
  {
    // Context reload is synchronous and does not pass through AsyncLoader's
    // exception handler. It must still wake waiters and publish failure.
    complete_loading(true);
    throw;
  }
}

bool blp_texture::is_uploaded() const
{
  return _uploaded;
}

GLuint blp_texture::texture_array() const
{
  return _texture_array;
}

int blp_texture::array_index() const
{
  return _array_index;
}

bool blp_texture::is_specular() const
{
  return _is_specular;
}

unsigned blp_texture::mip_level() const
{
  return static_cast<unsigned>(!_compression_format ? _data.size() : _compressed_data.size());
}

std::map<int, std::vector<uint32_t>>& blp_texture::data()
{
  return _data;
}

std::map<int, std::vector<uint8_t>>& blp_texture::compressed_data()
{
  return _compressed_data;
}

std::optional<GLint> const& blp_texture::compression_format() const
{
  return _compression_format;
}

Noggit::NoggitRenderContext blp_texture::getContext() const
{
  return _context;
}

[[nodiscard]]
async_priority blp_texture::loading_priority() const
{
  return async_priority::high;
}

// Mists HeightMapping
bool blp_texture::hasHeightMap() const
{
  return _has_heightmap;
}

blp_texture* blp_texture::getHeightMap()
{
  return heightMap.get();
}

void blp_texture::loadFromUncompressedData(BLPHeader const* lHeader, char const* lData)
{
  unsigned int const* pal = reinterpret_cast<unsigned int const*>(lData + sizeof(BLPHeader));

  unsigned char const* buf;
  unsigned int *p;
  unsigned char const* c;
  unsigned char const* a;

  int alphabits = lHeader->attr_1_alphadepth;
  bool hasalpha = alphabits != 0;

  int width = _width, height = _height;

  for (int i = 0; i<16; ++i)
  {
    width = std::max(1, width);
    height = std::max(1, height);

    if (lHeader->offsets[i] > 0 && lHeader->sizes[i] > 0)
    {
      buf = reinterpret_cast<unsigned char const*>(&lData[lHeader->offsets[i]]);

      std::vector<uint32_t> data(lHeader->sizes[i]);

      int cnt = 0;
      p = data.data();
      c = buf;
      a = buf + width*height;
      for (int y = 0; y<height; y++)
      {
        for (int x = 0; x<width; x++)
        {
          unsigned int k = pal[*c++];
          k = ((k & 0x00FF0000) >> 16) | ((k & 0x0000FF00)) | ((k & 0x000000FF) << 16);

          int alpha = 0xFF;

          if (_is_tileset && !_is_specular)
          {
            alpha = 0x00;
          }
          else if (hasalpha)
          {
            if (alphabits == 8)
            {
              alpha = (*a++);
            }
            else if (alphabits == 1)
            {
              alpha = (*a & (1 << cnt++)) ? 0xff : 0;
              if (cnt == 8)
              {
                cnt = 0;
                a++;
              }
            }
          }

          k |= alpha << 24;
          *p++ = k;
        }
      }

      _data[i] = data;
    }
    else
    {
      return;
    }

    width >>= 1;
    height >>= 1;
  }
}

void blp_texture::loadFromCompressedData(BLPHeader const* lHeader, char const* lData)
{
  //                         0 (0000) & 3 == 0                1 (0001) & 3 == 1                    7 (0111) & 3 == 3
  const int alphatypes[] = { GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT };
  const int blocksizes[] = { 8, 16, 0, 16 };

  int alpha_type = lHeader->attr_2_alphatype & 3;
  GLint format = alphatypes[alpha_type];
  _compression_format = format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ? (lHeader->attr_1_alphadepth == 1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT) : format;

  int width = _width, height = _height;

  for (int i = 0; i < 16; ++i)
  {
    if (lHeader->sizes[i] <= 0 || lHeader->offsets[i] <= 0)
    {
      return;
    }

    // make sure the vector is of the right size, blizzard seems to fuck those up for some small mipmaps
    int size = std::floor((width + 3) / 4) * std::floor((height + 3) / 4) * blocksizes[alpha_type];

    if (size < lHeader->sizes[i])
    {
      LogDebug << "mipmap size mismatch in '" << _file_key.stringRepr() << "'" << std::endl;
      return;
    }

    _compressed_data[i].resize(size);

    char const* start = lData + lHeader->offsets[i];
    std::copy(start, start + lHeader->sizes[i], _compressed_data[i].begin());

    width = std::max(width >> 1, 1);
    height = std::max(height >> 1, 1);
  }
}

int blp_texture::width() const
{
  return _width;
}

int blp_texture::height() const
{
  return _height;
}

blp_texture::blp_texture(BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)
  : AsyncObject(file_key)
  , _context(context)
{
}

blp_texture::~blp_texture()
{
  // Last references may disappear on a loader thread. GL deletion is deferred
  // until the next allocation in the owning render context.
  TextureManager::retire_texture(_context, _texture_array, static_cast<unsigned>(_array_index), _texture_generation);
}

void blp_texture::finishLoading()
{
  auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
  bool exists = client_data->exists(_file_key);
  if (!exists)
  {
    LogError << "file not found: '" <<  _file_key.stringRepr() << "'" << std::endl;
  }

  std::string spec_filename = "", height_filename = "";
  bool has_specular = false, has_height = false;

  if (_file_key.hasFilepath() && _file_key.filepath().starts_with("tileset/") )
  {
    _is_tileset = true;

    spec_filename = _file_key.filepath().substr(0, _file_key.filepath().find_last_of(".")) + "_s.blp";
    has_specular = Noggit::Application::NoggitApplication::instance()->clientData()->exists(spec_filename);

    if (has_specular)
    {
      _is_specular = true;
    }

    bool modern_features = Noggit::Application::NoggitApplication::instance()->getConfiguration()->modern_features;

    // Only load _h in map view when modern features are enabled
    if(_context == Noggit::NoggitRenderContext::MAP_VIEW && modern_features)
    {
        height_filename = _file_key.filepath().substr(0, _file_key.filepath().find_last_of(".")) + "_h.blp";
        has_height = Noggit::Application::NoggitApplication::instance()->clientData()->exists(height_filename);
        if (has_height)
        {
            _has_heightmap = true;
            heightMap = std::make_unique<blp_texture>(height_filename,_context);
            heightMap->finishLoading();
        }
    }
  }

  BlizzardArchive::Listfile::FileKey load_key = _file_key;
  if (!exists)
    load_key = BlizzardArchive::Listfile::FileKey("textures/shanecube.blp");
  else if (has_specular)
    load_key = BlizzardArchive::Listfile::FileKey(spec_filename);

  std::unique_ptr<BlizzardArchive::ClientFile> file;
  try
  {
    file = std::make_unique<BlizzardArchive::ClientFile>(load_key, client_data);
  }
  catch (BlizzardArchive::Exceptions::FileReadFailedError const&)
  {
    std::string fallback_path;
    if (has_specular)
      fallback_path = _file_key.filepath();
    else if (_file_key.hasFilepath())
    {
      auto const& path = _file_key.filepath();
      if (path.size() > 6 && path.compare(path.size() - 6, 6, "_s.blp") == 0)
        fallback_path = path.substr(0, path.size() - 6) + ".blp";
    }

    if (fallback_path.empty())
      throw;

    file = std::make_unique<BlizzardArchive::ClientFile>(fallback_path, client_data);
    _is_specular = false;
    LogDebug << "Using diffuse texture '" << fallback_path
             << "' because the specular variant could not be read." << std::endl;
  }

  auto& f = *file;
  if (f.isEof())
  {
    throw std::runtime_error ("File " + _file_key.stringRepr() + " does not exist");
  }

  char const* lData = f.getPointer();
  if (f.getSize() < sizeof(BLPHeader))
    throw std::runtime_error("Truncated BLP header: " + _file_key.stringRepr());
  BLPHeader const* lHeader = reinterpret_cast<BLPHeader const*>(lData);
  _width = lHeader->resx;
  _height = lHeader->resy;

  if (client_data->version() == BlizzardArchive::ClientVersion::RETAIL && lHeader->attr_0_compression == 3)
  {
    auto decoded = Noggit::Format::read_bgra_blp(lData, f.getSize());
    _width = static_cast<int>(decoded.width);
    _height = static_cast<int>(decoded.height);
    _compression_format.reset();
    _compressed_data.clear();
    _data = std::move(decoded.mipmaps);
  }
  else if (lHeader->attr_0_compression == 1)
  {
    loadFromUncompressedData(lHeader, lData);
  }
  else if (lHeader->attr_0_compression == 2)
  {
    loadFromCompressedData(lHeader, lData);
  }
  else
  {
      BlizzardArchive::ClientFile fallback("textures/shanecube.blp", Noggit::Application::NoggitApplication::instance()->clientData());

      char const* lData_f = fallback.getPointer();
      BLPHeader const* lHeader_f = reinterpret_cast<BLPHeader const*>(lData_f);
      _width = lHeader_f->resx;
      _height = lHeader_f->resy;

      if (lHeader_f->attr_0_compression == 1)
      {
          loadFromUncompressedData(lHeader_f, lData_f);
      }
      else if (lHeader_f->attr_0_compression == 2)
      {
          loadFromCompressedData(lHeader_f, lData_f);
      }
      else
      {
          throw std::logic_error("Unsupported BLP compression");
      }

      fallback.close();

      LogError << "Unsupported BLP compression: " << _file_key.stringRepr() << std::endl;
  }

  f.close();
  complete_loading();
}

namespace Noggit
{
    BLPRenderer& BLPRenderer::getInstance()
    {
        static BLPRenderer  instance;
        return instance;
    }

    QPixmap* BLPRenderer::render_blp_to_pixmap ( std::string const& blp_filename
                                               , int width
                                               , int height
                                               )
  {
    if (!_uploaded)
    [[unlikely]]
    {
      upload();
    }

    std::tuple<std::string, int, int> const curEntry{blp_filename, width, height};
    auto it{_cache.find(curEntry)};

    if(it != _cache.end())
      return &it->second;

    OpenGL::context::save_current_context const context_save (::gl);

    _context->makeCurrent(_surface.get());

    OpenGL::context::scoped_setter const context_set (::gl, _context.get());

    gl.activeTexture(GL_TEXTURE0);
    blp_texture texture(blp_filename, Noggit::NoggitRenderContext::BLP_RENDERER);
    texture.finishLoading();
    texture.upload();

    width = width == -1 ? texture.width() : width;
    height = height == -1 ? texture.height() : height;

    float h = static_cast<float>(height);
    float w = static_cast<float>(width);

    QOpenGLFramebufferObject pixel_buffer(width, height, *_fmt.get());
    pixel_buffer.bind();

    gl.viewport(0, 0, w, h);
    gl.clearColor(.0f, .0f, .0f, 1.f);
    gl.clear(GL_COLOR_BUFFER_BIT);
    
    OpenGL::Scoped::use_program shader (*_program.get());

    shader.uniform("tex", 0);
    shader.uniform("width", w);
    shader.uniform("height", h);

    gl.bindTexture(GL_TEXTURE_2D_ARRAY, texture.texture_array());
    shader.uniform("tex_index", texture.array_index());

    OpenGL::Scoped::vao_binder const _ (_vao[0]);
    
    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_buffers[0]);

    gl.drawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);

    QPixmap result{};
    result = std::move(QPixmap::fromImage(pixel_buffer.toImage()));
    pixel_buffer.release();

    if (result.isNull())
    {
      throw std::runtime_error
        ("failed rendering " + blp_filename + " to pixmap");
    }

    return &(_cache[curEntry] = std::move(result));
  }

  void BLPRenderer::upload()
  {
    _cache = {};

    OpenGL::context::save_current_context const context_save (::gl);

    _context = std::make_unique<QOpenGLContext>();
    _fmt = std::make_unique<QOpenGLFramebufferObjectFormat>();
    _surface = std::make_unique<QOffscreenSurface>();

    _context->setFormat(QSurfaceFormat::defaultFormat());
    _surface->setFormat(QSurfaceFormat::defaultFormat());

    if (auto* share_context = QOpenGLContext::globalShareContext())
    {
      _context->setShareContext(share_context);
    }

    _context->create();

    _fmt->setSamples(1);
    _fmt->setInternalTextureFormat(GL_RGBA8);

    _surface->create();
    _context->makeCurrent(_surface.get());

    OpenGL::context::scoped_setter const context_set (::gl, _context.get());

    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> cull;
    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_FALSE> depth;

    _vao.upload();
    _buffers.upload();

    GLuint const& indices_vbo = _buffers[0];
    GLuint const& vertices_vbo = _buffers[1];
    GLuint const& texcoords_vbo = _buffers[2];

    std::vector<glm::vec2> vertices =
        {
             {-1.0f, -1.0f}
            ,{-1.0f, 1.0f}
            ,{ 1.0f, 1.0f}
            ,{ 1.0f, -1.0f}
        };
    std::vector<glm::vec2> texcoords =
        {
             {0.f, 0.f}
            ,{0.f, 1.0f}
            ,{1.0f, 1.0f}
            ,{1.0f, 0.f}
        };
    std::vector<std::uint16_t> indices = {0,1,2, 2,3,0};

    gl.bufferData<GL_ARRAY_BUFFER,glm::vec2>(vertices_vbo, vertices, GL_STATIC_DRAW);
    gl.bufferData<GL_ARRAY_BUFFER,glm::vec2>(texcoords_vbo, texcoords, GL_STATIC_DRAW);
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(indices_vbo, indices, GL_STATIC_DRAW);


    _program.reset(new OpenGL::program
                       (
                           {
                               {
                                   GL_VERTEX_SHADER, R"code(
                                  #version 330 core

                                  in vec4 position;
                                  in vec2 tex_coord;
                                  out vec2 f_tex_coord;

                                  uniform float width;
                                  uniform float height;

                                  void main()
                                  {
                                    f_tex_coord = vec2(tex_coord.x * width, -tex_coord.y * height);
                                    gl_Position = vec4(position.x * width / 2, position.y * height / 2, position.z, 1.0);
                                  }
                                  )code"
                               },
                               {
                                   GL_FRAGMENT_SHADER, R"code(
                                  #version 330 core

                                  uniform sampler2DArray tex;
                                  uniform int tex_index;

                                  in vec2 f_tex_coord;

                                  layout(location = 0) out vec4 out_color;

                                  void main()
                                  {
                                    out_color = vec4(texture(tex, vec3(f_tex_coord/2.f + vec2(0.5), tex_index)).rgb, 1.);
                                  }
                                  )code"
                               }
                           }
                       ));

    OpenGL::Scoped::use_program shader (*_program.get());

    OpenGL::Scoped::vao_binder const _ (_vao[0]);

    {
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> vertices_binder (vertices_vbo);
      shader.attrib("position", 2, GL_FLOAT, GL_FALSE, 0, 0);
    }
    {
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> texcoords_binder (texcoords_vbo);
      shader.attrib("tex_coord", 2, GL_FLOAT, GL_FALSE, 0, 0);
    }

    _uploaded = true;
  }

  void BLPRenderer::unload()
  {
    if (!_uploaded)
    {
      return;
    }

    if (!_context || !_surface)
    {
      return;
    }

    OpenGL::context::save_current_context const context_save (::gl);

    if (!_context->makeCurrent(_surface.get()))
    {
      LogError << "BLP preview cleanup was skipped because the offscreen OpenGL context could not be made current." << std::endl;
      return;
    }

    OpenGL::context::scoped_setter const context_set (::gl, _context.get());

    _cache.clear();
    _vao.unload();
    _buffers.unload();
    _program.reset();
    _surface.reset();
    _fmt.reset();
    _context.reset();

    _uploaded = false;
  }

}
