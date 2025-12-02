#include "source.hpp"

#include <vector>
#include <filesystem>
#include <fstream>

#include "../util/logger.hpp"

namespace fs = std::filesystem;

constexpr auto FILE_EXTENSION = ".alloy";

static std::vector<fs::path> getFilePaths(const fs::path& rootDir)
{
	std::vector<fs::path> filesToCompile;
	for (const auto& entry : fs::recursive_directory_iterator(rootDir))
	{
		if (entry.is_regular_file() && entry.path().extension() == FILE_EXTENSION)
		{
			filesToCompile.push_back(entry);
		}
	}

	return filesToCompile;
}

static std::string getModuleName(const fs::path& relativePath)
{
	std::string moduleName;
	for (const fs::path& part : relativePath)
	{
		if (!moduleName.empty())
		{
			moduleName += "::";
		}

		moduleName += part.stem().string();
	}

	return moduleName;
}

std::vector<Source> getSources(const fs::path& rootDir)
{
	const std::vector<fs::path> filePaths = getFilePaths(rootDir);

	std::vector<Source> sources;
	for (const auto& filePath : filePaths)
	{
		const auto relativePath = fs::relative(filePath, rootDir);

		std::ifstream fileStream(filePath);

		if (!fileStream.is_open())
		{
			Log::error("Could not read source file '{}'.", filePath.generic_string(), 12);
		}

		std::stringstream stringStream;
		stringStream << fileStream.rdbuf();

		fileStream.close();

		sources.push_back({ getModuleName(relativePath), stringStream.str() });
	}

	return sources;
}