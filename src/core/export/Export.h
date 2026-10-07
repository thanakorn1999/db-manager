#pragma once

#include "core/database/Database.h"

#include <string>

namespace Export {

// CSV with a header row, PostgreSQL COPY style: NULL = empty field, empty string = "".
std::string csv(const ResultSet& rs);

// One INSERT per row into table (already quoted, e.g. "public"."users"); values as string
// literals so PostgreSQL casts them to the column types.
std::string inserts(const ResultSet& rs, const std::string& table);

// Excel workbook (.xlsx bytes) with one sheet: bold, frozen header row; numeric / bool columns
// (by PostgreSQL type) as numbers, everything else as text. Throws DbError past Excel's row / column limits.
std::string xlsx(const ResultSet& rs, const std::string& sheetName);

// {"structure": ..., "rows": [{column: value}, ...]}. structureJson is embedded as is (see
// PostgreSQL::tableStructureJson); empty = just the column names. Values typed like xlsx:
// numbers (up to 15 digits), booleans, json / jsonb embedded, NULL as null, the rest strings.
std::string json(const ResultSet& rs, const std::string& structureJson);

}
