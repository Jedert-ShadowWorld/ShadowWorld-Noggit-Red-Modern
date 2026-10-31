// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "map_horizon.h"

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/formats/RetailWorldTables.hpp>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <noggit/map_index.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/World.h>

#include <opengl/context.hpp>
#include <opengl/context.inl>
#include <noggit/Misc.h>

#include <algorithm>
#include <bitset>
#include <exception>
#include <sstream>

struct color
{
  color(unsigned char r, unsigned char g, unsigned char b)
    : _r(r)
    , _g(g)
    , _b(b)
  {}

  uint32_t to_int() const {
    return (_b) | (_g << 8) | (_r << 16) | (uint32_t)(0xFFu << 24);
  }

  operator uint32_t () const {
    return to_int();
  }

  unsigned char _r;
  unsigned char _g;
  unsigned char _b;
};

struct ranged_color
{
  ranged_color (const color& c, const int16_t& start, const int16_t& stop)
    : _color (c)
    , _start (start)
    , _stop (stop)
  {}

  const color   _color;
  const int16_t _start;
  const int16_t _stop;
};

static inline color lerp_color(const color& start, const color& end, float t)
{
  return color ( (end._r) * t + (start._r) * (1.0 - t)
               , (end._g) * t + (start._g) * (1.0 - t)
               , (end._b) * t + (start._b) * (1.0 - t)
               );
}

static inline uint32_t color_for_height (int16_t height)
{
  static const ranged_color colors[] =
    { ranged_color (color (20, 149, 7), 0, 600)
    , ranged_color (color (137, 84, 21), 600, 1200)
    , ranged_color (color (96, 96, 96), 1200, 1600)
    , ranged_color (color (255, 255, 255), 1600, 0x7FFF)
    };
  static const size_t num_colors (sizeof (colors) / sizeof (ranged_color));

  if (height < colors[0]._start)
  {
    return color (0, 0, 255 + std::max (height / 2.0, -255.0));
  }
  else if (height >= colors[num_colors - 1]._start)
  {
    return colors[num_colors - 1]._color;
  }

  float t (1.0);
  size_t correct_color (num_colors - 1);

  for (size_t i (0); i < num_colors - 1; ++i)
  {
    if (height >= colors[i]._start && height < colors[i]._stop)
    {
      t = float(height - colors[i]._start) / float (colors[i]._stop - colors[i]._start);
      correct_color = i;
      break;
    }
  }

  return lerp_color(colors[correct_color]._color,
                    colors[std::min(correct_color + 1, num_colors - 1)]._color,
                    std::clamp(t, 0.0f, 1.0f));
}
namespace Noggit
{

map_horizon::map_horizon(const std::string& basename, World * const world)
{
  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << ".wdl";
  _filename = filename.str();

  if (!Application::NoggitApplication::instance()->clientData()->exists(_filename))
  {
    LogError << "file \"World\\Maps\\" << basename << "\\" << basename << ".wdl\" does not exist." << std::endl;
    set_minimap(&world->mapIndex);
    return;
  }

  BlizzardArchive::ClientFile wdl_file (_filename, Application::NoggitApplication::instance()->clientData());

  uint32_t fourcc;
  uint32_t size;

  bool done = false;

  try
  {
  do
  {
    wdl_file.read(&fourcc, 4);
    wdl_file.read(&size, 4);

    switch (fourcc)
    {
      case 'MVER':
      {
        uint32_t version;
        wdl_file.read(&version, 4);
        assert(size == 4 && version == 18);

        break;
      }

      case 'MWMO':
      {
        {
          // TODO : use WMID instead for proper string parsing.

            char const* lCurPos = reinterpret_cast<char const*>(wdl_file.getPointer());
            char const* lEnd = lCurPos + size;
        
            while (lCurPos < lEnd)
            {
                mWMOFilenames.push_back(BlizzardArchive::ClientData::normalizeFilenameInternal(std::string(lCurPos)));
                lCurPos += strlen(lCurPos) + 1;
            }
        }
        wdl_file.seekRelative(size);
        break;
      }
      case 'MWID':
          // TODO
          wdl_file.seekRelative(size); // jump to end of chunk
          break;
      case 'MODF':
      {
        ENTRY_MODF const* modf_ptr = reinterpret_cast<ENTRY_MODF const*>(wdl_file.getPointer());
        for (unsigned int i = 0; i < size / sizeof(ENTRY_MODF); ++i)
        {
            lWMOInstances.push_back(modf_ptr[i]);
            if (lWMOInstances[i].scale == 0.0f)
              lWMOInstances[i].scale = 1024.0f;
        }
        
        wdl_file.seekRelative(size); // jump to end of chunk
        break;
      }
      case 'MAOF':
      {
        assert(size == 64 * 64 * sizeof(uint32_t));

        uint32_t mare_offsets[64][64];
        wdl_file.read(mare_offsets, 64 * 64 * sizeof(uint32_t));

        // - MARE and MAHO by offset ---------------------------
        for (size_t y(0); y < 64; ++y)
        {
          for (size_t x(0); x < 64; ++x)
          {
            if (!mare_offsets[y][x])
            {
              continue;
            }

            wdl_file.seek(mare_offsets[y][x]);
            wdl_file.read(&fourcc, 4);
            wdl_file.read(&size, 4);

            assert(fourcc == 'MARE');
            assert(size == 0x442);

            _tiles[y][x] = std::make_unique<map_horizon_tile>();

            //! \todo There also is MAHO giving holes into this heightmap.
            wdl_file.read(_tiles[y][x]->height_17, 17 * 17 * sizeof(int16_t));
            wdl_file.read(_tiles[y][x]->height_16, 16 * 16 * sizeof(int16_t));

            if (wdl_file.getPos() < wdl_file.getSize())
            {
                wdl_file.read(&fourcc, 4);
                if (fourcc == 'MAHO')
                {
                    wdl_file.read(&size, 4);
                    assert(size == 0x20);
                    wdl_file.read(_tiles[y][x]->holes, 16 * sizeof(int16_t));
                }
            }

          }
        }
        done = true;
        break;
      }
      case 'MLDD':
      case 'MLDX':
      case 'MLMD':
      case 'MLMX':
      {
        // Modern WDL object LOD tables are not terrain heightmap chunks.
        // This reader uses the tile-index minimap when MAOF/MARE are absent.
        if (wdl_file.getPos() > wdl_file.getSize() || size > wdl_file.getSize() - wdl_file.getPos())
        {
          LogError << "invalid WDL LOD chunk size in \"" << _filename << "\"" << std::endl;
          done = true;
        }
        else
          wdl_file.seekRelative(size);
        break;
      }
      default:
        LogError << "unknown chunk in wdl: code=" << fourcc << std::endl;
        if (wdl_file.getPos() > wdl_file.getSize() || size > wdl_file.getSize() - wdl_file.getPos())
        {
          LogError << "invalid WDL chunk size in \"" << _filename << "\", falling back to tile index minimap" << std::endl;
          done = true;
        }
        else
        {
          wdl_file.seekRelative(size);
        }
        break;
    }
  } while (!done && !wdl_file.isEof());
  }
  catch (std::exception const& e)
  {
    LogError << "failed reading WDL \"" << _filename << "\": " << e.what() << std::endl;
  }
  catch (...)
  {
    LogError << "failed reading WDL \"" << _filename << "\": unknown exception" << std::endl;
  }

  constexpr bool _load_models = true;
  if (_load_models)
  {
    // - Load WMOs -----------------------------------------

    // Don't load them to storage, they share UIDs wth regular models

    // for rendering in unloaded tiles
    for (auto const& object : lWMOInstances)
    {
      // world->add_wmo_instance(WMOInstance(mWMOFilenames[object.nameID],
      //   &object, world->getRenderContext()), false, false);

      // auto& filepath = mWMOFilenames[object.nameID];
      // wmos.push_back(scoped_wmo_reference(filepath, world->getRenderContext()));
    }
  }

  wdl_file.close();

  set_minimap(&world->mapIndex);
}

void map_horizon::update_minimap_tile(int y, int x, bool has_data = false )
{
    if (_tiles[y][x])
    {
        //! \todo There also is a second heightmap appended which has additional 16*16 pixels.
        //! \todo There also is MAHO giving holes into this heightmap.

        for (int j(0); j < 16; ++j)
        {
            for (int i(0); i < 16; ++i)
            {
                //! \todo R and B are inverted here
                _qt_minimap.setPixel(x * 16 + i, y * 16 + j, color_for_height(_tiles[y][x]->height_17[j][i]));
            }
        }
    }
    // the adt exist but there's no data in the wdl
    else if (has_data)
    {
        for (int j(0); j < 16; ++j)
        {
            for (int i(0); i < 16; ++i)
            {
                _qt_minimap.setPixel(x * 16 + i, y * 16 + j, color(200, 100, 25));
            }
        }
    }
}

void map_horizon::set_minimap(const MapIndex* const index, bool set_empty)
{
    _qt_minimap = QImage(16 * 64, 16 * 64, QImage::Format_ARGB32);
    _qt_minimap.fill(Qt::transparent);

    if (set_empty)
        return;

    for (int y(0); y < 64; ++y)
    {
        for (int x(0); x < 64; ++x)
        {
            update_minimap_tile(y, x, index->hasTile(TileIndex(x, y)));
        }
    }
}

void map_horizon::remove_horizon_tile(int y, int x)
{
    _tiles[y][x].reset();

    for (int j(0); j < 16; ++j)
    {
        for (int i(0); i < 16; ++i)
        {
            _qt_minimap.setPixel(x * 16 + i, y * 16 + j, color(255, 25, 25));
        }
    }
}

Noggit::map_horizon_tile* map_horizon::get_horizon_tile(int y, int x)
{
    return _tiles[y][x].get();
}

int16_t map_horizon::getWdlheight(MapTile* tile, float x, float y)
{
    int cx = std::min(std::max(static_cast<int>(x / CHUNKSIZE), 0), 15);
    int cy = std::min(std::max(static_cast<int>(y / CHUNKSIZE), 0), 15);

    x -= cx * CHUNKSIZE;
    y -= cy * CHUNKSIZE;

    int row = static_cast<int>(y / (UNITSIZE * 0.5f) + 0.5f);
    int col = static_cast<int>((x - UNITSIZE * 0.5f * (row % 2)) / UNITSIZE + 0.5f);
    bool inner = (row % 2) == 1;

    if (row < 0 || col < 0 || row > 16 || col > (inner ? 7 : 8))
        throw std::runtime_error("WDL height sample is outside the terrain grid");

    auto chunk = tile->getChunk(cx, cy);
    if (!chunk)
        throw std::runtime_error("WDL height sample has no terrain chunk");

    float height = chunk->getHeightmap()[17 * (row / 2) + (inner ? 9 : 0) + col].y;
    return Noggit::Formats::RetailWorldTables::wdlHeight(height);
}

void map_horizon::update_horizon_tile(MapTile* mTile)
{
    auto tile_index = mTile->index;
    auto updated = std::make_unique<map_horizon_tile>();

    // calculate the heightmap as a short array
    float x, y;
    for (int i = 0; i < 17; i++)
    {
        for (int j = 0; j < 17; j++)
        {
            // outer - correct
            x = j * CHUNKSIZE;
            y = i * CHUNKSIZE;

            updated->height_17[i][j] = getWdlheight(mTile, x, y);

            // inner - close enough; correct values appear to use some form of averaging
            if (i < 16 && j < 16)
                updated->height_16[i][j] = getWdlheight(mTile, x + CHUNKSIZE / 2.0f, y + CHUNKSIZE / 2.0f);
        }
    }
    // Holes
    for (int i = 0; i < 16; ++i)
    {
        std::bitset<16>wdlHoleMask(0);

        for (int j = 0; j < 16; ++j)
        {
            auto chunk = mTile->getChunk(j, i);
            if (!chunk)
                continue;
            // the ordering seems to be : short array = Y axis, flags values = X axis and the values are for a whole chunk.

            std::bitset<16> holeBits(chunk->getHoleMask());

            if (holeBits.count() == 16) // if all holes are set in a chunk
                wdlHoleMask.set(j, true);
        }
        updated->holes[i] = static_cast<int16_t>(wdlHoleMask.to_ulong());
    }

    _tiles[tile_index.z][tile_index.x] = std::move(updated);
    update_minimap_tile(tile_index.z, tile_index.x, true);
}

void map_horizon::save_wdl(World* world, bool regenerate, std::vector<TileIndex> const* scope)
{
    world->wait_for_all_tile_updates();

    std::stringstream filename;
    filename << "World\\Maps\\" << world->basename << "\\" << world->basename << ".wdl";
    auto* client = Noggit::Application::NoggitApplication::instance()->clientData();
    if (client->version() == BlizzardArchive::ClientVersion::RETAIL)
    {
      if (scope && scope->empty()) return;
      BlizzardArchive::ClientFile source(filename.str(), client);
      Noggit::Formats::RetailWorldTables::Bytes bytes(source.getBuffer(), source.getBuffer() + source.getSize());
      auto offsets = Noggit::Formats::RetailWorldTables::wdlOffsets(bytes);
      namespace Tables = Noggit::Formats::RetailWorldTables;
      auto mapPath = "World\\Maps\\" + world->basename + "\\" + world->basename;
      BlizzardArchive::ClientFile wdt(mapPath + ".wdt", client);
      Tables::Bytes wdtBytes(wdt.getBuffer(), wdt.getBuffer() + wdt.getSize());
      auto mphd = Tables::required(Tables::chunks(wdtBytes), "MPHD", 32);
      auto occId = Tables::read(wdtBytes, mphd.payload + 8);
      Tables::Bytes occlusion, originalOcclusion;
      std::map<unsigned, std::size_t> occlusionOffsets;
      std::size_t lowered = 0;
      if (occId)
      {
        // Honor the existing MPHD reference and project override. Never rewrite
        // the root WDT or silently replace an unreadable occlusion satellite.
        BlizzardArchive::ClientFile occ(BlizzardArchive::Listfile::FileKey(mapPath + "_occ.wdt", occId), client);
        occlusion.assign(occ.getBuffer(), occ.getBuffer() + occ.getSize());
        originalOcclusion = occlusion;
        occlusionOffsets = Tables::occlusionOffsets(occlusion);
      }
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
          TileIndex index(x, y);
          if (scope && std::find(scope->begin(), scope->end(), index) == scope->end()) continue;
          if (!world->mapIndex.hasTile(index)) continue;
          if (regenerate)
          {
            bool unload = !world->mapIndex.tileLoaded(index) && !world->mapIndex.tileAwaitingLoading(index);
            auto* tile = world->mapIndex.loadTile(index, false, false, false);
            if (!tile) throw std::runtime_error("Cannot load tile for Retail WDL");
            try
            {
              tile->wait_until_loaded();
              if (tile->loading_failed())
                throw std::runtime_error("Tile " + std::to_string(x) + "_" + std::to_string(y)
                  + " failed to load; Retail WDL heights were not updated");
              update_horizon_tile(tile);
            }
            catch (...)
            {
              if (unload) world->mapIndex.unloadTile(index);
              throw;
            }
            if (unload) world->mapIndex.unloadTile(index);
          }
          auto* tile = get_horizon_tile(y, x);
          if (!tile) throw std::runtime_error("Retail WDL tile has no sampled heights");
          // The editor's 4x4 hole mask merges partial 8x8 holes. Read the saved
          // ROOT so MAHO only removes chunks whose entire surface is a hole.
          auto root_name = "World\\Maps\\" + world->basename + "\\" + world->basename
            + "_" + std::to_string(x) + "_" + std::to_string(y) + ".adt";
          BlizzardArchive::ClientFile root(root_name, client);
          Tables::Bytes rootBytes(root.getBuffer(), root.getBuffer() + root.getSize());
          auto holes = Tables::wdlHoles(rootBytes);
          Noggit::Formats::RetailWorldTables::updateWdlTile(
            bytes, offsets, y*64+x, tile->height_17, tile->height_16, holes);
          lowered += Tables::lowerOcclusionTile(occlusion, occlusionOffsets, y*64+x, rootBytes);
          std::memcpy(tile->holes, holes.data(), sizeof(tile->holes));
        }
      auto publish = [&](std::string const& name, Tables::Bytes const& data) {
        auto path = QString::fromStdString(client->getDiskPath(name));
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("Cannot create world table directory");
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(data.data(), data.size()) != qint64(data.size()) || !output.commit())
          throw std::runtime_error("Cannot atomically save " + name);
      };
      // Publish the conservative occluder first. If either write fails, the
      // caller retains WDL pending state so the entire heightmap step is retried.
      if (occlusion != originalOcclusion)
      {
        publish(mapPath + "_occ.wdt", occlusion);
        Log << "Retail occlusion: lowered " << lowered << " samples for saved terrain" << std::endl;
      }
      if (bytes.size() == source.getSize()
          && std::memcmp(bytes.data(), source.getBuffer(), bytes.size()) == 0) return;
      publish(filename.str(), bytes);
      set_minimap(&world->mapIndex);
      return;
    }
    //Log << "Saving WDL \"" << filename << "\"." << std::endl;

    util::sExtendableArray wdlFile;
    int curPos = 0;

    // MVER
    //  {
    wdlFile.Extend(8 + 0x4);
    SetChunkHeader(wdlFile, curPos, 'MVER', 4);

    // MVER data
    *(wdlFile.GetPointer<int>(8)) = 18; // write version 18
    curPos += 8 + 0x4;
    //  }

    // WMO objects export code is copy pasta from MapTile

    struct filenameOffsetThing
    {
      int nameID;
      int filenamePosition;
    };

    filenameOffsetThing nullyThing = { 0, 0 };

    std::map<std::string, filenameOffsetThing> lObjects;

    // avoid duplicates, not really necessary here as we directly used MWMO string list
    for (auto const& filename : mWMOFilenames)
    {
      if (lObjects.find(filename) == lObjects.end())
      {
        lObjects.emplace(filename, nullyThing);
      }
    }

    int lID = 0;
    for (auto& object : lObjects)
    {
      object.second.nameID = lID++;
    }

    // MWMO
    //  {
    int lMWMO_Position = curPos;
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MWMO', 0);

    curPos += 8;

    // MWMO data
    for (auto& object : lObjects)
    {
      object.second.filenamePosition = wdlFile.GetPointer<sChunkHeader>(lMWMO_Position)->mSize;
      wdlFile.Insert(curPos, static_cast<unsigned long>(object.first.size() + 1), misc::normalize_adt_filename(object.first).c_str());
      curPos += static_cast<int>(object.first.size() + 1);
      wdlFile.GetPointer<sChunkHeader>(lMWMO_Position)->mSize += static_cast<int>(object.first.size() + 1);
      LogDebug << "Added WDL object \"" << object.first << "\"." << std::endl;
    }
    //  }

    // MWID
    //  {
    int lMWID_Size = static_cast<int>(4 * lObjects.size());
    wdlFile.Extend(8 + lMWID_Size);
    SetChunkHeader(wdlFile, curPos, 'MWID', lMWID_Size);

    // MWID data
    auto const lMWID_Data = wdlFile.GetPointer<int>(curPos + 8);

    lID = 0;
    for (auto const& object : lObjects)
      lMWID_Data[lID++] = object.second.filenamePosition;

    curPos += 8 + lMWID_Size;
    //  }

    // MODF
    //  {
    int lMODF_Size = static_cast<int>(0x40 * lWMOInstances.size());
    wdlFile.Extend(8 + lMODF_Size);
    SetChunkHeader(wdlFile, curPos, 'MODF', lMODF_Size);

    // MODF data
    auto const lMODF_Data = wdlFile.GetPointer<ENTRY_MODF>(curPos + 8);

    lID = 0;
    for (auto const& object : lWMOInstances)
    {
      auto filename_to_offset_and_name = lObjects.find(mWMOFilenames[object.nameID]);
      if (filename_to_offset_and_name == lObjects.end())
      {
        LogError << "There is a problem with saving the WDL objects. We have an object that somehow changed the name during the saving function." << std::endl;
        return;
      }

      lMODF_Data[lID] = object;
      // only need to update name id
      lMODF_Data[lID].nameID = filename_to_offset_and_name->second.nameID;
      lID++;
    }
    LogDebug << "Added " << lID << " wmos to WDL MODF" << std::endl;

    curPos += 8 + lMODF_Size;
    //  }

    //uint32_t mare_offsets[64][64] = { 0 };
    // MAOF
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MAOF', 64 * 64 * 4);
    curPos += 8;
    wdlFile.Extend(64 * 64 * 4);
    uint mareoffset = curPos + 64 * 64 * 4;

    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            TileIndex index(x, y);

            bool has_tile = world->mapIndex.hasTile(index);
            // write offset in MAOF entry
            *(wdlFile.GetPointer<uint>(curPos)) = has_tile ? mareoffset : 0;

            if (has_tile)
            {
                // MARE Header
                //  {
                wdlFile.Extend(8);
                SetChunkHeader(wdlFile, mareoffset, 'MARE', (2 * (17 * 17)) + (2 * (16 * 16))); // outer heights+inner heights
                mareoffset += 8;

                // this might be invalid if map had no WDL
                Noggit::map_horizon_tile* horizon_tile = get_horizon_tile(y, x);

                // laod tile and extract WDL data
                if (!horizon_tile || regenerate)
                {
                    bool unload = !world->mapIndex.tileLoaded(index) && !world->mapIndex.tileAwaitingLoading(index);
                    MapTile* mTile = world->mapIndex.loadTile(index, false, false, false);
                    if (!mTile)
                    {
                        continue;
                    }

                    auto nloadedtiles = world->mapIndex.getNLoadedTiles();

                    if (mTile)
                        mTile->wait_until_loaded();

                    update_horizon_tile(mTile);
                    if (unload)
                        world->mapIndex.unloadTile(index);

                    auto test = get_horizon_tile(y, x);
                    horizon_tile = get_horizon_tile(y, x);
                }
                if (!horizon_tile)
                {
                    return; // failed to generate data somehow
                    LogError << "Failed to generate the WDL file." << std::endl;
                }

                wdlFile.Insert(mareoffset, sizeof(Noggit::map_horizon_tile::height_17), reinterpret_cast<char*>(&horizon_tile->height_17));
                mareoffset += sizeof(Noggit::map_horizon_tile::height_17);
                wdlFile.Insert(mareoffset, sizeof(Noggit::map_horizon_tile::height_16), reinterpret_cast<char*>(&horizon_tile->height_16));
                mareoffset += sizeof(Noggit::map_horizon_tile::height_16);

                // MAHO (maparea holes) MAHO was added in WOTLK ?
                //  {
                wdlFile.Extend(8);
                SetChunkHeader(wdlFile, mareoffset, 'MAHO', (2 * 16)); // 1 hole mask for each chunk
                mareoffset += 8;
                wdlFile.Extend(32);
                for (int i = 0; i < 16; ++i)
                {
                    wdlFile.Insert(mareoffset, 2, (char*)&horizon_tile->holes[i]);
                    mareoffset += 2;
                }
            }
            curPos += 4;
        }
    }
    BlizzardArchive::ClientFile f(filename.str(), Noggit::Application::NoggitApplication::instance()->clientData(),
    BlizzardArchive::ClientFile::NEW_FILE);
    f.setBuffer(wdlFile.all_data());
    f.save();
    f.close();

    set_minimap(&world->mapIndex);
}

bool map_horizon::wmoHasLowRes(WMOInstance* instance)
{
  assert(instance->lowResWmo.has_value() == false);
  if (instance->lowResWmo.has_value())
    return true;

  int i = 0;
  for (auto& lowres_wmo : lWMOInstances)
  {
    if (lowres_wmo.uniqueID == instance->uid)
    {
      auto low_res_model = mWMOFilenames[lowres_wmo.nameID];

      // TODO check positions
      // need to convert coords?
      auto dir = math::degrees::vec3{ math::degrees(
        lowres_wmo.rot[0])._, math::degrees(lowres_wmo.rot[1])._, math::degrees(lowres_wmo.rot[2])._ };

      if (misc::vec3d_equals(glm::vec3(lowres_wmo.pos[0], lowres_wmo.pos[1], lowres_wmo.pos[2]), instance->pos)
        && misc::deg_vec3d_equals(dir, instance->dir)
        && misc::float_equals( (lowres_wmo.scale / 1024.0f), instance->scale))
      {
        // instance->lowResWmo = scoped_wmo_reference(low_res_model, instance->wmo->_context);
        instance->lowResInstance = &lowres_wmo;
        instance->lowResWmo = &wmos[instance->lowResInstance->nameID];

        return true;
      }
      else
      {
        assert(false);
      }
    }
    
    i++;
  }

  return false;
}

map_horizon::minimap::minimap(const map_horizon& horizon)
{
  std::vector<uint32_t> texture(1024 * 1024);

  for (size_t y (0); y < 64; ++y)
  {
    for (size_t x (0); x < 64; ++x)
    {
      if (!horizon._tiles[y][x])
        continue;

      //! \todo There also is a second heightmap appended which has additional 16*16 pixels.

      // use the (nearly) full resolution available to us.
      // the data is layed out as a triangle fans with with 17 outer values
      // and 16 midpoints per tile. which in turn means:
      //      _tiles[y][x]->height_17[16][16] == _tiles[y][x + 1]->height_17[0][0]
      for (size_t j (0); j < 16; ++j)
      {
        for (size_t i (0); i < 16; ++i)
        {
          texture[(y * 16 + j) * 1024 + x * 16 + i] = color_for_height (horizon._tiles[y][x]->height_17[j][i]);
        }
      }
    }
  }

  bind();
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1024, 0, GL_BGRA, GL_UNSIGNED_BYTE, texture.data());
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

map_horizon::render::render(const map_horizon& horizon)
{
  std::vector<glm::vec3> vertices;

  for (size_t y (0); y < 64; ++y)
  {
    for (size_t x (0); x < 64; ++x)
    {
      if (!horizon._tiles[y][x])
        continue;

      _batches[y][x] = map_horizon_batch (static_cast<std::uint32_t>(vertices.size()), 17 * 17 + 16 * 16);

      for (size_t j (0); j < 17; ++j)
      {
        for (size_t i (0); i < 17; ++i)
        {
          vertices.emplace_back ( TILESIZE * (x + i / 16.0f)
                                , horizon._tiles[y][x]->height_17[j][i]
                                , TILESIZE * (y + j / 16.0f)
                                );
        }
      }

      for (size_t j (0); j < 16; ++j)
      {
        for (size_t i (0); i < 16; ++i)
        {
          vertices.emplace_back ( TILESIZE * (x + (i + 0.5f) / 16.0f)
                                , horizon._tiles[y][x]->height_16[j][i]
                                , TILESIZE * (y + (j + 0.5f) / 16.0f)
                                );
        }
      }
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3> (_vertex_buffer, vertices, GL_STATIC_DRAW);
}

static inline uint32_t outer_index(const map_horizon_batch &batch, int y, int x)
{
  return batch.vertex_start + y * 17 + x;
};

static inline uint32_t inner_index(const map_horizon_batch &batch, int y, int x)
{
  return batch.vertex_start + 17 * 17 + y * 16 + x;
};

void map_horizon::render::draw( glm::mat4x4 const& model_view
                              , glm::mat4x4 const& projection
                              , MapIndex *index
                              , const glm::vec3& color
                              , const float& cull_distance
                              , const math::frustum& frustum
                              , const glm::vec3& camera 
                              , display_mode display
                              )
{
  std::vector<uint32_t> indices;

  const TileIndex current_index(camera);
  const int lrr = 2;

  for (size_t y (current_index.z - lrr); y <= current_index.z + lrr; ++y)
  {
    for (size_t x (current_index.x - lrr); x < current_index.x + lrr; ++x)
    {
      // x and y are unsigned so negative signed int value are positive and > 63
      if (x > 63 || y > 63)
      {
        continue;
      }

      map_horizon_batch const& batch = _batches[y][x];

      if (batch.vertex_count == 0)
        continue;

      for (int j (0); j < 16; ++j)
      {
        for (int i (0); i < 16; ++i)
        {
          // do not draw over visible chunks

          /* TODO: when this optimization is turned off, we end up with inconsistent rendering between chunks and horizon batches.
           * Potentially it is caused by inconsistent coordinate space in visibility checking or chunk update system.
          if (index->tileLoaded({y, x}) && index->getTile({y, x})->getChunk(j, i)->is_visible(cull_distance, frustum, camera, display))
          {
            //continue;
          }
          */

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i));
          indices.push_back (outer_index (batch, j + 1, i + 1));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i + 1));
          indices.push_back (outer_index (batch, j, i + 1));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j, i + 1));
          indices.push_back (outer_index (batch, j, i));
        }
      }
    }
  }

  if (_map_horizon_program)
  {
    gl.bufferSubData<GL_ELEMENT_ARRAY_BUFFER, std::uint32_t>(_index_buffer, 0, indices);
  }
  else
  {
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint32_t>(_index_buffer, indices, GL_DYNAMIC_DRAW);

    _map_horizon_program.reset
      ( new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("horizon_vs") }
          , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("horizon_fs") }
          }
      );
  
    _vaos.upload();
  }
   

  OpenGL::Scoped::use_program shader {*_map_horizon_program.get()};

  OpenGL::Scoped::vao_binder const _ (_vao);

  shader.uniform ("model_view", model_view);
  shader.uniform ("projection", projection);
  shader.uniform ("color", glm::vec3(color.x, color.y, color.z));

  shader.attrib ("position", _vertex_buffer, 3, GL_FLOAT, GL_FALSE, 0, 0);

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder (_index_buffer);

  
  gl.drawElements (GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, nullptr);
}

}
