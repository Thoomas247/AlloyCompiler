#include <filesystem>
#include <unordered_map>
#include <vector>

#include "source/source.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"
#include "resolver/resolver.hpp"
#include "typechecker/type_interner.hpp"
#include "typechecker/type_checker.hpp"

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

		// Stages are dependent: a failed stage produces output the next stage
		// cannot safely consume, so skip the rest of this module. Other modules
		// are still compiled — one error no longer aborts the whole batch.
		auto [resolveStatus, resolvedModule] = resolve(*m.source, m.moduleNode.value(), m.symbols, importedSymbols);
		if (resolveStatus != Status::Ok)
			continue;

		auto [internStatus, internedTypes] = intern(*m.source, m.moduleNode.value(), resolvedModule);
		if (internStatus != Status::Ok)
			continue;

		auto [checkStatus, typedModule] = typeCheck(*m.source, m.moduleNode.value(), resolvedModule, internedTypes, m.symbols);
		(void)checkStatus;
	}

	auto& diagnostics = DiagnosticEngine::instance();
	diagnostics.printAll();

	if (diagnostics.hasError())
	{
		std::fprintf(stderr, "Compilation failed with %zu error(s).\n", diagnostics.getErrorCount());
		return 1;
	}

	std::println("Compilation succeeded ({} module(s)).", modules.size());
	return 0;
}
