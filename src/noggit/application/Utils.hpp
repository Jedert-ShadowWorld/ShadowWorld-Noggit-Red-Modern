// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UTILS_HPP
#define NOGGIT_UTILS_HPP

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/project/CurrentProject.hpp>

#include <ClientFile.hpp>
#include <stream/StreamReader.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
  inline std::string noggitNormalizeProjectDbPath(std::string path)
  {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path;
  }

  inline std::shared_ptr<BlizzardDatabaseLib::Stream::IMemStream> noggitTryReadProjectDbFile(
    std::string const& file_path)
  {
    auto const project = Noggit::Project::CurrentProject::get();
    if (!project)
      return {};

    std::vector<std::filesystem::path> candidates;
    auto const normalized = noggitNormalizeProjectDbPath(file_path);
    auto const project_path = std::filesystem::path(project->ProjectPath);
    auto const requested_filename = std::filesystem::path(normalized).filename();

    candidates.emplace_back(project_path / normalized);
    candidates.emplace_back(project_path / "dbfilesclient" / requested_filename);
    candidates.emplace_back(project_path / "DBFilesClient" / requested_filename);

    for (auto const& candidate : candidates)
    {
      std::error_code error;
      if (!std::filesystem::exists(candidate, error) || !std::filesystem::is_regular_file(candidate, error))
        continue;

      auto const file_size = std::filesystem::file_size(candidate, error);
      if (error || !file_size)
        continue;

      std::ifstream input(candidate, std::ios_base::binary | std::ios_base::in);
      if (!input.is_open())
        continue;

      std::vector<char> buffer(static_cast<std::size_t>(file_size));
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      if (!input)
        continue;

      Log << "[DB2Fallback] Loaded " << file_path << " from " << candidate.generic_string() << std::endl;
      return std::make_shared<BlizzardDatabaseLib::Stream::IMemStream>(buffer.data(), buffer.size());
    }

    return {};
  }
}


inline auto readFileAsIMemStream = [](std::string const& file_path) -> std::shared_ptr<BlizzardDatabaseLib::Stream::IMemStream>
{
  try
  {
    BlizzardArchive::ClientFile f(file_path, Noggit::Application::NoggitApplication::instance()->clientData());

    return std::make_shared<BlizzardDatabaseLib::Stream::IMemStream>(f.getBuffer(), f.getSize());
  }
  catch (std::exception const& e)
  {
    if (auto fallback = noggitTryReadProjectDbFile(file_path))
      return fallback;

    throw;
  }
};


#endif //NOGGIT_UTILS_HPP
