#include "MainWindow.h"

#include "core/export/Export.h"
#include "core/query/SqlCompleter.h"
#include "ui/Settings.h"
#include "ui/TableIcons.h"
#include "ui/connection/ConnectionDialog.h"
#include "ui/connection/ConnectionStore.h"
#include "ui/er-diagram/ErDiagram.h"
#include "ui/ResultModel.h"
#include "ui/redis/RedisTab.h"
#include "ui/sql-editor/SqlEditorTab.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QProcess>
#include <QStandardPaths>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMessageBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>

namespace {
enum Role { RoleKind = Qt::UserRole + 1, RoleConn, RoleDb, RoleSchema, RoleName, RoleToken };

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString defaultDb(const ConnectionConfig& c) {
    if (!c.database.empty()) return qs(c.database);
    return c.type == DbType::Redis ? "0" : "postgres";
}

QStandardItem* makeItem(const QString& text, int kind, const QString& conn, const QString& db = {},
                        const QString& schema = {}, const QString& name = {}) {
    auto* item = new QStandardItem(text);
    item->setEditable(false);
    item->setData(kind, RoleKind);
    item->setData(conn, RoleConn);
    item->setData(db, RoleDb);
    item->setData(schema, RoleSchema);
    item->setData(name, RoleName);
    return item;
}
}

MainWindow::MainWindow() {
    setWindowTitle("DB Manager");

    model_ = new QStandardItemModel(this);
    tree_ = new QTreeView;
    tree_->setModel(model_);
    tree_->setHeaderHidden(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection); // ⇧ / ⌘-click tables to drop several
    // custom menu: right-click must first make the clicked row current (macOS doesn't),
    // or the actions would hit whatever was selected before; a click inside the selection keeps it
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (auto idx = tree_->indexAt(pos); idx.isValid()) {
            if (tree_->selectionModel()->isSelected(idx))
                tree_->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
            else
                tree_->setCurrentIndex(idx);
        }
        auto* item = selectedItem();
        dropDb_->setVisible(item && item->data(RoleKind).toInt() == PgDatabase);
        QMenu::exec(tree_->actions(), tree_->viewport()->mapToGlobal(pos), nullptr, tree_);
    });

    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    tabs_->setDocumentMode(true);
    tabs_->setMovable(true);

    auto* splitter = new QSplitter;
    // inside a database the tree shows only it, like opening a folder; this bar goes back / switches
    dbBack_ = new QToolButton;
    dbBack_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    dbBack_->setAutoRaise(true);
    dbBack_->setShortcut(QKeySequence::Back); // ⌘[
    dbPick_ = new QComboBox;
    dbPick_->setToolTip("Switch database");
    // long names elide instead of widening the explorer
    dbPick_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    dbPick_->setMinimumContentsLength(8);
    dbBar_ = new QWidget;
    auto* bar = new QHBoxLayout(dbBar_);
    bar->setContentsMargins(4, 4, 4, 4);
    bar->addWidget(dbBack_);
    bar->addWidget(dbPick_, 1);
    dbBar_->hide();
    connect(dbBack_, &QToolButton::clicked, this, &MainWindow::leaveDatabase);
    connect(dbPick_, &QComboBox::activated, this, [this](int i) {
        auto* conn = model_->itemFromIndex(tree_->rootIndex())->parent();
        enterDatabase(conn->child(dbPick_->itemData(i).toInt()));
    });
    // database row gone (refresh / disconnect / delete): the view falls back to the full tree
    connect(model_, &QAbstractItemModel::rowsRemoved, this, [this] {
        if (!tree_->rootIndex().isValid()) dbBar_->hide();
    });
    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);
    leftLayout->addWidget(dbBar_);
    leftLayout->addWidget(tree_);
    splitter->addWidget(left);
    splitter->addWidget(tabs_);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 920});
    setCentralWidget(splitter);

    auto* toolbar = addToolBar("Main");
    toolbar->setMovable(false);
    auto* newConn = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogNewFolder), "New Connection");
    auto* editConn = toolbar->addAction("Edit");
    auto* delConn = toolbar->addAction("Delete");
    toolbar->addSeparator();
    auto* sql = toolbar->addAction("SQL Editor");
    auto* er = new QAction("ER Diagram", this); // right-click menu only
    auto* backupAct = new QAction("Backup (pg_dump)…", this);
    auto* exportAct = new QAction("Export…", this);
    auto* refresh = toolbar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), "Refresh");
    auto* disconnect = toolbar->addAction("Disconnect");
    toolbar->addSeparator();
    auto* settings = toolbar->addAction("Settings");
    settings->setShortcut(QKeySequence::Preferences); // ⌘,
    settings->setMenuRole(QAction::PreferencesRole);
    connect(settings, &QAction::triggered, this, [this] { Settings::showDialog(this); });
    newConn->setShortcut(QKeySequence::New);
    sql->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    refresh->setShortcut(QKeySequence::Refresh);
    refresh->setShortcutContext(Qt::WidgetShortcut); // tree only: ⌘R in a SQL tab re-runs its query
    auto* setIcon = new QAction("Set Icon…", this);
    connect(setIcon, &QAction::triggered, this, [this] {
        auto* item = selectedItem();
        if (!item || item->data(RoleKind).toInt() != PgRelation) {
            statusBar()->showMessage("Select a table to set its icon", 3000);
            return;
        }
        QString schema = item->data(RoleSchema).toString(), table = item->data(RoleName).toString();
        if (!TableIcons::pick(this, schema, table)) return;
        // same table may be open under several connections / databases
        std::function<void(QStandardItem*)> update = [&](QStandardItem* it) {
            for (int r = 0; r < it->rowCount(); ++r) {
                auto* c = it->child(r);
                if (c->data(RoleKind).toInt() == PgRelation && c->data(RoleSchema) == schema && c->data(RoleName) == table)
                    c->setIcon(TableIcons::icon(schema, table));
                update(c);
            }
        };
        update(model_->invisibleRootItem());
    });
    auto* selectAll = new QAction("Select All Tables", this);
    selectAll->setShortcut(QKeySequence::SelectAll);
    selectAll->setShortcutContext(Qt::WidgetShortcut);
    connect(selectAll, &QAction::triggered, this, &MainWindow::selectAllTables);
    auto* dropAct = new QAction("Drop Tables…", this);
    dropAct->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Backspace), QKeySequence::Delete});
    dropAct->setShortcutContext(Qt::WidgetShortcut);
    connect(dropAct, &QAction::triggered, this, &MainWindow::dropTables);
    dropDb_ = new QAction("Drop Database…", this);
    connect(dropDb_, &QAction::triggered, this, [this] {
        if (auto* item = selectedItem(); item && item->data(RoleKind).toInt() == PgDatabase)
            dropDatabase(item->data(RoleConn).toString(), item->data(RoleDb).toString(), false);
    });
    delConn->setText("Delete Connection"); // the menu runs on any row; say what it deletes
    delConn->setIconText("Delete");
    tree_->addActions({sql, er, backupAct, exportAct, setIcon, selectAll, dropAct, dropDb_, refresh, disconnect,
                       editConn, delConn});

    connect(newConn, &QAction::triggered, this, &MainWindow::newConnection);
    connect(editConn, &QAction::triggered, this, &MainWindow::editConnection);
    connect(delConn, &QAction::triggered, this, &MainWindow::deleteConnection);
    connect(sql, &QAction::triggered, this, &MainWindow::newSqlEditor);
    connect(er, &QAction::triggered, this, &MainWindow::openErDiagram);
    connect(backupAct, &QAction::triggered, this, &MainWindow::backup);
    connect(exportAct, &QAction::triggered, this, &MainWindow::exportTable);
    connect(refresh, &QAction::triggered, this, [this] {
        if (auto* item = selectedItem(); item && item->hasChildren()) {
            resetChildren(item);
            if (tree_->isExpanded(item->index())) loadChildren(item);
        }
    });
    connect(disconnect, &QAction::triggered, this, [this] {
        auto* item = selectedItem();
        if (!item) return;
        while (item->parent()) item = item->parent();
        dropSessions(item->data(RoleConn).toString());
        tree_->collapse(item->index());
        resetChildren(item);
        statusBar()->showMessage("Disconnected " + item->text(), 3000);
    });
    connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& idx) {
        auto* item = model_->itemFromIndex(idx);
        if (item->data(RoleKind).toInt() == PgDatabase) enterDatabase(item);
        loadChildren(item);
    });
    connect(tree_, &QTreeView::doubleClicked, this,
            [this](const QModelIndex& idx) { activated(model_->itemFromIndex(idx)); });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);

    // tab hotkeys (Qt maps Ctrl to ⌘ on macOS)
    auto shortcut = [this](QList<QKeySequence> keys, auto fn) {
        auto* a = new QAction(this);
        a->setShortcuts(keys);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
    };
    shortcut(QKeySequence::keyBindings(QKeySequence::Close), [this] {
        if (tabs_->count()) closeTab(tabs_->currentIndex());
    });
    auto step = [this](int d) {
        if (int n = tabs_->count()) tabs_->setCurrentIndex((tabs_->currentIndex() + d + n) % n);
    };
    shortcut(QKeySequence::keyBindings(QKeySequence::NextChild) +
                 QList<QKeySequence>{QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight),
                                     QKeySequence(Qt::META | Qt::Key_Tab)}, // ⌃⇥ (⌘⇥ is taken by macOS)
             [step] { step(1); });
    shortcut(QKeySequence::keyBindings(QKeySequence::PreviousChild) +
                 QList<QKeySequence>{QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft),
                                     QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Backtab)},
             [step] { step(-1); });
    for (int i = 1; i <= 9; ++i) // ⌘9 = last tab, like browsers
        shortcut({QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_0 + i))},
                 [this, i] { tabs_->setCurrentIndex(i == 9 ? tabs_->count() - 1 : i - 1); });

    for (auto& cfg : ConnectionStore::load()) addConnectionItem(cfg);
    statusBar()->showMessage(model_->rowCount() ? "Ready" : "Create a connection to get started (⌘N)");
}

void MainWindow::closeTab(int i) {
    auto* sql = dynamic_cast<SqlEditorTab*>(tabs_->widget(i));
    if (sql && !sql->confirmDiscard()) return;
    delete tabs_->widget(i);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* sql = dynamic_cast<SqlEditorTab*>(tabs_->widget(i));
        if (sql && !sql->confirmDiscard()) {
            tabs_->setCurrentIndex(i);
            e->ignore();
            return;
        }
    }
    e->accept();
}

QStandardItem* MainWindow::addConnectionItem(const ConnectionConfig& cfg) {
    QString id = qs(cfg.id);
    configs_[id] = cfg;
    auto* item = makeItem(qs(cfg.name), Connection, id);
    item->setIcon(style()->standardIcon(QStyle::SP_DriveNetIcon));
    item->setToolTip(QString("%1 · %2:%3").arg(cfg.type == DbType::Redis ? "Redis" : "PostgreSQL",
                                               qs(cfg.host)).arg(cfg.port));
    model_->appendRow(item);
    resetChildren(item);
    return item;
}

QStandardItem* MainWindow::selectedItem() const { return model_->itemFromIndex(tree_->currentIndex()); }

// Replaces children with a single placeholder; loadChildren() fills it on expand.
void MainWindow::resetChildren(QStandardItem* item) {
    item->removeRows(0, item->rowCount());
    auto* ph = makeItem("Loading…", Placeholder, {});
    ph->setEnabled(false);
    ph->setData(0, RoleToken);
    item->appendRow(ph);
}

void MainWindow::loadChildren(QStandardItem* item) {
    if (!item || item->rowCount() != 1) return;
    auto* ph = item->child(0);
    if (ph->data(RoleKind).toInt() != Placeholder || ph->data(RoleToken).toInt() != 0) return;
    int token = ++loadSeq_;
    ph->setData(token, RoleToken);
    ph->setText("Loading…");

    QString conn = item->data(RoleConn).toString();
    QString db = item->data(RoleDb).toString();
    QString schema = item->data(RoleSchema).toString();
    // item pointers may die while a query runs (delete/refresh); resolve through a persistent index
    QPersistentModelIndex idx(item->index());
    auto fail = [this, idx, token](const QString& msg) {
        if (idx.isValid()) loadFailed(model_->itemFromIndex(idx), token, msg);
    };
    auto expandable = [this](QStandardItem* it) {
        resetChildren(it);
        return it;
    };

    const auto& cfg = configs_.at(conn);
    switch (item->data(RoleKind).toInt()) {
    case Connection:
        if (cfg.type == DbType::Redis) {
            redisSession(conn).run(
                this, [](Redis& r) { return std::pair{r.databaseCount(), r.keyspace()}; },
                [=, this](const std::pair<int, std::map<int, long long>>& res) {
                    if (!idx.isValid()) return;
                    QList<QStandardItem*> children;
                    for (int i = 0; i < res.first; ++i) {
                        auto it = res.second.find(i);
                        QString text = QString("DB %1").arg(i);
                        if (it != res.second.end()) text += QString("  (%1 keys)").arg(it->second);
                        auto* c = makeItem(text, RedisDb, conn, QString::number(i));
                        c->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
                        children << c;
                    }
                    fillChildren(model_->itemFromIndex(idx), token, children);
                },
                fail);
        } else {
            pgSession(conn, defaultDb(cfg)).run(
                this, [](PostgreSQL& pg) { return pg.databases(); },
                [=, this](const std::vector<std::string>& dbs) {
                    if (!idx.isValid()) return;
                    QList<QStandardItem*> children;
                    for (auto& d : dbs) {
                        auto* c = expandable(makeItem(qs(d), PgDatabase, conn, qs(d)));
                        c->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
                        children << c;
                    }
                    fillChildren(model_->itemFromIndex(idx), token, children);
                },
                fail);
        }
        break;
    case PgDatabase:
        pgSession(conn, db).run(
            this, [](PostgreSQL& pg) { return pg.schemas(); },
            [=, this](const std::vector<std::string>& schemas) {
                if (!idx.isValid()) return;
                QList<QStandardItem*> children;
                for (auto& s : schemas) {
                    auto* c = expandable(makeItem(qs(s), PgSchema, conn, db, qs(s)));
                    c->setIcon(style()->standardIcon(QStyle::SP_DirClosedIcon));
                    children << c;
                }
                fillChildren(model_->itemFromIndex(idx), token, children);
            },
            fail);
        break;
    case PgSchema:
        pgSession(conn, db).run(
            this, [s = schema.toStdString()](PostgreSQL& pg) { return pg.relations(s); },
            [=, this](const std::vector<std::pair<std::string, char>>& rels) {
                if (!idx.isValid()) return;
                static const std::map<char, QString> suffix = {
                    {'v', " (view)"}, {'m', " (materialized view)"}, {'f', " (foreign)"}};
                QList<QStandardItem*> children;
                for (auto& [name, kind] : rels) {
                    auto it = suffix.find(kind);
                    auto* c = makeItem(qs(name) + (it != suffix.end() ? it->second : QString()), PgRelation,
                                       conn, db, schema, qs(name));
                    c->setIcon(TableIcons::icon(schema, qs(name)));
                    children << c;
                }
                fillChildren(model_->itemFromIndex(idx), token, children);
            },
            fail);
        break;
    }
}

void MainWindow::fillChildren(QStandardItem* item, int token, QList<QStandardItem*> children) {
    // stale reply (node was refreshed / reconnected meanwhile)
    if (item->rowCount() != 1 || item->child(0)->data(RoleToken).toInt() != token) {
        qDeleteAll(children);
        return;
    }
    item->removeRows(0, item->rowCount());
    if (children.isEmpty()) {
        auto* empty = makeItem("(empty)", Placeholder, {});
        empty->setEnabled(false);
        empty->setData(-1, RoleToken); // never reloads by itself; use Refresh
        children << empty;
    }
    item->appendRows(children);
}

void MainWindow::loadFailed(QStandardItem* item, int token, const QString& msg) {
    if (item->rowCount() != 1 || item->child(0)->data(RoleToken).toInt() != token) return;
    item->child(0)->setText("Error: " + msg.simplified());
    item->child(0)->setToolTip(msg);
    item->child(0)->setData(0, RoleToken); // collapse + expand (or Refresh) retries
    statusBar()->showMessage(msg.simplified(), 8000);
}

void MainWindow::activated(QStandardItem* item) {
    if (!item) return;
    QString conn = item->data(RoleConn).toString();
    switch (item->data(RoleKind).toInt()) {
    case PgRelation:
        openSqlEditor(conn, item->data(RoleDb).toString(),
                      QString("SELECT * FROM %1 LIMIT 100;")
                          .arg(qs(SqlCompleter::tableName(item->data(RoleSchema).toString().toStdString(),
                                                          item->data(RoleName).toString().toStdString()))),
                      true);
        break;
    case RedisDb: openRedis(conn, item->data(RoleDb).toInt()); break;
    }
}

void MainWindow::enterDatabase(QStandardItem* db) {
    if (tree_->rootIndex() == db->index()) return;
    if (auto old = tree_->rootIndex(); old.isValid()) tree_->collapse(old); // so it can be entered again
    auto* conn = db->parent();
    dbBack_->setToolTip("Back to " + conn->text() + " (⌘[)");
    dbPick_->clear();
    for (int r = 0; r < conn->rowCount(); ++r)
        if (auto* c = conn->child(r); c->data(RoleKind).toInt() == PgDatabase) dbPick_->addItem(c->icon(), c->text(), r);
    dbPick_->setCurrentIndex(dbPick_->findData(db->row()));
    tree_->setRootIndex(db->index());
    tree_->expand(db->index()); // loads its schemas when entered from the switcher
    dbBar_->show();
}

void MainWindow::leaveDatabase() {
    QModelIndex db = tree_->rootIndex();
    tree_->setRootIndex({});
    dbBar_->hide();
    if (!db.isValid()) return;
    tree_->collapse(db);
    tree_->setCurrentIndex(db);
}

void MainWindow::newConnection() {
    ConnectionDialog dlg(ConnectionConfig{}, this);
    if (dlg.exec() != QDialog::Accepted) return;
    auto cfg = dlg.config();
    try {
        ConnectionStore::save(cfg);
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "Save failed", e.what());
        return;
    }
    passwords_[qs(cfg.id)] = cfg.password;
    cfg.password.clear();
    tree_->setCurrentIndex(addConnectionItem(cfg)->index());
}

void MainWindow::editConnection() {
    auto* item = selectedItem();
    if (!item) return;
    while (item->parent()) item = item->parent();
    QString conn = item->data(RoleConn).toString();

    ConnectionDialog dlg(configFor(conn), this);
    if (dlg.exec() != QDialog::Accepted) return;
    auto cfg = dlg.config();
    try {
        ConnectionStore::save(cfg);
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "Save failed", e.what());
        return;
    }
    passwords_[conn] = cfg.password;
    cfg.password.clear();
    configs_[conn] = cfg;
    item->setText(qs(cfg.name));
    dropSessions(conn);
    tree_->collapse(item->index());
    resetChildren(item);
}

void MainWindow::deleteConnection() {
    auto* item = selectedItem();
    if (!item) return;
    while (item->parent()) item = item->parent();
    if (QMessageBox::question(this, "Delete Connection", "Delete connection \"" + item->text() + "\"?") !=
        QMessageBox::Yes)
        return;
    QString conn = item->data(RoleConn).toString();
    dropSessions(conn);
    ConnectionStore::remove(conn.toStdString());
    model_->removeRow(item->row());
    configs_.erase(conn);
    passwords_.erase(conn);
}

void MainWindow::newSqlEditor() {
    auto* item = selectedItem();
    if (!item) {
        statusBar()->showMessage("Select a PostgreSQL connection first", 3000);
        return;
    }
    QString conn = item->data(RoleConn).toString();
    if (configs_.at(conn).type != DbType::PostgreSQL) {
        statusBar()->showMessage("SQL Editor is for PostgreSQL connections", 3000);
        return;
    }
    QString db = item->data(RoleDb).toString();
    openSqlEditor(conn, db.isEmpty() ? defaultDb(configs_.at(conn)) : db, {}, false);
}

void MainWindow::openSqlEditor(const QString& connId, const QString& db, const QString& sql, bool runNow) {
    auto* tab = new SqlEditorTab(configFor(connId, db), sql, runNow);
    tab->openSql = [this, connId, db](const QString& next) { openSqlEditor(connId, db, next, true); };
    int i = tabs_->addTab(tab, qs(configs_.at(connId).name) + " · " + db);
    tabs_->setCurrentIndex(i);
}

void MainWindow::openRedis(const QString& connId, int db) {
    auto* tab = new RedisTab(configFor(connId, QString::number(db)));
    int i = tabs_->addTab(tab, qs(configs_.at(connId).name) + QString(" · DB %1").arg(db));
    tabs_->setCurrentIndex(i);
}

// database item: every schema; schema / table item: that schema (table centred)
void MainWindow::openErDiagram() {
    auto* item = selectedItem();
    QString conn = item ? item->data(RoleConn).toString() : QString();
    if (!item || configs_.at(conn).type != DbType::PostgreSQL) {
        statusBar()->showMessage("Select a PostgreSQL database, schema or table first", 3000);
        return;
    }
    QString db = item->data(RoleDb).toString();
    if (db.isEmpty()) db = defaultDb(configs_.at(conn));
    QString schema = item->data(RoleSchema).toString();
    QString table = item->data(RoleKind).toInt() == PgRelation ? item->data(RoleName).toString() : QString();
    statusBar()->showMessage("Loading ER diagram…");
    pgSession(conn, db).run(
        this, [](PostgreSQL& pg) { return pg.schemaInfo(); },
        [=, this](SchemaInfo s) {
            if (!schema.isEmpty()) {
                std::string only = schema.toStdString();
                std::erase_if(s.tables, [&](auto& t) { return t.schema != only; });
            }
            int i = tabs_->addTab(makeErDiagram(s, table), QString("ER · %1 · %2")
                                                               .arg(qs(configs_.at(conn).name),
                                                                    schema.isEmpty() ? db : db + "." + schema));
            tabs_->setCurrentIndex(i);
            statusBar()->clearMessage();
        },
        [this](const QString& msg) { statusBar()->showMessage("ER diagram: " + msg, 5000); });
}

// connection / database item: whole database; schema item: that schema; table item: that table
void MainWindow::backup() {
    auto* item = selectedItem();
    QString conn = item ? item->data(RoleConn).toString() : QString();
    if (!item || configs_.at(conn).type != DbType::PostgreSQL) {
        QMessageBox::information(this, "Backup", "Right-click a PostgreSQL database, schema or table to back it up.");
        return;
    }
    QString db = item->data(RoleDb).toString();
    if (db.isEmpty()) db = defaultDb(configs_.at(conn));
    QString schema = item->data(RoleSchema).toString();
    QString table = item->data(RoleKind).toInt() == PgRelation ? item->data(RoleName).toString() : QString();
    QString base = (table.isEmpty() ? (schema.isEmpty() ? db : db + "_" + schema) : table) + "_" +
                   QDateTime::currentDateTime().toString("yyyyMMdd_HHmm");
    QString path = QFileDialog::getSaveFileName(this, "Backup", QDir(exportDir()).filePath(base + ".dump"),
                                                "pg_dump custom archive (*.dump);;Plain SQL (*.sql)");
    if (path.isEmpty()) return;

    // GUI apps on macOS don't get the shell PATH, so look where Homebrew / Postgres.app put it too
    QString pgDump = QStandardPaths::findExecutable("pg_dump");
    if (pgDump.isEmpty())
        pgDump = QStandardPaths::findExecutable(
            "pg_dump", {"/opt/homebrew/bin", "/opt/homebrew/opt/libpq/bin", "/usr/local/bin",
                        "/usr/local/opt/libpq/bin", "/Applications/Postgres.app/Contents/Versions/latest/bin"});
    if (pgDump.isEmpty()) {
        QMessageBox::warning(this, "Backup", "pg_dump not found. Install the PostgreSQL client tools "
                                             "(e.g. brew install libpq) and try again.");
        return;
    }

    auto cfg = configFor(conn, db);
    auto quote = [](const QString& s) { return qs(PostgreSQL::quoteIdent(s.toStdString())); };
    QStringList args{"--host", qs(cfg.host), "--port", QString::number(cfg.port), "--dbname", db,
                     "--file", path, "--no-password",
                     path.endsWith(".sql", Qt::CaseInsensitive) ? "--format=plain" : "--format=custom"};
    if (!cfg.username.empty()) args << "--username" << qs(cfg.username);
    if (!table.isEmpty()) args << "--table" << quote(schema) + "." + quote(table);
    else if (!schema.isEmpty()) args << "--schema" << quote(schema);

    auto* proc = new QProcess(this);
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("PGPASSWORD", qs(cfg.password)); // env, not argv: not visible in ps
    env.insert("PGSSLMODE", qs(cfg.sslMode));
    proc->setProcessEnvironment(env);
    // ponytail: no cancel / progress; add a Stop button if multi-hour dumps become a thing
    connect(proc, &QProcess::finished, this, [this, proc, path](int code, QProcess::ExitStatus st) {
        if (st == QProcess::NormalExit && code == 0) {
            statusBar()->clearMessage();
            showSaved(this, "Backup finished.", path);
        } else {
            statusBar()->clearMessage();
            QFile::remove(path); // don't leave a half-written dump that looks valid
            QMessageBox::warning(this, "Backup failed", QString::fromLocal8Bit(proc->readAllStandardError()).trimmed());
        }
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        statusBar()->clearMessage();
        QMessageBox::warning(this, "Backup failed", proc->errorString());
        proc->deleteLater();
    });
    statusBar()->showMessage("Backing up " + base + "…");
    proc->start(pgDump, args);
}

// Format first, then tick the tables (with Select all). Returns the extension and the picked
// tables as {schema, table}; empty if cancelled.
MainWindow::Picked MainWindow::pickTables(const std::vector<std::pair<std::string, std::string>>& tables, bool oneSchema) {
    QDialog dlg(this);
    dlg.setWindowTitle("Export Tables");
    auto* format = new QComboBox;
    format->addItem("CSV (.csv)", "csv");
    format->addItem("Excel (.xlsx)", "xlsx");
    format->addItem("JSON: data + structure (.json)", "json");
    auto* zip = new QCheckBox("One .zip file");
    zip->setChecked(true);
    zip->setToolTip("All tables in one .zip; off: one file per table in a folder");
    auto* all = new QCheckBox("Select all");
    auto* list = new QListWidget;
    for (auto& [schema, name] : tables) {
        auto* it = new QListWidgetItem(TableIcons::icon(qs(schema), qs(name)),
                                       oneSchema ? qs(name) : qs(schema) + "." + qs(name), list);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        it->setCheckState(Qt::Unchecked);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Export…");
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto checked = [list] {
        int n = 0;
        for (int i = 0; i < list->count(); ++i) n += list->item(i)->checkState() == Qt::Checked;
        return n;
    };
    connect(all, &QCheckBox::clicked, &dlg, [=] {
        auto st = checked() == list->count() ? Qt::Unchecked : Qt::Checked; // partial → all
        for (int i = 0; i < list->count(); ++i) list->item(i)->setCheckState(st);
    });
    connect(list, &QListWidget::itemChanged, &dlg, [=] {
        int n = checked();
        all->setCheckState(n == 0 ? Qt::Unchecked : n == list->count() ? Qt::Checked : Qt::PartiallyChecked);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(n > 0);
        buttons->button(QDialogButtonBox::Ok)->setText(n ? QString("Export %1…").arg(n) : "Export…");
    });

    auto* form = new QFormLayout;
    form->addRow("Format:", format);
    form->addRow("", zip);
    auto* layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(all);
    layout->addWidget(list);
    layout->addWidget(buttons);
    dlg.resize(380, 480);
    if (dlg.exec() != QDialog::Accepted) return {};

    std::vector<std::pair<std::string, std::string>> picked;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked) picked.push_back(tables[i]);
    return {format->currentData().toString(), zip->isChecked(), picked};
}

// database / schema item: tick tables, one file each in a chosen folder
void MainWindow::exportTables(const QString& conn, const QString& db, const QString& schema) {
    statusBar()->showMessage("Loading tables…");
    pgSession(conn, db).run(
        this,
        [s = schema.toStdString()](PostgreSQL& pg) {
            std::vector<std::pair<std::string, std::string>> out;
            for (auto& sc : s.empty() ? pg.schemas() : std::vector{s})
                for (auto& rel : pg.relations(sc)) out.emplace_back(sc, rel.first);
            return out;
        },
        [=, this](const std::vector<std::pair<std::string, std::string>>& tables) {
            statusBar()->clearMessage();
            if (tables.empty()) {
                QMessageBox::information(this, "Export Tables", "No tables in " + (schema.isEmpty() ? db : schema) + ".");
                return;
            }
            auto [ext, zip, picked] = pickTables(tables, !schema.isEmpty());
            if (picked.empty()) return;

            std::vector<std::pair<std::string, QString>> jobs; // quoted table, file name
            for (auto& [s, t] : picked)
                jobs.emplace_back(PostgreSQL::quoteIdent(s) + "." + PostgreSQL::quoteIdent(t),
                                  QString(qs(s) + "." + qs(t) + "." + ext).replace('/', '_').replace(':', '_'));
            QString target; // the .zip, or the folder
            if (zip) {
                target = QFileDialog::getSaveFileName(this, "Export Tables",
                                                      QDir(exportDir()).filePath(schema.isEmpty() ? db : schema),
                                                      "Zip (*.zip)");
                if (target.isEmpty()) return;
                if (!target.endsWith(".zip", Qt::CaseInsensitive)) target += ".zip";
            } else {
                target = QFileDialog::getExistingDirectory(this, "Export to folder", exportDir());
                if (target.isEmpty()) return;
                QStringList existing;
                for (auto& job : jobs)
                    if (QFile::exists(QDir(target).filePath(job.second))) existing << job.second;
                if (!existing.isEmpty() &&
                    QMessageBox::question(this, "Export Tables",
                                          QString("%1 file(s) already exist and will be replaced:\n%2")
                                              .arg(existing.size())
                                              .arg(existing.mid(0, 10).join('\n') + (existing.size() > 10 ? "\n…" : ""))) !=
                        QMessageBox::Yes)
                    return;
            }

            statusBar()->showMessage(QString("Exporting %1 tables…").arg(jobs.size()));
            // ponytail: one table after another on the worker, no progress / cancel; add when exports get slow.
            // A .zip is built in memory: fine until a database is several GB.
            pgSession(conn, db).run(
                this,
                [jobs, zip, target](PostgreSQL& pg) {
                    size_t rows = 0;
                    std::vector<std::pair<std::string, std::string>> files;
                    for (auto& [table, file] : jobs) {
                        auto rs = pg.execute("SELECT * FROM " + table);
                        rows += rs.rows.size();
                        try {
                            auto data = exportData(file, rs, table, isJsonExport(file) ? pg.tableStructureJson(table) : "");
                            if (zip) files.emplace_back(file.toStdString(), std::move(data));
                            else if (QString err = writeFile(QDir(target).filePath(file), data); !err.isEmpty())
                                throw DbError(err.toStdString());
                        } catch (const std::exception& e) {
                            throw DbError(table + ": " + e.what());
                        }
                    }
                    if (zip)
                        if (QString err = writeFile(target, Export::zip(files)); !err.isEmpty())
                            throw DbError(err.toStdString());
                    return rows;
                },
                [this, n = jobs.size(), target](size_t rows) {
                    statusBar()->clearMessage();
                    showSaved(this, QString("Exported %1 tables (%2 rows).").arg(n).arg(rows), target);
                },
                [this](const QString& msg) {
                    statusBar()->clearMessage();
                    QMessageBox::warning(this, "Export failed", msg);
                });
        },
        [this](const QString& msg) {
            statusBar()->clearMessage();
            QMessageBox::warning(this, "Export Tables", msg);
        });
}

void MainWindow::exportTable() {
    auto* item = selectedItem();
    int kind = item ? item->data(RoleKind).toInt() : 0;
    if (kind == PgDatabase || kind == PgSchema) {
        exportTables(item->data(RoleConn).toString(), item->data(RoleDb).toString(),
                     item->data(RoleSchema).toString());
        return;
    }
    if (kind != PgRelation) {
        QMessageBox::information(this, "Export", "Right-click a PostgreSQL database, schema or table to export it.");
        return;
    }
    QString conn = item->data(RoleConn).toString(), db = item->data(RoleDb).toString();
    QString name = item->data(RoleName).toString();
    QString path = exportPath(this, name);
    if (path.isEmpty()) return;
    std::string table = PostgreSQL::quoteIdent(item->data(RoleSchema).toString().toStdString()) + "." +
                        PostgreSQL::quoteIdent(name.toStdString());
    statusBar()->showMessage("Exporting " + name + "…");
    pgSession(conn, db).run(
        this,
        [table, path](PostgreSQL& pg) { // query + write on the worker: big tables don't freeze the UI
            auto rs = pg.execute("SELECT * FROM " + table);
            QString err = writeExport(path, rs, table, isJsonExport(path) ? pg.tableStructureJson(table) : "");
            if (!err.isEmpty()) throw DbError(err.toStdString());
            return rs.rows.size();
        },
        [this, path](size_t rows) {
            statusBar()->clearMessage();
            showSaved(this, QString("Exported %1 rows.").arg(rows), path);
        },
        [this](const QString& msg) {
            statusBar()->clearMessage();
            QMessageBox::warning(this, "Export failed", msg);
        });
}

// schema / table item: every table shown in that schema; database item: in its expanded schemas
void MainWindow::selectAllTables() {
    auto* item = selectedItem();
    if (!item && tree_->rootIndex().isValid()) item = model_->itemFromIndex(tree_->rootIndex());
    if (item && item->data(RoleKind).toInt() == PgRelation) item = item->parent();
    if (!item || (item->data(RoleKind).toInt() != PgSchema && item->data(RoleKind).toInt() != PgDatabase)) {
        statusBar()->showMessage("Select a PostgreSQL schema or table first", 3000);
        return;
    }
    tree_->expand(item->index());
    // rows on screen only: tables of a collapsed schema must not get dropped unseen
    QItemSelection sel;
    std::function<void(QStandardItem*)> walk = [&](QStandardItem* it) {
        if (!tree_->isExpanded(it->index())) return;
        for (int r = 0; r < it->rowCount(); ++r) {
            auto* c = it->child(r);
            if (c->data(RoleKind).toInt() == PgRelation) sel.select(c->index(), c->index());
            else walk(c);
        }
    };
    walk(item);
    if (sel.isEmpty()) {
        statusBar()->showMessage("No tables loaded yet; press ⌘A again", 3000);
        return;
    }
    tree_->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    statusBar()->showMessage(QString("%1 tables selected · ⌘⌫ drops them").arg(sel.indexes().size()), 3000);
}

// Selected tables (one database). Tables outside the selection with foreign keys to them are
// listed first: drop them too, or drop only the selection with CASCADE (removes those FKs).
void MainWindow::dropTables() {
    std::vector<std::pair<std::string, std::string>> rels;
    QString conn, db;
    QPersistentModelIndex dbIdx;
    for (auto& idx : tree_->selectionModel()->selectedRows()) {
        auto* it = model_->itemFromIndex(idx);
        if (it->data(RoleKind).toInt() != PgRelation) continue;
        if (rels.empty()) {
            conn = it->data(RoleConn).toString();
            db = it->data(RoleDb).toString();
            dbIdx = it->parent()->parent()->index();
        } else if (it->data(RoleConn) != conn || it->data(RoleDb) != db) {
            statusBar()->showMessage("Drop tables of one database at a time", 3000);
            return;
        }
        rels.emplace_back(it->data(RoleSchema).toString().toStdString(), it->data(RoleName).toString().toStdString());
    }
    if (rels.empty()) {
        statusBar()->showMessage("Select tables to drop (⌘A selects all in a schema)", 3000);
        return;
    }
    statusBar()->showMessage("Checking foreign keys…");
    pgSession(conn, db).run(
        this, [rels](PostgreSQL& pg) { return pg.dropClosure(rels); },
        [=, this](const std::vector<DropTarget>& all) {
            statusBar()->clearMessage();
            std::vector<DropTarget> picked, extra;
            for (auto& t : all) (t.via.empty() ? picked : extra).push_back(t);
            auto list = [](const std::vector<DropTarget>& v) {
                QStringList l;
                for (size_t i = 0; i < v.size() && i < 20; ++i)
                    l << qs(v[i].schema) + "." + qs(v[i].name) + (v[i].via.empty() ? "" : "  ← " + qs(v[i].via));
                if (v.size() > 20) l << QString("… %1 more").arg(v.size() - 20);
                return l.join('\n');
            };
            QMessageBox box(QMessageBox::Warning, "Drop Tables", {}, QMessageBox::Cancel, this);
            QAbstractButton *dropAll = nullptr, *cascade = nullptr;
            if (extra.empty()) {
                box.setText(QString("Drop %1 table(s)? This can't be undone.").arg(picked.size()));
                box.setInformativeText(list(picked));
                dropAll = box.addButton("Drop", QMessageBox::DestructiveRole);
            } else {
                box.setText(QString("%1 other table(s) have foreign keys to the selection.").arg(extra.size()));
                box.setInformativeText(list(extra) + "\n\nDrop them too, or keep them and remove only their "
                                                    "foreign keys (CASCADE, also drops dependent views)?");
                dropAll = box.addButton(QString("Drop All %1").arg(all.size()), QMessageBox::DestructiveRole);
                cascade = box.addButton(QString("Drop %1 + CASCADE").arg(picked.size()), QMessageBox::DestructiveRole);
            }
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() == dropAll) runDrop(conn, db, dbIdx, all, false);
            else if (cascade && box.clickedButton() == cascade) runDrop(conn, db, dbIdx, picked, true);
        },
        [this](const QString& msg) {
            statusBar()->clearMessage();
            QMessageBox::warning(this, "Drop Tables", msg);
        });
}

void MainWindow::runDrop(const QString& conn, const QString& db, const QPersistentModelIndex& dbIdx,
                         const std::vector<DropTarget>& targets, bool cascade) {
    statusBar()->showMessage(QString("Dropping %1 table(s)…").arg(targets.size()));
    pgSession(conn, db).run(
        this, [stmts = PostgreSQL::dropStatements(targets, cascade)](PostgreSQL& pg) { pg.executeInTransaction(stmts); },
        [=, this] {
            statusBar()->showMessage(QString("Dropped %1 table(s)").arg(targets.size()), 5000);
            if (!dbIdx.isValid()) return;
            auto* dbItem = model_->itemFromIndex(dbIdx);
            for (int r = 0; r < dbItem->rowCount(); ++r)
                if (auto* s = dbItem->child(r); s->data(RoleKind).toInt() == PgSchema) {
                    resetChildren(s);
                    if (tree_->isExpanded(s->index())) loadChildren(s);
                }
        },
        [=, this](const QString& msg) {
            statusBar()->clearMessage();
            // e.g. a view depends on a table: nothing was dropped (one transaction), offer CASCADE
            if (cascade) {
                QMessageBox::warning(this, "Drop failed", msg);
                return;
            }
            QMessageBox box(QMessageBox::Warning, "Drop failed", msg + "\n\nNothing was dropped.", QMessageBox::Cancel, this);
            auto* retry = box.addButton("Retry with CASCADE", QMessageBox::DestructiveRole);
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() == retry) runDrop(conn, db, dbIdx, targets, true);
        });
}

void MainWindow::dropDatabase(const QString& conn, const QString& db, bool force) {
    QString via = defaultDb(configs_.at(conn));
    if (db == via) {
        QMessageBox::information(this, "Drop Database",
                                 "\"" + db + "\" is the database this connection logs in to, so it can't drop it. "
                                 "Edit the connection to use another database (e.g. postgres) first.");
        return;
    }
    if (!force) {
        QMessageBox box(QMessageBox::Warning, "Drop Database",
                        "Drop database \"" + db + "\"? All its schemas, tables and data are deleted. "
                        "This can't be undone.", QMessageBox::Cancel, this);
        auto* drop = box.addButton("Drop Database", QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != drop) return;
    }
    pgSessions_.erase(conn + '/' + db); // our own explorer connection would block the drop
    statusBar()->showMessage("Dropping database " + db + "…");
    std::string stmt = "DROP DATABASE " + PostgreSQL::quoteIdent(db.toStdString()) + (force ? " WITH (FORCE)" : "");
    pgSession(conn, via).run(
        this, [stmt](PostgreSQL& pg) { pg.execute(stmt); },
        [=, this] {
            statusBar()->showMessage("Dropped database " + db, 5000);
            for (int r = 0; r < model_->rowCount(); ++r)
                if (auto* c = model_->item(r); c->data(RoleConn).toString() == conn) {
                    if (tree_->rootIndex().parent() == c->index()) leaveDatabase(); // re-list its databases
                    resetChildren(c);
                    if (tree_->isExpanded(c->index())) loadChildren(c);
                }
        },
        [=, this](const QString& msg) {
            statusBar()->clearMessage();
            // open SQL tabs (or other clients) on it: PostgreSQL 13+ can disconnect them
            if (force || !msg.contains("being accessed by other users")) {
                QMessageBox::warning(this, "Drop failed", msg);
                return;
            }
            QMessageBox box(QMessageBox::Warning, "Drop failed", msg, QMessageBox::Cancel, this);
            box.setInformativeText("Disconnect those sessions (SQL tabs on it included) and drop anyway?");
            auto* retry = box.addButton("Drop with FORCE", QMessageBox::DestructiveRole);
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() == retry) dropDatabase(conn, db, true);
        });
}

ConnectionConfig MainWindow::configFor(const QString& connId, const QString& db) {
    auto cfg = configs_.at(connId);
    auto pw = passwords_.find(connId);
    if (pw == passwords_.end()) pw = passwords_.emplace(connId, ConnectionStore::password(cfg.id)).first;
    cfg.password = pw->second;
    if (!db.isEmpty()) cfg.database = db.toStdString();
    return cfg;
}

Session<PostgreSQL>& MainWindow::pgSession(const QString& connId, const QString& db) {
    auto& s = pgSessions_[connId + '/' + db];
    if (!s) s = std::make_unique<Session<PostgreSQL>>(configFor(connId, db));
    return *s;
}

Session<Redis>& MainWindow::redisSession(const QString& connId) {
    auto& s = redisSessions_[connId];
    if (!s) s = std::make_unique<Session<Redis>>(configFor(connId));
    return *s;
}

void MainWindow::dropSessions(const QString& connId) {
    std::erase_if(pgSessions_, [&](auto& kv) { return kv.first.startsWith(connId + '/'); });
    redisSessions_.erase(connId);
}
