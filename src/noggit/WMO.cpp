// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <ClientFile.hpp>
#include <math/frustum.hpp>
#include <math/ray.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h> // LogDebug
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/TextureManager.h> // TextureManager, Texture
#include <noggit/WMO.h>
#include <noggit/wmo_liquid.hpp>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>


WMO::WMO(BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)
  : AsyncObject(file_key)
  , _context(context)
  , _renderer(this)
{
}

WMO::~WMO()
{
}

void WMO::finishLoading ()
{
  BlizzardArchive::ClientFile f(_file_key.filepath(), Noggit::Application::NoggitApplication::instance()->clientData());
  if (f.isEof()) {
    // mark as failed instead of leaving the object un-finished forever, which
    // silently skips every instance and hangs wait_until_loaded()
    error_on_loading();
    return;
  }

  struct RootChunk
  {
    std::uint32_t tag;
    std::size_t payload_offset;
    std::uint32_t size;
  };

  std::vector<RootChunk> root_chunks;
  std::vector<std::uint32_t> modern_doodad_file_ids;
  for (std::size_t chunk_offset = 0; chunk_offset + 8 <= f.getSize();)
  {
    std::uint32_t chunk_tag = 0;
    std::uint32_t chunk_size = 0;
    std::memcpy(&chunk_tag, f.getBuffer() + chunk_offset, sizeof(chunk_tag));
    std::memcpy(&chunk_size, f.getBuffer() + chunk_offset + 4, sizeof(chunk_size));

    auto const payload_offset = chunk_offset + 8;
    if (chunk_size > f.getSize() - payload_offset)
      throw std::runtime_error("Invalid WMO chunk size in \"" + _file_key.stringRepr() + "\".");

    root_chunks.push_back({chunk_tag, payload_offset, chunk_size});

    if (chunk_tag == 'MODI' && chunk_size % sizeof(std::uint32_t) == 0)
    {
      modern_doodad_file_ids.resize(chunk_size / sizeof(std::uint32_t));
      std::memcpy(modern_doodad_file_ids.data(), f.getBuffer() + payload_offset, chunk_size);
    }

    chunk_offset = payload_offset + chunk_size;
  }

  auto const find_root_chunk = [&root_chunks](std::uint32_t tag) -> RootChunk const*
  {
    auto const it = std::find_if(root_chunks.begin(), root_chunks.end(),
      [tag](RootChunk const& chunk) { return chunk.tag == tag; });
    return it == root_chunks.end() ? nullptr : &*it;
  };

  uint32_t fourcc;
  uint32_t size;

  float ff[3];

  char const* ddnames = nullptr;
  char const* groupnames = nullptr;

  // - MVER ----------------------------------------------

  uint32_t version;

  f.read (&fourcc, 4);
  f.seekRelative (4);
  f.read (&version, 4);

  assert (fourcc == 'MVER' && version == 17);

  // - MOHD ----------------------------------------------

  f.read (&fourcc, 4);
  f.seekRelative (4);

  assert (fourcc == 'MOHD');

  CArgb ambient_color;
  unsigned int nTextures, nGroups, nP, nLights, nModels, nDoodads, nDoodadSets;
  // header
  f.read (&nTextures, 4);
  f.read (&nGroups, 4);
  f.read (&nP, 4);
  f.read (&nLights, 4);
  f.read (&nModels, 4);
  f.read (&nDoodads, 4);
  f.read (&nDoodadSets, 4);
  f.read (&ambient_color, 4);
  f.read (&WmoId, 4);
  f.read (ff, 12);
  extents[0] = ::glm::vec3 (ff[0], ff[1], ff[2]);
  f.read (ff, 12);
  extents[1] = ::glm::vec3 (ff[0], ff[1], ff[2]);
  f.read(&flags, 2);

  f.seekRelative (2);

  ambient_light_color.x = static_cast<float>(ambient_color.r) / 255.f;
  ambient_light_color.y = static_cast<float>(ambient_color.g) / 255.f;
  ambient_light_color.z = static_cast<float>(ambient_color.b) / 255.f;
  ambient_light_color.w = static_cast<float>(ambient_color.a) / 255.f;

  // Modern WMOs store FileDataIDs directly in MOMT and omit MOTX.
  f.read (&fourcc, 4);
  f.read (&size, 4);

  bool const uses_file_data_ids = fourcc == 'MOMT';
  _uses_file_data_ids = uses_file_data_ids;
  std::vector<char> texbuf;

  if (!uses_file_data_ids)
  {
    if (fourcc != 'MOTX' || size > f.getSize() - f.getPos())
      throw std::runtime_error("Invalid WMO texture-name chunk in \"" + _file_key.stringRepr() + "\".");

    texbuf.resize(size);
    f.read(texbuf.data(), texbuf.size());

    f.read(&fourcc, 4);
    f.read(&size, 4);
  }

  if (fourcc != 'MOMT' || size > f.getSize() - f.getPos() || size % sizeof(WMOMaterial) != 0)
    throw std::runtime_error("Invalid WMO material chunk in \"" + _file_key.stringRepr() + "\".");

  std::size_t const num_materials (size / 0x40);
  materials.resize (num_materials);

  // note: used to map to size_t, but our other values don't support that.
  //std::map<std::uint32_t, std::size_t> texture_offset_to_inmem_index;
  std::map<std::uint32_t, std::uint32_t> texture_offset_to_inmem_index;

  auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();

  auto load_texture
    ( [&] (std::uint32_t texture_ref)
      {
        std::string texture = "textures/shanecube.blp";
        if (uses_file_data_ids)
        {
          if (texture_ref != 0)
          {
            auto const resolved = client_data->listfile()->getPath(texture_ref);
            if (!resolved.empty())
              texture = resolved;
            else
              LogError << "Unresolved WMO texture FileDataID " << texture_ref
                       << " in \"" << _file_key.stringRepr() << "\"." << std::endl;
          }
        }
        else if (texture_ref < texbuf.size() && texbuf[texture_ref])
        {
          texture = &texbuf[texture_ref];
        }

        auto const mapping
          (texture_offset_to_inmem_index.emplace(texture_ref, static_cast<std::uint32_t>(textures.size())));

        if (mapping.second)
        {
          textures.emplace_back(texture, _context);
        }
        return mapping.first->second;
      }
    );

  for (size_t i(0); i < num_materials; ++i)
  {
    f.read(&materials[i], sizeof(WMOMaterial));

    uint32_t shader = materials[i].shader;
    bool use_second_texture = (shader == 6 || shader == 5 || shader == 3 || shader == 21 || shader == 23);

    materials[i].texture1 = load_texture(materials[i].texture_offset_1);
    if (use_second_texture)
    {
      materials[i].texture2 = load_texture(materials[i].texture_offset_2);
    }
  }

  if (uses_file_data_ids)
  {
    auto const require_chunk = [&](std::uint32_t tag, char const* name) -> RootChunk const&
    {
      auto const* chunk = find_root_chunk(tag);
      if (!chunk)
        throw std::runtime_error("Modern WMO is missing " + std::string(name)
          + " in \"" + _file_key.stringRepr() + "\".");
      return *chunk;
    };

    auto const& mogn = require_chunk('MOGN', "MOGN");
    groupnames = reinterpret_cast<char const*>(f.getBuffer() + mogn.payload_offset);

    auto const& mogi = require_chunk('MOGI', "MOGI");
    if (mogi.size < static_cast<std::size_t>(nGroups) * 0x20)
      throw std::runtime_error("Truncated modern WMO MOGI in \"" + _file_key.stringRepr() + "\".");
    f.seek(mogi.payload_offset);
    groups.reserve(nGroups);
    for (unsigned int i = 0; i < nGroups; ++i)
      groups.emplace_back(this, &f, i, groupnames);

    if (auto const* mosb = find_root_chunk('MOSB'); mosb && mosb->size > 4)
    {
      auto const* skybox_name = reinterpret_cast<char const*>(f.getBuffer() + mosb->payload_offset);
      auto const max_length = static_cast<std::size_t>(mosb->size);
      auto const* terminator = static_cast<char const*>(std::memchr(skybox_name, '\0', max_length));
      if (terminator)
      {
        auto path = BlizzardArchive::ClientData::normalizeFilenameInternal(
          std::string(skybox_name, terminator));
        auto const extension = path.rfind(".mdx");
        if (extension != std::string::npos)
          path.replace(extension, 4, ".m2");
        if (!path.empty() && client_data->exists(path))
          skybox = scoped_model_reference(path, _context);
      }
    }

    auto const& molt = require_chunk('MOLT', "MOLT");
    if (molt.size < static_cast<std::size_t>(nLights) * 0x30)
      throw std::runtime_error("Truncated modern WMO MOLT in \"" + _file_key.stringRepr() + "\".");
    f.seek(molt.payload_offset);
    lights.reserve(nLights);
    for (size_t i = 0; i < nLights; ++i)
    {
      WMOLight light;
      light.init(&f);
      lights.push_back(light);
    }

    auto const& mods = require_chunk('MODS', "MODS");
    if (mods.size < static_cast<std::size_t>(nDoodadSets) * 0x20)
      throw std::runtime_error("Truncated modern WMO MODS in \"" + _file_key.stringRepr() + "\".");
    f.seek(mods.payload_offset);
    doodadsets.reserve(nDoodadSets);
    for (size_t i = 0; i < nDoodadSets; ++i)
    {
      WMODoodadSet doodad_set;
      f.read(&doodad_set, 0x20);
      doodadsets.push_back(doodad_set);
    }

    auto const& modd = require_chunk('MODD', "MODD");
    if (modd.size % 0x28 != 0 || (modd.size && modern_doodad_file_ids.empty()))
      throw std::runtime_error("Invalid modern WMO MODD/MODI in \"" + _file_key.stringRepr() + "\".");
    f.seek(modd.payload_offset);
    modelis.reserve(modd.size / 0x28);
    for (size_t i = 0; i < modd.size / 0x28; ++i)
    {
      struct
      {
        uint32_t name_offset : 24;
        uint32_t flag_AcceptProjTex : 1;
        uint32_t flag_0x2 : 1;
        uint32_t flag_0x4 : 1;
        uint32_t flag_0x8 : 1;
        uint32_t flags_unused : 4;
      } entry_header;

      auto const after_entry = f.getPos() + 0x28;
      f.read(&entry_header, sizeof(entry_header));
      auto const file_data_id = entry_header.name_offset < modern_doodad_file_ids.size()
        ? modern_doodad_file_ids[entry_header.name_offset]
        : 0;
      auto const path = client_data->listfile()->getPath(file_data_id);
      BlizzardArchive::Listfile::FileKey doodad_key(
        path.empty() ? "unknown/" + std::to_string(file_data_id) + ".m2" : path,
        file_data_id);
      modelis.emplace_back(doodad_key, &f, _context);
      model_nearest_light_vector.emplace_back();
      f.seek(after_entry);
    }

    auto const& mfog = require_chunk('MFOG', "MFOG");
    if (mfog.size % 0x30 != 0)
      throw std::runtime_error("Invalid modern WMO MFOG in \"" + _file_key.stringRepr() + "\".");
    f.seek(mfog.payload_offset);
    fogs.reserve(mfog.size / 0x30);
    for (size_t i = 0; i < mfog.size / 0x30; ++i)
    {
      WMOFog fog;
      fog.init(&f);
      fogs.push_back(std::move(fog));
    }

    Log << "[ModernWMO] Resolved root \"" << _file_key.stringRepr()
        << "\": material textures=" << textures.size()
        << ", doodad instances=" << modelis.size()
        << ", MODI entries=" << modern_doodad_file_ids.size() << std::endl;

    for (auto& group : groups)
      group.load();

    finished = true;
    _state_changed.notify_all();
    return;
  }

  // - MOGN ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOGN');

  groupnames = reinterpret_cast<char const*> (f.getPointer ());

  f.seekRelative (size);

  // - MOGI ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOGI');

  groups.reserve(nGroups);
  for (unsigned int i (0); i < nGroups; ++i) {
    groups.emplace_back (this, &f, i, groupnames);
  }

  // - MOSB ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOSB');

  if (size > 4)
  {
    std::string path = BlizzardArchive::ClientData::normalizeFilenameInternal(std::string (reinterpret_cast<char const*>(f.getPointer ())));
    auto from = std::string("mdx");
    auto to = std::string("m2");
    size_t start_pos = 0;
    while ((start_pos = path.find(from, start_pos)) != std::string::npos) {
        path.replace(start_pos, from.length(), to);
        start_pos += to.length(); // Handles case where 'to' is a substring of 'from'
    }

    if (path.length())
    {
      if (Noggit::Application::NoggitApplication::instance()->clientData()->exists(path))
      {
        skybox = scoped_model_reference(path, _context);
      }
    }
  }

  f.seekRelative (size);

  // - MOPV ----------------------------------------------

  f.read (&fourcc, 4);
  f.read(&size, 4);

  assert (fourcc == 'MOPV');

  f.seekRelative (size);

  /*
  std::vector<glm::vec3> portal_vertices;

  for (size_t i (0); i < size / 12; ++i) {
    f.read (ff, 12);
    portal_vertices.push_back(glm::vec3(ff[0], ff[2], -ff[1]));
  }

   */

  // - MOPT ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOPT');

  f.seekRelative (size);

  // - MOPR ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert(fourcc == 'MOPR');

  f.seekRelative (size);

  // - MOVV ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOVV');

  f.seekRelative (size);

  // - MOVB ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOVB');

  f.seekRelative (size);

  // - MOLT ----------------------------------------------

  f.read (&fourcc, 4);
  f.seekRelative (4);

  assert (fourcc == 'MOLT');

  lights.reserve(nLights);
  for (size_t i (0); i < nLights; ++i) {
    WMOLight l;
    l.init (&f);
    lights.push_back (l);
  }

  // - MODS ----------------------------------------------

  f.read (&fourcc, 4);
  f.seekRelative (4);

  assert (fourcc == 'MODS');

  doodadsets.reserve(nDoodadSets);
  for (size_t i (0); i < nDoodadSets; ++i) {
    WMODoodadSet dds;
    f.read (&dds, 32);
    doodadsets.push_back (dds);
  }

  // Modern WMOs omit MODN and store a FileDataID in each MODD entry.
  f.read (&fourcc, 4);
  f.read (&size, 4);

  bool const doodads_use_file_data_ids = fourcc == 'MODD';
  if (!doodads_use_file_data_ids)
  {
    if (fourcc != 'MODN' || size > f.getSize() - f.getPos())
      throw std::runtime_error("Invalid WMO doodad-name chunk in \"" + _file_key.stringRepr() + "\".");

    if (size)
      ddnames = reinterpret_cast<char const*> (f.getPointer ());
    f.seekRelative (size);

    f.read(&fourcc, 4);
    f.read(&size, 4);
  }

  if (fourcc != 'MODD' || size > f.getSize() - f.getPos() || size % 0x28 != 0)
    throw std::runtime_error("Invalid WMO doodad chunk in \"" + _file_key.stringRepr() + "\".");

  if (doodads_use_file_data_ids && size && modern_doodad_file_ids.empty())
    throw std::runtime_error("Modern WMO is missing its MODI table in \"" + _file_key.stringRepr() + "\".");

  modelis.reserve(size / 0x28);
  for (size_t i (0); i < size / 0x28; ++i)
  {
    struct
    {
      uint32_t name_offset : 24;
      uint32_t flag_AcceptProjTex : 1;
      uint32_t flag_0x2 : 1;
      uint32_t flag_0x4 : 1;
      uint32_t flag_0x8 : 1;
      uint32_t flags_unused : 4;
    } x;

    size_t after_entry (f.getPos() + 0x28);
    f.read (&x, sizeof (x));

    BlizzardArchive::Listfile::FileKey doodad_key;
    if (doodads_use_file_data_ids)
    {
      auto const file_data_id = x.name_offset < modern_doodad_file_ids.size()
        ? modern_doodad_file_ids[x.name_offset]
        : 0;
      auto const path = client_data->listfile()->getPath(file_data_id);
      doodad_key = BlizzardArchive::Listfile::FileKey(
        path.empty() ? "unknown/" + std::to_string(file_data_id) + ".m2" : path,
        file_data_id);
    }
    else
    {
      if (!ddnames)
        throw std::runtime_error("Missing WMO doodad names in \"" + _file_key.stringRepr() + "\".");
      doodad_key = BlizzardArchive::Listfile::FileKey(ddnames + x.name_offset);
    }

    modelis.emplace_back(doodad_key, &f, _context);
    model_nearest_light_vector.emplace_back();

    f.seek (after_entry);
  }

  if (uses_file_data_ids || doodads_use_file_data_ids)
  {
    Log << "[ModernWMO] Resolved root \"" << _file_key.stringRepr()
        << "\": material textures=" << textures.size()
        << ", doodad instances=" << modelis.size()
        << ", MODI entries=" << modern_doodad_file_ids.size() << std::endl;
  }

  // - MFOG ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MFOG');

  int nfogs = size / 0x30;
  fogs.reserve(nfogs);

  for (size_t i (0); i < nfogs; ++i)
  {
    WMOFog fog;
    fog.init (&f);
    fogs.push_back (std::move(fog));
  }

  for (auto& group : groups)
    group.load();

  finished = true;
  _state_changed.notify_all();
}

void WMO::waitForChildrenLoaded()
{
  for (auto& tex : textures)
  {
    tex.get()->wait_until_loaded();
  }

  for (auto& doodad : modelis)
  {
    doodad.model->wait_until_loaded();
    doodad.model->waitForChildrenLoaded();
  }
}

std::vector<float> WMO::intersect (math::ray const& ray, bool do_exterior, bool do_interior, bool first_occurence) const
{
  std::vector<float> results;

  if (!finishedLoading() || loading_failed())
  {
    return results;
  }

  for (auto& group : groups)
  {
    if (!do_exterior && !group.is_indoor())
      continue;

    else if (!do_interior && group.is_indoor())
      continue;

    group.intersect (ray, &results, first_occurence);
  }

  if (!do_exterior && results.size())
  {
      // dirty way to find the furthest face and ignore back culled(invisible) faces, cleaner way would be to do a direction check on faces
      // float max = *std::max_element(std::begin(results), std::end(results));
      // results.clear();
      // results.push_back(max);

      // other way, ignore the closest intersect, works well
      if (results.size() > 1)
      {
        auto it = std::min_element(results.begin(), results.end());
        results.erase(it);
      }
  }

  return results;
}

std::map<uint32_t, std::vector<wmo_doodad_instance>> WMO::doodads_per_group(uint16_t doodadset) const
{
  std::map<uint32_t, std::vector<wmo_doodad_instance>> doodads;

  if (doodadset >= doodadsets.size())
  {
    LogError << "Invalid doodadset for instance of wmo " << _file_key.stringRepr() << std::endl;
    return doodads;
  }

  auto const& dset = doodadsets[doodadset];
  uint32_t start = dset.start, end = start + dset.size;

  // the client always instantiates set 0 (Set_$DefaultGlobal) in addition to
  // the placement's selected set (CMap::CreateMapObjDefGroupDoodads)
  uint32_t start0 = 0, end0 = 0;
  if (doodadset != 0)
  {
    auto const& dset0 = doodadsets[0];
    start0 = dset0.start;
    end0 = start0 + dset0.size;
  }

  for (int i = 0; i < groups.size(); ++i)
  {
    for (uint16_t ref : groups[i].doodad_ref())
    {
      if ((ref >= start && ref < end) || (ref >= start0 && ref < end0))
      {
        doodads[i].push_back(modelis[ref]);
      }
    }
  }

  return doodads;
}

[[nodiscard]]
bool WMO::is_hidden() const
{
  return _hidden;
}

void WMO::toggle_visibility()
{
  _hidden = !_hidden;
}

void WMO::show()
{
  _hidden = false;
}

void WMO::hide()
{
  _hidden = true;
}

[[nodiscard]]
bool WMO::is_required_when_saving() const
{
  return true;
}

[[nodiscard]]
Noggit::Rendering::WMORender* WMO::renderer()
{
  return &_renderer;
}

void WMOLight::init(BlizzardArchive::ClientFile* f)
{
  char type[4];
  f->read(&type, 4);
  f->read(&color, 4);
  f->read(&pos, 12);
  f->read(&intensity, 4);
  f->read(unk, 4 * 5);
  f->read(&r, 4);

  pos = glm::vec3(pos.x, pos.z, -pos.y);

  // rgb? bgr? hm
  float fa = ((color & 0xff000000) >> 24) / 255.0f;
  float fr = ((color & 0x00ff0000) >> 16) / 255.0f;
  float fg = ((color & 0x0000ff00) >> 8) / 255.0f;
  float fb = ((color & 0x000000ff)) / 255.0f;

  fcolor = glm::vec4(fr, fg, fb, fa);
  fcolor *= intensity;
  fcolor.w = 1.0f;

  /*
  // light logging
  gLog("Light %08x @ (%4.2f,%4.2f,%4.2f)\t %4.2f, %4.2f, %4.2f, %4.2f, %4.2f, %4.2f, %4.2f\t(%d,%d,%d,%d)\n",
  color, pos.x, pos.y, pos.z, intensity,
  unk[0], unk[1], unk[2], unk[3], unk[4], r,
  type[0], type[1], type[2], type[3]);
  */
}

void WMOLight::setup(GLint)
{
  // not used right now -_-
}

void WMOLight::setupOnce(GLint, glm::vec3, glm::vec3)
{
  //glm::vec4position(dir, 0);
  //glm::vec4position(0,1,0,0);

  //glm::vec4ambient = glm::vec4(light_color * 0.3f, 1);
  //glm::vec4diffuse = glm::vec4(light_color, 1);


  //gl.enable(light);
}



WMOGroup::WMOGroup(WMO *_wmo, BlizzardArchive::ClientFile* f, int _num, char const* names)
  : wmo(_wmo)
  , num(_num)
  , _renderer(this)
{
  // extract group info from f
  std::uint32_t flags; // not used, the flags are in the group header
  f->read(&flags, 4);
  float ff[3];
  f->read(ff, 12);
  VertexBoxMax = glm::vec3(ff[0], ff[1], ff[2]);
  f->read(ff, 12);
  VertexBoxMin = glm::vec3(ff[0], ff[1], ff[2]);
  int nameOfs;
  f->read(&nameOfs, 4);

  //! \todo  get proper name from group header and/or dbc?
  if (nameOfs > 0) {
    name = std::string(names + nameOfs);
  }
  else name = "(no name)";
}

WMOGroup::WMOGroup(WMOGroup const& other)
  : BoundingBoxMin(other.BoundingBoxMin)
  , BoundingBoxMax(other.BoundingBoxMax)
  , VertexBoxMin(other.VertexBoxMin)
  , VertexBoxMax(other.VertexBoxMax)
  , use_outdoor_lights(other.use_outdoor_lights)
  , name(other.name)
  , wmo(other.wmo)
  , header(other.header)
  , center(other.center)
  , rad(other.rad)
  , num(other.num)
  , fog(other.fog)
  , _doodad_ref(other._doodad_ref)
  , _batches(other._batches)
  , _vertices(other._vertices)
  , _normals(other._normals)
  , _texcoords(other._texcoords)
  , _texcoords_2(other._texcoords_2)
  , _vertex_colors(other._vertex_colors)
  , _indices(other._indices)
  , _renderer(this)
{
  if (other.lq)
  {
    lq = std::make_unique<wmo_liquid>(*other.lq.get());
  }
}

namespace
{
  glm::vec4 colorFromInt(unsigned int col)
  {
    GLubyte r, g, b, a;
    a = (col & 0xFF000000) >> 24;
    r = (col & 0x00FF0000) >> 16;
    g = (col & 0x0000FF00) >> 8;
    b = (col & 0x000000FF);
    return glm::vec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
  }
}


void WMOGroup::load()
{
  // open group file
  std::stringstream curNum;
  curNum << "_" << std::setw (3) << std::setfill ('0') << num;

  std::string fname = wmo->file_key().filepath();
  fname.insert (fname.find (".wmo"), curNum.str ());

  BlizzardArchive::ClientFile f(fname, Noggit::Application::NoggitApplication::instance()->clientData());
  if (f.isEof()) {
    LogError << "Error loading WMO \"" << fname << "\"." << std::endl;
    return;
  }

  uint32_t fourcc;
  uint32_t size;

  // - MVER ----------------------------------------------

  f.read (&fourcc, 4);
  f.seekRelative (4);

  uint32_t version;

  f.read (&version, 4);

  assert (fourcc == 'MVER' && version == 17);

  // - MOGP ----------------------------------------------

  f.read (&fourcc, 4);
  f.seekRelative (4);

  assert (fourcc == 'MOGP');

  f.read (&header, sizeof (wmo_group_header));

  unsigned fog_index = header.fogs[0];

  // downport hack
  if (fog_index >= wmo->fogs.size())
  {
      fog_index = 0;
  }
  WMOFog &wf = wmo->fogs[fog_index];

  if (wf.r2 <= 0) fog = -1; // default outdoor fog..?
  else fog = header.fogs[0];

  BoundingBoxMin = ::glm::vec3 (header.box1[0], header.box1[2], -header.box1[1]);
  BoundingBoxMax = ::glm::vec3 (header.box2[0], header.box2[2], -header.box2[1]);

  struct GroupChunk
  {
    std::uint32_t tag;
    std::size_t payload_offset;
    std::uint32_t size;
  };

  std::vector<GroupChunk> group_chunks;
  for (std::size_t chunk_offset = f.getPos(); chunk_offset + 8 <= f.getSize();)
  {
    std::uint32_t chunk_tag = 0;
    std::uint32_t chunk_size = 0;
    std::memcpy(&chunk_tag, f.getBuffer() + chunk_offset, sizeof(chunk_tag));
    std::memcpy(&chunk_size, f.getBuffer() + chunk_offset + 4, sizeof(chunk_size));
    auto const payload_offset = chunk_offset + 8;
    if (chunk_size > f.getSize() - payload_offset)
      throw std::runtime_error("Invalid WMO group chunk size in \"" + fname + "\".");
    group_chunks.push_back({chunk_tag, payload_offset, chunk_size});
    chunk_offset = payload_offset + chunk_size;
  }

  auto const find_group_chunks = [&group_chunks](std::uint32_t tag)
  {
    std::vector<GroupChunk const*> matches;
    for (auto const& chunk : group_chunks)
      if (chunk.tag == tag)
        matches.push_back(&chunk);
    return matches;
  };

  bool const modern_chunk_layout = wmo->uses_file_data_ids()
    || !find_group_chunks('MOBS').empty()
    || !find_group_chunks('MOPB').empty();

  if (modern_chunk_layout)
  {
    auto const require_chunk = [&](std::uint32_t tag, char const* name) -> GroupChunk const&
    {
      auto const matches = find_group_chunks(tag);
      if (matches.empty())
        throw std::runtime_error("Modern WMO group is missing " + std::string(name)
          + " in \"" + fname + "\".");
      return *matches.front();
    };

    auto const& movi = require_chunk('MOVI', "MOVI");
    if (movi.size % sizeof(std::uint16_t) != 0)
      throw std::runtime_error("Invalid modern WMO MOVI in \"" + fname + "\".");
    _indices.resize(movi.size / sizeof(std::uint16_t));
    f.seek(movi.payload_offset);
    f.read(_indices.data(), movi.size);

    auto const& movt = require_chunk('MOVT', "MOVT");
    if (movt.size % sizeof(glm::vec3) != 0)
      throw std::runtime_error("Invalid modern WMO MOVT in \"" + fname + "\".");
    auto const* vertices = reinterpret_cast<glm::vec3 const*>(f.getBuffer() + movt.payload_offset);
    _vertices.resize(movt.size / sizeof(glm::vec3));
    VertexBoxMin = glm::vec3(std::numeric_limits<float>::max());
    VertexBoxMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (std::size_t i = 0; i < _vertices.size(); ++i)
    {
      _vertices[i] = glm::vec3(vertices[i].x, vertices[i].z, -vertices[i].y);
      VertexBoxMin = glm::min(VertexBoxMin, _vertices[i]);
      VertexBoxMax = glm::max(VertexBoxMax, _vertices[i]);
    }
    center = (VertexBoxMax + VertexBoxMin) * 0.5f;
    rad = glm::distance(center, VertexBoxMax);

    auto const& monr = require_chunk('MONR', "MONR");
    if (monr.size % sizeof(glm::vec3) != 0)
      throw std::runtime_error("Invalid modern WMO MONR in \"" + fname + "\".");
    _normals.resize(monr.size / sizeof(glm::vec3));
    f.seek(monr.payload_offset);
    f.read(_normals.data(), monr.size);
    for (auto& normal : _normals)
      normal = {normal.x, normal.z, -normal.y};

    auto const motv_chunks = find_group_chunks('MOTV');
    if (motv_chunks.empty())
    {
      Log << "[ModernWMOGroup] No MOTV chunk in \"" << fname
          << "\"; using zero texture coordinates for " << _vertices.size()
          << " vertices." << std::endl;
      _texcoords.assign(_vertices.size(), glm::vec2(0.f));
    }
    else
    {
      auto const primary_motv_bytes = motv_chunks.front()->size
        - (motv_chunks.front()->size % sizeof(glm::vec2));
      if (primary_motv_bytes == 0)
        throw std::runtime_error("Invalid modern WMO MOTV in \"" + fname + "\".");
      if (primary_motv_bytes != motv_chunks.front()->size)
        Log << "[ModernWMOGroup] Ignoring "
            << (motv_chunks.front()->size - primary_motv_bytes)
            << " trailing MOTV padding bytes in \"" << fname << "\"." << std::endl;
      _texcoords.resize(primary_motv_bytes / sizeof(glm::vec2));
      f.seek(motv_chunks.front()->payload_offset);
      f.read(_texcoords.data(), primary_motv_bytes);
      if (motv_chunks.size() > 1)
      {
        auto const secondary_motv_bytes = motv_chunks[1]->size
          - (motv_chunks[1]->size % sizeof(glm::vec2));
        if (secondary_motv_bytes == 0)
          throw std::runtime_error("Invalid modern WMO secondary MOTV in \"" + fname + "\".");
        if (secondary_motv_bytes != motv_chunks[1]->size)
          Log << "[ModernWMOGroup] Ignoring "
              << (motv_chunks[1]->size - secondary_motv_bytes)
              << " trailing secondary MOTV padding bytes in \"" << fname << "\"." << std::endl;
        _texcoords_2.resize(secondary_motv_bytes / sizeof(glm::vec2));
        f.seek(motv_chunks[1]->payload_offset);
        f.read(_texcoords_2.data(), secondary_motv_bytes);
      }
    }

    auto const moba_chunks = find_group_chunks('MOBA');
    if (moba_chunks.empty())
    {
      Log << "[ModernWMOGroup] No MOBA chunk in \"" << fname
          << "\"; treating the group as non-rendered geometry." << std::endl;
    }
    else
    {
      auto const& moba = *moba_chunks.front();
      if (moba.size % sizeof(wmo_batch) != 0)
        throw std::runtime_error("Invalid modern WMO MOBA in \"" + fname + "\".");
      _batches.resize(moba.size / sizeof(wmo_batch));
      f.seek(moba.payload_offset);
      f.read(_batches.data(), moba.size);
    }

    if (_normals.size() != _vertices.size())
    {
      LogError << "[ModernWMOGroup] MONR vertex count mismatch in \"" << fname
               << "\": vertices=" << _vertices.size() << " normals=" << _normals.size()
               << ". Padding/truncating normals." << std::endl;
      _normals.resize(_vertices.size(), glm::vec3(0.f, 1.f, 0.f));
    }
    if (_texcoords.size() != _vertices.size())
    {
      LogError << "[ModernWMOGroup] MOTV vertex count mismatch in \"" << fname
               << "\": vertices=" << _vertices.size() << " texcoords=" << _texcoords.size()
               << ". Padding/truncating texture coordinates." << std::endl;
      _texcoords.resize(_vertices.size(), glm::vec2(0.f));
    }

    auto const invalid_index = std::find_if(_indices.begin(), _indices.end(), [this](std::uint16_t index)
    {
      return index >= _vertices.size();
    });
    if (invalid_index != _indices.end())
      throw std::runtime_error("Modern WMO MOVI references a missing vertex in \"" + fname + "\".");

    auto const old_batch_count = _batches.size();
    _batches.erase(std::remove_if(_batches.begin(), _batches.end(), [this](wmo_batch const& batch)
    {
      auto const index_start = static_cast<std::size_t>(batch.index_start);
      auto const index_count = static_cast<std::size_t>(batch.index_count);
      return index_start > _indices.size()
        || index_count > _indices.size() - index_start
        || batch.vertex_start >= _vertices.size()
        || batch.vertex_end >= _vertices.size()
        || batch.vertex_end < batch.vertex_start;
    }), _batches.end());
    if (_batches.size() != old_batch_count)
      LogError << "[ModernWMOGroup] Dropped " << (old_batch_count - _batches.size())
               << " invalid render batches from \"" << fname << "\"." << std::endl;

    _renderer.initRenderBatches();

    auto const modr_chunks = find_group_chunks('MODR');
    if (!modr_chunks.empty())
    {
      auto const& modr = *modr_chunks.front();
      if (modr.size % sizeof(std::int16_t) != 0)
        throw std::runtime_error("Invalid modern WMO MODR in \"" + fname + "\".");
      _doodad_ref.resize(modr.size / sizeof(std::int16_t));
      f.seek(modr.payload_offset);
      f.read(_doodad_ref.data(), modr.size);
    }

    auto const mocv_chunks = find_group_chunks('MOCV');
    if (!mocv_chunks.empty())
    {
      f.seek(mocv_chunks.front()->payload_offset);
      load_mocv(f, mocv_chunks.front()->size);
    }
    if (mocv_chunks.size() > 1)
    {
      auto const& blend_colors = *mocv_chunks[1];
      if (blend_colors.size % sizeof(CImVector) != 0)
        throw std::runtime_error("Invalid modern WMO secondary MOCV in \"" + fname + "\".");
      auto const* colors = reinterpret_cast<CImVector const*>(f.getBuffer() + blend_colors.payload_offset);
      auto const color_count = blend_colors.size / sizeof(CImVector);
      if (_vertex_colors.empty())
        _vertex_colors.resize(color_count, glm::vec4(0.f));
      auto const count = std::min(_vertex_colors.size(), color_count);
      for (std::size_t i = 0; i < count; ++i)
        _vertex_colors[i].w = static_cast<float>(colors[i].a) / 255.f;
    }

    auto const mliq_chunks = find_group_chunks('MLIQ');
    if (!mliq_chunks.empty() && mliq_chunks.front()->size >= 0x1E)
    {
      auto const& mliq = *mliq_chunks.front();
      f.seek(mliq.payload_offset);
      WMOLiquidHeader liquid_header{};
      f.read(&liquid_header, 0x1E);

      bool valid_liquid = liquid_header.A > 0 && liquid_header.B > 0;
      std::size_t vertex_count = 0;
      std::size_t tile_count = 0;
      if (valid_liquid)
      {
        auto const width = static_cast<std::size_t>(liquid_header.A);
        auto const height = static_cast<std::size_t>(liquid_header.B);
        valid_liquid = width <= 512 && height <= 512
          && width <= (std::numeric_limits<std::size_t>::max() / height);
        if (valid_liquid)
        {
          tile_count = width * height;
          vertex_count = (width + 1) * (height + 1);
          auto const required_size = std::size_t{0x1E}
            + vertex_count * sizeof(WmoLiquidVertex)
            + tile_count * sizeof(SMOLTile);
          // Liquid rendering uses uint16 indices and emits four vertices per visible tile.
          valid_liquid = tile_count <= (std::numeric_limits<std::uint16_t>::max() / 4)
            && required_size <= mliq.size;
        }
      }

      if (valid_liquid)
      {
        lq = std::make_unique<wmo_liquid>(&f, liquid_header, header.group_liquid,
          static_cast<bool>(wmo->flags.use_liquid_type_dbc_id),
          static_cast<bool>(header.flags.ocean));
      }
      else
      {
        LogError << "[ModernWMOGroup] Skipping invalid MLIQ in \"" << fname
                 << "\": size=" << mliq.size << " dimensions=" << liquid_header.A
                 << "x" << liquid_header.B << "." << std::endl;
      }
    }

    Log << "[ModernWMOGroup] Loaded \"" << fname << "\": vertices="
        << _vertices.size() << ", indices=" << _indices.size()
        << ", batches=" << _batches.size() << ", chunks=" << group_chunks.size()
        << std::endl;
  }
  else
  {

  // - MOPY ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOPY');
  f.seekRelative (size);

  // - MOVI ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOVI');

  _indices.resize (size / sizeof (uint16_t));

  f.read (_indices.data (), size);

  // - MOVT ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOVT');

  // let's hope it's padded to 12 bytes, not 16...
  ::glm::vec3 const* vertices = reinterpret_cast< ::glm::vec3 const*>(f.getPointer ());

  VertexBoxMin = ::glm::vec3 (std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
  VertexBoxMax = ::glm::vec3 (std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());

  rad = 0;

  _vertices.resize(size / sizeof (::glm::vec3));

  for (size_t i = 0; i < _vertices.size(); ++i)
  {
    _vertices[i] = glm::vec3(vertices[i].x, vertices[i].z, -vertices[i].y);

    ::glm::vec3& v = _vertices[i];

    if (v.x < VertexBoxMin.x) VertexBoxMin.x = v.x;
    if (v.y < VertexBoxMin.y) VertexBoxMin.y = v.y;
    if (v.z < VertexBoxMin.z) VertexBoxMin.z = v.z;
    if (v.x > VertexBoxMax.x) VertexBoxMax.x = v.x;
    if (v.y > VertexBoxMax.y) VertexBoxMax.y = v.y;
    if (v.z > VertexBoxMax.z) VertexBoxMax.z = v.z;
  }

  center = (VertexBoxMax + VertexBoxMin) * 0.5f;
  rad = glm::distance(center, VertexBoxMax);

  f.seekRelative (size);

  // - MONR ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MONR');

  _normals.resize (size / sizeof (::glm::vec3));

  f.read (_normals.data(), size);

  for (auto& n : _normals)
  {
    n = {n.x, n.z, -n.y};
  }

  // - MOTV ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOTV');

  _texcoords.resize (size / sizeof (glm::vec2));

  f.read (_texcoords.data (), size);

  // - MOBA ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOBA');

  _batches.resize (size / sizeof (wmo_batch));
  f.read (_batches.data (), size);

  _renderer.initRenderBatches();

  // - MOLR ----------------------------------------------
  if (header.flags.has_light)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOLR')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

  }
  // - MODR ----------------------------------------------
  if (header.flags.has_doodads)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MODR')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      _doodad_ref.resize (size / sizeof (int16_t));
      f.read (_doodad_ref.data (), size);
    }

  }
  // - MOBN ----------------------------------------------
  if (header.flags.has_bsp_tree)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOBN')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative(size);
    }

  }
  // - MOBR ----------------------------------------------
  if (header.flags.has_bsp_tree)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOBR')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
      // std::vector<uint16_t> bsp_indices;
      // bsp_indices.resize(size / sizeof(uint16_t));
      // f.read(bsp_indices.data(), size);
      // _bsp_indices = bsp_indices;
    }
  }
  
  if (header.flags.flag_0x400)
  {
    // - MPBV ----------------------------------------------
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MPBV')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

    // - MPBP ----------------------------------------------
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MPBP')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

    // - MPBI ----------------------------------------------
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MPBI')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

    // - MPBG ----------------------------------------------
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MPBG')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {

      f.seekRelative (size);
    }
  }
  // - MOCV ----------------------------------------------
  if (header.flags.has_vertex_color)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOCV')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      load_mocv(f, size);
    }

  }
  // - MLIQ ----------------------------------------------
  if (header.flags.has_water)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MLIQ')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      WMOLiquidHeader hlq;
      f.read(&hlq, 0x1E);

      lq = std::make_unique<wmo_liquid> ( &f
          , hlq
          // , wmo->materials[hlq.material_id] // some models have mat_id = -1, eg "world/wmo/dungeon/md_fishinghole/md_fishingholeice_001.wmo"
          , header.group_liquid
          , (bool)wmo->flags.use_liquid_type_dbc_id
          , (bool)header.flags.ocean
      );

      // creating the wmo liquid doesn't move the position
      f.seekRelative(size - 0x1E);
    }

  }
  if (header.flags.has_mori_morb)
  {
    // - MORI ----------------------------------------------
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MORI')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

    // - MORB ----------------------------------------------
    f.read(&fourcc, 4);
    f.read(&size, 4);

    if (fourcc != 'MORB')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

  }

  // - MOTV ----------------------------------------------
  if (header.flags.has_two_motv)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOTV')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      _texcoords_2.resize(size / sizeof(glm::vec2));
      f.read(_texcoords_2.data(), size);
    }

  }
  // - MOCV ----------------------------------------------
  if (header.flags.use_mocv2_for_texture_blending)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOCV')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      std::vector<CImVector> mocv_2(size / sizeof(CImVector));
      f.read(mocv_2.data(), size);

      for (int i = 0; i < mocv_2.size(); ++i)
      {
        float alpha = static_cast<float>(mocv_2[i].a) / 255.f;

        // the second mocv is used for texture blending only
        if (header.flags.has_vertex_color)
        {
          _vertex_colors[i].w = alpha;
        }
        else // no vertex coloring, only texture blending with the alpha
        {
          _vertex_colors.emplace_back(0.f, 0.f, 0.f, alpha);
        }
      }
    }

  }
  }

  //dl_light = 0;
  // "real" lighting?
  if (header.flags.indoor && header.flags.has_vertex_color)
  {
    ::glm::vec3 dirmin(1, 1, 1);
    float lenmin;

    for (auto doodad : _doodad_ref)
    {
      if (doodad >= wmo->modelis.size())
      {
          continue;
          LogError << "The WMO file currently loaded is potentially corrupt. Non-existing doodad referenced." << std::endl;
      }

      lenmin = 999999.0f * 999999.0f;
      ModelInstance& mi = wmo->modelis[doodad];
      for (unsigned int j = 0; j < wmo->lights.size(); j++)
      {
        WMOLight& l = wmo->lights[j];
        ::glm::vec3 dir = l.pos - mi.pos;

        float ll = glm::length(dir) * glm::length(dir);
        if (ll < lenmin)
        {
          lenmin = ll;
          dirmin = dir;
        }
      }
      wmo->model_nearest_light_vector[doodad] = dirmin;
    }

    use_outdoor_lights = false;
  }
  else
  {
    use_outdoor_lights = true;
  }
}

void WMOGroup::load_mocv(BlizzardArchive::ClientFile& f, uint32_t size)
{
  if (size % sizeof(uint32_t) != 0)
  {
    LogError << "Invalid WMO MOCV size " << size << ". Ignoring trailing bytes." << std::endl;
  }

  uint32_t const* colors = reinterpret_cast<uint32_t const*> (f.getPointer());
  _vertex_colors.resize(size / sizeof(uint32_t));

  for (size_t i(0); i < size / sizeof(uint32_t); ++i)
  {
    _vertex_colors[i] = colorFromInt(colors[i]);
  }

  if (wmo->flags.do_not_fix_vertex_color_alpha)
  {
    int interior_batchs_start = 0;

    if (header.transparency_batches_count > 0 && header.transparency_batches_count <= _batches.size())
    {
      interior_batchs_start = _batches[header.transparency_batches_count - 1].vertex_end + 1;
    }

    for (int n = interior_batchs_start; n < _vertex_colors.size(); ++n)
    {
      _vertex_colors[n].w = header.flags.exterior ? 1.f : 0.f;
    }
  }
  else
  {
    fix_vertex_color_alpha();
  }

  // there's no read so this is required
  f.seekRelative(size);
}

void WMOGroup::fix_vertex_color_alpha()
{
  int interior_batchs_start = 0;

  if (header.transparency_batches_count > 0 && header.transparency_batches_count <= _batches.size())
  {
    interior_batchs_start = _batches[header.transparency_batches_count - 1].vertex_end + 1;
  }

  glm::vec4 wmo_ambient_color;

  if (wmo->flags.use_unified_render_path)
  {
    wmo_ambient_color = {0.f, 0.f, 0.f, 0.f};
  }
  else
  {
    wmo_ambient_color = wmo->ambient_light_color;
    // w is not used, set it to 0 to avoid changing the vertex color alpha
    wmo_ambient_color.w = 0.f;
  }

  for (int i = 0; i < _vertex_colors.size(); ++i)
  {
    auto& color = _vertex_colors[i];
    float r = color.x;
    float g = color.y;
    float b = color.z;
    float a = color.w;

    // I removed the color = color/2 because it's just multiplied by 2 in the shader afterward in blizzard's code
    if (i >= interior_batchs_start)
    {
      r += ((r * a / 64.f) - wmo_ambient_color.x);
      g += ((g * a / 64.f) - wmo_ambient_color.y);
      b += ((b * a / 64.f) - wmo_ambient_color.z);
    }
    else
    {
      r -= wmo_ambient_color.x;
      g -= wmo_ambient_color.y;
      b -= wmo_ambient_color.z;

      r = (r * (1.f - a));
      g = (g * (1.f - a));
      b = (b * (1.f - a));
    }

    color.x = std::min(255.f, std::max(0.f, r));
    color.y = std::min(255.f, std::max(0.f, g));
    color.z = std::min(255.f, std::max(0.f, b));
    color.w = 1.f; // default value used in the shader so I simplified it here,
                   // it can be overriden by the 2nd mocv chunk
  }
}

bool WMOGroup::is_visible( glm::mat4x4 const& transform
                         , math::frustum const& frustum
                         , float const& cull_distance
                         , glm::vec3 const& camera
                         , display_mode display
                         ) const
{
   // glm::vec3 pos = transform * glm::vec4(center, 0);
   // 
    // glm::vec3 pos = transform[3] * glm::vec4(center, 1.0f);
    // glm::vec3 test_pos = transform[3] + glm::vec4(center, 0);
    // glm::vec3 test_pos2 = transform[3];


    // TODO center is just the center of the group vertex box, and rad is distance from box max to center.
    // to do operation on group we need to get its true position
    // 
    // adjusted group transform mat = 
    glm::vec3 pos = transform *  glm::vec4(center, 1.0f);

  float dist = display == display_mode::in_3D
    ? glm::distance(pos, camera) - rad
    : std::abs(pos.y - camera.y) - rad;

  // Camera is within the bounding sphere, always draw
  if (dist < 0)
      return true;

  float cull = cull_distance;

  if (dist > cull_distance)
      return false;


  if (!frustum.intersects(pos + BoundingBoxMin, pos + BoundingBoxMax))
  {
    return false;
  }

  return true;
}

[[nodiscard]]
std::vector<uint16_t> WMOGroup::doodad_ref() const
{
  return _doodad_ref;
}

[[nodiscard]]
bool WMOGroup::has_skybox() const
{
  return header.flags.skybox;
}

[[nodiscard]]
bool WMOGroup::is_indoor() const
{
  return header.flags.indoor;
}

[[nodiscard]]
Noggit::Rendering::WMOGroupRender* WMOGroup::renderer()
{
  return &_renderer;
}

void WMOGroup::intersect (math::ray const& ray, std::vector<float>* results, bool first_occurence) const
{
  if (!ray.intersect_bounds (VertexBoxMin, VertexBoxMax))
  {
    return;
  }

  //! \todo Also allow clicking on doodads and liquids.
  for (auto&& batch : _batches)
  {
    for (int i (batch.index_start); i < batch.index_start + batch.index_count; i += 3)
    {
      // TODO : only intersect visible triangles
      // TODO : option to only check collision
      if ( auto&& distance
         = ray.intersect_triangle ( _vertices[_indices[i + 0]]
                                  , _vertices[_indices[i + 1]]
                                  , _vertices[_indices[i + 2]]
                                  )
         )
      {
        results->emplace_back (*distance);
        if (first_occurence)
          return;
      }
    }
  }
}

/*
void WMOGroup::drawLiquid ( glm::mat4x4 const& transform
                          , liquid_render& render
                          , bool // draw_fog
                          , int animtime
                          )
{
  // draw liquid
  //! \todo  culling for liquid boundingbox or something
  if (lq) 
  { 
    gl.enable(GL_BLEND);
    gl.depthMask(GL_TRUE);

    lq->draw ( transform, render, animtime);

    gl.disable(GL_BLEND);
  }
}
*/

void WMOGroup::setupFog (bool draw_fog, std::function<void (bool)> setup_fog)
{
  if (use_outdoor_lights || fog == -1) {
    setup_fog (draw_fog);
  }
  else {
    wmo->fogs[fog].setup();
  }
}

void WMOFog::init(BlizzardArchive::ClientFile* f)
{
  f->read(this, 0x30);
  color = glm::vec4(((color1 & 0x00FF0000) >> 16) / 255.0f, ((color1 & 0x0000FF00) >> 8) / 255.0f,
    (color1 & 0x000000FF) / 255.0f, ((color1 & 0xFF000000) >> 24) / 255.0f);
  float temp;
  temp = pos.y;
  pos.y = pos.z;
  pos.z = -temp;
  fogstart = fogstart * fogend * 1.5f;
  fogend *= 1.5;
}

void WMOFog::setup()
{

}

// WMOManager::_ is defined in AsyncObjectManagers.cpp to fix static destruction order.

void WMOManager::report()
{
  std::string output = "Still in the WMO manager:\n";
  _.apply ( [&] (BlizzardArchive::Listfile::FileKey const& key, WMO const&)
            {
              output += " - " + key.stringRepr() + "\n";
            }
          );
  LogDebug << output;
}

void WMOManager::clear_hidden_wmos()
{
  _.apply ( [&] (BlizzardArchive::Listfile::FileKey const&, WMO& wmo)
            {
              wmo.show();
            }
          );
}

void WMOManager::unload_all(Noggit::NoggitRenderContext context)
{
    _.context_aware_apply(
        [&] (BlizzardArchive::Listfile::FileKey const&, WMO& wmo)
        {
            wmo.renderer()->unload();
        }
        , context
    );
}

bool wmo_triangle_material_info::isTransFace() const
{
  return flags.flag_0x01 && (flags.detail || flags.render);
}

bool wmo_triangle_material_info::isColor() const
{
  return !flags.collision;
}

bool wmo_triangle_material_info::isRenderFace() const
{
  return flags.render && !flags.detail;
}

bool wmo_triangle_material_info::isCollidable() const
{
  return flags.collision || isRenderFace();
}

bool wmo_triangle_material_info::isCollision() const
{
  return texture == 0xff;
}
