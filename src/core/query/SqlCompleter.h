#pragma once

#include "core/database/Database.h"

#include <string>
#include <vector>

// Context-aware SQL completion: keywords for the current clause, tables after FROM/JOIN,
// columns of the tables in the statement, and FK-based JOIN ... ON suggestions.
// ponytail: token heuristics, not a parser; subqueries and CTE names aren't tracked
namespace SqlCompleter {

struct Item {
    std::string label;  // shown in the popup
    std::string insert; // replaces the typed prefix
    enum Kind { Keyword, Table, Column, Join } kind;
};

struct Result {
    std::string prefix;       // word being typed (replaced on accept)
    std::vector<Item> items;
    bool expectsName = false; // tables / join / ON expected: worth popping up after a space
};

// sql is UTF-8, cursor a byte offset into it.
Result complete(const std::string& sql, size_t cursor, const SchemaInfo& schema);

// Byte ranges [first, second) of names missing from schema: tables after FROM / JOIN / UPDATE / INTO
// and columns in WHERE. Empty while schema has no tables (not loaded yet).
std::vector<std::pair<size_t, size_t>> unknownNames(const std::string& sql, const SchemaInfo& schema);

}
