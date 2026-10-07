#include "ConnectionStore.h"

#include <QEventLoop>
#include <algorithm>
#include <QSettings>
#include <qt6keychain/keychain.h>

namespace {
const QString kService = QStringLiteral("db-manager");

// Runs a keychain job synchronously; returns false on error.
bool runJob(QKeychain::Job& job) {
    job.setAutoDelete(false);
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
    return job.error() == QKeychain::NoError;
}
}

namespace ConnectionStore {

std::vector<ConnectionConfig> load() {
    QSettings s;
    std::vector<ConnectionConfig> out;
    s.beginGroup("connections");
    for (const QString& id : s.childGroups()) {
        s.beginGroup(id);
        ConnectionConfig c;
        c.id = id.toStdString();
        c.name = s.value("name").toString().toStdString();
        c.type = s.value("type").toString() == "redis" ? DbType::Redis : DbType::PostgreSQL;
        c.host = s.value("host").toString().toStdString();
        c.port = s.value("port").toInt();
        c.database = s.value("database").toString().toStdString();
        c.username = s.value("username").toString().toStdString();
        c.sslMode = s.value("sslMode", "prefer").toString().toStdString();
        c.tls = s.value("tls").toBool();
        out.push_back(std::move(c));
        s.endGroup();
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

void save(const ConnectionConfig& c) {
    QSettings s;
    s.beginGroup("connections/" + QString::fromStdString(c.id));
    s.setValue("name", QString::fromStdString(c.name));
    s.setValue("type", c.type == DbType::Redis ? "redis" : "postgres");
    s.setValue("host", QString::fromStdString(c.host));
    s.setValue("port", c.port);
    s.setValue("database", QString::fromStdString(c.database));
    s.setValue("username", QString::fromStdString(c.username));
    s.setValue("sslMode", QString::fromStdString(c.sslMode));
    s.setValue("tls", c.tls);

    if (c.password.empty()) {
        QKeychain::DeletePasswordJob job(kService);
        job.setKey(QString::fromStdString(c.id));
        runJob(job);
    } else {
        QKeychain::WritePasswordJob job(kService);
        job.setKey(QString::fromStdString(c.id));
        job.setTextData(QString::fromStdString(c.password));
        if (!runJob(job)) throw DbError("Keychain: " + job.errorString().toStdString());
    }
}

void remove(const std::string& id) {
    QSettings().remove("connections/" + QString::fromStdString(id));
    QKeychain::DeletePasswordJob job(kService);
    job.setKey(QString::fromStdString(id));
    runJob(job);
}

std::string password(const std::string& id) {
    QKeychain::ReadPasswordJob job(kService);
    job.setKey(QString::fromStdString(id));
    return runJob(job) ? job.textData().toStdString() : std::string();
}

}
