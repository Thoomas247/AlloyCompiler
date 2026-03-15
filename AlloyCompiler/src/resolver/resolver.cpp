#include "resolver.hpp"

Result<SymbolTable> declare(const Source& moduleSource, const AST::Module& module)
{
    module.definitions.forEach([](auto& definition)
        {
            
        });
    return Result<SymbolTable>();
}
