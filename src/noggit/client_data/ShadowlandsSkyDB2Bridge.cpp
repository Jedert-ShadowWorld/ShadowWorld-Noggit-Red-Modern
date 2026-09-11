// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/client_data/ShadowlandsSkyDB2Bridge.hpp>

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/project/CurrentProject.hpp>

#include <blizzard-archive-library/include/ClientFile.hpp>
#include <blizzard-database-library/include/BlizzardDatabase.h>
#include <blizzard-database-library/include/stream/StreamReader.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
  constexpr float legacy_sky_coordinate_scale = 36.0f;
  constexpr std::size_t modern_color_count = 18;
  constexpr std::size_t modern_float_count = 6;
  constexpr std::size_t max_band_entries = 16;

  struct TimedColor
  {
    int time = 0;
    std::uint32_t value = 0;
  };

  struct TimedFloat
  {
    int time = 0;
    float value = 0.0f;
  };

  struct ModernParamData
  {
    std::uint32_t id = 0;
    std::uint32_t skybox_id = 0;
    std::uint32_t cloud_type_id = 0;
    std::uint32_t flags = 0;
    bool highlight_sky = false;
    float glow = 0.5f;
    float water_shallow_alpha = 0.5f;
    float water_deep_alpha = 1.0f;
    float ocean_shallow_alpha = 0.75f;
    float ocean_deep_alpha = 1.0f;
    std::array<std::vector<TimedColor>, modern_color_count> colors;
    std::array<std::vector<TimedFloat>, modern_float_count> floats;
  };

  std::uint32_t uint_value(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow const& row,
                           std::string const& field)
  {
    auto const found = row.Columns.find(field);
    if (found == row.Columns.end() || found->second.Value.empty())
      throw std::runtime_error("Modern DB2 row is missing numeric field " + field + ".");

    return static_cast<std::uint32_t>(std::stoul(found->second.Value));
  }

  std::uint32_t uint_value_or(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow const& row,
                              std::string const& field,
                              std::uint32_t fallback = 0)
  {
    auto const found = row.Columns.find(field);
    if (found == row.Columns.end() || found->second.Value.empty())
      return fallback;

    try
    {
      return static_cast<std::uint32_t>(std::stoul(found->second.Value));
    }
    catch (...)
    {
      return fallback;
    }
  }

  std::uint32_t row_id(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow const& row)
  {
    if (row.RecordId >= 0)
      return static_cast<std::uint32_t>(row.RecordId);
    return uint_value(row, "ID");
  }

  std::uint32_t relation_id(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow const& row,
                            std::string const& field)
  {
    auto const found = row.Columns.find(field);
    if (found == row.Columns.end())
      throw std::runtime_error("Modern DB2 row is missing relation field " + field + ".");
    if (!found->second.Value.empty())
      return static_cast<std::uint32_t>(std::stoul(found->second.Value));
    if (found->second.ReferenceId >= 0)
      return static_cast<std::uint32_t>(found->second.ReferenceId);
    throw std::runtime_error("Modern DB2 relation field " + field + " has no value.");
  }

  std::vector<float> float_array(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow const& row,
                                 std::string const& field)
  {
    auto const found = row.Columns.find(field);
    if (found == row.Columns.end())
      throw std::runtime_error("Modern DB2 row is missing array field " + field + ".");
    std::vector<float> values;
    values.reserve(found->second.Values.size());
    for (auto const& value : found->second.Values)
      values.push_back(std::stof(value));
    return values;
  }

  std::vector<std::uint32_t> uint_array(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow const& row,
                                        std::string const& field)
  {
    auto const found = row.Columns.find(field);
    if (found == row.Columns.end())
      throw std::runtime_error("Modern DB2 row is missing array field " + field + ".");
    std::vector<std::uint32_t> values;
    values.reserve(found->second.Values.size());
    for (auto const& value : found->second.Values)
      values.push_back(static_cast<std::uint32_t>(std::stoul(value)));
    return values;
  }

  std::shared_ptr<BlizzardDatabaseLib::Stream::IMemStream> open_db2_from_casc(
      std::shared_ptr<BlizzardArchive::ClientData> const& client_data,
      std::string const& filename)
  {
    BlizzardArchive::ClientFile file(filename, client_data.get());
    if (file.isEof() || file.getSize() == 0)
      throw std::runtime_error("Could not open modern DB2 from CASC: " + filename);
    return std::make_shared<BlizzardDatabaseLib::Stream::IMemStream>(file.getBuffer(), file.getSize());
  }

  void append_color(ModernParamData& param, std::size_t index, int time, std::uint32_t value)
  {
    if (index < param.colors.size())
      param.colors[index].push_back({time, value});
  }

  void append_float(ModernParamData& param, std::size_t index, int time, float value)
  {
    if (index < param.floats.size())
      param.floats[index].push_back({time, value});
  }

  void write_legacy_color_band(DBCFile& table,
                               std::uint32_t id,
                               std::vector<TimedColor> const& values)
  {
    auto record = table.addRecord(id);
    auto const count = std::min(values.size(), max_band_entries);
    record.write(LightIntBandDB::Entries, static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < max_band_entries; ++i)
    {
      record.write(LightIntBandDB::Times + i, i < count ? static_cast<std::uint32_t>(values[i].time) : 0u);
      record.write(LightIntBandDB::Values + i, i < count ? values[i].value : 0u);
    }
  }

  void write_legacy_float_band(DBCFile& table,
                               std::uint32_t id,
                               std::vector<TimedFloat> const& values)
  {
    auto record = table.addRecord(id);
    auto const count = std::min(values.size(), max_band_entries);
    record.write(LightFloatBandDB::Entries, static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < max_band_entries; ++i)
    {
      record.write(LightFloatBandDB::Times + i, i < count ? static_cast<std::uint32_t>(values[i].time) : 0u);
      record.write(LightFloatBandDB::Values + i, i < count ? values[i].value : 0.0f);
    }
  }
}

namespace Noggit::ClientData
{
  bool loadShadowlandsSkyDB2Bridge(std::shared_ptr<BlizzardArchive::ClientData> const& client_data)
  {
    try
    {
      auto const project = Noggit::Project::CurrentProject::get();
      if (!project || !project->ClientDatabase)
        throw std::runtime_error("Modern sky DB2 bridge has no active project client database.");

      auto& database = *project->ClientDatabase;

      auto callback = [&client_data](std::string const& filename)
      {
        return open_db2_from_casc(client_data, filename);
      };

      auto& light_table = database.LoadTable("Light", callback);
      auto& params_table = database.LoadTable("LightParams", callback);
      auto& data_table = database.LoadTable("LightData", callback);
      auto& skybox_table = database.LoadTable("LightSkybox", callback);

      std::map<std::uint32_t, std::pair<std::string, std::uint32_t>> skyboxes;
      for (std::uint32_t i = 0; i < skybox_table.RecordCount(); ++i)
      {
        auto row = skybox_table.RecordByPosition(i);
        auto const id = row_id(row);
        auto const primary_fdid = uint_value_or(row, "SkyboxFileDataID");
        auto const celestial_fdid = uint_value_or(row, "CelestialSkyboxFileDataID");
        auto const fdid = primary_fdid != 0 ? primary_fdid : celestial_fdid;
        if (id == 0 || fdid == 0)
          continue;

        auto path = client_data->listfile()->getPath(fdid);
        if (path.empty())
          continue;

        skyboxes.emplace(id, std::make_pair(std::move(path), uint_value_or(row, "Flags")));
      }

      std::map<std::uint32_t, ModernParamData> params;
      for (std::uint32_t i = 0; i < params_table.RecordCount(); ++i)
      {
        auto row = params_table.RecordByPosition(i);
        ModernParamData param;
        param.id = row_id(row);
        param.skybox_id = uint_value_or(row, "LightSkyboxID");
        param.cloud_type_id = uint_value_or(row, "CloudTypeID");
        param.flags = uint_value_or(row, "Flags");
        param.highlight_sky = uint_value(row, "HighlightSky") != 0;
        param.glow = row.getFloat("Glow");
        param.water_shallow_alpha = row.getFloat("WaterShallowAlpha");
        param.water_deep_alpha = row.getFloat("WaterDeepAlpha");
        param.ocean_shallow_alpha = row.getFloat("OceanShallowAlpha");
        param.ocean_deep_alpha = row.getFloat("OceanDeepAlpha");
        params.emplace(param.id, std::move(param));
      }

      for (std::uint32_t i = 0; i < data_table.RecordCount(); ++i)
      {
        auto row = data_table.RecordByPosition(i);
        auto const param_id = relation_id(row, "LightParamID");
        auto found = params.find(param_id);
        if (found == params.end())
          continue;

        auto& param = found->second;
        int const time = static_cast<int>(uint_value(row, "Time"));
        append_color(param, 0, time, uint_value(row, "DirectColor"));
        append_color(param, 1, time, uint_value(row, "AmbientColor"));
        append_color(param, 2, time, uint_value(row, "SkyTopColor"));
        append_color(param, 3, time, uint_value(row, "SkyMiddleColor"));
        append_color(param, 4, time, uint_value(row, "SkyBand1Color"));
        append_color(param, 5, time, uint_value(row, "SkyBand2Color"));
        append_color(param, 6, time, uint_value(row, "SkySmogColor"));
        append_color(param, 7, time, uint_value(row, "SkyFogColor"));
        append_color(param, 8, time, uint_value(row, "ShadowOpacity"));
        append_color(param, 9, time, uint_value(row, "SunColor"));
        append_color(param, 10, time, uint_value(row, "CloudSunColor"));
        append_color(param, 11, time, uint_value(row, "CloudEmissiveColor"));
        append_color(param, 12, time, uint_value(row, "CloudLayer1AmbientColor"));
        append_color(param, 13, time, uint_value(row, "CloudLayer2AmbientColor"));
        append_color(param, 14, time, uint_value(row, "OceanCloseColor"));
        append_color(param, 15, time, uint_value(row, "OceanFarColor"));
        append_color(param, 16, time, uint_value(row, "RiverCloseColor"));
        append_color(param, 17, time, uint_value(row, "RiverFarColor"));
        append_float(param, 0, time, row.getFloat("FogEnd"));
        append_float(param, 1, time, row.getFloat("FogScaler"));
        append_float(param, 3, time, row.getFloat("CloudDensity"));
      }

      for (auto& [id, param] : params)
      {
        for (auto& values : param.colors)
          std::sort(values.begin(), values.end(), [](auto const& lhs, auto const& rhs) { return lhs.time < rhs.time; });
        for (auto& values : param.floats)
          std::sort(values.begin(), values.end(), [](auto const& lhs, auto const& rhs) { return lhs.time < rhs.time; });
      }

      auto light_dbc = DBCFile::createNew("DBFilesClient\\Light.dbc", 15, 15 * sizeof(std::uint32_t));
      auto params_dbc = DBCFile::createNew("DBFilesClient\\LightParams.dbc", 10, 10 * sizeof(std::uint32_t));
      auto int_band_dbc = DBCFile::createNew("DBFilesClient\\LightIntBand.dbc", 34, 34 * sizeof(std::uint32_t));
      auto float_band_dbc = DBCFile::createNew("DBFilesClient\\LightFloatBand.dbc", 34, 34 * sizeof(std::uint32_t));
      auto skybox_dbc = DBCFile::createNew("DBFilesClient\\LightSkybox.dbc", 3, 3 * sizeof(std::uint32_t));

      for (auto const& [id, param] : params)
      {
        if (id == 0)
          continue;

        auto record = params_dbc.addRecord(id);
        record.write(LightParamsDB::highlightSky, static_cast<std::uint32_t>(param.highlight_sky));
        record.write(LightParamsDB::skybox, skyboxes.contains(param.skybox_id) ? param.skybox_id : 0u);
        record.write(LightParamsDB::cloudTypeID, param.cloud_type_id);
        record.write(LightParamsDB::glow, param.glow);
        record.write(LightParamsDB::water_shallow_alpha, param.water_shallow_alpha);
        record.write(LightParamsDB::water_deep_alpha, param.water_deep_alpha);
        record.write(LightParamsDB::ocean_shallow_alpha, param.ocean_shallow_alpha);
        record.write(LightParamsDB::ocean_deep_alpha, param.ocean_deep_alpha);
        record.write(LightParamsDB::flags, param.flags);

        auto const color_start = id * modern_color_count - (modern_color_count - 1);
        for (std::size_t band = 0; band < modern_color_count; ++band)
          write_legacy_color_band(int_band_dbc, static_cast<std::uint32_t>(color_start + band), param.colors[band]);

        auto const float_start = id * modern_float_count - (modern_float_count - 1);
        for (std::size_t band = 0; band < modern_float_count; ++band)
          write_legacy_float_band(float_band_dbc, static_cast<std::uint32_t>(float_start + band), param.floats[band]);
      }

      for (auto const& [id, skybox] : skyboxes)
      {
        auto record = skybox_dbc.addRecord(id);
        record.writeString(LightSkyboxDB::filename, skybox.first);
        record.write(LightSkyboxDB::flags, skybox.second);
      }

      std::size_t loaded_lights = 0;
      std::size_t unresolved_param_refs = 0;
      std::size_t repaired_primary_param_refs = 0;
      std::size_t lights_without_usable_params = 0;
      for (std::uint32_t i = 0; i < light_table.RecordCount(); ++i)
      {
        auto row = light_table.RecordByPosition(i);
        auto const coords = float_array(row, "GameCoords");
        auto const param_ids = uint_array(row, "LightParamsID");
        if (coords.size() < 3)
          continue;

        std::array<std::uint32_t, 8> resolved_params{};
        for (std::size_t param = 0; param < resolved_params.size(); ++param)
        {
          if (param >= param_ids.size() || param_ids[param] == 0)
            continue;

          if (params.contains(param_ids[param]))
            resolved_params[param] = param_ids[param];
          else
            ++unresolved_param_refs;
        }

        // The legacy Noggit renderer starts on LightParams slot 0. Modern Light rows can
        // legally have an empty/unresolved first slot while later slots still reference
        // valid environment data. Do not turn such a perfectly usable modern light into
        // a black/fallback sky: promote the first valid modern parameter to the primary
        // compatibility slot while preserving all remaining slots verbatim.
        if (resolved_params[0] == 0)
        {
          auto const replacement = std::find_if(
              resolved_params.begin() + 1,
              resolved_params.end(),
              [](std::uint32_t id) { return id != 0; });
          if (replacement != resolved_params.end())
          {
            resolved_params[0] = *replacement;
            ++repaired_primary_param_refs;
          }
          else
          {
            ++lights_without_usable_params;
          }
        }

        auto record = light_dbc.addRecord(row_id(row));
        record.write(LightDB::Map, uint_value(row, "ContinentID"));
        record.write(LightDB::PositionX, coords[0] * legacy_sky_coordinate_scale);
        record.write(LightDB::PositionY, coords[1] * legacy_sky_coordinate_scale);
        record.write(LightDB::PositionZ, coords[2] * legacy_sky_coordinate_scale);
        record.write(LightDB::RadiusInner, row.getFloat("GameFalloffStart") * legacy_sky_coordinate_scale);
        record.write(LightDB::RadiusOuter, row.getFloat("GameFalloffEnd") * legacy_sky_coordinate_scale);

        for (std::size_t param = 0; param < resolved_params.size(); ++param)
          record.write(LightDB::DataIDs + param, resolved_params[param]);

        ++loaded_lights;
      }

      gLightDB.overwriteWith(light_dbc);
      gLightParamsDB.overwriteWith(params_dbc);
      gLightIntBandDB.overwriteWith(int_band_dbc);
      gLightFloatBandDB.overwriteWith(float_band_dbc);
      gLightSkyboxDB.overwriteWith(skybox_dbc);

      Log << "[ModernDB2][Sky] Loaded Shadowlands CASC DB2 lighting: Light=" << loaded_lights
          << " LightParams=" << params_table.RecordCount()
          << " LightData=" << data_table.RecordCount()
          << " LightSkybox=" << skyboxes.size()
          << " unresolved-param-refs=" << unresolved_param_refs
          << " repaired-primary-param-refs=" << repaired_primary_param_refs
          << " lights-without-usable-params=" << lights_without_usable_params << std::endl;
      return loaded_lights != 0 && !params.empty();
    }
    catch (std::exception const& e)
    {
      LogError << "[ModernDB2][Sky] Failed loading Shadowlands sky DB2 data: " << e.what() << std::endl;
    }
    catch (...)
    {
      LogError << "[ModernDB2][Sky] Failed loading Shadowlands sky DB2 data: unknown error." << std::endl;
    }
    return false;
  }
}
