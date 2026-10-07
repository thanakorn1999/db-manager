// Unit checks always run; live checks run when DBM_TEST_PG / DBM_TEST_REDIS = "host:port".
#include "core/database/postgres/PostgreSQL.h"
#include "core/database/redis/Redis.h"
#include "core/export/Export.h"
#include "core/query/SqlCompleter.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

static ConnectionConfig fromEnv(const char* var, DbType type) {
    ConnectionConfig c;
    c.type = type;
    std::string v = std::getenv(var);
    c.host = v.substr(0, v.find(':'));
    c.port = std::stoi(v.substr(v.find(':') + 1));
    return c;
}

// completion at the end of sql, or at '|' if present
static SqlCompleter::Result comp(std::string sql, const SchemaInfo& s) {
    size_t cur = sql.find('|');
    if (cur == std::string::npos) cur = sql.size();
    else sql.erase(cur, 1);
    return SqlCompleter::complete(sql, cur, s);
}
static bool has(const SqlCompleter::Result& r, const std::string& label) {
    for (auto& i : r.items)
        if (i.label == label) return true;
    return false;
}

static void completerTests() {
    SchemaInfo s;
    s.tables = {{"public", "users", {"id", "name", "email"}},
                {"public", "orders", {"id", "user_id", "total"}},
                {"public", "order_items", {"id", "order_id", "product"}},
                {"sales", "invoices", {"id", "order_id"}},
                {"public", "Mixed Case", {"Col"}}};
    s.foreignKeys = {{"public", "orders", "public", "users", {"user_id"}, {"id"}},
                     {"public", "order_items", "public", "orders", {"order_id"}, {"id"}},
                     {"sales", "invoices", "public", "orders", {"order_id"}, {"id"}}};

    auto r = comp("", s);
    assert(has(r, "SELECT") && r.items[0].insert == "SELECT ");
    r = comp("sel", s);
    assert(r.prefix == "sel" && r.items.size() == 1 && r.items[0].label == "SELECT");
    r = comp("SELECT 1; ", s);
    assert(has(r, "SELECT") && has(r, "DELETE FROM"));

    r = comp("SELECT * FROM ", s);
    assert(r.expectsName && has(r, "users") && has(r, "sales.invoices") && has(r, "\"Mixed Case\""));
    r = comp("SELECT * FROM us", s);
    assert(r.items.size() == 1 && r.items[0].insert == "users ");
    assert(SqlCompleter::tableName("public", "agreements") == "agreements" &&
           SqlCompleter::tableName("Sales", "order") == "\"Sales\".\"order\"");
    // PK / FK menu: JOIN the tables whose FKs point at users.id, before WHERE / LIMIT / ;
    auto j = SqlCompleter::joinsOn("SELECT * FROM users LIMIT 100;", s, "public", "users", "id");
    assert(j.size() == 1 && j[0].label == "JOIN orders o ON o.user_id = users.id" &&
           j[0].sql == "SELECT * FROM users JOIN orders o ON o.user_id = users.id LIMIT 100;");
    j = SqlCompleter::joinsOn("SELECT *\nFROM orders o2\nWHERE o2.id > 1", s, "public", "orders", "id");
    assert(j.size() == 2 && j[0].sql == "SELECT *\nFROM orders o2\nJOIN order_items oi ON oi.order_id = o2.id\nWHERE o2.id > 1" &&
           j[1].label == "JOIN sales.invoices i ON i.order_id = o2.id");
    j = SqlCompleter::joinsOn("SELECT * FROM users u -- note", s, "public", "users", "id");
    assert(j.size() == 1 && j[0].sql == "SELECT * FROM users u JOIN orders o ON o.user_id = u.id -- note");
    assert(SqlCompleter::joinsOn("SELECT * FROM orders", s, "public", "users", "id").empty());
    j = SqlCompleter::joinsOn("SELECT * FROM orders WHERE total > 0", s, "public", "orders", "user_id"); // FK: parent
    assert(j.size() == 1 && j[0].sql == "SELECT * FROM orders JOIN users u ON u.id = orders.user_id WHERE total > 0");
    // filter: WHERE added or ANDed, OR kept together, column qualified only when several tables
    auto f = [&](const std::string& sql, const std::string& col, const std::string& op, const std::string& v) {
        return SqlCompleter::addFilter(sql, s, "public", "orders", col, op, v);
    };
    assert(f("SELECT * FROM orders LIMIT 100;", "total", ">", "5") == "SELECT * FROM orders WHERE total > '5' LIMIT 100;");
    assert(f("SELECT * FROM orders", "id", "IS NULL", "") == "SELECT * FROM orders WHERE id IS NULL");
    assert(f("SELECT * FROM orders WHERE a = 1 OR b = 2\nORDER BY id", "id", "IN", "1, 2") ==
           "SELECT * FROM orders WHERE (a = 1 OR b = 2) AND id IN ('1', '2')\nORDER BY id");
    assert(f("SELECT * FROM users u JOIN orders o ON o.user_id = u.id WHERE u.id = 1", "total", "contains", "it's") ==
           "SELECT * FROM users u JOIN orders o ON o.user_id = u.id WHERE u.id = 1 AND o.total::text ILIKE '%it''s%'");
    assert(f("SELECT * FROM users", "id", "=", "1").empty());
    r = comp("SELECT * FROM mix", s);
    assert(r.items.size() == 1 && r.items[0].label == "\"Mixed Case\"");

    // FK joins, both directions, alias-aware, schema-qualified
    r = comp("SELECT * FROM users u JOIN ", s);
    assert(r.items[0].label == "orders o ON o.user_id = u.id" && r.items[0].kind == SqlCompleter::Item::Join);
    r = comp("SELECT * FROM orders LEFT JOIN ", s);
    assert(has(r, "users u ON u.id = orders.user_id"));
    assert(has(r, "order_items oi ON oi.order_id = orders.id"));
    assert(has(r, "sales.invoices i ON i.order_id = orders.id"));
    r = comp("SELECT * FROM orders o JOIN ord", s); // prefix filters joins by table name
    assert(r.items[0].label == "order_items oi ON oi.order_id = o.id" && !has(r, "users u ON u.id = o.user_id"));
    r = comp("SELECT * FROM users u JOIN orders o2 ", s);
    assert(has(r, "ON o2.user_id = u.id") && has(r, "ON"));
    r = comp("SELECT * FROM users u JOIN orders o ON ", s);
    assert(r.items[0].label == "o.user_id = u.id");

    // columns
    r = comp("SELECT u.| FROM users u", s);
    assert(r.items.size() == 3 && r.items[0].label == "id");
    r = comp("SELECT | FROM users u JOIN orders o ON o.user_id = u.id", s);
    assert(has(r, "*") && has(r, "u.id") && has(r, "o.id") && has(r, "name") && has(r, "total"));
    r = comp("SELECT * FROM users WHERE na", s);
    assert(r.items.size() == 1 && r.items[0].insert == "name");
    r = comp("SELECT * FROM users WHERE name = 'x' ", s);
    assert(has(r, "AND") && has(r, "ORDER BY"));
    r = comp("UPDATE users SET ", s);
    assert(has(r, "name") && r.items[1].insert == "name = ");
    r = comp("INSERT INTO users (", s);
    assert(has(r, "email"));
    r = comp("SELECT * FROM sales.", s);
    assert(r.items.size() == 1 && r.items[0].label == "invoices");

    // follow-ups
    assert(comp("DELETE ", s).items[0].label == "FROM");
    assert(comp("SELECT * FROM users ORDER ", s).items[0].label == "BY");
    assert(has(comp("SELECT * FROM users ", s), "LEFT JOIN"));

    // nothing inside strings / comments
    assert(comp("SELECT 'FROM ", s).items.empty());
    assert(comp("-- SELECT", s).items.empty());
    assert(comp("/* SELECT ", s).items.empty());
    assert(!comp("/* x */ SEL", s).items.empty());

    // unknown names: byte ranges -> the words they cover
    auto unknown = [&](const std::string& sql) {
        std::vector<std::string> out;
        for (auto [a, b] : SqlCompleter::unknownNames(sql, s)) out.push_back(sql.substr(a, b - a));
        return out;
    };
    using W = std::vector<std::string>;
    assert(unknown("SELECT * FROM users WHERE name = 'x' AND id > 1") == W{});
    assert(unknown("SELECT * FROM userz WHERE nope = 1") == W{"userz"}); // columns not judged on unknown table
    assert(unknown("SELECT * FROM users u WHERE u.nme = 1 OR emial IS NULL") == (W{"nme", "emial"}));
    assert(unknown("SELECT * FROM sales.invoicez; DELETE FROM \"Mixed Case\" WHERE \"Col\" = 1") == W{"sales.invoicez"});
    assert(unknown("SELECT * FROM users WHERE x.id = 1 AND lower(name) = 'a' AND created < now()::date") == (W{"x", "created"}));
    assert(unknown("WITH t AS (SELECT 1) SELECT * FROM t, generate_series(1, 2), pg_class") == W{});
    assert(unknown("SELECT * FROM users WHERE id IN (SELECT user_id FROM orders WHERE total > 0)") == W{});
    assert(SqlCompleter::unknownNames("SELECT * FROM nope", SchemaInfo{}).empty()); // schema not loaded
    std::puts("completer ok");
}

int main() {
    completerTests();
    ResultSet rs;
    rs.columns = {"id", "note"};
    rs.rows = {{"1", "a,b"}, {"2", std::nullopt}, {"3", ""}, {"4", "say \"hi\"\nit's"}};
    assert(Export::csv(rs) == "id,note\r\n1,\"a,b\"\r\n2,\r\n3,\"\"\r\n4,\"say \"\"hi\"\"\nit's\"\r\n");
    assert(Export::inserts(rs, "t") == "INSERT INTO t (\"id\", \"note\") VALUES ('1', 'a,b');\n"
                                       "INSERT INTO t (\"id\", \"note\") VALUES ('2', NULL);\n"
                                       "INSERT INTO t (\"id\", \"note\") VALUES ('3', '');\n"
                                       "INSERT INTO t (\"id\", \"note\") VALUES ('4', 'say \"hi\"\nit''s');\n");
    {
        ResultSet j;
        j.columns = {"id", "ok", "doc", "name", "big"};
        j.columnType = {23, 16, 3802, 25, 20}; // int4, bool, jsonb, text, int8
        j.rows = {{"1", "t", "{\"a\": [1]}", "say \"hi\"\n\x01", "12345678901234567"}, {"2", std::nullopt, "null", "", "7"}};
        assert(Export::json(j, "") ==
               "{\n  \"structure\": {\"columns\": [{\"name\": \"id\"}, {\"name\": \"ok\"}, {\"name\": \"doc\"}, "
               "{\"name\": \"name\"}, {\"name\": \"big\"}]},\n  \"rows\": [\n"
               "    {\"id\": 1, \"ok\": true, \"doc\": {\"a\": [1]}, \"name\": \"say \\\"hi\\\"\\n\\u0001\", \"big\": \"12345678901234567\"},\n"
               "    {\"id\": 2, \"ok\": null, \"doc\": null, \"name\": \"\", \"big\": 7}\n  ]\n}\n");
        j.rows.clear();
        assert(Export::json(j, "{\"name\": \"t\"}") == "{\n  \"structure\": {\"name\": \"t\"},\n  \"rows\": []\n}\n");
    }
    rs.columnType = {23, 25}; // int4, text
    auto book = Export::xlsx(rs, "t");
    assert(book.compare(0, 4, "PK\x03\x04") == 0 && book.find("xl/worksheets/sheet1.xml") != std::string::npos);
    if (auto* out = std::getenv("DBM_XLSX_OUT")) { // for opening the sample by hand
        FILE* f = std::fopen(out, "wb");
        std::fwrite(book.data(), 1, book.size(), f);
        std::fclose(f);
    }
    using V = std::vector<std::string>;
    assert(Redis::tokenize("GET user:1") == (V{"GET", "user:1"}));
    assert(Redis::tokenize("  SET k  \"a b\\n\"  'c d' ") == (V{"SET", "k", "a b\n", "c d"}));
    assert(Redis::tokenize("SET k \"\"") == (V{"SET", "k", ""}));
    assert(Redis::tokenize("") == V{});
    bool threw = false;
    try { Redis::tokenize("GET \"oops"); } catch (const DbError&) { threw = true; }
    assert(threw);

    if (std::getenv("DBM_TEST_PG")) {
        auto cfg = fromEnv("DBM_TEST_PG", DbType::PostgreSQL);
        PostgreSQL pg(cfg);
        pg.connect();
        assert(pg.connected());
        auto rs = pg.execute("SELECT 1 AS a, NULL AS b, 'x''y' AS c");
        assert(rs.columns == (V{"a", "b", "c"}));
        assert(rs.rows.size() == 1 && *rs.rows[0][0] == "1" && !rs.rows[0][1] && *rs.rows[0][2] == "x'y");
        assert(!pg.databases().empty());
        pg.execute("CREATE TEMP TABLE dbm_t(id int); CREATE TEMP VIEW dbm_v AS SELECT 1");
        bool err = false;
        try { pg.execute("SELEC 1"); } catch (const DbError&) { err = true; }
        assert(err && pg.connected());

        // editing: PK detection, generated / computed columns read-only, all-or-nothing writes
        pg.execute("CREATE TEMP TABLE dbm_e(id int PRIMARY KEY, name text, "
                   "n int GENERATED ALWAYS AS (id * 2) STORED); INSERT INTO dbm_e VALUES (1, 'a'), (2, 'b')");
        auto t = pg.editTarget(pg.execute("SELECT name AS nm, id, n, 1 + 1 AS x FROM dbm_e ORDER BY id"));
        assert(t.readOnlyReason.empty() && t.table == "dbm_e");
        assert(t.columns == (V{"name", "id", "", ""}) && t.keyColumns == std::vector<int>{1});
        assert(!pg.editTarget(pg.execute("SELECT name FROM dbm_e")).readOnlyReason.empty()); // no PK
        assert(!pg.editTarget(pg.execute("SELECT 1")).readOnlyReason.empty());
        pg.execute("CREATE TEMP TABLE dbm_j(id int PRIMARY KEY, e_id int)");
        auto jt = pg.editTarget(pg.execute("SELECT * FROM dbm_e JOIN dbm_j j ON j.e_id = dbm_e.id"));
        assert(jt.readOnlyReason.empty() && jt.joined && jt.table == "dbm_e" && // JOIN: the FROM table is editable
               jt.columns == (V{"id", "name", "", "", ""}) && jt.keyColumns == std::vector<int>{0});
        auto tbl = PostgreSQL::quoteIdent(t.schema) + "." + PostgreSQL::quoteIdent(t.table);
        err = false;
        try {
            pg.executeInTransaction({{"UPDATE " + tbl + " SET name = $1 WHERE id = $2", {"z", "1"}, true},
                                     {"UPDATE " + tbl + " SET name = $1 WHERE id = $2", {"z", "99"}, true}});
        } catch (const DbError&) { err = true; }
        assert(err && *pg.execute("SELECT name FROM dbm_e WHERE id = 1").rows[0][0] == "a"); // rolled back
        pg.executeInTransaction({{"UPDATE " + tbl + " SET name = $1 WHERE id = $2", {std::nullopt, "1"}, true}});
        assert(!pg.execute("SELECT name FROM dbm_e WHERE id = 1").rows[0][0]);
        assert(PostgreSQL::quoteIdent("a\"b") == "\"a\"\"b\"");

        // column types + PK / FK flags for colouring
        pg.execute("CREATE TEMP TABLE dbm_f(id int PRIMARY KEY, e_id int REFERENCES dbm_e, ok bool)");
        auto kf = pg.execute("SELECT ok, e_id, id, 1 AS x FROM dbm_f");
        assert(kf.columnType[0] == 16 && kf.columnType[1] == 23);
        auto flags = pg.keyFlags(kf);
        assert(flags[0] == 0 && flags[1] == KeyForeign && (flags[2] & KeyPrimary) && (flags[2] & KeyUnique) && flags[3] == 0);
        // FK target per result column; composite FKs only when all their columns are in the result
        auto refs = pg.foreignRefs(kf);
        assert(refs.size() == 1 && refs[0].table == "dbm_e" && refs[0].columns == std::vector<int>{1} &&
               refs[0].targetColumns == V{"id"});
        pg.execute("CREATE TEMP TABLE dbm_p(a int, b int, PRIMARY KEY (a, b)); "
                   "CREATE TEMP TABLE dbm_c(x int, y int, u text UNIQUE, FOREIGN KEY (x, y) REFERENCES dbm_p)");
        refs = pg.foreignRefs(pg.execute("SELECT y, u, x FROM dbm_c"));
        assert(refs.size() == 1 && refs[0].columns == (std::vector<int>{2, 0}) && refs[0].targetColumns == (V{"a", "b"}));
        assert(pg.foreignRefs(pg.execute("SELECT x FROM dbm_c")).empty());
        assert(pg.keyFlags(pg.execute("SELECT u, x FROM dbm_c"))[0] == (KeyIndexed | KeyUnique));
        assert(pg.keyFlags(pg.execute("SELECT a FROM dbm_p"))[0] == (KeyPrimary | KeyIndexed)); // composite: not unique alone
        assert(pg.keyFlags(pg.execute("SELECT 1")) == std::vector<uint8_t>{0});
        // source table / column per result column, also across a JOIN (PK / FK ⋮ menu)
        auto src = pg.sources(pg.execute("SELECT f.id, e.name AS nm, 1 FROM dbm_f f JOIN dbm_e e ON e.id = f.e_id"));
        assert(src[0].table == "dbm_f" && src[0].column == "id" && src[1].table == "dbm_e" && src[1].column == "name" &&
               src[2].table.empty());

        // catalog for completion: columns in order, single + composite FKs
        pg.execute("DROP SCHEMA IF EXISTS dbm_test CASCADE; CREATE SCHEMA dbm_test; "
                   "CREATE TABLE dbm_test.a(id int PRIMARY KEY, x int, y int, UNIQUE (x, y)); "
                   "CREATE TABLE dbm_test.b(id int, ax int, ay int, a_id int REFERENCES dbm_test.a, "
                   "FOREIGN KEY (ax, ay) REFERENCES dbm_test.a(x, y))");
        auto si = pg.schemaInfo();
        pg.execute("DROP SCHEMA dbm_test CASCADE");
        bool foundB = false;
        for (auto& t : si.tables)
            if (t.schema == "dbm_test" && t.name == "b") foundB = t.columns == (V{"id", "ax", "ay", "a_id"});
        assert(foundB);
        int fks = 0;
        for (auto& fk : si.foreignKeys)
            if (fk.fromSchema == "dbm_test") {
                ++fks;
                assert(fk.fromTable == "b" && fk.toTable == "a");
                assert((fk.fromColumns == V{"a_id"} && fk.toColumns == V{"id"}) ||
                       (fk.fromColumns == V{"ax", "ay"} && fk.toColumns == V{"x", "y"}));
            }
        assert(fks == 2);
        std::puts("postgres ok");
    }

    if (std::getenv("DBM_TEST_REDIS")) {
        auto cfg = fromEnv("DBM_TEST_REDIS", DbType::Redis);
        cfg.database = "15";
        Redis r(cfg);
        bool err;
        r.connect();
        r.execute({"FLUSHDB"});
        r.execute({"HSET", "dbm:h", "id", "1", "name", "admin"});
        r.execute({"SET", "dbm:s", "hello", "EX", "300"});
        r.execute({"RPUSH", "dbm:l", "a", "b"});
        r.execute({"ZADD", "dbm:z", "2", "m"});
        r.execute({"XADD", "dbm:x", "*", "f", "v"});

        std::string cur = "0";
        std::vector<Redis::KeyInfo> keys;
        do {
            auto [next, page] = r.scan(cur, "dbm:*", 100);
            keys.insert(keys.end(), page.begin(), page.end());
            cur = next;
        } while (cur != "0");
        assert(keys.size() == 5);
        for (auto& k : keys) {
            if (k.key == "dbm:s") assert(k.type == "string" && k.size == 5 && k.ttl > 0);
            if (k.key == "dbm:h") assert(k.type == "hash" && k.size == 2 && k.ttl == -1);
            assert(!r.value(k.key, k.type).rows.empty());
        }
        assert(r.value("dbm:h", "hash").rows.size() == 2);
        assert(r.execute({"GET", "dbm:s"}) == "\"hello\"");
        assert(r.execute({"GET", "nope"}) == "(nil)");
        assert(r.execute({"LRANGE", "dbm:l", "0", "-1"}) == "1) \"a\"\n2) \"b\"");
        assert(r.execute({"BOGUS"}).rfind("(error)", 0) == 0);
        err = false;
        try { r.call({"BOGUS"}); } catch (const DbError&) { err = true; }
        assert(err);
        assert(r.keyspace()[15] == 5);
        assert(r.databaseCount() >= 16);
        r.execute({"FLUSHDB"});
        std::puts("redis ok");
    }
    std::puts("core_test passed");
}
