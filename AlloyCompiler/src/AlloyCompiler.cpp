#include <filesystem>

#include "logger.hpp"
#include "source.hpp"
#include "tokenizer.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;

static void compileModule(const Source& source)
{
	const auto tokenizeResult = tokenize(source);
	//const auto parseResult = parse(tokenizeResult.value);
	__debugbreak();
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
