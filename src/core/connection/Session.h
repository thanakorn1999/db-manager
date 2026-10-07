#pragma once

#include "core/database/Database.h"

#include <QObject>
#include <QString>
#include <QThread>
#include <functional>
#include <memory>
#include <type_traits>

// One database connection living on its own worker thread.
// Own it as a member of the ctx object passed to run(): it joins the thread on destruction,
// so no callback can fire after ctx is gone.
template <class Db>
class Session {
public:
    explicit Session(ConnectionConfig cfg) : db_(std::make_unique<Db>(std::move(cfg))) {
        worker_.moveToThread(&thread_);
        thread_.start();
    }
    ~Session() {
        db_->cancel();
        thread_.quit();
        thread_.wait();
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Runs f(db) on the worker thread (connecting first if needed), then ok(result) or
    // fail(message) on ctx's thread.
    template <class F, class Ok>
    void run(QObject* ctx, F f, Ok ok, std::function<void(const QString&)> fail) {
        QMetaObject::invokeMethod(&worker_, [db = db_.get(), ctx, f = std::move(f), ok = std::move(ok),
                                             fail = std::move(fail)]() mutable {
            try {
                if (!db->connected()) db->connect();
                if constexpr (std::is_void_v<std::invoke_result_t<F, Db&>>) {
                    f(*db);
                    QMetaObject::invokeMethod(ctx, [ok] { ok(); }, Qt::QueuedConnection);
                } else {
                    auto r = f(*db);
                    QMetaObject::invokeMethod(ctx, [ok, r = std::move(r)] { ok(r); }, Qt::QueuedConnection);
                }
            } catch (const std::exception& e) {
                QMetaObject::invokeMethod(ctx, [fail, m = QString::fromUtf8(e.what())] { fail(m); },
                                          Qt::QueuedConnection);
            }
        }, Qt::QueuedConnection);
    }

    // Thread-safe: aborts the running query (PostgreSQL only).
    void cancel() { db_->cancel(); }

private:
    std::unique_ptr<Db> db_;
    QThread thread_;
    QObject worker_;
};
