#include "PostgreSQL.h"

#include <map>

namespace {
struct ResultDeleter { void operator()(PGresult* r) const { PQclear(r); } };
using ResultPtr = std::unique_ptr<PGresult, ResultDeleter>;
}

void PostgreSQL::connect() {
    std::string port = std::to_string(cfg_.port);
    const char* keys[] = {"host", "port", "dbname", "user", "password", "sslmode",
                          "connect_timeout", "application_name", nullptr};
    const char* vals[] = {cfg_.host.c_str(), port.c_str(),
                          cfg_.database.empty() ? "postgres" : cfg_.database.c_str(),
                          cfg_.username.empty() ? nullptr : cfg_.username.c_str(),
                          cfg_.password.empty() ? nullptr : cfg_.password.c_str(),
                          cfg_.sslMode.c_str(), "10", "db-manager", nullptr};
    conn_.reset(PQconnectdbParams(keys, vals, 0));
    if (PQstatus(conn_.get()) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn_.get());
        conn_.reset();
        throw DbError(err);
    }
    std::lock_guard lock(cancelMu_);
    cancel_.reset(PQgetCancel(conn_.get()));
}

void PostgreSQL::disconnect() {
    {
        std::lock_guard lock(cancelMu_);
        cancel_.reset();
    }
    conn_.reset();
}

void PostgreSQL::ping() { execute("SELECT 1"); }

void PostgreSQL::cancel() {
    std::lock_guard lock(cancelMu_);
    if (!cancel_) return;
    char err[256];
    PQcancel(cancel_.get(), err, sizeof err);
}

PGconn* PostgreSQL::conn() {
    if (!conn_) throw DbError("Not connected");
    return conn_.get();
}

ResultSet PostgreSQL::toResultSet(PGresult* raw) {
    ResultPtr res(raw);
    auto st = PQresultStatus(raw);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK && st != PGRES_EMPTY_QUERY)
        throw DbError(PQresultErrorMessage(raw)[0] ? PQresultErrorMessage(raw) : PQerrorMessage(conn_.get()));

    ResultSet rs;
    rs.status = PQcmdStatus(raw);
    if (*PQcmdTuples(raw)) rs.affected = std::stoll(PQcmdTuples(raw));
    int nf = PQnfields(raw), nt = PQntuples(raw);
    for (int c = 0; c < nf; ++c) {
        rs.columns.emplace_back(PQfname(raw, c));
        rs.sourceTable.push_back(PQftable(raw, c));
        rs.sourceColumn.push_back(PQftablecol(raw, c));
        rs.columnType.push_back(PQftype(raw, c));
    }
    rs.rows.reserve(nt);
    for (int r = 0; r < nt; ++r) {
        auto& row = rs.rows.emplace_back();
        row.reserve(nf);
        for (int c = 0; c < nf; ++c) {
            if (PQgetisnull(raw, r, c)) row.emplace_back(std::nullopt);
            else row.emplace_back(std::string(PQgetvalue(raw, r, c), PQgetlength(raw, r, c)));
        }
    }
    return rs;
}

ResultSet PostgreSQL::execute(const std::string& sql) {
    // PQexec runs multi-statement scripts and returns the last result.
    return toResultSet(PQexec(conn(), sql.c_str()));
}

ResultSet PostgreSQL::execute(const std::string& sql, const Params& params) {
    std::vector<const char*> p;
    for (auto& s : params) p.push_back(s ? s->c_str() : nullptr);
    return toResultSet(PQexecParams(conn(), sql.c_str(), int(p.size()), nullptr, p.data(),
                                    nullptr, nullptr, 0));
}

std::vector<std::string> PostgreSQL::databases() {
    std::vector<std::string> out;
    for (auto& row : execute("SELECT datname FROM pg_database "
                             "WHERE NOT datistemplate AND datallowconn ORDER BY 1").rows)
        out.push_back(*row[0]);
    return out;
}

std::vector<std::string> PostgreSQL::schemas() {
    std::vector<std::string> out;
    for (auto& row : execute("SELECT nspname FROM pg_namespace "
                             "WHERE nspname NOT LIKE 'pg\\_%' AND nspname <> 'information_schema' "
                             "ORDER BY 1").rows)
        out.push_back(*row[0]);
    return out;
}

std::vector<std::pair<std::string, char>> PostgreSQL::relations(const std::string& schema) {
    std::vector<std::pair<std::string, char>> out;
    auto rs = execute("SELECT c.relname, c.relkind FROM pg_class c "
                      "JOIN pg_namespace n ON n.oid = c.relnamespace "
                      "WHERE n.nspname = $1 AND c.relkind IN ('r','v','m','p','f') ORDER BY 1",
                      {schema});
    for (auto& row : rs.rows) out.emplace_back(*row[0], (*row[1])[0]);
    return out;
}

std::string PostgreSQL::quoteIdent(const std::string& ident) {
    std::string out = "\"";
    for (char c : ident) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + '"';
}

void PostgreSQL::executeInTransaction(const std::vector<Statement>& stmts) {
    bool nested = PQtransactionStatus(conn()) == PQTRANS_INTRANS;
    execute(nested ? "SAVEPOINT dbm_edit" : "BEGIN");
    try {
        for (auto& st : stmts) {
            auto rs = execute(st.sql, st.params);
            if (st.expectOneRow && rs.affected != 1)
                throw DbError(std::to_string(rs.affected) +
                              " rows affected, expected 1 (row changed or deleted meanwhile?)\n" + st.sql);
        }
    } catch (...) {
        try {
            execute(nested ? "ROLLBACK TO SAVEPOINT dbm_edit" : "ROLLBACK");
        } catch (const DbError&) {
        }
        throw;
    }
    execute(nested ? "RELEASE SAVEPOINT dbm_edit" : "COMMIT");
}

EditTarget PostgreSQL::editTarget(const ResultSet& rs) {
    EditTarget t;
    t.columns.assign(rs.columns.size(), "");
    auto readOnly = [&t](std::string why) {
        t.readOnlyReason = std::move(why);
        return t;
    };
    unsigned oid = 0;
    for (unsigned o : rs.sourceTable) {
        if (!o) continue;
        if (oid && o != oid) return readOnly("result spans several tables");
        oid = o;
    }
    if (!oid) return readOnly("result is not from a table");
    std::string o = std::to_string(oid);

    auto rel = execute("SELECT n.nspname, c.relname, c.relkind FROM pg_class c "
                       "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = $1", {o});
    if (rel.rows.empty() || (*rel.rows[0][2] != "r" && *rel.rows[0][2] != "p"))
        return readOnly("source is not a plain table");
    t.schema = *rel.rows[0][0];
    t.table = *rel.rows[0][1];

    std::map<int, std::string> names; // attnum -> attname, generated columns left out
    for (auto& row : execute("SELECT attnum, attname FROM pg_attribute WHERE attrelid = $1 "
                             "AND attnum > 0 AND NOT attisdropped AND attgenerated = ''", {o}).rows)
        names[std::stoi(*row[0])] = *row[1];
    for (size_t c = 0; c < rs.columns.size(); ++c)
        if (rs.sourceTable[c] == oid && names.count(rs.sourceColumn[c])) t.columns[c] = names[rs.sourceColumn[c]];

    auto pk = execute("SELECT unnest(conkey) FROM pg_constraint WHERE conrelid = $1 AND contype = 'p'", {o});
    if (pk.rows.empty()) return readOnly(t.table + " has no primary key");
    for (auto& row : pk.rows) {
        int attnum = std::stoi(*row[0]);
        int found = -1;
        for (size_t c = 0; c < rs.columns.size() && found < 0; ++c)
            if (rs.sourceTable[c] == oid && rs.sourceColumn[c] == attnum) found = int(c);
        if (found < 0) return readOnly("primary key column not in result");
        t.keyColumns.push_back(found);
    }
    return t;
}

SchemaInfo PostgreSQL::schemaInfo() {
    const char* userSchemas = "n.nspname NOT LIKE 'pg\\_%' AND n.nspname <> 'information_schema'";
    SchemaInfo info;
    std::map<std::pair<std::string, std::string>, size_t> index;
    auto cols = execute(std::string("SELECT n.nspname, c.relname, a.attname, format_type(a.atttypid, a.atttypmod), "
                                    "EXISTS (SELECT 1 FROM pg_constraint p WHERE p.conrelid = c.oid "
                                    "AND p.contype = 'p' AND a.attnum = ANY (p.conkey)) FROM pg_class c "
                                    "JOIN pg_namespace n ON n.oid = c.relnamespace "
                                    "JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped "
                                    "WHERE c.relkind IN ('r','v','m','p','f') AND ") +
                        userSchemas + " ORDER BY 1, 2, a.attnum");
    for (auto& row : cols.rows) {
        auto key = std::pair{*row[0], *row[1]};
        auto [it, added] = index.try_emplace(key, info.tables.size());
        if (added) info.tables.push_back({*row[0], *row[1], {}});
        auto& t = info.tables[it->second];
        t.columns.push_back(*row[2]);
        t.types.push_back(*row[3]);
        t.primary.push_back(*row[4] == "t");
    }

    // column lists joined with \x1f (unit separator) to keep one row per constraint
    auto keyCols = [](const char* keys, const char* rel) {
        return std::string("array_to_string(array(SELECT a.attname FROM unnest(con.") + keys +
               ") WITH ORDINALITY k(n, i) JOIN pg_attribute a ON a.attrelid = con." + rel +
               " AND a.attnum = k.n ORDER BY k.i), chr(31))";
    };
    auto fks = execute("SELECT fn.nspname, fc.relname, tn.nspname, tc.relname, " + keyCols("conkey", "conrelid") +
                       ", " + keyCols("confkey", "confrelid") +
                       " FROM pg_constraint con "
                       "JOIN pg_class fc ON fc.oid = con.conrelid JOIN pg_namespace fn ON fn.oid = fc.relnamespace "
                       "JOIN pg_class tc ON tc.oid = con.confrelid JOIN pg_namespace tn ON tn.oid = tc.relnamespace "
                       "WHERE con.contype = 'f'");
    auto split = [](const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i)
            if (i == s.size() || s[i] == '\x1f') {
                out.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        return out;
    };
    for (auto& row : fks.rows)
        info.foreignKeys.push_back({*row[0], *row[1], *row[2], *row[3], split(*row[4]), split(*row[5])});
    return info;
}

std::string PostgreSQL::tableStructureJson(const std::string& qualifiedName) {
    // column names of constraint key arrays, in key order
    auto names = [](const char* keys, const char* rel) {
        return std::string("(SELECT json_agg(a.attname ORDER BY k.i) FROM unnest(") + keys +
               ") WITH ORDINALITY k(n, i) JOIN pg_attribute a ON a.attrelid = " + rel + " AND a.attnum = k.n)";
    };
    auto rs = execute(
        "SELECT json_build_object("
        "'schema', n.nspname, 'name', c.relname, "
        "'kind', CASE c.relkind WHEN 'r' THEN 'table' WHEN 'p' THEN 'partitioned table' WHEN 'v' THEN 'view' "
        "WHEN 'm' THEN 'materialized view' WHEN 'f' THEN 'foreign table' ELSE c.relkind::text END, "
        "'comment', obj_description(c.oid, 'pg_class'), "
        "'columns', (SELECT json_agg(json_build_object('name', a.attname, 'type', format_type(a.atttypid, a.atttypmod), "
        "  'nullable', NOT a.attnotnull, 'default', pg_get_expr(d.adbin, d.adrelid), "
        "  'comment', col_description(c.oid, a.attnum)) ORDER BY a.attnum) "
        "  FROM pg_attribute a LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
        "  WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped), "
        "'primaryKey', (SELECT " + names("p.conkey", "p.conrelid") +
        "  FROM pg_constraint p WHERE p.conrelid = c.oid AND p.contype = 'p'), "
        "'foreignKeys', (SELECT json_agg(json_build_object('name', f.conname, "
        "  'columns', " + names("f.conkey", "f.conrelid") + ", "
        "  'referencesTable', f.confrelid::regclass::text, "
        "  'referencesColumns', " + names("f.confkey", "f.confrelid") + ", "
        "  'definition', pg_get_constraintdef(f.oid)) ORDER BY f.conname) "
        "  FROM pg_constraint f WHERE f.conrelid = c.oid AND f.contype = 'f'), "
        "'constraints', (SELECT json_agg(json_build_object('name', o.conname, 'definition', pg_get_constraintdef(o.oid)) "
        "  ORDER BY o.conname) FROM pg_constraint o WHERE o.conrelid = c.oid AND o.contype IN ('c', 'u', 'x')), "
        "'indexes', (SELECT json_agg(json_build_object('name', ic.relname, 'definition', pg_get_indexdef(i.indexrelid)) "
        "  ORDER BY ic.relname) FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid WHERE i.indrelid = c.oid)"
        ")::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = $1::regclass",
        {qualifiedName});
    if (rs.rows.empty() || !rs.rows[0][0]) throw DbError("No such table: " + qualifiedName);
    return *rs.rows[0][0];
}

std::vector<uint8_t> PostgreSQL::keyFlags(const ResultSet& rs) {
    std::vector<uint8_t> flags(rs.columns.size(), 0);
    std::string oids;
    for (unsigned o : rs.sourceTable)
        if (o) oids += (oids.empty() ? "" : ",") + std::to_string(o);
    if (oids.empty()) return flags;
    std::map<std::pair<unsigned, int>, uint8_t> byColumn;
    for (auto& row : execute("SELECT conrelid::bigint, unnest(conkey), contype::text FROM pg_constraint "
                             "WHERE contype IN ('p', 'f') AND conrelid = ANY($1::oid[]) "
                             "UNION ALL SELECT indrelid::bigint, unnest(indkey::int2[]), "
                             "CASE WHEN indisunique AND indnkeyatts = 1 THEN 'u' ELSE 'i' END FROM pg_index "
                             "WHERE indrelid = ANY($1::oid[])", // expression index parts are attnum 0: no column
                             {"{" + oids + "}"}).rows)
        byColumn[{unsigned(std::stoul(*row[0])), std::stoi(*row[1])}] |=
            *row[2] == "p"   ? KeyPrimary
            : *row[2] == "f" ? KeyForeign
            : *row[2] == "u" ? KeyIndexed | KeyUnique
                             : KeyIndexed;
    for (size_t c = 0; c < flags.size(); ++c) {
        auto it = byColumn.find({rs.sourceTable[c], rs.sourceColumn[c]});
        if (it != byColumn.end()) flags[c] = it->second;
    }
    return flags;
}

std::vector<ResultSet::ForeignRef> PostgreSQL::foreignRefs(const ResultSet& rs) {
    std::vector<ResultSet::ForeignRef> out;
    std::string oids;
    for (unsigned o : rs.sourceTable)
        if (o) oids += (oids.empty() ? "" : ",") + std::to_string(o);
    if (oids.empty()) return out;
    auto fks = execute("SELECT con.conrelid::bigint, array_to_string(con.conkey, ','), tn.nspname, tc.relname, "
                       "array_to_string(array(SELECT a.attname FROM unnest(con.confkey) WITH ORDINALITY k(n, i) "
                       "JOIN pg_attribute a ON a.attrelid = con.confrelid AND a.attnum = k.n ORDER BY k.i), chr(31)) "
                       "FROM pg_constraint con JOIN pg_class tc ON tc.oid = con.confrelid "
                       "JOIN pg_namespace tn ON tn.oid = tc.relnamespace "
                       "WHERE con.contype = 'f' AND con.conrelid = ANY($1::oid[]) ORDER BY con.oid",
                       {"{" + oids + "}"});
    for (auto& row : fks.rows) {
        unsigned rel = unsigned(std::stoul(*row[0]));
        ResultSet::ForeignRef ref{*row[2], *row[3], {}, {}};
        bool complete = true;
        std::string attnums = *row[1] + ",";
        for (size_t start = 0, comma; (comma = attnums.find(',', start)) != std::string::npos; start = comma + 1) {
            int attnum = std::stoi(attnums.substr(start, comma - start));
            int col = -1;
            for (size_t c = 0; c < rs.columns.size() && col < 0; ++c)
                if (rs.sourceTable[c] == rel && rs.sourceColumn[c] == attnum) col = int(c);
            complete = complete && col >= 0;
            ref.columns.push_back(col);
        }
        std::string targets = *row[4];
        for (size_t start = 0;;) {
            size_t sep = targets.find('\x1f', start);
            ref.targetColumns.push_back(targets.substr(start, sep - start));
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        if (complete) out.push_back(std::move(ref)); // can't filter the parent on a partial key
    }
    return out;
}
