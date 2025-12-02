#pragma once

#include <string>
#include <vector>

#include "../util/result.hpp"
#include "../util/allocator.hpp"
#include "../source/source.hpp"
#include "AST.hpp"

Result<std::pair<Required<AST::Module>, Allocator>> parse(const Source& source, const std::vector<Token>& tokens);