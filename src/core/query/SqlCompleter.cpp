#include "SqlCompleter.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace SqlCompleter {
namespace {

struct Token {
    enum Kind { Word, Quoted, String, Comment, Punct } kind;
    std::string text; // Quoted: without the quotes
    std::string upper;
    size_t pos, end;
    bool open = false; // unterminated string / quoted name / block comment, or a -- comment
    bool is(const char* kw) const { return kind == Word && upper == kw; }
    bool punct(char c) const { return kind == Punct && text[0] == c; }
    bool name() const { return kind == Word || kind == Quoted; }
};

bool identChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

std::string upper(std::string s) {
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0, n = s.size();
    bool unterminated = false;
    auto closing = [&](char q, size_t from) { // index after the closing quote ('' / "" escape)
        size_t j = from;
        while (j < n) {
            if (s[j] == q) {
                if (j + 1 < n && s[j + 1] == q) j += 2;
                else return j + 1;
            } else {
                ++j;
            }
        }
        unterminated = true;
        return n;
    };
    while (i < n) {
        unsigned char c = s[i];
        if (std::isspace(c)) {
            ++i;
        } else if (s.compare(i, 2, "--") == 0) {
            size_t j = s.find('\n', i);
            j = j == std::string::npos ? n : j;
            out.push_back({Token::Comment, s.substr(i, j - i), "", i, j, true});
            i = j;
        } else if (s.compare(i, 2, "/*") == 0) {
            size_t j = s.find("*/", i + 2);
            bool open = j == std::string::npos;
            j = open ? n : j + 2;
            out.push_back({Token::Comment, s.substr(i, j - i), "", i, j, open});
            i = j;
        } else if (c == '\'' || c == '"') {
            unterminated = false;
            size_t j = closing(char(c), i + 1);
            if (c == '\'') {
                out.push_back({Token::String, s.substr(i, j - i), "", i, j, unterminated});
            } else {
                std::string inner = s.substr(i + 1, j - i - (unterminated ? 1 : 2));
                out.push_back({Token::Quoted, inner, upper(inner), i, j, unterminated});
            }
            i = j;
        } else if (identChar(c)) {
            size_t j = i;
            while (j < n && identChar(s[j])) ++j;
            out.push_back({Token::Word, s.substr(i, j - i), upper(s.substr(i, j - i)), i, j});
            i = j;
        } else {
            out.push_back({Token::Punct, std::string(1, char(c)), "", i, i + 1});
            ++i;
        }
    }
    return out;
}

const std::set<std::string>& keywords() {
    static const std::set<std::string> k = {
        "ALL",    "ALTER",  "AND",    "AS",       "ASC",     "BEGIN",     "BETWEEN", "BY",      "CASE",
        "COMMIT", "CREATE", "CROSS",  "DELETE",   "DESC",    "DISTINCT",  "DROP",    "ELSE",    "END",
        "EXCEPT", "EXISTS", "EXPLAIN", "FALSE",   "FETCH",   "FOR",       "FROM",    "FULL",    "GROUP",
        "HAVING", "ILIKE",  "IN",     "INNER",    "INSERT",  "INTERSECT", "INTO",    "IS",      "JOIN",
        "LATERAL", "LEFT",  "LIKE",   "LIMIT",    "NATURAL", "NOT",       "NULL",    "OFFSET",  "ON",
        "OR",     "ORDER",  "OUTER",  "RETURNING", "RIGHT",  "ROLLBACK",  "SELECT",  "SET",     "TABLE",
        "THEN",   "TRUE",   "TRUNCATE", "UNION",  "UPDATE",  "USING",     "VALUES",  "WHEN",    "WHERE",
        "WINDOW", "WITH"};
    return k;
}
bool isKeyword(const Token& t) { return t.kind == Token::Word && keywords().count(t.upper); }

std::string quoteIdent(const std::string& s) {
    bool plain = !s.empty() && (std::islower(static_cast<unsigned char>(s[0])) || s[0] == '_') &&
                 std::all_of(s.begin(), s.end(), [](unsigned char c) {
                     return std::islower(c) || std::isdigit(c) || c == '_' || c == '$';
                 }) &&
                 !keywords().count(upper(s));
    if (plain) return s;
    std::string out = "\"";
    for (char c : s) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + '"';
}

struct Ref {
    std::string schema, name, alias;
    size_t pos, end = 0; // byte range of [schema.]name
    const SchemaInfo::Table* table = nullptr;
    std::string handle() const { return alias.empty() ? quoteIdent(name) : alias; } // how to qualify columns
};

const SchemaInfo::Table* findTable(const SchemaInfo& s, const std::string& schema, const std::string& name) {
    const SchemaInfo::Table* found = nullptr;
    for (auto& t : s.tables) {
        if (lower(t.name) != lower(name)) continue;
        if (!schema.empty()) {
            if (lower(t.schema) == lower(schema)) return &t;
        } else if (!found || t.schema == "public") {
            found = &t;
        }
    }
    return found;
}

std::string tableName(const SchemaInfo::Table& t) { return SqlCompleter::tableName(t.schema, t.name); }

// Table references: FROM a [AS] x, b / JOIN c y / UPDATE d / INTO e
std::vector<Ref> findRefs(const std::vector<Token>& toks, const SchemaInfo& schema) {
    std::vector<Ref> refs;
    bool inFrom = false;
    int depth = 0, fromDepth = 0; // commas inside f(a, b) don't start a table
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        depth += t.punct('(') - t.punct(')');
        bool starts = t.is("FROM") || t.is("JOIN") || t.is("UPDATE") || t.is("INTO") ||
                      (inFrom && t.punct(',') && depth == fromDepth);
        if (t.is("FROM")) inFrom = true, fromDepth = depth;
        else if (isKeyword(t) && !t.is("AS")) inFrom = false;
        if (!starts || i + 1 >= toks.size() || !toks[i + 1].name() || isKeyword(toks[i + 1])) continue;

        size_t j = i + 1;
        Ref r{"", toks[j].text, "", toks[j].pos};
        if (j + 2 < toks.size() && toks[j + 1].punct('.') && toks[j + 2].name()) {
            r.schema = r.name;
            r.name = toks[j + 2].text;
            j += 2;
        }
        r.end = toks[j].end;
        ++j;
        if (j < toks.size() && toks[j].is("AS")) ++j;
        if (j < toks.size() && toks[j].name() && !isKeyword(toks[j])) r.alias = toks[j].text;
        r.table = findTable(schema, r.schema, r.name);
        refs.push_back(std::move(r));
    }
    return refs;
}

std::string makeAlias(const std::string& table, const std::vector<Ref>& refs) {
    std::string base;
    bool start = true;
    for (char c : table) {
        if (c == '_') start = true;
        else if (start && std::isalpha(static_cast<unsigned char>(c))) {
            base += char(std::tolower(static_cast<unsigned char>(c)));
            start = false;
        } else start = false;
    }
    if (base.empty()) base = "t";
    if (keywords().count(upper(base))) base += "1"; // e.g. "on", "or", "as"
    auto used = [&](const std::string& a) {
        return std::any_of(refs.begin(), refs.end(), [&](const Ref& r) { return lower(r.handle()) == a; });
    };
    std::string alias = base;
    for (int n = 2; used(alias); ++n) alias = base + std::to_string(n);
    return alias;
}

// "x.a = y.b AND ..." joining `other` (aliased `otherHandle`) to `ref` through fk, oriented by fromRef.
std::string condition(const SchemaInfo::ForeignKey& fk, bool refIsFrom, const std::string& refHandle,
                      const std::string& otherHandle) {
    std::string out;
    for (size_t i = 0; i < fk.fromColumns.size() && i < fk.toColumns.size(); ++i) {
        const auto& refCol = refIsFrom ? fk.fromColumns[i] : fk.toColumns[i];
        const auto& otherCol = refIsFrom ? fk.toColumns[i] : fk.fromColumns[i];
        out += (out.empty() ? "" : " AND ") + otherHandle + "." + quoteIdent(otherCol) + " = " + refHandle + "." +
               quoteIdent(refCol);
    }
    return out;
}

bool sameTable(const SchemaInfo::Table& t, const std::string& schema, const std::string& name) {
    return t.schema == schema && t.name == name;
}

struct Builder {
    const std::string& prefix;
    std::vector<Item>& items;
    std::set<std::string> seen;

    bool matches(const std::string& s) const {
        return upper(s).rfind(upper(prefix), 0) == 0 ||
               // also match quoted identifiers by their bare name
               (s.size() > 1 && s[0] == '"' && upper(s.substr(1)).rfind(upper(prefix), 0) == 0);
    }
    void add(const std::string& label, const std::string& insert, Item::Kind kind, const std::string& matchOn = {}) {
        if (!matches(matchOn.empty() ? label : matchOn) || !seen.insert(label).second) return;
        items.push_back({label, insert, kind});
    }
    void keywords(std::initializer_list<const char*> kws) {
        for (auto* k : kws) {
            std::string s = k;
            bool spaced = std::isalpha(static_cast<unsigned char>(s.back())) || s == "*";
            add(s, spaced ? s + " " : s, Item::Keyword);
        }
    }
};

} // namespace

Result complete(const std::string& sql, size_t cursor, const SchemaInfo& schema) {
    Result res;
    cursor = std::min(cursor, sql.size());
    auto all = tokenize(sql);

    // no completion inside strings, comments or quoted identifiers
    for (auto& t : all)
        if (t.kind != Token::Word && t.kind != Token::Punct && t.pos < cursor &&
            (cursor < t.end || (cursor == t.end && t.open)))
            return res;

    // current statement: between the semicolons around the cursor
    size_t from = 0, to = all.size();
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].punct(';')) {
            if (all[i].end <= cursor) from = i + 1;
            else if (to == all.size()) to = i;
        }
    std::vector<Token> stmt;
    for (size_t i = from; i < to; ++i)
        if (all[i].kind != Token::Comment) stmt.push_back(all[i]);

    size_t prefixStart = cursor;
    while (prefixStart > 0 && identChar(sql[prefixStart - 1])) --prefixStart;
    res.prefix = sql.substr(prefixStart, cursor - prefixStart);

    // qualifier in "q.prefix"
    std::string qualifier;
    size_t contextEnd = prefixStart;
    if (prefixStart > 0 && sql[prefixStart - 1] == '.')
        for (auto& t : stmt)
            if (t.end == prefixStart - 1 && t.name()) {
                qualifier = t.text;
                contextEnd = t.pos;
            }
    std::vector<Token> before;
    for (auto& t : stmt)
        if (t.end <= contextEnd) before.push_back(t);

    auto refs = findRefs(stmt, schema);
    Builder b{res.prefix, res.items, {}};

    auto addColumns = [&](const std::vector<const Ref*>& from, bool assign = false) {
        std::map<std::string, int> count; // qualify names that appear in several tables
        for (auto* r : from)
            if (r->table)
                for (auto& c : r->table->columns) ++count[c];
        for (auto* r : from)
            if (r->table)
                for (auto& c : r->table->columns) {
                    std::string ins = (count[c] > 1 ? r->handle() + "." : "") + quoteIdent(c);
                    b.add(ins, assign ? ins + " = " : ins, Item::Column, quoteIdent(c));
                }
    };
    auto allRefs = [&] {
        std::vector<const Ref*> out;
        for (auto& r : refs) out.push_back(&r);
        return out;
    };
    auto addTables = [&] {
        for (auto& t : schema.tables) b.add(tableName(t), tableName(t) + " ", Item::Table, quoteIdent(t.name));
    };

    // q.| : columns of the alias / table, or tables of a schema
    if (!qualifier.empty()) {
        for (auto& r : refs)
            if (r.table && (lower(r.handle()) == lower(qualifier) || lower(r.name) == lower(qualifier))) {
                for (auto& c : r.table->columns) b.add(quoteIdent(c), quoteIdent(c), Item::Column);
                return res;
            }
        if (auto* t = findTable(schema, "", qualifier)) {
            for (auto& c : t->columns) b.add(quoteIdent(c), quoteIdent(c), Item::Column);
            return res;
        }
        for (auto& t : schema.tables)
            if (lower(t.schema) == lower(qualifier)) b.add(quoteIdent(t.name), quoteIdent(t.name) + " ", Item::Table);
        res.expectsName = true;
        return res;
    }

    if (before.empty()) {
        b.keywords({"SELECT", "INSERT INTO", "UPDATE", "DELETE FROM", "WITH", "EXPLAIN", "CREATE TABLE",
                    "ALTER TABLE", "DROP TABLE", "TRUNCATE", "BEGIN", "COMMIT", "ROLLBACK"});
        return res;
    }

    const Token& last = before.back();
    const std::string L = last.kind == Token::Word ? last.upper : last.text;
    const std::string kind = stmt.empty() ? "" : stmt[0].upper; // SELECT / UPDATE / DELETE / INSERT / WITH

    // the clause the cursor is in
    std::string clause;
    for (size_t i = before.size(); i-- > 0;) {
        static const std::set<std::string> clauses = {"SELECT", "FROM",   "JOIN",  "ON",     "WHERE",  "BY",
                                                      "HAVING", "LIMIT",  "SET",   "INTO",   "VALUES", "UPDATE",
                                                      "DELETE", "INSERT", "OFFSET", "RETURNING", "USING", "TABLE"};
        if (before[i].kind == Token::Word && clauses.count(before[i].upper)) {
            clause = before[i].upper;
            if (clause == "BY" && i > 0) clause = before[i - 1].upper + " BY";
            break;
        }
    }

    bool expectOperand;
    if (last.kind == Token::Punct) {
        if (last.text == "*")
            expectOperand = !(before.size() > 1 && (before[before.size() - 2].is("SELECT") ||
                                                     before[before.size() - 2].punct(',') ||
                                                     before[before.size() - 2].punct('.')));
        else
            expectOperand = std::string(",(=<>!+-/%|&").find(last.text[0]) != std::string::npos;
    } else if (last.kind == Token::Word) {
        static const std::set<std::string> operandEnds = {"NULL", "TRUE", "FALSE", "END", "ASC", "DESC"};
        expectOperand = isKeyword(last) && !operandEnds.count(L);
    } else {
        expectOperand = false; // quoted identifier or string literal
    }

    const Ref* lastRef = nullptr; // nearest table reference before the cursor
    for (auto& r : refs)
        if (r.pos < prefixStart) lastRef = &r;

    // FK joins between `table` and the tables already in the statement
    auto fkJoins = [&](bool withOn) {
        for (auto& r : refs) {
            if (!r.table || r.pos >= prefixStart) continue;
            for (auto& fk : schema.foreignKeys) {
                for (bool refIsFrom : {true, false}) {
                    if (!sameTable(*r.table, refIsFrom ? fk.fromSchema : fk.toSchema,
                                   refIsFrom ? fk.fromTable : fk.toTable))
                        continue;
                    auto* other = findTable(schema, refIsFrom ? fk.toSchema : fk.fromSchema,
                                            refIsFrom ? fk.toTable : fk.fromTable);
                    if (!other) continue;
                    std::string alias = makeAlias(other->name, refs);
                    std::string text = tableName(*other) + " " + alias + (withOn ? " ON " : " ") +
                                       condition(fk, refIsFrom, r.handle(), alias);
                    b.add(text, text + " ", Item::Join, quoteIdent(other->name));
                }
            }
        }
    };
    // ON conditions for the table just joined (the last ref before the cursor)
    auto onConditions = [&](const std::string& lead) {
        const Ref* joined = lastRef;
        if (!joined || !joined->table) return;
        for (auto& r : refs) {
            if (&r == joined || !r.table || r.pos >= joined->pos) continue;
            for (auto& fk : schema.foreignKeys)
                for (bool refIsFrom : {true, false}) {
                    if (!sameTable(*r.table, refIsFrom ? fk.fromSchema : fk.toSchema,
                                   refIsFrom ? fk.fromTable : fk.toTable) ||
                        !sameTable(*joined->table, refIsFrom ? fk.toSchema : fk.fromSchema,
                                   refIsFrom ? fk.toTable : fk.fromTable))
                        continue;
                    std::string cond = condition(fk, refIsFrom, r.handle(), joined->handle());
                    b.add(lead + cond, lead + cond + " ", Item::Join, lead.empty() ? joined->handle() : "ON");
                }
        }
    };

    // single-keyword follow-ups
    if (L == "DELETE") b.keywords({"FROM"});
    else if (L == "INSERT") b.keywords({"INTO"});
    else if (L == "GROUP" || L == "ORDER") b.keywords({"BY"});
    else if (L == "LEFT" || L == "RIGHT" || L == "FULL") b.keywords({"JOIN", "OUTER JOIN"});
    else if (L == "INNER" || L == "CROSS" || L == "NATURAL" || L == "OUTER") b.keywords({"JOIN"});
    else if (L == "IS") b.keywords({"NULL", "NOT NULL"});
    else if (L == "UNION") b.keywords({"SELECT", "ALL"});
    if (!res.items.empty()) return res;

    // a table name goes right after FROM / JOIN / UPDATE / INTO ... (or after a comma in FROM)
    bool tableSlot = clause == "FROM" || clause == "JOIN" || clause == "UPDATE" || clause == "INTO" ||
                     clause == "TABLE" || clause == "USING";
    bool wantTable = tableSlot && (L == clause || (L == "," && clause == "FROM"));
    auto joins = {"JOIN", "LEFT JOIN", "INNER JOIN", "RIGHT JOIN", "FULL JOIN", "CROSS JOIN"};
    if (wantTable) {
        res.expectsName = true;
        if (clause == "JOIN") fkJoins(true);
        addTables();
    } else if (clause == "FROM" || clause == "USING") {
        res.expectsName = true; // JOIN suggestions are a space away
        if (kind == "DELETE") b.keywords({"WHERE", "USING", "RETURNING"});
        else {
            b.keywords({"WHERE"});
            b.keywords(joins);
            b.keywords({"GROUP BY", "ORDER BY", "LIMIT", "AS"});
        }
    } else if (clause == "JOIN") {
        res.expectsName = true;
        onConditions("ON ");
        b.keywords({"ON", "USING", "AS"});
    } else if (clause == "UPDATE") {
        b.keywords({"SET"});
    } else if (clause == "INTO") {
        if (expectOperand && lastRef) addColumns({lastRef}); // INSERT INTO t (|
        else b.keywords({"VALUES", "SELECT"});
    } else if (clause == "ON") {
        if (expectOperand) {
            if (L == "ON") {
                res.expectsName = true;
                onConditions("");
            }
            addColumns(allRefs());
        } else {
            b.keywords({"AND", "OR", "WHERE"});
            b.keywords(joins);
            b.keywords({"GROUP BY", "ORDER BY", "LIMIT"});
        }
    } else if (clause == "SELECT" || clause == "RETURNING") {
        if (expectOperand) {
            if (L == "SELECT") b.keywords({"*", "DISTINCT"});
            addColumns(allRefs());
            if (clause == "SELECT") b.keywords({"COUNT(*)", "CASE"});
        } else if (clause == "SELECT") {
            b.keywords({"FROM", "AS"});
        } else {
            b.keywords({"AS"});
        }
    } else if (clause == "WHERE" || clause == "HAVING") {
        if (expectOperand) {
            addColumns(allRefs());
            b.keywords({"NOT", "EXISTS", "NULL", "TRUE", "FALSE"});
        } else {
            b.keywords({"AND", "OR", "IS NULL", "IS NOT NULL", "IN", "LIKE", "ILIKE", "BETWEEN"});
            if (kind == "UPDATE" || kind == "DELETE") b.keywords({"RETURNING"});
            else b.keywords({"GROUP BY", "ORDER BY", "LIMIT"});
        }
    } else if (clause == "GROUP BY" || clause == "ORDER BY") {
        if (expectOperand) addColumns(allRefs());
        else if (clause == "GROUP BY") b.keywords({"HAVING", "ORDER BY", "LIMIT"});
        else b.keywords({"ASC", "DESC", "NULLS LAST", "LIMIT"});
    } else if (clause == "SET") {
        std::vector<const Ref*> target;
        if (!refs.empty()) target.push_back(&refs[0]);
        if (expectOperand && (L == "SET" || L == ",")) addColumns(target, true);
        else if (expectOperand) addColumns(allRefs());
        else b.keywords({"WHERE", "FROM", "RETURNING"});
    } else if (clause == "LIMIT") {
        if (!expectOperand) b.keywords({"OFFSET"});
    }
    if (res.items.size() > 200) res.items.resize(200);
    return res;
}

namespace {

void checkStatement(const std::vector<Token>& toks, const SchemaInfo& schema,
                    std::vector<std::pair<size_t, size_t>>& out) {
    std::set<std::string> ctes; // WITH x AS (
    for (size_t i = 0; i + 2 < toks.size(); ++i)
        if (toks[i].name() && toks[i + 1].is("AS") && toks[i + 2].punct('(')) ctes.insert(lower(toks[i].text));

    auto refs = findRefs(toks, schema);
    bool resolved = !refs.empty();
    for (auto& r : refs) {
        if (r.table) continue;
        resolved = false;
        if (ctes.count(lower(r.name))) continue;
        auto next = std::find_if(toks.begin(), toks.end(), [&](const Token& t) { return t.pos >= r.end; });
        bool call = next != toks.end() && next->punct('('); // FROM generate_series(...)
        bool system = lower(r.schema) == "pg_catalog" || lower(r.schema) == "information_schema" ||
                      lower(r.name).rfind("pg_", 0) == 0;
        if (!call && !system) out.push_back({r.pos, r.end});
    }
    // columns are only judged when every table is known and there's no FROM (subquery)
    if (!resolved) return;
    for (size_t i = 0; i + 1 < toks.size(); ++i)
        if ((toks[i].is("FROM") || toks[i].is("JOIN")) && toks[i + 1].punct('(')) return;

    // ponytail: hand-picked non-column words seen in WHERE; extend when a false warning shows up
    static const std::set<std::string> notColumns = {
        "ANY",          "SOME",         "ARRAY",      "INTERVAL",          "DATE",           "TIME",
        "TIMESTAMP",    "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP", "CURRENT_USER", "LOCALTIME",
        "LOCALTIMESTAMP", "SIMILAR",    "TO",         "ESCAPE",            "ISNULL",         "NOTNULL",
        "SYMMETRIC",    "COLLATE",      "AT",         "ZONE",              "UNKNOWN"};
    auto hasColumn = [](const SchemaInfo::Table& t, const Token& c) {
        return std::any_of(t.columns.begin(), t.columns.end(), [&](const std::string& n) {
            return c.kind == Token::Quoted ? n == c.text : lower(n) == lower(c.text);
        });
    };
    auto refFor = [&](const Token& q) -> const Ref* {
        for (auto& r : refs)
            if (lower(r.handle()) == lower(q.text) || lower(r.name) == lower(q.text)) return &r;
        return nullptr;
    };
    bool inWhere = false;
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        if (isKeyword(t)) {
            static const std::set<std::string> ends = {"SELECT", "FROM",   "JOIN",      "ON",     "GROUP",
                                                       "ORDER",  "LIMIT",  "OFFSET",    "HAVING", "RETURNING",
                                                       "UNION",  "EXCEPT", "INTERSECT", "WINDOW", "FETCH",
                                                       "FOR",    "SET",    "VALUES",    "USING"};
            if (t.is("WHERE")) inWhere = true;
            else if (ends.count(t.upper)) inWhere = false;
            continue;
        }
        if (!inWhere || !t.name()) continue;
        if (t.kind == Token::Word &&
            (std::isdigit(static_cast<unsigned char>(t.text[0])) || t.text[0] == '$' || notColumns.count(t.upper)))
            continue;
        const Token* next = i + 1 < toks.size() ? &toks[i + 1] : nullptr;
        if (next && (next->punct('(') || next->kind == Token::String)) continue; // f(...) / DATE '...'
        if (i > 0 && toks[i - 1].punct(':')) continue;                            // ::type
        if (next && next->punct('.')) { // qualifier
            bool known = refFor(t) || std::any_of(refs.begin(), refs.end(),
                                                  [&](const Ref& r) { return lower(r.schema) == lower(t.text); });
            if (!known) out.push_back({t.pos, t.end});
            continue;
        }
        bool known;
        if (i >= 2 && toks[i - 1].punct('.')) {
            auto* r = refFor(toks[i - 2]);
            known = !r || hasColumn(*r->table, t); // unknown qualifier is already marked
        } else {
            known = std::any_of(refs.begin(), refs.end(), [&](const Ref& r) { return hasColumn(*r.table, t); });
        }
        if (!known) out.push_back({t.pos, t.end});
    }
}

} // namespace

std::string tableName(const std::string& schema, const std::string& name) {
    return (schema == "public" ? "" : quoteIdent(schema) + ".") + quoteIdent(name);
}

std::vector<Join> joinsOn(const std::string& sql, const SchemaInfo& schema, const std::string& tableSchema,
                          const std::string& table, const std::string& column) {
    std::vector<Join> out;
    std::vector<Token> toks;
    for (auto& t : tokenize(sql))
        if (t.kind != Token::Comment) toks.push_back(t);
    // ponytail: first reference to the table in the text; several statements on it pick the first
    auto refs = findRefs(toks, schema);
    auto ref = std::find_if(refs.begin(), refs.end(),
                            [&](const Ref& r) { return r.table && sameTable(*r.table, tableSchema, table); });
    if (ref == refs.end()) return out;

    // end of the FROM clause: the first top-level clause keyword, ; or ) after the table
    static const std::set<std::string> clauseEnd = {"WHERE", "GROUP",  "HAVING",    "ORDER",  "LIMIT",
                                                    "OFFSET", "UNION", "INTERSECT", "EXCEPT", "WINDOW",
                                                    "FETCH", "FOR",    "RETURNING"};
    size_t i = 0;
    while (toks[i].pos != ref->pos) ++i;
    int depth = 0;
    for (; i < toks.size(); ++i) {
        const auto& t = toks[i];
        if (t.punct('(')) ++depth;
        else if (t.punct(')') && --depth < 0) break;
        else if (depth == 0 && (t.punct(';') || (t.kind == Token::Word && clauseEnd.count(t.upper)))) break;
    }
    bool atEnd = i == toks.size();
    size_t at = atEnd ? toks.back().end : toks[i].pos;

    for (auto& fk : schema.foreignKeys)
        for (bool refIsFrom : {true, false}) { // FK column: its parent table; PK column: the referencing tables
            const auto& cols = refIsFrom ? fk.fromColumns : fk.toColumns;
            if ((refIsFrom ? fk.fromSchema : fk.toSchema) != tableSchema ||
                (refIsFrom ? fk.fromTable : fk.toTable) != table ||
                std::find(cols.begin(), cols.end(), column) == cols.end())
                continue;
            auto* other = findTable(schema, refIsFrom ? fk.toSchema : fk.fromSchema,
                                    refIsFrom ? fk.toTable : fk.fromTable);
            if (!other) continue;
            std::string alias = makeAlias(other->name, refs);
            std::string join =
                "JOIN " + tableName(*other) + " " + alias + " ON " + condition(fk, refIsFrom, ref->handle(), alias);
            std::string text = atEnd ? " " + join : join + (sql[at - 1] == '\n' ? "\n" : " ");
            out.push_back({join, sql.substr(0, at) + text + sql.substr(at)});
        }
    return out;
}

std::vector<std::pair<size_t, size_t>> unknownNames(const std::string& sql, const SchemaInfo& schema) {
    std::vector<std::pair<size_t, size_t>> out;
    if (schema.tables.empty()) return out;
    std::vector<Token> stmt;
    for (auto& t : tokenize(sql)) {
        if (t.punct(';')) {
            checkStatement(stmt, schema, out);
            stmt.clear();
        } else if (t.kind != Token::Comment) {
            stmt.push_back(t);
        }
    }
    checkStatement(stmt, schema, out);
    std::sort(out.begin(), out.end());
    return out;
}

}
