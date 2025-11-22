#include <filesystem>

#include "logger.hpp"
#include "source.hpp"
#include "tokenizer.hpp"

namespace fs = std::filesystem;

static bool compileModule(const Source& source)
{
	const auto tokenizeResult = tokenize(source);

	const auto parseResult = parse(tokenizeResult.value);
}

int main()
{
	fs::path rootDir = "./examples";
	const auto sources = getSources(rootDir);

	for (const auto& source : sources)
	{
		compileModule(source);
	}

	__debugbreak();
}
