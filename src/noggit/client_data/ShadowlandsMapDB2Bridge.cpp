// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/client_data/ShadowlandsMapDB2Bridge.hpp>

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>

#include <blizzard-archive-library/include/ClientFile.hpp>
#include <blizzard-database-library/include/BlizzardDatabase.h>
#include <blizzard-database-library/include/stream/StreamReader.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{
  using Row = BlizzardDatabaseLib::Structures::BlizzardDatabaseRow;
  using Table = BlizzardDatabaseLib::BlizzardDatabaseTable;

  std::unique_ptr<BlizzardDatabaseLib::BlizzardDatabase> g_shadowlands_map_database;

  std::shared_ptr<BlizzardDatabaseLib::Stream::IMemStream> open_db2_from_casc(
      std::shared_ptr<BlizzardArchive::ClientData> const& client_data,
      std::string const& filename)
  {
    BlizzardArchive::ClientFile file(filename, client_data.get());
    if (file.isEof() || file.getSize() == 0)
      throw std::runtime_error("Could not open modern DB2 from CASC: " + filename);

    return std::make_shared<BlizzardDatabaseLib::Stream::IMemStream>(file.getBuffer(), file.getSize());
  }

  bool has(Row const& row, std::string const& field)
  {
    return row.Columns.find(field) != row.Columns.end();
  }

  std::uint32_t id_of(Row const& row)
  {
    if (row.RecordId >= 0)
      return static_cast<std::uint32_t>(row.RecordId);
    auto const it = row.Columns.find("ID");
    if (it != row.Columns.end() && !it->second.Value.empty())
      return static_cast<std::uint32_t>(std::stoul(it->second.Value));
    return 0;
  }

  std::uint32_t u32(Row const& row, std::string const& field, std::uint32_t fallback = 0)
  {
    auto const it = row.Columns.find(field);
    if (it == row.Columns.end())
      return fallback;
    if (!it->second.Value.empty())
      return static_cast<std::uint32_t>(std::stoull(it->second.Value));
    if (it->second.ReferenceId >= 0)
      return static_cast<std::uint32_t>(it->second.ReferenceId);
    return fallback;
  }

  int i32(Row const& row, std::string const& field, int fallback = 0)
  {
    auto const it = row.Columns.find(field);
    if (it == row.Columns.end() || it->second.Value.empty())
      return fallback;
    return std::stoi(it->second.Value);
  }

  float f32(Row const& row, std::string const& field, float fallback = 0.0f)
  {
    auto const it = row.Columns.find(field);
    if (it == row.Columns.end() || it->second.Value.empty())
      return fallback;
    return std::stof(it->second.Value);
  }

  std::string text(Row const& row, std::string const& field)
  {
    auto const it = row.Columns.find(field);
    if (it == row.Columns.end())
      return {};
    if (!it->second.Value.empty())
      return it->second.Value;
    for (auto const& value : it->second.Values)
      if (!value.empty())
        return value;
    return {};
  }

  template<typename T>
  std::vector<T> numbers(Row const& row, std::string const& field)
  {
    std::vector<T> result;
    auto const it = row.Columns.find(field);
    if (it == row.Columns.end())
      return result;
    result.reserve(it->second.Values.size());
    for (auto const& value : it->second.Values)
    {
      if constexpr (std::is_floating_point_v<T>)
        result.push_back(static_cast<T>(std::stof(value)));
      else
        result.push_back(static_cast<T>(std::stoll(value)));
    }
    return result;
  }

  Table* load_optional(BlizzardDatabaseLib::BlizzardDatabase& database,
                       std::shared_ptr<BlizzardArchive::ClientData> const& client_data,
                       std::string const& name)
  {
    try
    {
      auto callback = [&client_data](std::string const& filename)
      {
        return open_db2_from_casc(client_data, filename);
      };
      auto& table = database.LoadTable(name, callback);
      Log << "[ModernDB2][Map] " << name << ".db2 records=" << table.RecordCount() << std::endl;
      return &table;
    }
    catch (std::exception const& e)
    {
      LogError << "[ModernDB2][Map] Failed " << name << ".db2: " << e.what() << std::endl;
      return nullptr;
    }
  }

  void bridge_area_table(Table& table)
  {
    auto out = DBCFile::createNew("DBFilesClient\\AreaTable.dbc", 36, 36 * sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < table.RecordCount(); ++i)
    {
      auto row = table.RecordByPosition(i);
      auto rec = out.addRecord(id_of(row));
      rec.write(AreaDB::Continent, u32(row, "ContinentID"));
      rec.write(AreaDB::Region, u32(row, "ParentAreaID"));
      rec.write(AreaDB::AreaBit, u32(row, "AreaBit"));
      auto flags = numbers<std::uint32_t>(row, "Flags");
      rec.write(AreaDB::Flags, flags.empty() ? u32(row, "Flags") : flags.front());
      rec.write(AreaDB::SoundProviderPreferences, u32(row, "SoundProviderPref"));
      rec.write(AreaDB::UnderwaterSoundProviderPreferences, u32(row, "SoundProviderPrefUnderwater"));
      rec.write(AreaDB::SoundAmbience, u32(row, "AmbienceID"));
      rec.write(AreaDB::ZoneMusic, u32(row, "ZoneMusic"));
      rec.write(AreaDB::ZoneIntroMusicTable, u32(row, "IntroSound"));
      rec.write(AreaDB::ExplorationLevel, i32(row, "ExplorationLevel"));
      rec.writeLocalizedString(AreaDB::Name, text(row, "AreaName_lang"), 0);
      rec.write(AreaDB::FactionGroup, u32(row, "FactionGroupMask"));
      auto liquids = numbers<std::uint32_t>(row, "LiquidTypeID");
      for (std::size_t n = 0; n < 4; ++n)
        rec.write(AreaDB::LiquidType + n, n < liquids.size() ? liquids[n] : 0u);
      rec.write(AreaDB::MinElevation, f32(row, "MinElevation"));
      rec.write(AreaDB::AmbientMultiplier, f32(row, "Ambient_multiplier", 1.0f));
      rec.write(AreaDB::LightId, u32(row, "LightID"));
    }
    gAreaDB.overwriteWith(out);
  }

  void bridge_area_trigger(Table& table)
  {
    auto out = DBCFile::createNew("DBFilesClient\\AreaTrigger.dbc", 10, 10 * sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < table.RecordCount(); ++i)
    {
      auto row = table.RecordByPosition(i);
      auto rec = out.addRecord(id_of(row));
      rec.write(AreaTriggerDB::MapId, u32(row, "ContinentID"));
      auto pos = numbers<float>(row, "Pos");
      rec.write(AreaTriggerDB::X, pos.size() > 0 ? pos[0] : 0.0f);
      rec.write(AreaTriggerDB::Y, pos.size() > 1 ? pos[1] : 0.0f);
      rec.write(AreaTriggerDB::Z, pos.size() > 2 ? pos[2] : 0.0f);
      rec.write(AreaTriggerDB::Radius, f32(row, "Radius"));
      rec.write(AreaTriggerDB::Length, f32(row, "Box_length"));
      rec.write(AreaTriggerDB::Width, f32(row, "Box_width"));
      rec.write(AreaTriggerDB::Height, f32(row, "Box_height"));
      rec.write(AreaTriggerDB::Orientation, f32(row, "Box_yaw"));
    }
    gAreaTriggerDB.overwriteWith(out);
  }

  void bridge_map(Table& table)
  {
    auto out = DBCFile::createNew("DBFilesClient\\Map.dbc", 66, 66 * sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < table.RecordCount(); ++i)
    {
      auto row = table.RecordByPosition(i);
      auto rec = out.addRecord(id_of(row));
      rec.writeString(MapDB::InternalName, text(row, "Directory"));
      rec.write(MapDB::AreaType, u32(row, "InstanceType"));
      auto flags = numbers<std::uint32_t>(row, "Flags");
      rec.write(MapDB::Flags, flags.empty() ? u32(row, "Flags") : flags.front());
      rec.writeLocalizedString(MapDB::Name, text(row, "MapName_lang"), 0);
      rec.write(MapDB::AreaTableID, u32(row, "AreaTableID"));
      rec.writeLocalizedString(MapDB::MapDescriptionAlliance, text(row, "MapDescription1_lang"), 0);
      rec.writeLocalizedString(MapDB::MapDescriptionHorde, text(row, "MapDescription0_lang"), 0);
      rec.write(MapDB::LoadingScreen, u32(row, "LoadingScreenID"));
      rec.write(MapDB::minimapIconScale, f32(row, "MinimapIconScale", 1.0f));
      rec.write(MapDB::corpseMapID, i32(row, "CorpseMapID", -1));
      auto corpse = numbers<float>(row, "Corpse");
      rec.write(MapDB::corpseX, has(row, "CorpseX") ? f32(row, "CorpseX") : (corpse.size() > 0 ? corpse[0] : 0.0f));
      rec.write(MapDB::corpseY, has(row, "CorpseY") ? f32(row, "CorpseY") : (corpse.size() > 1 ? corpse[1] : 0.0f));
      rec.write(MapDB::TimeOfDayOverride, i32(row, "TimeOfDayOverride", -1));
      rec.write(MapDB::ExpansionID, u32(row, "ExpansionID"));
      rec.write(MapDB::RaidOffset, u32(row, "RaidOffset"));
      rec.write(MapDB::NumberOfPlayers, u32(row, "MaxPlayers"));
    }
    gMapDB.overwriteWith(out);
  }

  void bridge_loading_screens(Table& table)
  {
    auto out = DBCFile::createNew("DBFilesClient\\LoadingScreens.dbc", 3, 3 * sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < table.RecordCount(); ++i)
    {
      auto row = table.RecordByPosition(i);
      auto rec = out.addRecord(id_of(row));
      rec.writeString(LoadingScreensDB::Name, text(row, "Name"));
      // Modern clients use FileDataIDs instead of a string path. Keep Path empty;
      // modern consumers should use the DB2 row directly.
      rec.writeString(LoadingScreensDB::Path, {});
    }
    gLoadingScreensDB.overwriteWith(out);
  }

  void bridge_wmo_area(Table& table)
  {
    auto out = DBCFile::createNew("DBFilesClient\\WMOAreaTable.dbc", 28, 28 * sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < table.RecordCount(); ++i)
    {
      auto row = table.RecordByPosition(i);
      auto rec = out.addRecord(id_of(row));
      rec.write(WMOAreaTableDB::WmoId, u32(row, "WMOID"));
      rec.write(WMOAreaTableDB::NameSetId, u32(row, "NameSetID"));
      rec.write(WMOAreaTableDB::WMOGroupID, i32(row, "WMOGroupID"));
      rec.write(WMOAreaTableDB::SoundProviderPreferences, u32(row, "SoundProviderPref"));
      rec.write(WMOAreaTableDB::UnderwaterSoundProviderPreferences, u32(row, "SoundProviderPrefUnderwater"));
      rec.write(WMOAreaTableDB::SoundAmbience, u32(row, "AmbienceID"));
      rec.write(WMOAreaTableDB::ZoneMusic, u32(row, "ZoneMusic"));
      rec.write(WMOAreaTableDB::ZoneIntroMusicTable, u32(row, "IntroSound"));
      rec.write(WMOAreaTableDB::Flags, u32(row, "Flags"));
      rec.write(WMOAreaTableDB::AreaTableRefId, u32(row, "AreaTableID"));
      rec.writeLocalizedString(WMOAreaTableDB::Name, text(row, "AreaName_lang"), 0);
    }
    gWMOAreaTableDB.overwriteWith(out);
  }

  void bridge_ground_effect_texture(Table& table)
  {
    auto out = DBCFile::createNew("DBFilesClient\\GroundEffectTexture.dbc", 11, 11 * sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < table.RecordCount(); ++i)
    {
      auto row = table.RecordByPosition(i);
      auto rec = out.addRecord(id_of(row));
      auto doodads = numbers<std::uint32_t>(row, "DoodadID");
      auto weights = numbers<std::uint32_t>(row, "DoodadWeight");
      for (std::size_t n = 0; n < 4; ++n)
      {
        rec.write(GroundEffectTextureDB::Doodads + n, n < doodads.size() ? doodads[n] : 0u);
        rec.write(GroundEffectTextureDB::Weights + n, n < weights.size() ? weights[n] : 0u);
      }
      rec.write(GroundEffectTextureDB::Amount, u32(row, "Density"));
      rec.write(GroundEffectTextureDB::TerrainType, u32(row, "Sound"));
    }
    gGroundEffectTextureDB.overwriteWith(out);
  }

  void bridge_ground_effect_doodad(Table& table)
  {
    auto out = DBCFile::createNew("DBFilesClient\\GroundEffectDoodad.dbc", 3, 3 * sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < table.RecordCount(); ++i)
    {
      auto row = table.RecordByPosition(i);
      auto rec = out.addRecord(id_of(row));
      // 9.2.7 stores ModelFileID rather than a path. Preserve a readable token in
      // the legacy compatibility view; modern asset resolution should use FDID.
      auto const fdid = u32(row, "ModelFileID");
      rec.writeString(GroundEffectDoodadDB::Filename, fdid ? ("filedataid:" + std::to_string(fdid)) : text(row, "Doodadpath"));
      rec.write(GroundEffectDoodadDB::Flags, u32(row, "Flags"));
    }
    gGroundEffectDoodadDB.overwriteWith(out);
  }

  void bridge_liquid_type(Table& table)
  {
    auto out = DBCFile::createNew("DBFilesClient\\LiquidType.dbc", 31, 31 * sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < table.RecordCount(); ++i)
    {
      auto row = table.RecordByPosition(i);
      auto rec = out.addRecord(id_of(row));
      rec.writeString(LiquidTypeDB::Name, text(row, "Name"));
      // Do not pretend modern MaterialID/SoundBank fields are the old liquid
      // Type/ShaderType enums. Those are intentionally left zero until the
      // renderer/editor consumes LiquidMaterial.db2 natively.
    }
    gLiquidTypeDB.overwriteWith(out);
  }
}

namespace Noggit::ClientData
{
  bool loadShadowlandsMapDB2Bridge(std::shared_ptr<BlizzardArchive::ClientData> const& client_data)
  {
    try
    {
      auto const definitions = Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationDatabaseDefinitionsPath;
      g_shadowlands_map_database = std::make_unique<BlizzardDatabaseLib::BlizzardDatabase>(
          definitions,
          BlizzardDatabaseLib::Structures::Build("9.2.7.45745"));

      auto& database = *g_shadowlands_map_database;

      // Direct modern equivalents of the map/editor DBC set plus modern-only
      // environment tables used by 9.2.7 maps.
      Table* area = load_optional(database, client_data, "AreaTable");
      Table* area_trigger = load_optional(database, client_data, "AreaTrigger");
      Table* map = load_optional(database, client_data, "Map");
      Table* loading = load_optional(database, client_data, "LoadingScreens");
      load_optional(database, client_data, "LightSkybox");
      Table* ground_doodad = load_optional(database, client_data, "GroundEffectDoodad");
      Table* ground_texture = load_optional(database, client_data, "GroundEffectTexture");
      load_optional(database, client_data, "TerrainType");
      Table* liquid = load_optional(database, client_data, "LiquidType");
      load_optional(database, client_data, "LiquidMaterial");
      load_optional(database, client_data, "SoundProviderPreferences");
      load_optional(database, client_data, "SoundAmbience");
      load_optional(database, client_data, "ZoneMusic");
      load_optional(database, client_data, "ZoneIntroMusicTable");
      Table* wmo_area = load_optional(database, client_data, "WMOAreaTable");
      load_optional(database, client_data, "SoundKit");
      load_optional(database, client_data, "WindSettings");
      load_optional(database, client_data, "Weather");
      load_optional(database, client_data, "WeatherXParticulate");
      load_optional(database, client_data, "ZoneLight");
      load_optional(database, client_data, "ZoneLightPoint");

      if (area) bridge_area_table(*area);
      if (area_trigger) bridge_area_trigger(*area_trigger);
      if (map) bridge_map(*map);
      if (loading) bridge_loading_screens(*loading);
      if (ground_doodad) bridge_ground_effect_doodad(*ground_doodad);
      if (ground_texture) bridge_ground_effect_texture(*ground_texture);
      if (liquid) bridge_liquid_type(*liquid);
      if (wmo_area) bridge_wmo_area(*wmo_area);

      bool const core_ok = area && map && wmo_area;
      Log << "[ModernDB2][Map] Shadowlands map DB2 bootstrap " << (core_ok ? "ready" : "incomplete") << std::endl;
      return core_ok;
    }
    catch (std::exception const& e)
    {
      LogError << "[ModernDB2][Map] Bootstrap failed: " << e.what() << std::endl;
      return false;
    }
  }
}
