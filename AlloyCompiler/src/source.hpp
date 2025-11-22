#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

struct Source
{
	std::string moduleName;
	std::string data;
};

std::vector<Source> getSources(const fs::path& rootDir);