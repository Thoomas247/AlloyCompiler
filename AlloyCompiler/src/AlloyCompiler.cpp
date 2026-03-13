#include <filesystem>
#include <vector>

#include "source/source.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"

namespace fs = std::filesystem;

int main()
{
	fs::path rootDir = "./examples";
	const auto sources = getSources(rootDir);

	for (const auto& source : sources)
	{
		auto [tokenStatus, tokens] = tokenize(source);
		auto [parseStatus, parseResult] = parse(source, tokens);
		auto& [moduleNode, allocator] = parseResult;
	}
	
	__debugbreak();
}
