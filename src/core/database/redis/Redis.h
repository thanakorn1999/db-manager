#pragma once

#include "core/database/Database.h"

#include <hiredis/hiredis.h>
#include <map>
#include <memory>

struct redisSSLContext;

class Redis : public Database {
public:
    struct KeyInfo {
        std::string key;
        std::string type;
        long long ttl = -1;  // seconds, -1 = no expiry
        long long size = 0;  // STRLEN / HLEN / LLEN / SCARD / ZCARD / XLEN
    };

    explicit Redis(ConnectionConfig cfg) : cfg_(std::move(cfg)) {}
    ~Redis() override;

    void connect() override;
    void disconnect() override;
    void ping() override;
    bool connected() const override { return ctx_ != nullptr; }
    Capabilities capabilities() const override {
        return Capability::Keys | Capability::Ttl | Capability::ServerInfo;
    }

    void select(int db);
    int databaseCount();
    std::map<int, long long> keyspace(); // db index -> key count (INFO keyspace)

    // One SCAN step. Returns the next cursor ("0" when done) and the keys with type/TTL/size.
    std::pair<std::string, std::vector<KeyInfo>> scan(const std::string& cursor,
                                                       const std::string& pattern, int count);
    // TYPE / TTL / size for each key; keys that no longer exist are dropped.
    std::vector<KeyInfo> describe(const std::vector<std::string>& keys);
    ResultSet value(const std::string& key, const std::string& type);

    // Runs one command and formats the reply like redis-cli.
    std::string execute(const std::vector<std::string>& args);
    // Same, but an error reply throws DbError.
    std::string call(const std::vector<std::string>& args);

    // Splits a command line into args, honoring "double" (with \ escapes) and 'single' quotes.
    static std::vector<std::string> tokenize(const std::string& line);
    static std::string format(const redisReply* r, const std::string& indent = "");

private:
    struct ReplyDeleter { void operator()(redisReply* r) const { freeReplyObject(r); } };
    using ReplyPtr = std::unique_ptr<redisReply, ReplyDeleter>;

    redisContext* ctx();
    ReplyPtr command(const std::vector<std::string>& args);
    std::vector<ReplyPtr> pipeline(const std::vector<std::vector<std::string>>& cmds);

    ConnectionConfig cfg_;
    redisContext* ctx_ = nullptr;
    redisSSLContext* ssl_ = nullptr;
};
