#include <filesystem>
#include <unordered_map>
#include <vector>

#include "source/source.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"
#include "resolver/resolver.hpp"

namespace fs = std::filesystem;

struct CompiledModule
{
	const Source* source;
	std::vector<Token> tokens;
	Allocator allocator;
	Required<AST::Module> moduleNode;
	SymbolTable symbols;
};

int main()
{
	fs::path rootDir = "./examples";
	const auto sources = getSources(rootDir);

	std::vector<CompiledModule> modules;
	modules.reserve(sources.size());

	for (const auto& source : sources)
	{
		auto [tokenStatus, tokens] = tokenize(source);
		auto [parseStatus, parseResult] = parse(source, tokens);
		auto& [moduleNode, allocator] = parseResult;
		auto [declareStatus, symbols] = declare(source, moduleNode.value());

		modules.push_back(CompiledModule{ &source,std::move(tokens), std::move(allocator), moduleNode, std::move(symbols), });
	}

	std::unordered_map<std::string_view, const SymbolTable*> symbolsByPath;
	symbolsByPath.reserve(modules.size());
	for (const auto& m : modules)
	{
		symbolsByPath[m.source->moduleName] = &m.symbols;
	}

	for (const auto& m : modules)
	{
		// build importedSymbols in import-declaration order
		std::vector<const SymbolTable*> importedSymbols;
		m.moduleNode.value().imports.forEach([&](const Required<AST::Import>& import)
			{
				auto it = symbolsByPath.find(import.value().path);
				if (it != symbolsByPath.end())
				{
					importedSymbols.push_back(it->second);
				}
				else
				{
					Log::error("Imported module '{}' not found.", import.value().path);
				}
			});

		auto [resolveStatus, resolvedModule] = resolve(*m.source, m.moduleNode.value(), m.symbols, importedSymbols);
		__debugbreak();
	}

	// TODO:
	// Type interning and type checking

	__debugbreak();
}
