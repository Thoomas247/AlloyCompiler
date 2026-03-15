#include <filesystem>
#include <vector>

#include "source/source.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"
#include "resolver/resolver.hpp"

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

		auto [declareStatus, topLevelSymbols] = declare(source, moduleNode.value());
	}

	// TODO:
	// 1) Symbol resolution (name -> definition node):
	//		a) Global (top-level) definitions table per module
	//		b) Local (inside functions/nested scopes) definitions table, ensures no shadowing
	// 2) Type interning and type checking
	
	__debugbreak();
}
