#include "Import.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamReader>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>

namespace Import {
namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// PostgreSQL COPY-style CSV, as Export::csv writes it: unquoted empty = NULL, "" = empty string.
ResultSet csv(const std::string& s) {
    ResultSet rs;
    std::vector<std::optional<std::string>> row;
    std::string field;
    bool quoted = false;
    size_t line = 1;
    auto endField = [&] {
        row.push_back(quoted || !field.empty() ? std::optional(field) : std::nullopt);
        field.clear();
        quoted = false;
    };
    auto endRow = [&] {
        endField();
        if (rs.columns.empty()) {
            for (auto& f : row) rs.columns.push_back(f.value_or(""));
        } else {
            if (row.size() != rs.columns.size())
                throw DbError("line " + std::to_string(line) + " has " + std::to_string(row.size()) + " fields, the header " +
                              std::to_string(rs.columns.size()));
            rs.rows.push_back(std::move(row));
        }
        row.clear();
    };
    size_t i = s.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0; // BOM (Excel's "CSV UTF-8")
    while (i < s.size()) {
        char c = s[i];
        if (c == '"' && field.empty() && !quoted) {
            quoted = true;
            for (++i;; ++i) {
                if (i >= s.size()) throw DbError("unterminated quote");
                if (s[i] == '\n') ++line;
                if (s[i] != '"') field += s[i];
                else if (i + 1 < s.size() && s[i + 1] == '"') field += s[++i];
                else break;
            }
            ++i;
        } else if (c == ',') {
            endField();
            ++i;
        } else if (c == '\r' || c == '\n') {
            endRow();
            ++line;
            i += c == '\r' && i + 1 < s.size() && s[i + 1] == '\n' ? 2 : 1;
        } else {
            field += c;
            ++i;
        }
    }
    if (!field.empty() || quoted || !row.empty()) endRow(); // no newline at the end
    if (rs.columns.empty()) throw DbError("empty file");
    return rs;
}

// text of <t> elements up to the end of the current element (skips <rPh>, Excel's phonetic hints)
std::string runText(QXmlStreamReader& x) {
    std::string out;
    int depth = 1;
    while (depth && !x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            if (x.name() == u"rPh") x.skipCurrentElement();
            else if (x.name() == u"t") out += x.readElementText().toStdString();
            else ++depth;
        } else if (x.isEndElement()) {
            --depth;
        }
    }
    return out;
}

// First sheet; row 1 is the header. Empty cell = NULL (that's how Export::xlsx writes NULL).
ResultSet xlsx(const std::string& bytes) {
    std::map<std::string, std::string> files;
    for (auto& [name, data] : unzip(bytes)) files[name] = std::move(data);
    auto sheet = files.find("xl/worksheets/sheet1.xml");
    if (sheet == files.end()) throw DbError("no worksheet (not an .xlsx?)");

    std::vector<std::string> shared;
    if (auto ss = files.find("xl/sharedStrings.xml"); ss != files.end()) { // Excel-saved files
        QXmlStreamReader x(QByteArray::fromStdString(ss->second));
        while (!x.atEnd())
            if (x.readNext() == QXmlStreamReader::StartElement && x.name() == u"si") shared.push_back(runText(x));
    }

    std::vector<std::vector<std::optional<std::string>>> rows;
    QXmlStreamReader x(QByteArray::fromStdString(sheet->second));
    while (!x.atEnd()) {
        if (x.readNext() != QXmlStreamReader::StartElement) continue;
        if (x.name() == u"row") {
            rows.emplace_back();
        } else if (x.name() == u"c" && !rows.empty()) {
            auto& row = rows.back();
            std::string ref = x.attributes().value("r").toString().toStdString(), type = x.attributes().value("t").toString().toStdString();
            size_t col = 0;
            for (char ch : ref)
                if (ch >= 'A' && ch <= 'Z') col = col * 26 + size_t(ch - 'A' + 1);
            col = col ? col - 1 : row.size();
            std::optional<std::string> value, v;
            while (!(x.readNext() == QXmlStreamReader::EndElement && x.name() == u"c") && !x.atEnd()) {
                if (!x.isStartElement()) continue;
                if (x.name() == u"v") v = x.readElementText().toStdString();
                else if (x.name() == u"is") value = runText(x);
            }
            if (type == "s" && v) value = shared.at(std::stoul(*v));
            else if (type == "b" && v) value = *v == "1" ? "true" : "false";
            else if (type != "inlineStr") value = v;
            if (row.size() <= col) row.resize(col + 1);
            row[col] = value;
        }
    }
    if (x.hasError()) throw DbError("bad worksheet XML: " + x.errorString().toStdString());
    if (rows.empty()) throw DbError("empty sheet");
    ResultSet rs;
    for (auto& h : rows[0]) rs.columns.push_back(h.value_or(""));
    while (!rs.columns.empty() && rs.columns.back().empty()) rs.columns.pop_back();
    for (size_t r = 1; r < rows.size(); ++r) {
        rows[r].resize(rs.columns.size());
        rs.rows.push_back(std::move(rows[r]));
    }
    return rs;
}

std::optional<std::string> jsonText(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined: return std::nullopt;
    case QJsonValue::Bool: return v.toBool() ? "true" : "false";
    case QJsonValue::Double: return QString::number(v.toDouble(), 'g', 15).toStdString(); // Export: <= 15 digits
    case QJsonValue::String: return v.toString().toStdString();
    case QJsonValue::Array: return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact).toStdString();
    case QJsonValue::Object: return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact).toStdString();
    }
    return std::nullopt;
}

// Export::json ({"structure", "rows"}) or a plain [{...}] array (Copy as JSON)
void json(const std::string& bytes, Table& t) {
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(bytes), &err);
    if (doc.isNull()) throw DbError("bad JSON: " + err.errorString().toStdString());
    QJsonArray rows = doc.isArray() ? doc.array() : doc.object()["rows"].toArray();
    if (doc.isObject()) {
        auto st = doc.object()["structure"].toObject();
        t.structureJson = QJsonDocument(st).toJson(QJsonDocument::Compact).toStdString();
        if (st["name"].isString()) t.name = st["name"].toString().toStdString();
        if (st["schema"].isString()) t.schema = st["schema"].toString().toStdString();
        for (const auto& c : st["columns"].toArray()) t.data.columns.push_back(c.toObject()["name"].toString().toStdString());
    }
    if (t.data.columns.empty() && !rows.isEmpty()) // QJsonObject sorts its keys: the original order is gone
        for (const auto& k : rows[0].toObject().keys()) t.data.columns.push_back(k.toStdString());
    for (const auto& r : rows) {
        auto o = r.toObject();
        auto& row = t.data.rows.emplace_back();
        for (auto& c : t.data.columns) row.push_back(jsonText(o[QString::fromStdString(c)]));
    }
}

std::string lit(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("''") : std::string(1, c);
    return out + "'";
}

std::string qi(const std::string& s) { return PostgreSQL::quoteIdent(s); }

// how PostgreSQL prints a name (regclass, constraint definitions): quoted only when it has to be
// ponytail: keywords used as names (a table called "user") aren't detected; they just won't be remapped
std::string pgName(const std::string& s) {
    bool bare = !s.empty() && !std::isdigit(static_cast<unsigned char>(s[0]));
    for (char c : s) bare = bare && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
    return bare ? s : qi(s);
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (auto& s : v) out += (out.empty() ? "" : sep) + s;
    return out;
}

std::vector<std::string> strings(const QJsonValue& a) {
    std::vector<std::string> out;
    for (const auto& v : a.toArray()) out.push_back(v.toString().toStdString());
    return out;
}

uint32_t le(const std::string& s, size_t at, int bytes) {
    if (at + bytes > s.size()) throw DbError("not a valid .zip (truncated)");
    uint32_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = v << 8 | static_cast<unsigned char>(s[at + i]);
    return v;
}

} // namespace

std::vector<std::pair<std::string, std::string>> unzip(const std::string& z) {
    // end of central directory: last "PK\5\6" (a comment of up to 64 KB may follow it)
    size_t end = std::string::npos;
    for (size_t i = z.size() >= 22 ? z.size() - 22 : 0; i != size_t(-1) && z.size() - i <= 22 + 65535; --i)
        if (z.compare(i, 4, "PK\x05\x06") == 0) {
            end = i;
            break;
        }
    if (z.size() < 22 || end == std::string::npos) throw DbError("not a valid .zip");
    std::vector<std::pair<std::string, std::string>> out;
    size_t p = le(z, end + 16, 4);
    for (uint32_t n = le(z, end + 10, 2); n--;) {
        if (le(z, p, 4) != 0x02014b50) throw DbError("not a valid .zip (central directory)");
        uint32_t method = le(z, p + 10, 2), packed = le(z, p + 20, 4), size = le(z, p + 24, 4);
        uint32_t nameLen = le(z, p + 28, 2), extraLen = le(z, p + 30, 2), commentLen = le(z, p + 32, 2);
        uint32_t local = le(z, p + 42, 4);
        std::string name = z.substr(p + 46, nameLen);
        p += 46 + nameLen + extraLen + commentLen;
        if (name.empty() || name.back() == '/') continue; // folder
        size_t data = local + 30 + le(z, local + 26, 2) + le(z, local + 28, 2);
        if (data + packed > z.size()) throw DbError("not a valid .zip (truncated)");
        std::string body(size, '\0');
        if (method == 0) {
            body = z.substr(data, packed);
        } else if (method == 8) {
            z_stream s{};
            inflateInit2(&s, -15); // raw deflate
            s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(z.data() + data));
            s.avail_in = packed;
            s.next_out = reinterpret_cast<Bytef*>(body.data());
            s.avail_out = size;
            int rc = inflate(&s, Z_FINISH);
            inflateEnd(&s);
            if (rc != Z_STREAM_END || s.total_out != size) throw DbError(name + ": damaged in the .zip");
        } else {
            throw DbError(name + ": unsupported .zip compression");
        }
        out.emplace_back(std::move(name), std::move(body));
    }
    return out;
}

std::vector<Table> read(const std::string& fileName, const std::string& bytes) {
    try {
        std::string base = fileName.substr(fileName.find_last_of("/\\") + 1);
        size_t dot = base.rfind('.');
        std::string ext = dot == std::string::npos ? "" : lower(base.substr(dot + 1));
        if (ext == "zip") {
            std::vector<Table> out;
            for (auto& [name, data] : unzip(bytes)) {
                std::string leaf = name.substr(name.find_last_of('/') + 1);
                std::string e = lower(leaf.substr(leaf.rfind('.') + 1));
                // Finder's re-zips add __MACOSX/ and dot files; skip anything we didn't export
                if (name.rfind("__MACOSX/", 0) == 0 || leaf[0] == '.' || (e != "csv" && e != "xlsx" && e != "json" && e != "sql"))
                    continue;
                for (auto& t : read(name, data)) out.push_back(std::move(t));
            }
            if (out.empty()) throw DbError("no .csv / .xlsx / .json / .sql files inside");
            return out;
        }
        Table t;
        t.file = fileName;
        std::string stem = base.substr(0, dot); // "schema.table" (database export) or "table"
        if (size_t d = stem.find('.'); d != std::string::npos) t.schema = stem.substr(0, d), t.name = stem.substr(d + 1);
        else t.name = stem;
        if (ext == "csv") t.data = csv(bytes);
        else if (ext == "xlsx") t.data = xlsx(bytes);
        else if (ext == "json") json(bytes, t);
        else if (ext == "sql") t.sql = bytes;
        else throw DbError("unknown file type: expected .csv, .xlsx, .json, .sql or .zip");
        return {std::move(t)};
    } catch (const std::exception& e) {
        throw DbError(fileName + ": " + e.what());
    }
}

std::vector<Statement> plan(const std::vector<Target>& targets, bool structure, bool dropFirst) {
    std::vector<Statement> out;
    auto add = [&](std::string sql) { out.push_back({std::move(sql), {}}); };
    auto qname = [](const Target& t) { return qi(t.schema) + "." + qi(t.name); };

    std::set<std::string> schemas;
    for (auto& t : targets)
        if (t.table->sql.empty()) schemas.insert(t.schema); // .sql files name their own tables
    // one target schema: the structure's unqualified names (sequences, FK targets) resolve there first
    if (structure && schemas.size() == 1) add("SET LOCAL search_path TO " + qi(*schemas.begin()) + ", public");

    struct Info {
        QJsonObject st;
        std::set<std::string> generated;
        std::vector<std::pair<std::string, std::string>> sequences; // nextval('seq') default: {seq, column}
    };
    std::vector<Info> info(targets.size());
    static const std::regex nextval(R"(^nextval\('((?:[^']|'')+)'::regclass\)$)");
    for (size_t i = 0; i < targets.size(); ++i) {
        auto& t = *targets[i].table;
        if (!t.structureJson.empty()) info[i].st = QJsonDocument::fromJson(QByteArray::fromStdString(t.structureJson)).object();
        for (const auto& c : info[i].st["columns"].toArray())
            if (c.toObject()["generated"].toString() == "s") info[i].generated.insert(c.toObject()["name"].toString().toStdString());
    }

    // tables of this import as the export printed them ("src.parent", or "parent" when it was on the
    // search_path), so FKs between them and their serial sequences follow them to the new place
    std::map<std::string, std::string> moved;     // printed source name -> target
    std::map<std::string, std::string> schemaMap; // "src." -> "target." prefix, for sequences
    for (size_t i = 0; i < targets.size(); ++i) {
        auto& st = info[i].st;
        std::string src = st["schema"].toString().toStdString(), name = st["name"].toString().toStdString();
        if (name.empty()) continue;
        moved[pgName(src) + "." + pgName(name)] = moved[pgName(name)] = qname(targets[i]);
        schemaMap[pgName(src) + "."] = qi(targets[i].schema) + ".";
    }
    auto moveSequence = [&](std::string seq) {
        for (auto& [from, to] : schemaMap)
            if (seq.rfind(from, 0) == 0) return to + seq.substr(from.size());
        return seq;
    };

    if (structure) {
        for (auto& s : schemas) add("CREATE SCHEMA IF NOT EXISTS " + qi(s));
        for (size_t i = 0; i < targets.size(); ++i) {
            auto& t = *targets[i].table;
            if (!t.sql.empty()) continue;
            auto& st = info[i].st;
            std::string q = qname(targets[i]);
            if (dropFirst) add("DROP TABLE IF EXISTS " + q + " CASCADE");
            std::vector<std::string> defs, comments;
            auto cols = st["columns"].toArray();
            if (cols.isEmpty() || !cols[0].toObject().contains("type")) { // no structure: every column text
                cols = {};
                for (auto& c : t.data.columns) cols.append(QJsonObject{{"name", QString::fromStdString(c)}});
            }
            for (const auto& cv : cols) {
                auto c = cv.toObject();
                std::string name = c["name"].toString().toStdString(), def = c["default"].toString().toStdString();
                std::string d = qi(name) + " " + (c["type"].isString() ? c["type"].toString().toStdString() : "text");
                std::smatch m;
                if (c["generated"].toString() == "s") {
                    d += " GENERATED ALWAYS AS (" + def + ") STORED";
                } else if (c["identity"].isString()) {
                    d += c["identity"].toString() == "a" ? " GENERATED ALWAYS AS IDENTITY" : " GENERATED BY DEFAULT AS IDENTITY";
                } else if (!def.empty()) {
                    if (std::regex_match(def, m, nextval)) { // serial: its sequence isn't in the export
                        std::string seq = moveSequence(std::regex_replace(m[1].str(), std::regex("''"), "'"));
                        add("CREATE SEQUENCE IF NOT EXISTS " + seq);
                        info[i].sequences.emplace_back(seq, name);
                        def = "nextval(" + lit(seq) + "::regclass)";
                    }
                    d += " DEFAULT " + def;
                }
                if (c.contains("nullable") && !c["nullable"].toBool()) d += " NOT NULL";
                defs.push_back(d);
                if (c["comment"].isString())
                    comments.push_back("COMMENT ON COLUMN " + q + "." + qi(name) + " IS " + lit(c["comment"].toString().toStdString()));
            }
            if (auto pk = strings(st["primaryKey"]); !pk.empty()) {
                for (auto& k : pk) k = qi(k);
                defs.push_back("PRIMARY KEY (" + join(pk, ", ") + ")");
            }
            for (const auto& k : st["constraints"].toArray()) // check / unique / exclusion
                defs.push_back("CONSTRAINT " + qi(k.toObject()["name"].toString().toStdString()) + " " +
                               k.toObject()["definition"].toString().toStdString());
            add("CREATE TABLE " + q + " (\n  " + join(defs, ",\n  ") + "\n)");
            if (st["comment"].isString()) add("COMMENT ON TABLE " + q + " IS " + lit(st["comment"].toString().toStdString()));
            for (auto& c : comments) add(c);
        }
    }

    for (size_t i = 0; i < targets.size(); ++i) {
        auto& t = *targets[i].table;
        if (!t.sql.empty()) {
            add(t.sql);
            continue;
        }
        std::vector<size_t> cols; // generated columns compute themselves
        for (size_t c = 0; c < t.data.columns.size(); ++c)
            if (!info[i].generated.count(t.data.columns[c])) cols.push_back(c);
        if (t.data.rows.empty() || cols.empty()) continue;
        std::vector<std::string> names;
        for (size_t c : cols) names.push_back(qi(t.data.columns[c]));
        // OVERRIDING SYSTEM VALUE: keep the ids of GENERATED ALWAYS identity columns (no-op elsewhere)
        std::string head = "INSERT INTO " + qname(targets[i]) + " (" + join(names, ", ") + ") OVERRIDING SYSTEM VALUE VALUES\n";
        constexpr size_t batch = 500;
        for (size_t r = 0; r < t.data.rows.size(); r += batch) {
            std::vector<std::string> rows;
            for (size_t k = r; k < std::min(r + batch, t.data.rows.size()); ++k) {
                std::vector<std::string> vals;
                for (size_t c : cols) {
                    auto& v = t.data.rows[k][c];
                    vals.push_back(v ? lit(*v) : "NULL");
                }
                rows.push_back("(" + join(vals, ", ") + ")");
            }
            add(head + join(rows, ",\n"));
        }
    }

    if (structure) {
        for (size_t i = 0; i < targets.size(); ++i) {
            if (!targets[i].table->sql.empty()) continue;
            auto& st = info[i].st;
            std::string q = qname(targets[i]);
            std::set<std::string> constraintNames;
            for (const auto& k : st["constraints"].toArray()) constraintNames.insert(k.toObject()["name"].toString().toStdString());
            for (const auto& fv : st["foreignKeys"].toArray()) {
                auto f = fv.toObject();
                std::string def = f["definition"].toString().toStdString();
                std::string ref = "REFERENCES " + f["referencesTable"].toString().toStdString() + "(";
                if (auto to = moved.find(f["referencesTable"].toString().toStdString()); to != moved.end())
                    if (size_t at = def.find(ref); at != std::string::npos)
                        def.replace(at, ref.size(), "REFERENCES " + to->second + "(");
                add("ALTER TABLE " + q + " ADD CONSTRAINT " + qi(f["name"].toString().toStdString()) + " " + def);
            }
            for (const auto& iv : st["indexes"].toArray()) {
                auto ix = iv.toObject();
                std::string name = ix["name"].toString().toStdString(), def = ix["definition"].toString().toStdString();
                // PK / unique constraints made their own index; older exports lack "constraint": guess by name
                bool own = ix.contains("constraint") ? ix["constraint"].toBool()
                                                     : constraintNames.count(name) || name.ends_with("_pkey");
                size_t on = def.find(" ON "), index = def.find("INDEX "), using_ = def.find(" USING ", on);
                if (own || on == std::string::npos || using_ == std::string::npos || index > on) continue;
                // "CREATE INDEX x ON public.t USING …": onto the target table, and not twice
                add(def.substr(0, index + 6) + "IF NOT EXISTS " + def.substr(index + 6, on - index - 6) + " ON " + q +
                    def.substr(using_));
            }
        }
    }

    // serial / identity sequences past the imported ids, so the next INSERT doesn't collide
    for (size_t i = 0; i < targets.size(); ++i) {
        if (!targets[i].table->sql.empty() || targets[i].table->data.rows.empty()) continue;
        std::string q = qname(targets[i]);
        for (auto& [seq, col] : info[i].sequences)
            add("SELECT setval(" + lit(seq) + ", COALESCE((SELECT max(" + qi(col) + ") FROM " + q + "), 0) + 1, false)");
        add("DO $dbm$ DECLARE r record; BEGIN\n"
            "FOR r IN SELECT a.attname, pg_get_serial_sequence(" + lit(q) + ", a.attname) AS s FROM pg_attribute a\n"
            "  WHERE a.attrelid = " + lit(q) + "::regclass AND a.attnum > 0 AND NOT a.attisdropped LOOP\n"
            "  IF r.s IS NOT NULL THEN\n"
            "    EXECUTE format('SELECT setval(%L, COALESCE((SELECT max(%I) FROM %s), 0) + 1, false)', r.s, r.attname, " + lit(q) + ");\n"
            "  END IF;\nEND LOOP; END $dbm$");
    }
    return out;
}

}
