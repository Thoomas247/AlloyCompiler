#pragma once

#include "types.hpp"
#include "../resolver/resolver.hpp"
#include "../source/source.hpp"
#include "../util/result.hpp"

/**
 * Pass 1: Type Interner
 *
 * Walks every explicit type annotation in the AST and assigns a canonical TypeId.
 * Structural types (anonymous structs, arrays, function types, etc.) are
 * deduplicated — two structurally identical types share the same TypeId.
 * Named types (type aliases) always get their own TypeId.
 *
 * Prerequisites: resolver::declare() and resolver::resolve() must have run.
 * Source must outlive the returned InternedTypes (string_views point into source.data).
 */
Result<InternedTypes> intern(
    const Source& source,
    const AST::Module& module,
    const ResolvedModule& resolved
);
