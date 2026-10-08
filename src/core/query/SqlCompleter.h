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

// How to write a table in SQL: schema dropped for public, quotes only where needed (agreements, "Foo".bar)
std::string tableName(const std::string& schema, const std::string& name);

// One per foreign key on table.column, either way: sql with "JOIN parent p ON p.pk = table.fk" (FK column)
// or "JOIN child c ON c.fk = table.pk" (key column) added at the end of the FROM clause that names table.
// Empty when sql doesn't name table.
struct Join {
    std::string label; // the JOIN clause
    std::string sql;
};
std::vector<Join> joinsOn(const std::string& sql, const SchemaInfo& schema, const std::string& tableSchema,
                          const std::string& table, const std::string& column);

// sql with "column op value" ANDed into the WHERE of the statement that names table (WHERE added when
// missing). op: = <> < > <= >= contains, starts with, IN (value comma-separated), IS NULL, IS NOT NULL.
// Values are quoted literals, so they take the column's type. Empty when sql doesn't name table.
std::string addFilter(const std::string& sql, const SchemaInfo& schema, const std::string& tableSchema,
                      const std::string& table, const std::string& column, const std::string& op,
                      const std::string& value);

// sql with the ORDER BY of the statement naming table (the last statement when table is "") set to
// result column `column` (by name, by position when the name repeats), replacing any ORDER BY there.
// dir: ASC, DESC, or "" to remove it. Empty when the statement can't be found or isn't a query.
std::string sortBy(const std::string& sql, const SchemaInfo& schema, const std::string& tableSchema,
                   const std::string& table, const std::vector<std::string>& columns, size_t column,
                   const std::string& dir);

}
