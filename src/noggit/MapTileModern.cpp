// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>

#include <ClientFile.hpp>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

void MapTile::finishLoadingShadowlandsTerrainOnly()
{
  if (finished)
    return;

  BlizzardArchive::ClientFile root_file(
    _file_key,
    Noggit::Application::NoggitApplication::instance()->clientData());

  auto const* data = root_file.getBuffer();
  auto const file_size = root_file.getSize();

  std::vector<std::size_t> mcnk_offsets;
  mcnk_offsets.reserve(256);

  std::size_t pos = 0;
  while (pos + 8 <= file_size)
  {
    std::uint32_t magic = 0;
    std::uint32_t declared_size = 0;
    std::memcpy(&magic, data + pos, sizeof(magic));
    std::memcpy(&declared_size, data + pos + 4, sizeof(declared_size));

    auto const payload_pos = pos + 8;
    auto const remaining = file_size - payload_pos;
    if (declared_size > remaining)
    {
      throw std::runtime_error(
        "Shadowlands terrain loader encountered a truncated top-level ADT chunk.");
    }

    // WoW ADT FourCC bytes are reversed on disk: canonical MCNK is stored as KNCM.
    if (magic == 0x4D434E4Bu)
      mcnk_offsets.push_back(pos);

    pos = payload_pos + declared_size;
  }

  if (pos != file_size)
    throw std::runtime_error(
      "Shadowlands terrain loader did not end on the root ADT file boundary.");

  if (mcnk_offsets.size() != 256)
  {
    LogError << "[ModernADT] Terrain-only loader expected 256 ROOT MCNK chunks but found "
             << mcnk_offsets.size() << " in '" << _file_key.stringRepr() << "'."
             << std::endl;
    throw std::runtime_error(
      "Shadowlands terrain loader requires exactly 256 ROOT MCNK chunks.");
  }

  LogDebug << "[ModernADT] Building terrain-only MapTile " << index.x << ',' << index.z
           << " from 256 ROOT MCNK chunks. Textures, objects and liquids are intentionally disabled."
           << std::endl;

  // The 9.2.7 ROOT MCNK header still contains usable legacy-compatible
  // terrain offsets for MCVT/MCNR. Feed only the ROOT file to MapChunk and
  // explicitly disable texture loading; TEX0/OBJ0 integration comes later.
  for (std::size_t next_chunk = 0; next_chunk < mcnk_offsets.size(); ++next_chunk)
  {
    root_file.seek(mcnk_offsets[next_chunk]);

    unsigned const x = static_cast<unsigned>(next_chunk / 16);
    unsigned const z = static_cast<unsigned>(next_chunk % 16);

    mChunks[x][z] = std::make_unique<MapChunk>(
      this,
      &root_file,
      mBigAlpha,
      _mode,
      _context,
      false,
      0,
      false);

    _renderer.initChunkData(mChunks[x][z].get());
  }

  // There are deliberately no modern textures/objects wired into this first
  // milestone, so downstream readiness checks should not wait for them.
  mTextureFilenames.clear();
  _mtxf_entries.clear();
  _textures_finished_loading = true;
  _objects_finished_loading = true;

  root_file.close();

  finished = true;
  _tile_is_being_reloaded = false;
  _state_changed.notify_all();

  LogDebug << "[ModernADT] Terrain-only Shadowlands tile loaded successfully: "
           << index.x << ',' << index.z << '.' << std::endl;
}
