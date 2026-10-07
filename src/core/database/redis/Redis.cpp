#include "Redis.h"

#include <hiredis/hiredis_ssl.h>
#include <cctype>
#include <mutex>
#include <sstream>

namespace {
// ponytail: value viewer caps at 1000 elements; add paging when people browse giant collections
constexpr size_t kValueLimit = 1000;

std::string str(const redisReply* r) { return r->str ? std::string(r->str, r->len) : std::string(); }
}

Redis::~Redis() { disconnect(); }

void Redis::connect() {
    disconnect();
    timeval connectTimeout{10, 0};
    ctx_ = redisConnectWithTimeout(cfg_.host.c_str(), cfg_.port, connectTimeout);
    if (!ctx_ || ctx_->err) {
        std::string err = ctx_ ? ctx_->errstr : "Cannot allocate redis context";
        disconnect();
        throw DbError(err);
    }
    if (cfg_.tls) {
        static std::once_flag once;
        std::call_once(once, [] { redisInitOpenSSL(); });
        redisSSLContextError sslErr = REDIS_SSL_CTX_NONE;
        // null CA paths = system default trust store, peer verification on
        ssl_ = redisCreateSSLContext(nullptr, nullptr, nullptr, nullptr, cfg_.host.c_str(), &sslErr);
        if (!ssl_) {
            disconnect();
            throw DbError(std::string("TLS: ") + redisSSLContextGetError(sslErr));
        }
        if (redisInitiateSSLWithContext(ctx_, ssl_) != REDIS_OK) {
            std::string err = std::string("TLS: ") + ctx_->errstr;
            disconnect();
            throw DbError(err);
        }
    }
    // ponytail: blocking commands (BLPOP 0, SUBSCRIBE) give up after 30s; no async/cancel yet
    timeval commandTimeout{30, 0};
    redisSetTimeout(ctx_, commandTimeout);

    try {
        if (!cfg_.password.empty()) {
            if (cfg_.username.empty()) command({"AUTH", cfg_.password});
            else command({"AUTH", cfg_.username, cfg_.password});
        }
        select(cfg_.database.empty() ? 0 : std::stoi(cfg_.database));
    } catch (...) {
        disconnect();
        throw;
    }
}

void Redis::disconnect() {
    if (ctx_) redisFree(ctx_);
    if (ssl_) redisFreeSSLContext(ssl_);
    ctx_ = nullptr;
    ssl_ = nullptr;
}

void Redis::ping() { command({"PING"}); }

redisContext* Redis::ctx() {
    if (!ctx_) throw DbError("Not connected");
    return ctx_;
}

Redis::ReplyPtr Redis::command(const std::vector<std::string>& args) {
    auto rs = pipeline({args});
    if (rs[0]->type == REDIS_REPLY_ERROR) throw DbError(str(rs[0].get()));
    return std::move(rs[0]);
}

std::vector<Redis::ReplyPtr> Redis::pipeline(const std::vector<std::vector<std::string>>& cmds) {
    auto* c = ctx();
    for (auto& args : cmds) {
        std::vector<const char*> argv;
        std::vector<size_t> argvlen;
        for (auto& a : args) {
            argv.push_back(a.data());
            argvlen.push_back(a.size());
        }
        redisAppendCommandArgv(c, int(argv.size()), argv.data(), argvlen.data());
    }
    std::vector<ReplyPtr> out;
    for (size_t i = 0; i < cmds.size(); ++i) {
        void* raw = nullptr;
        if (redisGetReply(c, &raw) != REDIS_OK) {
            // context is unusable after an I/O or protocol error
            std::string err = c->errstr;
            disconnect();
            throw DbError(err);
        }
        out.emplace_back(static_cast<redisReply*>(raw));
    }
    return out;
}

void Redis::select(int db) { command({"SELECT", std::to_string(db)}); }

int Redis::databaseCount() {
    try {
        auto r = command({"CONFIG", "GET", "databases"});
        if (r->type == REDIS_REPLY_ARRAY && r->elements == 2) return std::stoi(str(r->element[1]));
    } catch (const std::exception&) {
        // CONFIG is often disabled on managed Redis
    }
    return 16;
}

std::map<int, long long> Redis::keyspace() {
    std::map<int, long long> out;
    std::istringstream in(str(command({"INFO", "keyspace"}).get()));
    std::string line;
    while (std::getline(in, line)) {
        // db0:keys=12,expires=0,avg_ttl=0
        if (line.rfind("db", 0) != 0) continue;
        auto colon = line.find(':'), keys = line.find("keys=");
        if (colon == std::string::npos || keys == std::string::npos) continue;
        out[std::stoi(line.substr(2, colon - 2))] = std::stoll(line.substr(keys + 5));
    }
    return out;
}

std::pair<std::string, std::vector<Redis::KeyInfo>> Redis::scan(const std::string& cursor,
                                                                 const std::string& pattern, int count) {
    auto r = command({"SCAN", cursor, "MATCH", pattern.empty() ? "*" : pattern, "COUNT",
                      std::to_string(count)});
    std::vector<std::string> keys;
    for (size_t i = 0; i < r->element[1]->elements; ++i) keys.push_back(str(r->element[1]->element[i]));
    return {str(r->element[0]), describe(keys)};
}

std::vector<Redis::KeyInfo> Redis::describe(const std::vector<std::string>& keys) {
    std::vector<std::vector<std::string>> cmds;
    for (auto& k : keys) {
        cmds.push_back({"TYPE", k});
        cmds.push_back({"TTL", k});
    }
    auto meta = pipeline(cmds);

    static const std::map<std::string, std::string> sizeCmd = {
        {"string", "STRLEN"}, {"hash", "HLEN"}, {"list", "LLEN"},
        {"set", "SCARD"}, {"zset", "ZCARD"}, {"stream", "XLEN"}};
    std::vector<KeyInfo> infos;
    cmds.clear();
    for (size_t i = 0; i < keys.size(); ++i) {
        KeyInfo k{keys[i], str(meta[i * 2].get()), meta[i * 2 + 1]->integer, 0};
        if (k.type == "none") continue; // expired / deleted meanwhile
        auto it = sizeCmd.find(k.type);
        cmds.push_back({it != sizeCmd.end() ? it->second : "EXISTS", k.key});
        infos.push_back(std::move(k));
    }
    auto sizes = pipeline(cmds);
    for (size_t i = 0; i < infos.size(); ++i)
        if (sizes[i]->type == REDIS_REPLY_INTEGER) infos[i].size = sizes[i]->integer;
    return infos;
}

ResultSet Redis::value(const std::string& key, const std::string& type) {
    ResultSet rs;
    auto add = [&](std::initializer_list<std::string> cells) {
        auto& row = rs.rows.emplace_back();
        for (auto& c : cells) row.emplace_back(c);
    };
    // HSCAN / SSCAN until done or limit, so huge collections don't block the server
    auto scanAll = [&](const char* cmd, bool pairs) {
        std::string cur = "0";
        do {
            auto r = command({cmd, key, cur, "COUNT", "500"});
            cur = str(r->element[0]);
            auto* items = r->element[1];
            for (size_t i = 0; i < items->elements; i += pairs ? 2 : 1) {
                if (pairs) add({str(items->element[i]), str(items->element[i + 1])});
                else add({str(items->element[i])});
            }
        } while (cur != "0" && rs.rows.size() < kValueLimit);
    };
    auto limit = std::to_string(kValueLimit - 1);

    if (type == "string") {
        rs.columns = {"value"};
        auto r = command({"GET", key});
        if (r->type != REDIS_REPLY_NIL) add({str(r.get())});
    } else if (type == "hash") {
        rs.columns = {"field", "value"};
        scanAll("HSCAN", true);
    } else if (type == "set") {
        rs.columns = {"member"};
        scanAll("SSCAN", false);
    } else if (type == "list") {
        rs.columns = {"index", "value"};
        auto r = command({"LRANGE", key, "0", limit});
        for (size_t i = 0; i < r->elements; ++i) add({std::to_string(i), str(r->element[i])});
    } else if (type == "zset") {
        rs.columns = {"member", "score"};
        auto r = command({"ZRANGE", key, "0", limit, "WITHSCORES"});
        for (size_t i = 0; i + 1 < r->elements; i += 2)
            add({str(r->element[i]), str(r->element[i + 1])});
    } else if (type == "stream") {
        rs.columns = {"id", "fields"};
        auto r = command({"XRANGE", key, "-", "+", "COUNT", std::to_string(kValueLimit)});
        for (size_t i = 0; i < r->elements; ++i) {
            auto* entry = r->element[i];
            std::string fields;
            auto* kv = entry->element[1];
            for (size_t j = 0; j + 1 < kv->elements; j += 2)
                fields += (j ? ", " : "") + str(kv->element[j]) + "=" + str(kv->element[j + 1]);
            add({str(entry->element[0]), fields});
        }
    } else {
        throw DbError("Unsupported type: " + type);
    }
    rs.status = std::to_string(rs.rows.size()) + " rows" +
                (rs.rows.size() >= kValueLimit ? " (truncated)" : "");
    return rs;
}

std::string Redis::execute(const std::vector<std::string>& args) {
    if (args.empty()) return {};
    auto rs = pipeline({args});
    return format(rs[0].get());
}

std::string Redis::call(const std::vector<std::string>& args) { return format(command(args).get()); }

std::vector<std::string> Redis::tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inToken = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else if (quote == '"' && c == '\\' && i + 1 < line.size()) {
                char n = line[++i];
                cur += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
            } else {
                cur += c;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
            inToken = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (inToken) out.push_back(std::move(cur));
            cur.clear();
            inToken = false;
        } else {
            cur += c;
            inToken = true;
        }
    }
    if (quote) throw DbError("Unbalanced quotes");
    if (inToken) out.push_back(std::move(cur));
    return out;
}

std::string Redis::format(const redisReply* r, const std::string& indent) {
    switch (r->type) {
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_VERB: return "\"" + str(r) + "\"";
    case REDIS_REPLY_STATUS: return str(r);
    case REDIS_REPLY_ERROR: return "(error) " + str(r);
    case REDIS_REPLY_INTEGER: return "(integer) " + std::to_string(r->integer);
    case REDIS_REPLY_NIL: return "(nil)";
    case REDIS_REPLY_DOUBLE: return "(double) " + str(r);
    case REDIS_REPLY_BOOL: return r->integer ? "(true)" : "(false)";
    case REDIS_REPLY_BIGNUM: return "(big number) " + str(r);
    default: { // ARRAY, SET, MAP, PUSH, ATTR
        if (r->elements == 0) return "(empty array)";
        std::string out;
        for (size_t i = 0; i < r->elements; ++i) {
            std::string prefix = std::to_string(i + 1) + ") ";
            if (i) out += "\n" + indent;
            out += prefix + format(r->element[i], indent + std::string(prefix.size(), ' '));
        }
        return out;
    }
    }
}
