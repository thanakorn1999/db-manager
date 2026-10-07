#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

enum class DbType { PostgreSQL, Redis };

struct ConnectionConfig {
    std::string id;          // stable key for settings / keychain
    std::string name;
    DbType type = DbType::PostgreSQL;
    std::string host = "127.0.0.1";
    int port = 5432;
    std::string database;    // PostgreSQL: dbname, Redis: db index
    std::string username;
    std::string password;
    std::string sslMode = "prefer"; // PostgreSQL
    bool tls = false;               // Redis
};

// Catalog snapshot used for SQL completion.
struct SchemaInfo {
    struct Table {
        std::string schema, name;
        std::vector<std::string> columns;
        std::vector<std::string> types; // per column, e.g. "integer", "character varying(80)"
        std::vector<bool> primary;      // per column: part of the primary key
    };
    struct ForeignKey {
        std::string fromSchema, fromTable, toSchema, toTable;
        std::vector<std::string> fromColumns, toColumns;
    };
    std::vector<Table> tables;
    std::vector<ForeignKey> foreignKeys;
};

struct DbError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Tabular result shared by SQL queries and Redis value views.
struct ResultSet {
    std::vector<std::string> columns;
    std::vector<std::vector<std::optional<std::string>>> rows; // nullopt = NULL
    std::string status; // e.g. "SELECT 10", "INSERT 0 1"
    long long affected = -1;           // rows touched by INSERT / UPDATE / DELETE
    std::vector<unsigned> sourceTable; // PostgreSQL: table OID per column, 0 = computed
    std::vector<int> sourceColumn;     // PostgreSQL: attnum per column
    std::vector<unsigned> columnType;  // PostgreSQL: type OID per column (16 = bool)
    std::vector<uint8_t> keyFlags;     // per column: KeyFlag bits, empty = unknown
    // PostgreSQL: foreign keys whose columns are all in the result (for "go to parent row")
    struct ForeignRef {
        std::string schema, table;              // referenced table
        std::vector<int> columns;               // result column indexes, in FK order
        std::vector<std::string> targetColumns; // referenced columns, same order
    };
    std::vector<ForeignRef> foreignRefs;
};
// indexed: in any index; unique: a single-column unique index / constraint on its own
enum KeyFlag : uint8_t { KeyPrimary = 1, KeyForeign = 2, KeyIndexed = 4, KeyUnique = 8 };

enum class Capability : uint32_t {
    Query = 1 << 0,
    Tables = 1 << 1,
    Views = 1 << 2,
    Transactions = 1 << 3,
    Keys = 1 << 10,
    Ttl = 1 << 11,
    ServerInfo = 1 << 12,
};
using Capabilities = uint32_t;
constexpr Capabilities operator|(Capability a, Capability b) { return uint32_t(a) | uint32_t(b); }
constexpr Capabilities operator|(Capabilities a, Capability b) { return a | uint32_t(b); }

class Database {
public:
    virtual ~Database() = default;
    virtual void connect() = 0; // throws DbError
    virtual void disconnect() = 0;
    virtual void ping() = 0;
    virtual bool connected() const = 0;
    // Thread-safe: may be called from any thread to abort the running call.
    virtual void cancel() {}
    virtual Capabilities capabilities() const = 0;
};
