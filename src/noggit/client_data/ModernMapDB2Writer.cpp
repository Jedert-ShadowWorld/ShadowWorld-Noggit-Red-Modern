#include "ModernMapDB2Writer.hpp"
#include "ModernDB2Delta.hpp"

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <blizzard-archive-library/include/ClientFile.hpp>
#include <blizzard-database-library/include/BlizzardDatabase.h>
#include <core/Table.h>
#include <formats/BinaryTable.h>
#include <schema/DbdParser.h>

#include <QDialog>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QHeaderView>
#include <QMessageBox>
#include <QSaveFile>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <map>
#include <set>

namespace
{
  std::filesystem::path path(QString const& text) { return std::filesystem::path(text.toStdWString()); }

  bool parseCell(dbc::CellValue& value, dbc::Column const& column, std::string const& text, std::string& error)
  {
    if (!dbc::SetCellFromString(value, column.type, text, error)) return false;
    if (column.type == dbc::ValueType::Float)
    {
      auto number = static_cast<float>(std::get<double>(value));
      if (!std::isfinite(number)) { error = "Number exceeds the 32-bit float range."; return false; }
      value = static_cast<double>(number);
    }
    else if (column.type == dbc::ValueType::SignedInteger && column.storageBits && column.storageBits < 64)
    {
      auto number = std::get<std::int64_t>(value);
      auto limit = std::int64_t{1} << (column.storageBits - 1);
      if (number < -limit || number >= limit) { error = "Integer exceeds the field's signed range."; return false; }
    }
    else if (column.type == dbc::ValueType::UnsignedInteger && column.storageBits && column.storageBits < 64)
    {
      if (std::get<std::uint64_t>(value) >= (std::uint64_t{1} << column.storageBits))
      { error = "Integer exceeds the field's unsigned range."; return false; }
    }
    return true;
  }

  struct NativeTable
  {
    QTemporaryDir scratch;
    dbc::Table table;
    QString target;
    std::string definitionDirectory;
    std::string schemaBuild;
    std::string name;

    explicit NativeTable(std::string tableName = "Map", bool readOnly = false) : name(std::move(tableName))
    {
      auto project = Noggit::Project::CurrentProject::get();
      if (!project || !project->ClientData || !scratch.isValid())
        throw std::runtime_error("No active modern client or temporary directory.");
      auto filename = BlizzardArchive::ClientData::normalizeFilenameInternal("dbfilesclient/" + name + ".db2");
      target = QString::fromStdString(project->ClientData->getDiskPath(
          BlizzardArchive::Listfile::FileKey(filename)));
      BlizzardArchive::ClientFile source(filename, project->ClientData.get());
      if (source.isEof() || !source.getSize())
        throw std::runtime_error("The source " + name + ".db2 could not be read.");
      QString input = scratch.filePath(QString::fromStdString(name + ".db2"));
      QFile file(input);
      if (!file.open(QIODevice::WriteOnly)
          || file.write(source.getBuffer(), source.getSize()) != static_cast<qint64>(source.getSize()))
        throw std::runtime_error("Could not stage the source " + name + ".db2.");
      file.close();

      std::string error;
      auto header = dbc::InspectHeader(path(input), error);
      if (!error.empty()) throw std::runtime_error(error);
      if (header.format < dbc::FileFormat::WDC1 || header.format > dbc::FileFormat::WDC5 || !header.layoutHash)
        throw std::runtime_error(name + ".db2 is not a supported WDC1-WDC5 table.");
      auto configuration = Noggit::Application::NoggitApplication::instance()->getConfiguration();
      std::optional<dbc::DbdFile> schema;
      std::optional<dbc::SchemaSelection> selection;
      auto executable = path(QCoreApplication::applicationDirPath());
      for (auto const& directory : {std::filesystem::path(configuration->ApplicationDatabaseDefinitionsPath),
                                   executable / "native-db2-definitions", executable / "definitions-forever"})
      {
        auto definition = directory / (name + ".dbd");
        if (!std::filesystem::exists(definition)) continue;
        schema = dbc::DbdParser::Parse(definition, error);
        if (!schema) continue;
        // Never substitute a guessed build for an unknown physical layout.
        selection = dbc::SelectSchema(*schema, dbc::Profile(dbc::GameVersion::Forever),
                                      {}, header.layoutHash, error);
        if (selection) { definitionDirectory = directory.string(); break; }
      }
      if (!selection) throw std::runtime_error(error);
      schemaBuild = selection->selectedBuild.ToString();
      auto loaded = dbc::BinaryTableCodec::LoadWdc(path(input),
          dbc::FlattenForWdc5(*schema, selection->version), error);
      if (!loaded) throw std::runtime_error(error);
      table = std::move(*loaded);
      if (!readOnly && !table.writable) throw std::runtime_error(table.readOnlyReason);
    }

    std::size_t idColumn() const
    {
      auto it = std::find_if(table.columns.begin(), table.columns.end(), [](auto const& c) { return c.id; });
      if (it == table.columns.end()) throw std::runtime_error(name + ".dbd has no ID field.");
      return static_cast<std::size_t>(it - table.columns.begin());
    }

    auto find(std::uint32_t id)
    {
      auto index = idColumn();
      return std::find_if(table.rows.begin(), table.rows.end(), [&](auto const& row) {
        return dbc::CellToString(row.cells.at(index)) == std::to_string(id);
      });
    }

    void commit()
    {
      std::string error;
      auto output = scratch.filePath(QString::fromStdString(name + ".saved.db2"));
      if (!dbc::BinaryTableCodec::SaveWdc(table, path(output), error))
        throw std::runtime_error(error);
      // Use the application's reader too: a codec round trip alone is not enough.
      auto project = Noggit::Project::CurrentProject::get();
      QFile input(output);
      if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not read the saved " + name + ".db2.");
      auto bytes = input.readAll();
      if (bytes.size() != input.size()) throw std::runtime_error("Incomplete saved Map.db2.");
      if (name == "Map")
      {
      BlizzardDatabaseLib::BlizzardDatabase check(definitionDirectory,
          BlizzardDatabaseLib::Structures::Build(schemaBuild),
          project->projectVersion == Noggit::Project::ProjectVersion::FOREVER);
      auto& checked = check.LoadTable("Map", [&](std::string const&) {
        return std::make_shared<BlizzardDatabaseLib::Stream::IMemStream>(bytes.constData(), bytes.size());
      });
      if (checked.RecordCount() != table.rows.size())
        throw std::runtime_error("Noggit could not reread every saved Map.db2 record.");
      for (std::uint32_t i = 0; i < checked.RecordCount(); ++i)
      {
        auto decoded = checked.RecordByPosition(i);
        for (std::size_t c = 0; c < table.columns.size(); ++c)
        {
          auto const& column = table.columns[c];
          if (column.name != "Directory" && column.name != "MapName_lang") continue;
          auto const entry = decoded.Columns.find(column.name);
          if (entry == decoded.Columns.end()) throw std::runtime_error("Saved Map field is missing: " + column.name);
          auto const& value = entry->second.Value.empty() && !entry->second.Values.empty()
              ? entry->second.Values.front() : entry->second.Value;
          if (value != dbc::CellToString(table.rows[i].cells[c]))
            throw std::runtime_error("Noggit Map.db2 readback mismatch: " + column.name);
        }
      }
      }
      auto destination = path(target);
      std::filesystem::create_directories(destination.parent_path());
      if (std::filesystem::exists(destination))
      {
        auto backup = destination;
        backup += ".bak";
        std::filesystem::copy_file(destination, backup, std::filesystem::copy_options::overwrite_existing);
      }
      QSaveFile saved(target);
      saved.setDirectWriteFallback(false);
      if (!saved.open(QIODevice::WriteOnly) || saved.write(bytes) != bytes.size() || !saved.commit())
        throw std::runtime_error(name + ".db2 could not be committed atomically: " + saved.errorString().toStdString());
      Log << "[ModernDB2][" << name << "] Saved native DB2: " << target.toStdString()
          << ", records=" << table.rows.size() << std::endl;
    }
  };

  dbc::CellValue defaultValue(dbc::Column const& column)
  {
    switch (column.type)
    {
      case dbc::ValueType::String: return std::string{};
      case dbc::ValueType::Float: return 0.0;
      case dbc::ValueType::UnsignedInteger: return std::uint64_t{0};
      default: return std::int64_t{0};
    }
  }

  enum class LegacyKind { Integer, Float, String, Localized, FileDataID };
  struct EditorField
  {
    std::size_t offset;
    std::string name;
    LegacyKind kind = LegacyKind::Integer;
    double scale = 1.0;
  };

  std::vector<EditorField> editorFields(std::string const& name)
  {
    using K = LegacyKind;
    if (name == "AreaTable")
      return {{1,"ContinentID"},{2,"ParentAreaID"},{3,"AreaBit"},{4,"Flags[0]"},
              {5,"SoundProviderPref"},{6,"SoundProviderPrefUnderwater"},{7,"AmbienceID"},
              {8,"ZoneMusic"},{9,"IntroSound"},{10,"ExplorationLevel"},{11,"AreaName_lang",K::Localized},
              {28,"FactionGroupMask"},{29,"LiquidTypeID[0]"},{30,"LiquidTypeID[1]"},
              {31,"LiquidTypeID[2]"},{32,"LiquidTypeID[3]"},{33,"MinElevation",K::Float},
              {34,"Ambient_multiplier",K::Float},{35,"LightID"}};
    if (name == "AreaTrigger")
      return {{1,"ContinentID"},{2,"Pos[0]",K::Float},{3,"Pos[1]",K::Float},
              {4,"Pos[2]",K::Float},{5,"Radius",K::Float},{6,"Box_length",K::Float},
              {7,"Box_width",K::Float},{8,"Box_height",K::Float},{9,"Box_yaw",K::Float}};
    if (name == "Light")
      return {{1,"ContinentID"},{2,"GameCoords[0]",K::Float,36},{3,"GameCoords[1]",K::Float,36},
              {4,"GameCoords[2]",K::Float,36},{5,"GameFalloffStart",K::Float,36},
              {6,"GameFalloffEnd",K::Float,36},{7,"LightParamsID[0]"},{8,"LightParamsID[1]"},
              {9,"LightParamsID[2]"},{10,"LightParamsID[3]"},{11,"LightParamsID[4]"},
              {12,"LightParamsID[5]"},{13,"LightParamsID[6]"},{14,"LightParamsID[7]"}};
    if (name == "LightParams")
      return {{1,"HighlightSky"},{2,"LightSkyboxID"},{3,"CloudTypeID"},{4,"Glow",K::Float},
              {5,"WaterShallowAlpha",K::Float},{6,"WaterDeepAlpha",K::Float},
              {7,"OceanShallowAlpha",K::Float},{8,"OceanDeepAlpha",K::Float},{9,"Flags"}};
    if (name == "LightSkybox")
      return {{1,"SkyboxFileDataID",K::FileDataID},{2,"Flags"}};
    if (name == "GroundEffectDoodad")
      return {{1,"ModelFileID",K::FileDataID},{2,"Flags"}};
    if (name == "GroundEffectTexture")
      return {{1,"DoodadID[0]"},{2,"DoodadID[1]"},{3,"DoodadID[2]"},{4,"DoodadID[3]"},
              {5,"DoodadWeight[0]"},{6,"DoodadWeight[1]"},{7,"DoodadWeight[2]"},
              {8,"DoodadWeight[3]"},{9,"Density"},{10,"Sound"}};
    if (name == "SoundAmbience") return {{1,"AmbienceID[0]"},{2,"AmbienceID[1]"}};
    if (name == "ZoneMusic")
      return {{1,"SetName",K::String},{2,"SilenceIntervalMin[0]"},{3,"SilenceIntervalMin[1]"},
              {4,"SilenceIntervalMax[0]"},{5,"SilenceIntervalMax[1]"},{6,"Sounds[0]"},{7,"Sounds[1]"}};
    if (name == "ZoneIntroMusicTable")
      return {{1,"Name",K::String},{2,"SoundID"},{3,"Priority"},{4,"MinDelayMinutes"}};
    throw std::runtime_error(name + ": this legacy editor has no native DB2 adapter. No file was written.");
  }

  std::size_t columnIndex(NativeTable const& native, std::string name)
  {
    auto found = std::find_if(native.table.columns.begin(), native.table.columns.end(),
                             [&](auto const& c) { return c.name == name; });
    if (found == native.table.columns.end() && name == "Flags[0]")
      return columnIndex(native, "Flags");
    if (found == native.table.columns.end())
      throw std::runtime_error(native.name + ": the selected schema has no field " + name);
    return found - native.table.columns.begin();
  }

  std::string legacyValue(DBCFile::Record const& row, EditorField const& field, dbc::Column const& column)
  {
    switch (field.kind)
    {
      case LegacyKind::Float: return dbc::CellToString(static_cast<double>(row.getFloat(field.offset)));
      case LegacyKind::String:
      case LegacyKind::FileDataID: return row.getString(field.offset);
      case LegacyKind::Localized: return row.getLocalizedString(field.offset);
      default: return column.type == dbc::ValueType::SignedInteger
          ? std::to_string(row.getInt(field.offset)) : std::to_string(row.getUInt(field.offset));
    }
  }

  std::string nativeValue(DBCFile::Record const& row, EditorField const& field, dbc::Column const& column)
  {
    auto value = legacyValue(row, field, column);
    if (field.kind == LegacyKind::Float)
      return dbc::CellToString(static_cast<double>(row.getFloat(field.offset)) / field.scale);
    if (field.kind != LegacyKind::FileDataID) return value;
    if (value.empty()) return "0";
    if (value.rfind("filedataid:", 0) == 0) return value.substr(11);
    auto project = Noggit::Project::CurrentProject::get();
    auto id = project->ClientData->listfile()->getFileDataID(
        BlizzardArchive::ClientData::normalizeFilenameInternal(value));
    if (!id) throw std::runtime_error("No FileDataID mapping for " + value + ". Add it to the project listfile first.");
    return std::to_string(id);
  }

  void setCell(NativeTable const& native, dbc::Row& row, std::size_t column, std::string const& text)
  {
    std::string error;
    if (!parseCell(row.cells.at(column), native.table.columns.at(column), text, error))
      throw std::runtime_error(native.name + "." + native.table.columns.at(column).name + ": " + error);
  }

  dbc::Row emptyRow(NativeTable& native, std::uint32_t id)
  {
    dbc::Row row;
    row.stableId = native.table.nextStableId++;
    for (auto const& column : native.table.columns) row.cells.push_back(defaultValue(column));
    setCell(native, row, native.idColumn(), std::to_string(id));
    return row;
  }

  void saveEditorTable(DBCFile& legacy, std::string const& name)
  {
    auto fields = editorFields(name);
    auto baseline = legacy.saveBaseline();
    if (!baseline) throw std::runtime_error(name + ": the original editor snapshot is missing; reopen the project before saving.");
    NativeTable native(name);
    std::vector<std::size_t> columns;
    for (auto const& field : fields)
    {
      try { columns.push_back(columnIndex(native, field.name)); }
      catch (std::exception const&)
      {
        if (name != "AreaTable" || (field.name != "ExplorationLevel" && field.name != "MinElevation" && field.name != "LightID")) throw;
        columns.push_back(native.table.columns.size());
      }
    }
    auto comparisonColumn = [&](std::size_t index) {
      if (columns[index] < native.table.columns.size()) return native.table.columns[columns[index]];
      dbc::Column column;
      column.type = fields[index].kind == LegacyKind::Float ? dbc::ValueType::Float : dbc::ValueType::SignedInteger;
      return column;
    };
    std::vector<Noggit::ClientData::NativeRowChange> changes;
    std::set<std::uint32_t> removed;
    for (auto const& source : legacy)
    {
      auto id = source.getUInt(0);
      bool added = !baseline->CheckIfIdExists(id);
      auto previous = added ? source : baseline->getByID(id);
      bool changed = added;
      for (std::size_t i = 0; i < fields.size(); ++i)
        changed |= legacyValue(source, fields[i], comparisonColumn(i))
                != legacyValue(previous, fields[i], comparisonColumn(i));
      if (!changed) continue;
      Noggit::ClientData::NativeRowChange change{id, added, {}};
      if (added && name == "GroundEffectDoodad")
      {
        for (auto const* scale : {"Animscale", "Size_variation_min", "Size_variation_max"})
        {
          auto found = std::find_if(native.table.columns.begin(), native.table.columns.end(),
                                   [&](auto const& column) { return column.name == scale; });
          if (found == native.table.columns.end()) continue;
          auto value = defaultValue(*found);
          std::string error;
          if (!parseCell(value, *found, "1", error)) throw std::runtime_error(error);
          change.fields.emplace(found - native.table.columns.begin(), std::move(value));
        }
      }
      for (std::size_t i = 0; i < fields.size(); ++i)
      {
        auto const column = comparisonColumn(i);
        if (added || legacyValue(source, fields[i], column) != legacyValue(previous, fields[i], column))
        {
          if (columns[i] == native.table.columns.size())
          {
            if (legacyValue(source, fields[i], column) != "0")
              throw std::runtime_error(name + ": " + fields[i].name + " no longer exists in this client schema.");
            continue;
          }
          auto value = defaultValue(column);
          std::string error;
          if (!parseCell(value, column, nativeValue(source, fields[i], column), error))
            throw std::runtime_error(name + "." + column.name + ": " + error);
          change.fields.emplace(columns[i], std::move(value));
        }
      }
      changes.push_back(std::move(change));
    }
    // Rows hidden by the compatibility bridge are not deletions.
    for (auto const& old : *baseline)
      if (!legacy.CheckIfIdExists(old.getUInt(0)))
      {
        removed.insert(old.getUInt(0));
      }
    if (!changes.empty() || !removed.empty())
    {
      Noggit::ClientData::applyNativeEditorChanges(native.table, native.idColumn(), changes, removed);
      native.commit();
    }
  }

  struct LightBand
  {
    std::string field;
    std::map<std::uint32_t, double> keys;
    std::map<std::uint32_t, double> oldKeys;
    bool color = false;
    bool changed() const { return keys != oldKeys; }
  };

  std::map<std::uint32_t, double> bandKeys(DBCFile& file, std::uint32_t id, bool color)
  {
    std::map<std::uint32_t, double> result;
    if (!file.CheckIfIdExists(id)) return result;
    auto record = file.getByID(id);
    auto count = record.getUInt(1);
    if (count > 16) throw std::runtime_error("Light band exceeds the editor's 16-key limit.");
    for (std::uint32_t i = 0; i < count; ++i)
    {
      auto time = record.getUInt(2 + i);
      if (time >= 2880 || result.contains(time))
        throw std::runtime_error("Light band contains an invalid or duplicate time.");
      result.emplace(time, color ? static_cast<double>(record.getUInt(18 + i)) : record.getFloat(18 + i));
    }
    return result;
  }

  double sampleBand(LightBand const& band, std::uint32_t time)
  {
    return Noggit::ClientData::sampleNativeLightBand(band.keys, time, band.color && band.field != "ShadowOpacity");
  }

  void saveLightData()
  {
    auto oldColors = gLightIntBandDB.saveBaseline();
    auto oldFloats = gLightFloatBandDB.saveBaseline();
    if (!oldColors || !oldFloats) throw std::runtime_error("LightData: reopen the project to establish the original bands.");
    std::set<std::uint32_t> parameters;
    for (auto* file : {static_cast<DBCFile*>(&gLightIntBandDB), oldColors.get()})
      for (auto const& row : *file) if (row.getUInt(0)) parameters.insert((row.getUInt(0) - 1) / 18 + 1);
    for (auto* file : {static_cast<DBCFile*>(&gLightFloatBandDB), oldFloats.get()})
      for (auto const& row : *file) if (row.getUInt(0)) parameters.insert((row.getUInt(0) - 1) / 6 + 1);
    std::map<std::uint32_t, std::vector<LightBand>> changes;
    std::array<char const*, 18> const colorFields = {"DirectColor", "AmbientColor", "SkyTopColor", "SkyMiddleColor",
        "SkyBand1Color", "SkyBand2Color", "SkySmogColor", "SkyFogColor", "ShadowOpacity", "SunColor",
        "CloudSunColor", "CloudEmissiveColor", "CloudLayer1AmbientColor", "CloudLayer2AmbientColor",
        "OceanCloseColor", "OceanFarColor", "RiverCloseColor", "RiverFarColor"};
    std::array<char const*, 6> const floatFields = {"FogEnd", "FogScaler", "", "CloudDensity", "", ""};
    for (auto param : parameters)
    {
      std::vector<LightBand> bands;
      bool changed = false;
      for (std::uint32_t i = 0; i < 24; ++i)
      {
        bool color = i < 18;
        auto index = color ? i : i - 18;
        auto id = (param - 1) * (color ? 18 : 6) + index + 1;
        LightBand band{color ? colorFields[index] : floatFields[index],
            bandKeys(color ? static_cast<DBCFile&>(gLightIntBandDB) : gLightFloatBandDB, id, color),
            bandKeys(color ? *oldColors : *oldFloats, id, color), color};
        if (band.changed())
        {
          if (band.field.empty()) throw std::runtime_error("This legacy float channel has no modern LightData equivalent.");
          if (band.keys.empty()) throw std::runtime_error("Deleting every key of a LightData channel is not supported.");
          changed = true;
        }
        bands.push_back(std::move(band));
      }
      if (changed) changes.emplace(param, std::move(bands));
    }
    if (changes.empty()) return;
    NativeTable native("LightData");
    auto relation = columnIndex(native, "LightParamID");
    auto timeColumn = columnIndex(native, "Time");
    auto idColumn = native.idColumn();
    std::uint32_t nextID = 1;
    for (auto const& row : native.table.rows)
    {
      auto id = std::stoull(dbc::CellToString(row.cells[idColumn]));
      if (id >= std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("LightData ID space is exhausted.");
      nextID = std::max(nextID, static_cast<std::uint32_t>(id + 1));
    }
    for (auto const& [param, bands] : changes)
    {
      std::set<std::uint32_t> times, oldTimes;
      for (auto const& band : bands)
      {
        for (auto const& [time, value] : band.keys) times.insert(time);
        for (auto const& [time, value] : band.oldKeys) oldTimes.insert(time);
      }
      std::map<std::uint32_t, dbc::Row> originals;
      for (auto const& row : native.table.rows)
        if (std::stoul(dbc::CellToString(row.cells[relation])) == param)
        {
          auto time = static_cast<std::uint32_t>(std::stoul(dbc::CellToString(row.cells[timeColumn])));
          if (!originals.emplace(time, row).second) throw std::runtime_error("Duplicate native LightData parameter/time.");
          // The compatibility bridge may show only the first 16 native keys.
          if (!oldTimes.contains(time)) times.insert(time);
        }
      std::erase_if(native.table.rows, [&](auto const& row) {
        return std::stoul(dbc::CellToString(row.cells[relation])) == param;
      });
      for (auto time : times)
      {
        auto existing = originals.find(time);
        bool added = existing == originals.end();
        dbc::Row row;
        if (!added) row = existing->second;
        else
        {
          if (!nextID) throw std::runtime_error("LightData ID space is exhausted.");
          row = originals.empty() ? emptyRow(native, nextID) : originals.begin()->second;
          row.stableId = native.table.nextStableId++;
          setCell(native, row, idColumn, std::to_string(nextID++));
          setCell(native, row, relation, std::to_string(param));
          setCell(native, row, timeColumn, std::to_string(time));
        }
        for (auto const& band : bands)
          if (!band.field.empty() && !band.keys.empty() && (added || (oldTimes.contains(time) && band.changed())))
          {
            auto column = columnIndex(native, band.field);
            double value = sampleBand(band, time);
            auto text = native.table.columns[column].type == dbc::ValueType::Float
                ? dbc::CellToString(value) : std::to_string(static_cast<std::uint32_t>(std::lround(value)));
            setCell(native, row, column, text);
          }
        native.table.rows.push_back(std::move(row));
      }
    }
    native.commit();
    gLightIntBandDB.markSaved();
    gLightFloatBandDB.markSaved();
  }
}

bool Noggit::ClientData::saveModernMapDB2(std::uint32_t map_id, QWidget* parent)
{
  NativeTable map;
  auto found = map.find(map_id);
  bool const created = found == map.table.rows.end();
  if (created)
  {
    dbc::Row row;
    row.stableId = map.table.nextStableId++;
    for (auto const& column : map.table.columns) row.cells.push_back(defaultValue(column));
    map.table.rows.push_back(std::move(row));
    found = std::prev(map.table.rows.end());
  }
  auto& row = *found;
  std::unordered_set<std::string> wizardFields;
  auto set = [&](std::string const& name, std::string const& value, bool wizard = true) {
    auto column = std::find_if(map.table.columns.begin(), map.table.columns.end(),
                              [&](auto const& c) { return c.name == name; });
    if (column == map.table.columns.end()) return;
    std::string error;
    if (!parseCell(row.cells[column - map.table.columns.begin()], *column, value, error))
      throw std::runtime_error(name + ": " + error);
    if (wizard) wizardFields.insert(name);
  };
  auto legacy = gMapDB.getByID(map_id);
  set("ID", std::to_string(map_id));
  set("Directory", legacy.getString(MapDB::InternalName));
  set("MapName_lang", legacy.getLocalizedString(MapDB::Name));
  set("MapDescription1_lang", legacy.getLocalizedString(MapDB::MapDescriptionAlliance));
  set("MapDescription0_lang", legacy.getLocalizedString(MapDB::MapDescriptionHorde));
  for (auto const& field : {std::pair{"InstanceType", MapDB::AreaType}, {"AreaTableID", MapDB::AreaTableID},
                           {"LoadingScreenID", MapDB::LoadingScreen}, {"CorpseMapID", MapDB::corpseMapID},
                           {"TimeOfDayOverride", MapDB::TimeOfDayOverride}, {"ExpansionID", MapDB::ExpansionID},
                           {"RaidOffset", MapDB::RaidOffset}, {"MaxPlayers", MapDB::NumberOfPlayers}})
    set(field.first, std::to_string(legacy.getInt(field.second)));
  for (auto const& field : {std::pair{"MinimapIconScale", MapDB::minimapIconScale},
                           {"Corpse[0]", MapDB::corpseX}, {"Corpse[1]", MapDB::corpseY},
                           {"CorpseX", MapDB::corpseX}, {"CorpseY", MapDB::corpseY}})
    set(field.first, dbc::CellToString(static_cast<double>(legacy.getFloat(field.second))));
  // The legacy wizard controls only bit 4, not the other modern Flags bits.
  for (auto const& name : {"Flags", "Flags[0]"})
  {
    auto col = std::find_if(map.table.columns.begin(), map.table.columns.end(),
                           [&](auto const& c) { return c.name == name; });
    if (col != map.table.columns.end())
    {
      auto bits = std::stoll(dbc::CellToString(row.cells[col - map.table.columns.begin()]));
      set(name, std::to_string((bits & ~16LL) | (legacy.getUInt(MapDB::Flags) & 16u)));
    }
  }
  if (created)
  {
    set("ParentMapID", "-1", false);
    set("CosmeticParentMapID", "-1", false);
    auto directory = std::string(legacy.getString(MapDB::InternalName));
    auto project = Noggit::Project::CurrentProject::get();
    auto id = project->ClientData->listfile()->getFileDataID(
        BlizzardArchive::ClientData::normalizeFilenameInternal("world/maps/" + directory + "/" + directory + ".wdt"));
    set("WdtFileDataID", std::to_string(id), false);
  }

  QDialog dialog(parent);
  dialog.setWindowTitle("Map.db2 - native schema");
  dialog.resize(720, 640);
  auto layout = new QVBoxLayout(&dialog);
  auto fields = new QTableWidget(static_cast<int>(map.table.columns.size()), 2, &dialog);
  fields->setHorizontalHeaderLabels({"Field", "Value"});
  fields->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  for (int i = 0; i < fields->rowCount(); ++i)
  {
    auto const& column = map.table.columns[i];
    auto label = new QTableWidgetItem(QString::fromStdString(column.name));
    label->setFlags(label->flags() & ~Qt::ItemIsEditable);
    fields->setItem(i, 0, label);
    auto value = new QTableWidgetItem(QString::fromStdString(dbc::CellToString(row.cells[i])));
    if (wizardFields.contains(column.name)) value->setFlags(value->flags() & ~Qt::ItemIsEditable);
    fields->setItem(i, 1, value);
  }
  layout->addWidget(fields);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    auto cells = row.cells;
    std::string error;
    for (int i = 0; i < fields->rowCount(); ++i)
      if (!parseCell(cells[i], map.table.columns[i],
                                 fields->item(i, 1)->text().toStdString(), error))
      {
        QMessageBox::warning(&dialog, "Invalid Map field", QString::fromStdString(map.table.columns[i].name + ": " + error));
        return;
      }
    row.cells = std::move(cells);
    dialog.accept();
  });
  if (dialog.exec() != QDialog::Accepted) return false;
  map.commit();
  return true;
}

void Noggit::ClientData::removeModernMapDB2(std::uint32_t map_id)
{
  NativeTable map;
  auto row = map.find(map_id);
  if (row == map.table.rows.end()) throw std::runtime_error("Map.db2 record was not found.");
  map.table.rows.erase(row);
  map.commit();
}

void Noggit::ClientData::saveModernEditorDB2(DBCFile& legacy, std::string const& filename)
{
  auto leaf = filename.substr(filename.find_last_of("/\\") + 1);
  auto name = leaf.substr(0, leaf.find_last_of('.'));
  if (name == "LightIntBand" || name == "LightFloatBand") { saveLightData(); return; }
  saveEditorTable(legacy, name);
}

bool Noggit::ClientData::saveEditorDatabase(DBCFile& table, QWidget* parent)
{
  try { table.save(); return true; }
  catch (std::exception const& error)
  {
    LogError << "[ModernDB2] Editor database save failed: " << error.what() << std::endl;
    QMessageBox::warning(parent, "Database save failed", QString::fromUtf8(error.what()));
    return false;
  }
}

void Noggit::ClientData::loadModernEditorSoundDB2()
{
  try
  {
    NativeTable preferences("SoundProviderPreferences", true);
    auto out = DBCFile::createNew("DBFilesClient\\SoundProviderPreferences.dbc", 2, 8);
    auto description = columnIndex(preferences, "Description");
    for (auto const& row : preferences.table.rows)
    {
      auto record = out.addRecord(std::stoul(dbc::CellToString(row.cells[preferences.idColumn()])));
      record.writeString(1, dbc::CellToString(row.cells[description]));
    }
    gSoundProviderPreferencesDB.overwriteWith(out);
  }
  catch (std::exception const& error)
  {
    LogError << "[ModernDB2][SoundProviderPreferences] " << error.what() << std::endl;
  }
  try
  {
    // A read-only compatibility view supplies SoundKit IDs to the existing
    // ambience/music pickers. It must never be written as SoundEntries.dbc.
    NativeTable kits("SoundKit", true);
    auto out = DBCFile::createNew("DBFilesClient\\SoundEntries.dbc", 30, 120);
    std::array<EditorField, 8> const fields = {{{1,"SoundType"},{24,"VolumeFloat",LegacyKind::Float},
        {25,"Flags"},{26,"MinDistance",LegacyKind::Float},{27,"DistanceCutoff",LegacyKind::Float},
        {28,"EAXDef"},{29,"SoundKitAdvancedID"},{0,"ID"}}};
    for (auto const& row : kits.table.rows)
    {
      auto id = std::stoul(dbc::CellToString(row.cells[kits.idColumn()]));
      auto record = out.addRecord(id);
      record.writeString(2, "SoundKit " + std::to_string(id));
      for (auto const& field : fields)
      {
        auto const value = dbc::CellToString(row.cells[columnIndex(kits, field.name)]);
        if (field.kind == LegacyKind::Float) record.write(field.offset, std::stof(value));
        else record.write(field.offset, static_cast<std::uint32_t>(std::stoul(value)));
      }
    }
    gSoundEntriesDB.overwriteWith(out);
  }
  catch (std::exception const& error)
  {
    LogError << "[ModernDB2][SoundKit] " << error.what() << std::endl;
  }
  for (auto const& entry : {std::pair{"SoundAmbience", static_cast<DBCFile*>(&gSoundAmbienceDB)},
                           {"ZoneMusic", static_cast<DBCFile*>(&gZoneMusicDB)},
                           {"ZoneIntroMusicTable", static_cast<DBCFile*>(&gZoneIntroMusicTableDB)}})
  {
    try
    {
      NativeTable native(entry.first);
      auto fields = editorFields(entry.first);
      auto count = entry.second->getFieldCount();
      if (!count) count = std::string(entry.first) == "SoundAmbience" ? 3 : std::string(entry.first) == "ZoneMusic" ? 8 : 5;
      auto out = DBCFile::createNew(std::string("DBFilesClient\\") + entry.first + ".dbc", count, count * 4);
      for (auto const& row : native.table.rows)
      {
        auto record = out.addRecord(std::stoul(dbc::CellToString(row.cells[native.idColumn()])));
        for (auto const& field : fields)
        {
          auto const value = dbc::CellToString(row.cells[columnIndex(native, field.name)]);
          if (field.kind == LegacyKind::String) record.writeString(field.offset, value);
          else record.write(field.offset, static_cast<std::uint32_t>(std::stoul(value)));
        }
      }
      entry.second->overwriteWith(out);
    }
    catch (std::exception const& e)
    {
      LogError << "[ModernDB2][Sound] " << e.what() << std::endl;
    }
  }
}
