#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "source/source.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"
#include "resolver/resolver.hpp"
#include "comptime/comptime.hpp"
#include "typechecker/type_interner.hpp"
#include "typechecker/type_checker.hpp"
#include "codegen/codegen.hpp"

namespace fs = std::filesystem;

struct CompiledModule
{
	const Source* source;
	std::vector<Token> tokens;
	Allocator allocator;
	Required<AST::Module> moduleNode;
	SymbolTable symbols;
};

// ---------------------------------------------------------------------------
// Normal compilation — compiles every *.alloy under ./examples as one program.
// ---------------------------------------------------------------------------

static int compileExamples()
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

	for (auto& m : modules)
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

		// B1/B3 (§6): evaluate '#' comptime constructs — substitute values into
		// the AST and record synthesised types — before interning. Comptime
		// errors are reported to the DiagnosticEngine; the pipeline keeps going.
		// synthTypes must outlive intern + typeCheck (its strings back type names).
		SynthTypeMap synthTypes;
		comptimeEval(*m.source, m.moduleNode.value(), resolvedModule, m.allocator, synthTypes);

		auto [internStatus, internedTypes] = intern(*m.source, m.moduleNode.value(), resolvedModule, synthTypes);
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

// ---------------------------------------------------------------------------
// Test harness (F3) — runs each *.alloy under ./tests in isolation and checks
// it against expectation annotations embedded in the source as comments:
//
//   //@expect-error <substring>   one per expected error; the substring must
//                                 appear in some diagnostic message.
//
// A file with no annotations is a positive test — it must compile clean.
// A file passes when the expected and actual error counts match and every
// expected substring is matched by a distinct diagnostic.
// ---------------------------------------------------------------------------

// Compiles a single self-contained source (no cross-file imports) through the
// full pipeline. Diagnostics land in the process-wide DiagnosticEngine.
static void compileSource(const Source& source)
{
	auto [tokenStatus, tokens] = tokenize(source);
	auto [parseStatus, parseResult] = parse(source, tokens);
	auto& [moduleNode, allocator] = parseResult;
	auto [declareStatus, symbols] = declare(source, moduleNode.value());

	std::vector<const SymbolTable*> noImports;
	auto [resolveStatus, resolved] = resolve(source, moduleNode.value(), symbols, noImports);
	if (resolveStatus != Status::Ok)
		return;

	SynthTypeMap synthTypes;
	comptimeEval(source, moduleNode.value(), resolved, allocator, synthTypes);

	auto [internStatus, interned] = intern(source, moduleNode.value(), resolved, synthTypes);
	if (internStatus != Status::Ok)
		return;

	auto [checkStatus, typed] = typeCheck(source, moduleNode.value(), resolved, interned, symbols);
	(void)checkStatus;
}

static std::string trim(std::string_view s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string_view::npos)
		return {};
	size_t e = s.find_last_not_of(" \t\r\n");
	return std::string(s.substr(b, e - b + 1));
}

// Collects every "//@expect-error <substring>" annotation from a test source.
static std::vector<std::string> parseExpectations(const std::string& text)
{
	std::vector<std::string> expectations;
	const std::string marker = "//@expect-error";
	size_t pos = 0;
	while ((pos = text.find(marker, pos)) != std::string::npos)
	{
		size_t start = pos + marker.size();
		size_t end = text.find('\n', start);
		std::string_view rest = (end == std::string::npos)
			? std::string_view(text).substr(start)
			: std::string_view(text).substr(start, end - start);
		expectations.push_back(trim(rest));
		pos = (end == std::string::npos) ? text.size() : end;
	}
	return expectations;
}

// True when actual error messages exactly satisfy the expected substrings:
// equal counts, and every expected pattern matched by a distinct message.
static bool matchExpectations(const std::vector<std::string>& expected,
	const std::vector<std::string>& errors)
{
	if (expected.size() != errors.size())
		return false;

	std::vector<bool> used(errors.size(), false);
	for (const auto& pattern : expected)
	{
		bool found = false;
		for (size_t i = 0; i < errors.size(); ++i)
		{
			if (!used[i] && errors[i].find(pattern) != std::string::npos)
			{
				used[i] = true;
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}
	return true;
}

static int runTests()
{
	fs::path testDir = "./tests";
	if (!fs::exists(testDir))
	{
		std::println("No './tests' directory — nothing to run.");
		return 0;
	}

	const auto sources = getSources(testDir);
	int passed = 0;
	int failed = 0;

	for (const auto& source : sources)
	{
		const auto expectations = parseExpectations(source.data);

		DiagnosticEngine::instance().clear();
		compileSource(source);

		std::vector<std::string> errors;
		for (const auto& d : DiagnosticEngine::instance().diagnostics())
			if (d.severity == Diagnostic::Severity::Error)
				errors.push_back(d.message);

		if (matchExpectations(expectations, errors))
		{
			++passed;
			std::println("PASS  {}", source.moduleName);
		}
		else
		{
			++failed;
			std::println("FAIL  {}  (expected {} error(s), got {})",
				source.moduleName, expectations.size(), errors.size());
			for (const auto& e : errors)
				std::println("        actual: {}", e);
			for (const auto& x : expectations)
				std::println("        expected: {}", x);
		}
	}

	std::println("---");
	std::println("{} passed, {} failed ({} total).", passed, failed, passed + failed);
	return failed == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Back-end driver (§6.C) — compiles a single .alloy file end to end: front-end
// pipeline, LLVM code generation, then links the emitted object into a native
// executable with clang. Output lands in ./build.
// ---------------------------------------------------------------------------

static int buildFile(const std::string& path)
{
	fs::path file(path);
	if (!fs::exists(file))
	{
		std::fprintf(stderr, "ERROR: source file '%s' not found.\n", path.c_str());
		return 1;
	}

	std::ifstream stream(file);
	std::stringstream buffer;
	buffer << stream.rdbuf();

	Source source{ file.stem().string(), buffer.str() };

	auto [tokenStatus, tokens] = tokenize(source);
	auto [parseStatus, parseResult] = parse(source, tokens);
	auto& [moduleNode, allocator] = parseResult;
	auto [declareStatus, symbols] = declare(source, moduleNode.value());

	std::vector<const SymbolTable*> noImports;
	auto [resolveStatus, resolved] = resolve(source, moduleNode.value(), symbols, noImports);

	auto failOut = [&]() -> int
	{
		auto& diags = DiagnosticEngine::instance();
		diags.printAll();
		std::fprintf(stderr, "Compilation failed with %zu error(s).\n", diags.getErrorCount());
		return 1;
	};

	if (resolveStatus != Status::Ok)
		return failOut();

	SynthTypeMap synthTypes;
	comptimeEval(source, moduleNode.value(), resolved, allocator, synthTypes);

	auto [internStatus, interned] = intern(source, moduleNode.value(), resolved, synthTypes);
	if (internStatus != Status::Ok)
		return failOut();

	auto [checkStatus, typed] = typeCheck(source, moduleNode.value(), resolved, interned, symbols);
	if (DiagnosticEngine::instance().hasError())
		return failOut();

	fs::create_directories("build");
	const std::string base = "build/" + source.moduleName;

	Status cgStatus = codegen(source, moduleNode.value(), resolved, interned, typed, symbols, base);

	DiagnosticEngine::instance().printAll();
	if (cgStatus != Status::Ok || DiagnosticEngine::instance().hasError())
	{
		std::fprintf(stderr, "Code generation failed.\n");
		return 1;
	}

	std::println("Emitted {}.ll and {}.obj", base, base);

	// Link the object into an executable with clang (which knows the MSVC toolchain).
	// C:\LLVM is an LLVM 21 dev-libs build without clang/lld; bak18 keeps the old
	// clang for linking — object format is forward-compatible.
	const std::string clang = "C:\\LLVM.bak18\\bin\\clang.exe";
	const std::string exePath = "build/" + source.moduleName + ".exe";
	const std::string command =
		"\"\"" + clang + "\" \"" + base + ".obj\" -o \"" + exePath + "\"\"";

	int rc = std::system(command.c_str());
	if (rc != 0)
	{
		std::fprintf(stderr, "Linking failed (clang exit code %d).\n", rc);
		return 1;
	}

	std::println("Linked executable: {}", exePath);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc > 1 && std::string_view(argv[1]) == "--test")
		return runTests();

	if (argc > 2 && std::string_view(argv[1]) == "--build")
		return buildFile(argv[2]);

	return compileExamples();
}
