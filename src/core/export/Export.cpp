#include "Export.h"

#include "core/database/postgres/PostgreSQL.h"

#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Export {
namespace {

std::string quoted(const std::string& s, char q) {
    std::string out(1, q);
    for (char c : s) out += c == q ? std::string(2, q) : std::string(1, c);
    return out + q;
}

std::string csvField(const std::string& s) {
    return s.empty() || s.find_first_of(",\"\r\n") != std::string::npos ? quoted(s, '"') : s;
}

std::string xmlText(const std::string& s) {
    std::string out;
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c >= 0x20 || c == '\t' || c == '\n' || c == '\r') out += ch; // other control chars are invalid XML
    }
    return out;
}

std::string columnName(size_t c) { // 0 -> A, 26 -> AA
    std::string s;
    for (++c; c; c = (c - 1) / 26) s.insert(s.begin(), char('A' + (c - 1) % 26));
    return s;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') out += '\\', out += ch;
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += ch;
    }
    return out + '"';
}

// Number / bool / json kind per column from PostgreSQL type OIDs: 'n' number, 'b' bool, 'j' json, 's' text
std::vector<char> columnKinds(const ResultSet& rs) {
    std::vector<char> kind(rs.columns.size(), 's');
    for (size_t c = 0; c < rs.columnType.size() && c < kind.size(); ++c) {
        unsigned t = rs.columnType[c]; // int2, int4, int8, float4, float8, numeric / bool / json, jsonb
        if (t == 21 || t == 23 || t == 20 || t == 700 || t == 701 || t == 1700) kind[c] = 'n';
        else if (t == 16) kind[c] = 'b';
        else if (t == 114 || t == 3802) kind[c] = 'j';
    }
    return kind;
}

// Safe as a spreadsheet / JSON number: finite, and no more than 15 digits (doubles round beyond that,
// e.g. bigint ids)
bool plainNumber(const std::string& v) {
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    size_t digits = 0;
    for (char ch : v) digits += std::isdigit(static_cast<unsigned char>(ch)) != 0;
    return !v.empty() && *end == '\0' && std::isfinite(d) && digits <= 15;
}

void put16(std::string& out, unsigned v) { out += char(v & 0xff), out += char(v >> 8 & 0xff); }
void put32(std::string& out, unsigned long v) { put16(out, v & 0xffff), put16(out, v >> 16 & 0xffff); }


// ",\n<indent>{...}" per row, typed as described for json()
std::string jsonRows(const ResultSet& rs, const std::string& indent) {
    auto kind = columnKinds(rs);
    std::vector<std::string> keys;
    for (auto& c : rs.columns) keys.push_back(jsonString(c) + ": ");
    std::string out;
    for (size_t r = 0; r < rs.rows.size(); ++r) {
        out += (r ? ",\n" : "\n") + indent + "{";
        for (size_t c = 0; c < rs.rows[r].size() && c < keys.size(); ++c) {
            auto& v = rs.rows[r][c];
            out += (c ? ", " : "") + keys[c];
            if (!v) out += "null";
            else if (kind[c] == 'n' && plainNumber(*v)) out += *v;
            else if (kind[c] == 'b' && (*v == "t" || *v == "f")) out += *v == "t" ? "true" : "false";
            else if (kind[c] == 'j') out += *v; // PostgreSQL only returns valid JSON for these
            else out += jsonString(*v);
        }
        out += "}";
    }
    return out;
}

} // namespace

// ponytail: in-memory, no zip64 (4 GB cap); fine for Excel files and table exports
std::string zip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, dir;
    for (auto& [name, data] : files) {
        z_stream z{};
        deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY); // raw deflate
        std::string packed(deflateBound(&z, uLong(data.size())), '\0');
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        z.avail_in = uInt(data.size());
        z.next_out = reinterpret_cast<Bytef*>(packed.data());
        z.avail_out = uInt(packed.size());
        deflate(&z, Z_FINISH);
        packed.resize(z.total_out);
        deflateEnd(&z);
        unsigned long crc = crc32(0, reinterpret_cast<const Bytef*>(data.data()), uInt(data.size()));

        auto common = [&](std::string& h) { // version, flags (UTF-8 names), method, time, date, crc, sizes, name length
            put16(h, 20), put16(h, 0x800), put16(h, 8), put16(h, 0), put16(h, 0x21);
            put32(h, crc), put32(h, packed.size()), put32(h, data.size()), put16(h, unsigned(name.size()));
        };
        put32(dir, 0x02014b50), put16(dir, 20), common(dir);
        put16(dir, 0), put16(dir, 0), put16(dir, 0), put16(dir, 0), put32(dir, 0), put32(dir, out.size());
        dir += name;
        put32(out, 0x04034b50), common(out), put16(out, 0);
        out += name + packed;
    }
    size_t dirAt = out.size();
    out += dir;
    put32(out, 0x06054b50), put16(out, 0), put16(out, 0), put16(out, unsigned(files.size())),
        put16(out, unsigned(files.size())), put32(out, dir.size()), put32(out, dirAt), put16(out, 0);
    return out;
}

std::string csv(const ResultSet& rs) {
    std::string out;
    for (size_t c = 0; c < rs.columns.size(); ++c) out += (c ? "," : "") + csvField(rs.columns[c]);
    out += "\r\n";
    for (auto& row : rs.rows) {
        for (size_t c = 0; c < row.size(); ++c) out += (c ? "," : "") + (row[c] ? csvField(*row[c]) : "");
        out += "\r\n";
    }
    return out;
}

std::string inserts(const ResultSet& rs, const std::string& table) {
    std::string cols;
    for (auto& c : rs.columns) cols += (cols.empty() ? "" : ", ") + PostgreSQL::quoteIdent(c);
    std::string out;
    for (auto& row : rs.rows) {
        std::string vals;
        for (auto& v : row) vals += (vals.empty() ? "" : ", ") + (v ? quoted(*v, '\'') : "NULL");
        out += "INSERT INTO " + table + " (" + cols + ") VALUES (" + vals + ");\n";
    }
    return out;
}

std::string xlsx(const ResultSet& rs, const std::string& sheetName) {
    if (rs.rows.size() + 1 > 1048576 || rs.columns.size() > 16384)
        throw DbError("Too big for Excel (max 1,048,575 rows and 16,384 columns); export as CSV instead");

    auto kind = columnKinds(rs);

    std::string sheet = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
                        R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
                        R"(<sheetViews><sheetView workbookViewId="0"><pane ySplit="1" topLeftCell="A2" )"
                        R"(activePane="bottomLeft" state="frozen"/></sheetView></sheetViews><sheetData>)";
    auto text = [](const std::string& ref, const std::string& v, const char* style) {
        return "<c r=\"" + ref + "\" t=\"inlineStr\"" + style + "><is><t xml:space=\"preserve\">" + xmlText(v) +
               "</t></is></c>";
    };
    sheet += "<row r=\"1\">";
    for (size_t c = 0; c < rs.columns.size(); ++c) sheet += text(columnName(c) + "1", rs.columns[c], " s=\"1\"");
    sheet += "</row>";
    for (size_t r = 0; r < rs.rows.size(); ++r) {
        std::string row = std::to_string(r + 2);
        sheet += "<row r=\"" + row + "\">";
        for (size_t c = 0; c < rs.rows[r].size(); ++c) {
            auto& v = rs.rows[r][c];
            if (!v) continue; // NULL = empty cell
            std::string ref = columnName(c) + row;
            if (c < kind.size() && kind[c] == 'n' && plainNumber(*v))
                sheet += "<c r=\"" + ref + "\"><v>" + *v + "</v></c>";
            else if (c < kind.size() && kind[c] == 'b' && (*v == "t" || *v == "f"))
                sheet += "<c r=\"" + ref + "\" t=\"b\"><v>" + (*v == "t" ? "1" : "0") + "</v></c>";
            else
                sheet += text(ref, *v, "");
        }
        sheet += "</row>";
    }
    sheet += "</sheetData></worksheet>";

    std::string name; // Excel: max 31 chars, none of []:*?/\ (cut on a UTF-8 boundary)
    for (char ch : sheetName)
        if (ch && std::string("[]:*?/\\").find(ch) == std::string::npos) name += ch;
    while (name.size() > 31) {
        name.pop_back();
        while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back();
        if (!name.empty() && static_cast<unsigned char>(name.back()) >= 0xC0) name.pop_back();
    }
    if (name.empty()) name = "Sheet1";

    const std::string ns = "http://schemas.openxmlformats.org/";
    return zip({
        {"[Content_Types].xml",
         R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
         "<Types xmlns=\"" + ns + "package/2006/content-types\">"
         R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
         R"(<Default Extension="xml" ContentType="application/xml"/>)"
         R"(<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>)"
         R"(<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>)"
         R"(<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>)"
         "</Types>"},
        {"_rels/.rels",
         R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
         "<Relationships xmlns=\"" + ns + "package/2006/relationships\">"
         "<Relationship Id=\"rId1\" Type=\"" + ns + "officeDocument/2006/relationships/officeDocument\" "
         "Target=\"xl/workbook.xml\"/></Relationships>"},
        {"xl/workbook.xml",
         R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
         "<workbook xmlns=\"" + ns + "spreadsheetml/2006/main\" xmlns:r=\"" + ns +
         "officeDocument/2006/relationships\"><sheets><sheet name=\"" + xmlText(name) +
         "\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
        {"xl/_rels/workbook.xml.rels",
         R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
         "<Relationships xmlns=\"" + ns + "package/2006/relationships\">"
         "<Relationship Id=\"rId1\" Type=\"" + ns + "officeDocument/2006/relationships/worksheet\" "
         "Target=\"worksheets/sheet1.xml\"/>"
         "<Relationship Id=\"rId2\" Type=\"" + ns + "officeDocument/2006/relationships/styles\" "
         "Target=\"styles.xml\"/></Relationships>"},
        {"xl/styles.xml", // style 1 = bold (header)
         R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
         "<styleSheet xmlns=\"" + ns + "spreadsheetml/2006/main\">"
         R"(<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font>)"
         R"(<font><b/><sz val="11"/><name val="Calibri"/></font></fonts>)"
         R"(<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>)"
         R"(<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>)"
         R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)"
         R"(<cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>)"
         R"(<xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/></cellXfs>)"
         "</styleSheet>"},
        {"xl/worksheets/sheet1.xml", sheet},
    });
}

std::string json(const ResultSet& rs, const std::string& structureJson) {
    std::string out = "{\n  \"structure\": ";
    if (structureJson.empty()) {
        out += "{\"columns\": [";
        for (size_t c = 0; c < rs.columns.size(); ++c)
            out += (c ? ", " : "") + std::string("{\"name\": ") + jsonString(rs.columns[c]) + "}";
        out += "]}";
    } else {
        out += structureJson;
    }
    out += ",\n  \"rows\": [" + jsonRows(rs, "    ");
    out += rs.rows.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

std::string jsonArray(const ResultSet& rs) {
    return "[" + jsonRows(rs, "  ") + (rs.rows.empty() ? "]\n" : "\n]\n");
}

std::string markdown(const ResultSet& rs) {
    auto cell = [](const std::string& s) {
        std::string out;
        for (char ch : s) out += ch == '|' ? "\\|" : ch == '\n' ? "<br>" : ch == '\r' ? "" : std::string(1, ch);
        return out;
    };
    std::string out = "|", rule = "|";
    for (auto& c : rs.columns) out += " " + cell(c) + " |", rule += " --- |";
    out += "\n" + rule + "\n";
    for (auto& row : rs.rows) {
        out += "|";
        for (auto& v : row) out += " " + (v ? cell(*v) : "NULL") + " |";
        out += "\n";
    }
    return out;
}

}
