#pragma once

#include "core/database/postgres/PostgreSQL.h"

#include <string>
#include <utility>
#include <vector>

// Reads files written by Export back into PostgreSQL.
namespace Import {

// One table's worth of an exported file.
struct Table {
    std::string file;          // file name; for a .zip, the entry inside it
    std::string schema, name;  // where it came from: the JSON structure, else "schema.table.ext" / "table.ext"
    std::string structureJson; // JSON exports only
    ResultSet data;            // NULL = nullopt
    std::string sql;           // .sql exports: the script, run as is (no data / structure here)
};

// .csv, .xlsx, .json, .sql, or a .zip of them (one Table per entry). Throws DbError naming the file.
std::vector<Table> read(const std::string& fileName, const std::string& bytes);

// {name, contents} of every file in a .zip (stored or deflated entries, no zip64). Throws DbError.
std::vector<std::pair<std::string, std::string>> unzip(const std::string& bytes);

struct Target {
    const Table* table;
    std::string schema, name; // where it goes
};

// Statements for one transaction. structure: CREATE TABLE from the JSON structure (files without one get text
// columns; dropFirst drops an existing table first, CASCADE), then all rows, then foreign keys and indexes, so
// the file order doesn't matter. Without structure the rows go into existing tables (columns by name).
// Either way the sequences behind serial / identity columns end up past the imported ids.
std::vector<Statement> plan(const std::vector<Target>& targets, bool structure, bool dropFirst);

}
