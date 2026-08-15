#ifndef NOGGIT_APPLICATION_PROJECT_READER_HPP
#define NOGGIT_APPLICATION_PROJECT_READER_HPP

#include <filesystem>
#include <optional>

namespace Noggit::Project
{
	class NoggitProject;

	class ApplicationProjectReader
	{
	public:
		ApplicationProjectReader() = default;

		std::optional<NoggitProject> readProject(std::filesystem::path const& project_path);
		std::optional<NoggitProject> readProjectFile(std::filesystem::path const& project_file_path);

		void readPalettes(NoggitProject* project);
		void readObjectSelectionGroups(NoggitProject* project);
	};
}

#endif //NOGGIT_APPLICATION_PROJECT_READER_HPP
