#pragma once

#include "core/connection/Session.h"
#include "core/database/postgres/PostgreSQL.h"
#include "core/database/redis/Redis.h"

#include <QMainWindow>
#include <map>

class QPersistentModelIndex;
class QStandardItem;
class QStandardItemModel;
class QTabWidget;
class QTreeView;
class QAction;
class QComboBox;
class QToolButton;

class MainWindow : public QMainWindow {
public:
    MainWindow();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void closeTab(int i);

    enum Kind { Connection = 1, PgDatabase, PgSchema, PgRelation, RedisDb, Placeholder };

    QStandardItem* addConnectionItem(const ConnectionConfig& cfg);
    QStandardItem* selectedItem() const;
    void resetChildren(QStandardItem* item);
    void loadChildren(QStandardItem* item);
    void fillChildren(QStandardItem* item, int token, QList<QStandardItem*> children);
    void loadFailed(QStandardItem* item, int token, const QString& msg);
    void activated(QStandardItem* item);
    void enterDatabase(QStandardItem* db);
    void leaveDatabase();

    void newConnection();
    void editConnection();
    void deleteConnection();
    void newSqlEditor();
    void openSqlEditor(const QString& connId, const QString& db, const QString& sql, bool runNow);
    void openRedis(const QString& connId, int db);
    void openErDiagram();
    void backup();
    void exportTable();
    void exportTables(const QString& conn, const QString& db, const QString& schema);
    struct Picked {
        QString ext; // csv, xlsx, json
        bool zip;    // one .zip instead of a file per table in a folder
        std::vector<std::pair<std::string, std::string>> tables;
    };
    Picked pickTables(const std::vector<std::pair<std::string, std::string>>& tables, bool oneSchema);
    void selectAllTables();
    void dropTables();
    void runDrop(const QString& conn, const QString& db, const QPersistentModelIndex& dbIdx,
                 const std::vector<DropTarget>& targets, bool cascade);
    // force: WITH (FORCE), disconnecting other sessions on it (PostgreSQL 13+)
    void dropDatabase(const QString& conn, const QString& db, bool force);

    ConnectionConfig configFor(const QString& connId, const QString& db = {});
    Session<PostgreSQL>& pgSession(const QString& connId, const QString& db);
    Session<Redis>& redisSession(const QString& connId);
    void dropSessions(const QString& connId);

    QStandardItemModel* model_;
    QTreeView* tree_;
    QWidget* dbBar_;       // shown while inside a database: back + switch
    QToolButton* dbBack_;
    QComboBox* dbPick_;
    QAction* dropDb_; // shown in the right-click menu on a database only
    QTabWidget* tabs_;
    int loadSeq_ = 0;
    std::map<QString, ConnectionConfig> configs_;
    std::map<QString, std::string> passwords_; // keychain reads cached for this run
    // explorer sessions keyed "connId/db"; tabs own their own
    std::map<QString, std::unique_ptr<Session<PostgreSQL>>> pgSessions_;
    std::map<QString, std::unique_ptr<Session<Redis>>> redisSessions_;
};
