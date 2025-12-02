#include <filesystem>

#include "source/source.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"

namespace fs = std::filesystem;

static void compileModule(const Source& source)
{
	auto [tokenizeStatus, tokens] = tokenize(source);
	auto [parseStatus, parseResult] = parse(source, tokens);
	auto& [moduleNode, allocator] = parseResult;
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
