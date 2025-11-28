#pragma once

#include <string>
#include <vector>

#include "result.hpp"
#include "AST.hpp"
#include "source.hpp"

Result<Required<AST::Program>> parse(const Source& source, const std::vector<std::string>& tokens);