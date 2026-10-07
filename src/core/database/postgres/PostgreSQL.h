#pragma once

#include "core/database/Database.h"

#include <libpq-fe.h>
#include <memory>
#include <mutex>

using Params = std::vector<std::optional<std::string>>; // nullopt = NULL

// Where edits to a result grid go: one table with its primary key in the result.
struct EditTarget {
    std::string schema, table;
    std::vector<std::string> columns; // attname per result column, "" = not editable
    std::vector<int> keyColumns;      // result column indexes of the primary key
    std::string readOnlyReason;       // non-empty = result can't be edited
};

struct Statement {
    std::string sql;
    Params params;
    bool expectOneRow = false; // fail unless exactly one row is affected
};

class PostgreSQL : public Database {
public:
    explicit PostgreSQL(ConnectionConfig cfg) : cfg_(std::move(cfg)) {}

    void connect() override;
    void disconnect() override;
    void ping() override;
    void cancel() override;
    bool connected() const override { return conn_ && PQstatus(conn_.get()) == CONNECTION_OK; }
    Capabilities capabilities() const override {
        return Capability::Query | Capability::Tables | Capability::Views | Capability::Transactions;
    }

    // ponytail: PQexec buffers the whole result in memory; switch to single-row mode for huge result sets
    ResultSet execute(const std::string& sql);
    ResultSet execute(const std::string& sql, const Params& params);
    // All-or-nothing; uses a savepoint if a transaction is already open.
    void executeInTransaction(const std::vector<Statement>& stmts);
    EditTarget editTarget(const ResultSet& rs);
    // KeyFlag per result column, from the constraints of the source tables.
    std::vector<uint8_t> keyFlags(const ResultSet& rs);
    std::vector<ResultSet::ForeignRef> foreignRefs(const ResultSet& rs);
    std::vector<ResultSet::Source> sources(const ResultSet& rs);
    static std::string quoteIdent(const std::string& ident);

    std::vector<std::string> databases();
    std::vector<std::string> schemas();
    // {name, kind}: kind is pg_class.relkind (r table, v view, m matview, p partitioned, f foreign)
    std::vector<std::pair<std::string, char>> relations(const std::string& schema);
    // ponytail: whole-catalog snapshot per editor tab; fine up to tens of thousands of columns
    SchemaInfo schemaInfo();
    // One relation's structure as a JSON object (columns, primary key, foreign keys, other constraints,
    // indexes, comment), built by PostgreSQL itself. qualifiedName is quoted, e.g. "public"."users".
    std::string tableStructureJson(const std::string& qualifiedName);

private:
    struct ConnDeleter { void operator()(PGconn* c) const { PQfinish(c); } };
    struct CancelDeleter { void operator()(PGcancel* c) const { PQfreeCancel(c); } };

    PGconn* conn();
    ResultSet toResultSet(PGresult* res);

    ConnectionConfig cfg_;
    std::unique_ptr<PGconn, ConnDeleter> conn_;
    std::mutex cancelMu_;
    std::unique_ptr<PGcancel, CancelDeleter> cancel_;
};
