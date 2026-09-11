#include "ApplicationProject.h"
#include "ApplicationProjectReader.h"
#include "ApplicationProjectWriter.h"

#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/World.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>

#include <blizzard-database-library/include/BlizzardDatabase.h>
#include <blizzard-archive-library/include/CASCArchive.hpp>
#include <string>
#include <blizzard-archive-library/include/Exception.hpp>
#include <blizzard-archive-library/include/ClientFile.hpp>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <exception>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <urlmon.h>
#pragma comment(lib, "urlmon.lib")
#endif
namespace
{
  constexpr wchar_t WOWDEV_LISTFILE_URL[] = L"https://github.com/wowdev/wow-listfile/releases/latest/download/community-listfile.csv";

  bool fileExistsWithContent(std::filesystem::path const& path)
  {
    std::error_code error;
    return std::filesystem::exists(path, error) && std::filesystem::is_regular_file(path, error)
      && std::filesystem::file_size(path, error) > 0;
  }

  bool filesAreEqual(std::filesystem::path const& lhs, std::filesystem::path const& rhs)
  {
    std::error_code error;
    if (!fileExistsWithContent(lhs) || !fileExistsWithContent(rhs))
      return false;

    if (std::filesystem::file_size(lhs, error) != std::filesystem::file_size(rhs, error))
      return false;

    std::ifstream lhs_stream(lhs, std::ios::binary);
    std::ifstream rhs_stream(rhs, std::ios::binary);
    return std::equal(std::istreambuf_iterator<char>(lhs_stream), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(rhs_stream));
  }

  bool looksLikeListfile(std::filesystem::path const& path)
  {
    std::ifstream stream(path);
    std::string line;
    for (int i = 0; i < 25 && std::getline(stream, line); ++i)
    {
      if (line.find(';') != std::string::npos)
        return true;
    }

    return false;
  }

  bool refreshWowdevListfile(std::filesystem::path const& project_path, std::string& error_message)
  {
    std::error_code error;
    std::filesystem::create_directories(project_path, error);
    if (error)
    {
      error_message = "Could not create project directory for listfile.csv: " + error.message();
      return false;
    }

    auto const listfile_path = project_path / "listfile.csv";
    auto const download_path = project_path / "listfile.csv.download";

#ifdef _WIN32
    std::filesystem::remove(download_path, error);
    HRESULT const result = URLDownloadToFileW(nullptr, WOWDEV_LISTFILE_URL, download_path.wstring().c_str(), 0, nullptr);

    if (SUCCEEDED(result) && fileExistsWithContent(download_path) && looksLikeListfile(download_path))
    {
      if (!filesAreEqual(listfile_path, download_path))
      {
        std::filesystem::copy_file(download_path, listfile_path, std::filesystem::copy_options::overwrite_existing, error);
        if (error)
        {
          error_message = "Downloaded wowdev listfile, but could not write listfile.csv: " + error.message();
          std::filesystem::remove(download_path, error);
          return fileExistsWithContent(listfile_path);
        }
      }

      std::filesystem::remove(download_path, error);
      return true;
    }

    std::filesystem::remove(download_path, error);
#endif

    if (fileExistsWithContent(listfile_path))
      return true;

    error_message = "listfile.csv is missing and automatic download from wowdev/wow-listfile failed.";
    return false;
  }

  std::vector<std::string> splitBuildInfoLine(std::string const& line)
  {
    std::vector<std::string> values;
    std::string value;
    std::stringstream stream(line);

    while (std::getline(stream, value, '|'))
      values.push_back(value);

    return values;
  }

  std::string detectClientBuildVersion(std::filesystem::path const& raw_client_path)
  {
    std::vector<std::filesystem::path> candidates;
    auto client_path = raw_client_path;

    candidates.push_back(client_path / ".build.info");

    if (client_path.filename() == "_retail_")
      candidates.push_back(client_path.parent_path() / ".build.info");
    else
      candidates.push_back(client_path / "_retail_" / ".build.info");

    for (auto const& build_info_path : candidates)
    {
      if (!fileExistsWithContent(build_info_path))
        continue;

      std::ifstream stream(build_info_path);
      std::string header_line;
      if (!std::getline(stream, header_line))
        continue;

      auto const headers = splitBuildInfoLine(header_line);
      int version_index = -1;
      int active_index = -1;

      for (std::size_t i = 0; i < headers.size(); ++i)
      {
        if (headers[i].rfind("Version!", 0) == 0)
          version_index = static_cast<int>(i);
        else if (headers[i].rfind("Active!", 0) == 0)
          active_index = static_cast<int>(i);
      }

      if (version_index < 0)
        continue;

      std::string first_version;
      std::string line;
      while (std::getline(stream, line))
      {
        auto const values = splitBuildInfoLine(line);
        if (version_index >= static_cast<int>(values.size()) || values[version_index].empty())
          continue;

        if (first_version.empty())
          first_version = values[version_index];

        if (active_index < 0 || (active_index < static_cast<int>(values.size()) && values[active_index] == "1"))
          return values[version_index];
      }

      if (!first_version.empty())
        return first_version;
    }

    return {};
  }
}
namespace Noggit::Project
{
  ApplicationProject::ApplicationProject(std::shared_ptr<Application::NoggitApplicationConfiguration> configuration)
  {
    _active_project = nullptr;
    _configuration = configuration;
  }

  void ApplicationProject::createProject(std::filesystem::path const& project_path, std::filesystem::path const& client_path, std::string const& client_version, std::string const& project_name)
  {
    if (!std::filesystem::exists(project_path))
      std::filesystem::create_directory(project_path);

    auto project = NoggitProject();
    project.ProjectName = project_name;
    project.projectVersion = ClientVersionFactory::mapToEnumVersion(client_version);
    project.ClientPath = client_path.generic_string();
    project.ProjectPath = project_path.generic_string();

    auto project_writer = ApplicationProjectWriter();
    project_writer.saveProject(&project, project_path);
  }

  std::shared_ptr<NoggitProject> ApplicationProject::loadProject(std::filesystem::path const& project_path)
  {
    ApplicationProjectReader project_reader{};
    auto project = project_reader.readProject(project_path);

    if (!project.has_value())
    {
      LogError << "loadProject() failed, Project is null" << std::endl;
      return {};
    }
    else
    {
      Log << "loadProject(): Loading Project Data" << std::endl;
    }


    project_reader.readPalettes(&project.value());
    project_reader.readObjectSelectionGroups(&project.value());

    std::string dbd_file_directory = _configuration->ApplicationDatabaseDefinitionsPath;

    BlizzardDatabaseLib::Structures::Build client_build("3.3.5.12340");
    auto client_archive_version = BlizzardArchive::ClientVersion::WOTLK;
    auto client_archive_locale = BlizzardArchive::Locale::AUTO;
    if (project->projectVersion == ProjectVersion::SL || project->projectVersion == ProjectVersion::RETAIL)
    {
      client_archive_version = project->projectVersion == ProjectVersion::RETAIL
        ? BlizzardArchive::ClientVersion::RETAIL
        : BlizzardArchive::ClientVersion::SL;
      auto detected_build = detectClientBuildVersion(project->ClientPath);
      if (detected_build.empty())
      {
        detected_build = "9.2.7.45745";
        LogError << "Could not detect client build from .build.info, falling back to " << detected_build << std::endl;
      }
      else
      {
        Log << "Detected modern client build: " << detected_build << std::endl;
      }

      client_build = BlizzardDatabaseLib::Structures::Build(detected_build);
      try
      {
        std::size_t parsed_length = 0;
        auto const major_version = std::stoi(detected_build, &parsed_length);
        if (parsed_length != 0)
        {
          client_archive_version = major_version >= 10
            ? BlizzardArchive::ClientVersion::RETAIL
            : BlizzardArchive::ClientVersion::SL;
          Log << "Selected modern CASC profile from build: "
              << (client_archive_version == BlizzardArchive::ClientVersion::SL ? "Shadowlands" : "Retail")
              << std::endl;
        }
      }
      catch (std::exception const& e)
      {
        LogError << "Could not select CASC profile from build " << detected_build
                 << ": " << e.what() << std::endl;
      }
      client_archive_locale = BlizzardArchive::Locale::enUS;
    }

    else if (project->projectVersion == ProjectVersion::WOTLK)
    {
      client_archive_version = BlizzardArchive::ClientVersion::WOTLK;
      client_build = BlizzardDatabaseLib::Structures::Build("3.3.5.12340");
      client_archive_locale = BlizzardArchive::Locale::AUTO;
    }

    else
    {
      LogError << "Unsupported project version" << std::endl;
      return {};
    }

    project->ClientDatabase = std::make_shared<BlizzardDatabaseLib::BlizzardDatabase>(dbd_file_directory, client_build);

    Log << "Loading Client Path : " << project->ClientPath << std::endl;

    std::string listfile_error;
    if (!refreshWowdevListfile(project_path, listfile_error))
    {
      LogError << listfile_error << std::endl;
      QMessageBox::critical(nullptr, "Error", QString::fromStdString(listfile_error));
      return {};
    }

    try
    {
      project->ClientData = std::make_shared<BlizzardArchive::ClientData>(
        project->ClientPath, client_archive_version, client_archive_locale, project_path.generic_string());
    }
    catch (BlizzardArchive::Exceptions::Locale::LocaleNotFoundError& e)
    {
      LogError << e.what() << std::endl;
      QMessageBox::critical(nullptr, "Error", e.what());
      return {};
    }
    catch (BlizzardArchive::Exceptions::Locale::IncorrectLocaleModeError& e)
    {
      LogError << e.what() << std::endl;
      QMessageBox::critical(nullptr, "Error", e.what());
      return {};
    }
    catch (BlizzardArchive::Exceptions::Archive::ArchiveOpenError& e)
    {
      LogError << e.what() << std::endl;
      QMessageBox::critical(nullptr, "Error", e.what());
      return {};
    }
    catch (std::exception const& e)
    {
      LogError << "Failed loading Client data: " << e.what() << std::endl;
      QMessageBox::critical(nullptr, "Error", QString("Failed loading Client data:\n%1").arg(e.what()));
      return {};
    }
    catch (...)
    {
      LogError << "Failed loading Client data. Unhandled exception." << std::endl;
      QMessageBox::critical(nullptr, "Error", "Failed loading Client data. Unhandled exception.");
      return {};
    }

    if (!project->ClientData)
    {
      LogError << "Failed loading Client data." << std::endl;
      return {};
    }

    // Log << "Client Version: " << static_cast<int>(project->ClientData->version()) << std::endl;

    Log << "Client Locale: " << project->ClientData->locale_name() << std::endl;

    for (auto const loaded_achive : *project->ClientData->loadedArchives())
    {
      Log << "Loaded client Archive: " << loaded_achive->path() << std::endl;
    }

    // QSettings settings;
    // bool modern_features = settings.value("modern_features", false).toBool();
    bool modern_features = _configuration->modern_features;
    if (modern_features)
    {
      Log << "Modern Features Enabled" << std::endl;
      loadExtraData(project.value());
    }
    else
    {
      Log << "Modern Features Disabled" << std::endl;
    }
    return std::make_shared<NoggitProject>(project.value());
  }

  void ApplicationProject::loadExtraData(NoggitProject& project)
    {
        std::filesystem::path extraDataFolder = (project.ProjectPath);
        extraDataFolder /= "extraData";

        Log << "Loading extra data from " << extraDataFolder << std::endl;

        if (std::filesystem::exists(extraDataFolder) && std::filesystem::is_directory(extraDataFolder))
        {
            for (const auto& entry : std::filesystem::directory_iterator(extraDataFolder))
            {
                if (entry.path().extension() == ".cfg")
                {
                    QFile input_file(QString::fromStdString(entry.path().generic_string()));
                    input_file.open(QIODevice::ReadOnly);
                    QJsonParseError err;
                    auto document = QJsonDocument().fromJson(input_file.readAll(), &err);
                    auto root = document.object();
                    auto keys = root.keys();
                    if (entry.path().stem() == "global")
                    {
                        for (auto const& entry : keys)
                        {
                            texture_heightmapping_data newData;
                            newData.uvScale = root[entry].toObject()["Scale"].toInt();
                            newData.heightOffset = root[entry].toObject()["HeightOffset"].toDouble();
                            newData.heightScale = root[entry].toObject()["HeightScale"].toDouble();
                            project.ExtraMapData.SetTextureHeightData_Global(entry.toStdString(), newData);
                        }
                    }
                }
            }
        }
    }
    void NoggitExtraMapData::SetTextureHeightData_Global(const std::string& texture, texture_heightmapping_data data, World* worldToUpdate)
    {
        TextureHeightData_Global[texture] = data;
        if (worldToUpdate)
        {
            for (MapTile* tile : worldToUpdate->mapIndex.loaded_tiles())
            {
                tile->registerChunkUpdate(ChunkUpdateFlags::ALPHAMAP);
                tile->forceAlphaUpdate();
                tile->forceRecalcExtents();
            }
        }
    }
    void NoggitExtraMapData::SetTextureHeightDataForADT(int mapID, const TileIndex& ti, const std::string& texture, texture_heightmapping_data data, World* worldToUpdate)
    {
        TextureHeightData_ADT[mapID][ti.x][ti.z][texture] = data;
        if (worldToUpdate)
        {
            MapTile* tile = worldToUpdate->mapIndex.getTile(ti);
            tile->registerChunkUpdate(ChunkUpdateFlags::ALPHAMAP);
            tile->forceAlphaUpdate();
            tile->forceRecalcExtents();
        }
    }
    const texture_heightmapping_data NoggitExtraMapData::GetTextureHeightDataForADT(int mapID, const TileIndex& tileIndex, const std::string& texture) const
    {
        static texture_heightmapping_data defaultValue;
        auto foundMapIter = TextureHeightData_ADT.find(mapID);
        if (foundMapIter != TextureHeightData_ADT.end())
        {
            auto foundXIter = foundMapIter->second.find(tileIndex.x);
            if (foundXIter != foundMapIter->second.end())
            {
                auto foundYIter = foundXIter->second.find(tileIndex.z);
                if (foundYIter != foundXIter->second.end())
                {
                    auto foundTexData = foundYIter->second.find(texture);
                    if (foundTexData != foundYIter->second.end())
                    {
                        return foundTexData->second;
                    }
                }
            }
        }
        auto foundGenericSettings = TextureHeightData_Global.find(texture);
        if (foundGenericSettings != TextureHeightData_Global.end())
        {
            return foundGenericSettings->second;
        }
        return defaultValue;
    }

    ProjectVersion ClientVersionFactory::mapToEnumVersion(std::string const& projectVersion)
    {
      if (projectVersion == "Wrath Of The Lich King")
        return ProjectVersion::WOTLK;
      if (projectVersion == "Shadowlands" || projectVersion == "Modern")
        return ProjectVersion::SL;
      if (projectVersion == "Retail")
        return ProjectVersion::RETAIL;

      LogError << "Unknown project version '" << projectVersion << "', falling back to Shadowlands compatibility." << std::endl;
      return ProjectVersion::SL;
    }

    std::string ClientVersionFactory::MapToStringVersion(ProjectVersion const& projectVersion)
    {
      if (projectVersion == ProjectVersion::WOTLK)
        return std::string("Wrath Of The Lich King");
      if (projectVersion == ProjectVersion::SL)
        return std::string("Shadowlands");
      if (projectVersion == ProjectVersion::RETAIL)
        return std::string("Retail");

      LogError << "Unknown project version enum, falling back to Shadowlands." << std::endl;
      return std::string("Shadowlands");
    }

    NoggitProject::NoggitProject()
    {
      _projectWriter = std::make_shared<ApplicationProjectWriter>();
    }

    unsigned int NoggitProject::buildId()
    {
      return ClientDatabase->getBuild();
    }

    void NoggitProject::createBookmark(const NoggitProjectBookmarkMap& bookmark)
    {
      Bookmarks.push_back(bookmark);

      _projectWriter->saveProject(this, std::filesystem::path(ProjectPath));
    }

    void NoggitProject::deleteBookmark()
    {
    }

    void NoggitProject::pinMap(int map_id, const std::string& map_name)
    {
      auto pinnedMap = NoggitProjectPinnedMap();
      pinnedMap.MapName = map_name;
      pinnedMap.MapId = map_id;

      auto pinnedMapFound = std::find_if(std::begin(PinnedMaps), std::end(PinnedMaps),
        [&](Project::NoggitProjectPinnedMap pinnedMap)
        {
          return pinnedMap.MapId == map_id;
        });

      if (pinnedMapFound != std::end(PinnedMaps))
        return;

      PinnedMaps.push_back(pinnedMap);

      _projectWriter->saveProject(this, std::filesystem::path(ProjectPath));
    }

    void NoggitProject::unpinMap(int mapId)
    {
      PinnedMaps.erase(std::remove_if(PinnedMaps.begin(), PinnedMaps.end(),
        [=](NoggitProjectPinnedMap pinnedMap)
        {
          return pinnedMap.MapId == mapId;
        }),
        PinnedMaps.end());

      _projectWriter->saveProject(this, std::filesystem::path(ProjectPath));
    }

    void NoggitProject::saveTexturePalette(const NoggitProjectTexturePalette& new_texture_palette)
    {
      TexturePalettes.erase(std::remove_if(TexturePalettes.begin(), TexturePalettes.end(),
        [=](NoggitProjectTexturePalette texture_palette)
        {
          return texture_palette.MapId == new_texture_palette.MapId;
        }),
        TexturePalettes.end());

      TexturePalettes.push_back(new_texture_palette);

      _projectWriter->savePalettes(this, std::filesystem::path(ProjectPath));
    }

    void NoggitProject::saveObjectPalette(const NoggitProjectObjectPalette& new_object_palette)
    {
      ObjectPalettes.erase(std::remove_if(ObjectPalettes.begin(), ObjectPalettes.end(),
        [=](NoggitProjectObjectPalette obj_palette)
        {
          return obj_palette.MapId == new_object_palette.MapId;
        }),
        ObjectPalettes.end());

      ObjectPalettes.push_back(new_object_palette);

      _projectWriter->savePalettes(this, std::filesystem::path(ProjectPath));
    }

    void NoggitProject::saveObjectSelectionGroups(const NoggitProjectSelectionGroups& new_selection_groups)
    {
      ObjectSelectionGroups.erase(std::remove_if(ObjectSelectionGroups.begin(), ObjectSelectionGroups.end(),
        [=](NoggitProjectSelectionGroups proj_selection_group)
        {
          return proj_selection_group.MapId == new_selection_groups.MapId;
        }),
        ObjectSelectionGroups.end());

      ObjectSelectionGroups.push_back(new_selection_groups);

      _projectWriter->saveObjectSelectionGroups(this, std::filesystem::path(ProjectPath));
    }
};
