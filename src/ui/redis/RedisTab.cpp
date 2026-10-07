#include "RedisTab.h"

#include "ui/ResultModel.h"

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QInputDialog>
#include <QUuid>
#include <climits>
#include <cstdlib>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
enum KeyColumn { ColKey, ColType, ColTtl, ColSize };

QTableWidgetItem* numberItem(long long v) {
    auto* item = new QTableWidgetItem;
    item->setData(Qt::DisplayRole, v); // numeric sort
    return item;
}

QString qs(const std::string& s) { return QString::fromStdString(s); }

// Redis scores: decimal or ±inf
bool isScore(const std::string& s) {
    if (s == "inf" || s == "+inf" || s == "-inf") return true;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    // strtod also accepts nan / hex / "infinity"; Redis doesn't
    return !s.empty() && *end == 0 && s.find_first_of("nNxX") == std::string::npos;
}

// Small form of line edits; nullopt on cancel.
std::optional<QStringList> askValues(QWidget* parent, const QString& title, const QStringList& labels,
                                     const QStringList& defaults = {}) {
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setMinimumWidth(380);
    auto* form = new QFormLayout(&dlg);
    QList<QLineEdit*> edits;
    for (int i = 0; i < labels.size(); ++i) {
        edits << new QLineEdit(defaults.value(i));
        form->addRow(labels[i], edits.last());
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return std::nullopt;
    QStringList out;
    for (auto* e : edits) out << e->text();
    return out;
}
}

RedisTab::RedisTab(const ConnectionConfig& cfg, QWidget* parent) : QWidget(parent), session_(cfg) {
    auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    // key browser
    pattern_ = new QLineEdit;
    pattern_->setPlaceholderText("Pattern, e.g. user:*  (⌘F)");
    auto* scanButton = new QPushButton("Scan");
    more_ = new QPushButton("Load more");
    more_->setEnabled(false);
    keyCount_ = new QLabel;
    auto* searchRow = new QHBoxLayout;
    searchRow->addWidget(pattern_, 1);
    searchRow->addWidget(scanButton);
    searchRow->addWidget(more_);
    auto* newKey = new QPushButton("+ Key");
    searchRow->addWidget(newKey);

    keys_ = new QTableWidget(0, 4);
    keys_->setHorizontalHeaderLabels({"Key", "Type", "TTL", "Size"});
    keys_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    keys_->horizontalHeader()->setSectionResizeMode(ColKey, QHeaderView::Stretch);
    keys_->horizontalHeader()->setMinimumSectionSize(60);
    keys_->verticalHeader()->hide();
    keys_->verticalHeader()->setDefaultSectionSize(22);
    keys_->setSelectionBehavior(QAbstractItemView::SelectRows);
    keys_->setSelectionMode(QAbstractItemView::SingleSelection);
    keys_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    keys_->setSortingEnabled(true);

    auto* browser = new QWidget;
    auto* browserLayout = new QVBoxLayout(browser);
    browserLayout->setContentsMargins(0, 0, 0, 0);
    browserLayout->addLayout(searchRow);
    browserLayout->addWidget(keys_, 1);
    browserLayout->addWidget(keyCount_);

    // value viewer
    keyHeader_ = new QLabel("Select a key");
    keyHeader_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    keyHeader_->setWordWrap(true);
    auto* refresh = new QPushButton("Refresh");
    delete_ = new QPushButton("Delete Key");
    rename_ = new QPushButton("Rename…");
    ttl_ = new QPushButton("TTL…");
    auto* headerRow = new QHBoxLayout;
    headerRow->addWidget(keyHeader_, 1);
    headerRow->addWidget(refresh);
    headerRow->addWidget(rename_);
    headerRow->addWidget(ttl_);
    headerRow->addWidget(delete_);

    addItem_ = new QPushButton("+ Add");
    removeItems_ = new QPushButton("− Remove");
    saveString_ = new QPushButton("Save (⌘S)");
    auto* editRow = new QHBoxLayout;
    editRow->addWidget(addItem_);
    editRow->addWidget(removeItems_);
    editRow->addWidget(saveString_);
    editRow->addStretch(1);
    for (auto* b : {delete_, rename_, ttl_, addItem_, removeItems_, saveString_}) b->setEnabled(false);

    stringValue_ = new QPlainTextEdit;
    stringValue_->setReadOnly(true);
    stringValue_->setFont(mono);
    valueModel_ = new ResultModel(this);
    valueTable_ = new QTableView;
    valueTable_->setModel(valueModel_);
    valueTable_->setWordWrap(false);
    valueTable_->horizontalHeader()->setStretchLastSection(true);
    valueTable_->verticalHeader()->setDefaultSectionSize(22);
    valueStack_ = new QStackedWidget;
    valueStack_->addWidget(stringValue_);
    valueStack_->addWidget(valueTable_);

    auto* viewer = new QWidget;
    auto* viewerLayout = new QVBoxLayout(viewer);
    viewerLayout->setContentsMargins(0, 0, 0, 0);
    viewerLayout->addLayout(headerRow);
    viewerLayout->addWidget(valueStack_, 1);
    viewerLayout->addLayout(editRow);

    // command console
    console_ = new QPlainTextEdit;
    console_->setReadOnly(true);
    console_->setFont(mono);
    console_->setMaximumBlockCount(5000);
    command_ = new QLineEdit;
    command_->setFont(mono);
    command_->setPlaceholderText("Redis command, e.g. HGETALL user:1  (Enter to run)");
    auto* consoleBox = new QWidget;
    auto* consoleLayout = new QVBoxLayout(consoleBox);
    consoleLayout->setContentsMargins(0, 0, 0, 0);
    consoleLayout->addWidget(console_, 1);
    consoleLayout->addWidget(command_);

    auto* top = new QSplitter(Qt::Horizontal);
    top->addWidget(browser);
    top->addWidget(viewer);
    top->setStretchFactor(1, 1);
    top->setSizes({480, 600});
    auto* main = new QSplitter(Qt::Vertical);
    main->addWidget(top);
    main->addWidget(consoleBox);
    main->setStretchFactor(0, 3);
    main->setStretchFactor(1, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(main);

    connect(scanButton, &QPushButton::clicked, this, [this] { scan(true); });
    connect(pattern_, &QLineEdit::returnPressed, this, [this] { scan(true); });
    connect(more_, &QPushButton::clicked, this, [this] { scan(false); });
    connect(keys_, &QTableWidget::itemSelectionChanged, this, &RedisTab::showSelectedKey);
    connect(refresh, &QPushButton::clicked, this, &RedisTab::showSelectedKey);
    connect(delete_, &QPushButton::clicked, this, &RedisTab::deleteSelectedKey);
    connect(command_, &QLineEdit::returnPressed, this, &RedisTab::runCommand);
    connect(newKey, &QPushButton::clicked, this, &RedisTab::newKey);
    connect(rename_, &QPushButton::clicked, this, &RedisTab::renameKey);
    connect(ttl_, &QPushButton::clicked, this, &RedisTab::editTtl);
    connect(addItem_, &QPushButton::clicked, this, &RedisTab::addItem);
    connect(removeItems_, &QPushButton::clicked, this, &RedisTab::removeItems);
    connect(saveString_, &QPushButton::clicked, this, &RedisTab::saveString);
    connect(stringValue_, &QPlainTextEdit::modificationChanged, saveString_, &QPushButton::setEnabled);
    valueModel_->setCommitHook([this](int r, int c, const auto& v) { return commitCell(r, c, v); });
    valueTable_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

    auto* save = new QAction(this);
    save->setShortcut(QKeySequence::Save);
    save->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(save, &QAction::triggered, this, [this] { if (saveString_->isEnabled()) saveString(); });
    addAction(save);

    auto* find = new QAction(this);
    find->setShortcut(QKeySequence::Find);
    find->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QAction::triggered, this, [this] { pattern_->setFocus(); pattern_->selectAll(); });
    addAction(find);
    auto* del = new QAction(keys_);
    del->setShortcuts({QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)});
    del->setShortcutContext(Qt::WidgetShortcut);
    connect(del, &QAction::triggered, this, &RedisTab::deleteSelectedKey);
    keys_->addAction(del);
    auto* delItems = new QAction(valueTable_);
    delItems->setShortcuts({QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)});
    delItems->setShortcutContext(Qt::WidgetShortcut);
    connect(delItems, &QAction::triggered, this, [this] { if (removeItems_->isEnabled()) removeItems(); });
    valueTable_->addAction(delItems);
    addCopyShortcut(keys_);
    setZebra(keys_);
    addCopyShortcut(valueTable_);
    setZebra(valueTable_);

    scan(true);
}

std::string RedisTab::selectedKey() const {
    auto rows = keys_->selectionModel()->selectedRows(ColKey);
    if (rows.isEmpty()) return {};
    return rows[0].data(Qt::UserRole).toByteArray().toStdString();
}

void RedisTab::scan(bool reset) {
    if (reset) {
        cursor_ = "0";
        keys_->clearSelection();
        keys_->setRowCount(0);
        keyHeader_->setText("Select a key");
        stringValue_->clear();
        stringValue_->setPlaceholderText({});
        valueModel_->setResult({});
        showSelectedKey(); // nothing selected: disables the edit buttons
    }
    more_->setEnabled(false);
    keyCount_->setText("Scanning…");
    session_.run(
        this,
        [cursor = cursor_, pattern = pattern_->text().toStdString()](Redis& r) {
            // Keep stepping until a page fills up: with MATCH, single SCAN steps are often empty.
            // ponytail: max 100 steps per click; enough for millions of keys with a page-sized hit rate
            std::vector<Redis::KeyInfo> found;
            std::string cur = cursor;
            for (int step = 0; step < 100; ++step) {
                auto [next, page] = r.scan(cur, pattern, 500);
                found.insert(found.end(), page.begin(), page.end());
                cur = next;
                if (cur == "0" || found.size() >= 200) break;
            }
            return std::pair{cur, found};
        },
        [this](const std::pair<std::string, std::vector<Redis::KeyInfo>>& res) {
            cursor_ = res.first;
            keys_->setSortingEnabled(false);
            for (auto& k : res.second) {
                int row = keys_->rowCount();
                keys_->insertRow(row);
                auto* keyItem = new QTableWidgetItem(qs(k.key));
                keyItem->setData(Qt::UserRole, QByteArray::fromStdString(k.key));
                keys_->setItem(row, ColKey, keyItem);
                keys_->setItem(row, ColType, new QTableWidgetItem(qs(k.type).toUpper()));
                keys_->setItem(row, ColTtl, numberItem(k.ttl));
                keys_->setItem(row, ColSize, numberItem(k.size));
            }
            keys_->setSortingEnabled(true);
            bool done = cursor_ == "0";
            more_->setEnabled(!done);
            keyCount_->setText(QString("%1 keys%2").arg(keys_->rowCount()).arg(done ? "" : " (more available)"));
        },
        [this](const QString& msg) {
            more_->setEnabled(cursor_ != "0");
            keyCount_->setText("<font color=red>" + msg.toHtmlEscaped() + "</font>");
        });
}

int RedisTab::rowOf(const std::string& key) const {
    for (int row = 0; row < keys_->rowCount(); ++row)
        if (keys_->item(row, ColKey)->data(Qt::UserRole).toByteArray().toStdString() == key) return row;
    return -1;
}

void RedisTab::showSelectedKey() {
    std::string key = selectedKey();
    if (key != currentKey_) { // reloading the same key keeps it editable meanwhile
        for (auto* b : {delete_, rename_, ttl_, addItem_, removeItems_, saveString_}) b->setEnabled(false);
        currentKey_.clear();
        currentType_.clear();
        if (!key.empty()) keyHeader_->setText("Loading…");
    }
    if (key.empty()) return;
    // re-read type/TTL: the key may have changed since the scan
    session_.run(
        this,
        [key](Redis& r) {
            auto info = r.describe({key});
            if (info.empty()) throw DbError("Key no longer exists");
            return std::pair{info[0], r.value(key, info[0].type)};
        },
        [this, key](const std::pair<Redis::KeyInfo, ResultSet>& res) {
            if (key != selectedKey()) return; // selection moved on meanwhile
            auto& [info, value] = res;
            currentKey_ = key;
            currentType_ = info.type;
            keyHeader_->setText(QString("<b>%1</b><br>Type: %2 · TTL: %3 · Size: %4 · %5")
                                    .arg(qs(info.key).toHtmlEscaped(), qs(info.type).toUpper(),
                                         info.ttl < 0 ? QString("no expiry") : QString("%1 s").arg(info.ttl))
                                    .arg(info.size)
                                    .arg(qs(value.status)));
            if (int row = rowOf(key); row >= 0) { // keep the key list in sync
                keys_->item(row, ColType)->setText(qs(info.type).toUpper());
                keys_->item(row, ColTtl)->setData(Qt::DisplayRole, info.ttl);
                keys_->item(row, ColSize)->setData(Qt::DisplayRole, info.size);
            }
            for (auto* b : {delete_, rename_, ttl_}) b->setEnabled(true);

            if (info.type == "string") {
                std::string v = value.rows.empty() ? std::string() : *value.rows[0][0];
                QString text = qs(v);
                stringValue_->setPlainText(text);
                stringValue_->setPlaceholderText("<EMPTY>"); // shown while the value is ""
                // binary values don't survive a QString round trip: view only
                stringValue_->setReadOnly(text.toStdString() != v);
                stringValue_->document()->setModified(false);
                valueStack_->setCurrentWidget(stringValue_);
            } else {
                valueModel_->setResult(value);
                static const std::map<std::string, std::vector<bool>> editable = {
                    {"hash", {false, true}}, {"list", {false, true}}, {"set", {true}}, {"zset", {true, true}}};
                if (auto it = editable.find(info.type); it != editable.end())
                    valueModel_->setEditable(it->second, it->second);
                addItem_->setEnabled(info.type != "stream");
                removeItems_->setEnabled(true);
                valueStack_->setCurrentWidget(valueTable_);
            }
        },
        [this](const QString& msg) {
            keyHeader_->setText("<font color=red>" + msg.toHtmlEscaped() + "</font>");
        });
}

void RedisTab::mutate(std::function<void(Redis&)> f, std::function<void()> after) {
    session_.run(
        this, std::move(f),
        [this, after] {
            if (after) after();
            showSelectedKey();
        },
        [this](const QString& msg) {
            QMessageBox::warning(this, "Redis", msg);
            showSelectedKey();
        });
}

// In-place edit from the value grid; written immediately.
bool RedisTab::commitCell(int row, int col, const std::optional<std::string>& v) {
    if (!v) return false;
    const auto& old = valueModel_->result().rows[row];
    std::string key = currentKey_;
    std::vector<std::vector<std::string>> cmds; // add before remove: a failure never loses data
    if (currentType_ == "hash") {
        cmds = {{"HSET", key, *old[0], *v}};
    } else if (currentType_ == "list") {
        cmds = {{"LSET", key, *old[0], *v}};
    } else if (currentType_ == "set") {
        cmds = {{"SADD", key, *v}, {"SREM", key, *old[0]}};
    } else if (currentType_ == "zset" && col == 0) {
        cmds = {{"ZADD", key, *old[1], *v}, {"ZREM", key, *old[0]}};
    } else if (currentType_ == "zset") {
        if (!isScore(*v)) {
            QMessageBox::warning(this, "Redis", "Score must be a number");
            return false;
        }
        cmds = {{"ZADD", key, *v, *old[0]}};
    } else {
        return false;
    }
    mutate([cmds](Redis& r) {
        for (auto& c : cmds) r.call(c);
    });
    return true;
}

void RedisTab::newKey() {
    bool ok = false;
    QString type = QInputDialog::getItem(this, "New Key", "Type:", {"string", "hash", "list", "set", "zset"}, 0,
                                         false, &ok);
    if (!ok) return;
    static const std::map<QString, QStringList> labels = {
        {"string", {"Key", "Value"}}, {"hash", {"Key", "Field", "Value"}}, {"list", {"Key", "Value"}},
        {"set", {"Key", "Member"}},   {"zset", {"Key", "Member", "Score"}}};
    auto vals = askValues(this, "New " + type, labels.at(type));
    if (!vals || (*vals)[0].isEmpty()) return;
    std::vector<std::string> v;
    for (auto& s : *vals) v.push_back(s.toStdString());
    if (type == "zset" && !isScore(v[2])) {
        QMessageBox::warning(this, "Redis", "Score must be a number");
        return;
    }
    static const std::map<QString, std::vector<std::string>> cmd = {
        {"string", {"SET"}}, {"hash", {"HSET"}}, {"list", {"RPUSH"}}, {"set", {"SADD"}}, {"zset", {"ZADD"}}};
    std::vector<std::string> args = cmd.at(type);
    if (type == "zset") args.insert(args.end(), {v[0], v[2], v[1]});
    else args.insert(args.end(), v.begin(), v.end());

    std::string key = v[0];
    mutate(
        [key, args](Redis& r) {
            if (r.call({"EXISTS", key}) != "(integer) 0") throw DbError("Key already exists: " + key);
            r.call(args);
        },
        [this, key] {
            keys_->setSortingEnabled(false);
            int row = keys_->rowCount();
            keys_->insertRow(row);
            auto* keyItem = new QTableWidgetItem(qs(key));
            keyItem->setData(Qt::UserRole, QByteArray::fromStdString(key));
            keys_->setItem(row, ColKey, keyItem);
            keys_->setItem(row, ColType, new QTableWidgetItem);
            keys_->setItem(row, ColTtl, numberItem(-1));
            keys_->setItem(row, ColSize, numberItem(0));
            keys_->setSortingEnabled(true);
            keys_->selectRow(keys_->row(keyItem)); // triggers showSelectedKey
        });
}

void RedisTab::renameKey() {
    std::string key = currentKey_;
    bool ok = false;
    QString name = QInputDialog::getText(this, "Rename Key", "New name:", QLineEdit::Normal, qs(key), &ok);
    if (!ok || name.isEmpty() || name.toStdString() == key) return;
    std::string to = name.toStdString();
    mutate(
        [key, to](Redis& r) {
            if (r.call({"RENAMENX", key, to}) == "(integer) 0") throw DbError("Key already exists: " + to);
        },
        [this, key, to] {
            if (int row = rowOf(key); row >= 0) {
                keys_->item(row, ColKey)->setText(qs(to));
                keys_->item(row, ColKey)->setData(Qt::UserRole, QByteArray::fromStdString(to));
            }
        });
}

void RedisTab::editTtl() {
    std::string key = currentKey_;
    int row = rowOf(key);
    int current = row >= 0 ? keys_->item(row, ColTtl)->data(Qt::DisplayRole).toInt() : -1;
    bool ok = false;
    int ttl = QInputDialog::getInt(this, "TTL", "Seconds until expiry (-1 = never expires):", current, -1,
                                   INT_MAX, 1, &ok);
    if (!ok) return;
    if (ttl == 0) {
        QMessageBox::warning(this, "TTL", "TTL 0 would delete the key; use Delete Key instead.");
        return;
    }
    mutate([key, ttl](Redis& r) {
        if (ttl < 0) r.call({"PERSIST", key});
        else r.call({"EXPIRE", key, std::to_string(ttl)});
    });
}

void RedisTab::addItem() {
    std::string key = currentKey_, type = currentType_;
    static const std::map<std::string, QStringList> labels = {
        {"hash", {"Field", "Value"}}, {"list", {"Value (appended)"}}, {"set", {"Member"}}, {"zset", {"Member", "Score"}}};
    auto it = labels.find(type);
    if (it == labels.end()) return;
    auto vals = askValues(this, "Add to " + qs(key), it->second);
    if (!vals) return;
    std::vector<std::string> v;
    for (auto& s : *vals) v.push_back(s.toStdString());
    std::vector<std::string> args;
    if (type == "hash") args = {"HSET", key, v[0], v[1]};
    else if (type == "list") args = {"RPUSH", key, v[0]};
    else if (type == "set") args = {"SADD", key, v[0]};
    else {
        if (!isScore(v[1])) {
            QMessageBox::warning(this, "Redis", "Score must be a number");
            return;
        }
        args = {"ZADD", key, v[1], v[0]};
    }
    mutate([args](Redis& r) { r.call(args); });
}

void RedisTab::removeItems() {
    std::set<int> rows;
    for (auto& i : valueTable_->selectionModel()->selectedIndexes()) rows.insert(i.row());
    if (rows.empty()) return;
    if (QMessageBox::question(this, "Remove", QString("Remove %1 item(s) from \"%2\"?").arg(rows.size()).arg(qs(currentKey_))) !=
        QMessageBox::Yes)
        return;
    std::string key = currentKey_, type = currentType_;
    std::vector<std::string> first; // column 0 of each row: field / index / member / id
    for (int r : rows) first.push_back(*valueModel_->result().rows[r][0]);

    mutate([key, type, first](Redis& r) {
        if (type == "list") {
            // Redis has no "delete by index": tag the slots, then remove the tags
            std::string tag = "__dbm_deleted_" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            for (auto& idx : first) r.call({"LSET", key, idx, tag});
            r.call({"LREM", key, "0", tag});
            return;
        }
        static const std::map<std::string, std::string> cmd = {
            {"hash", "HDEL"}, {"set", "SREM"}, {"zset", "ZREM"}, {"stream", "XDEL"}};
        std::vector<std::string> args = {cmd.at(type), key};
        args.insert(args.end(), first.begin(), first.end());
        r.call(args);
    });
}

void RedisTab::saveString() {
    std::string key = currentKey_, value = stringValue_->toPlainText().toStdString();
    mutate([key, value](Redis& r) {
        auto info = r.describe({key});
        r.call({"SET", key, value});
        if (!info.empty() && info[0].ttl > 0) r.call({"EXPIRE", key, std::to_string(info[0].ttl)}); // keep TTL
    });
}

void RedisTab::deleteSelectedKey() {
    std::string key = selectedKey();
    if (key.empty()) return;
    if (QMessageBox::question(this, "Delete Key", "Delete key \"" + qs(key) + "\"?") != QMessageBox::Yes)
        return;
    session_.run(
        this, [key](Redis& r) { return r.execute({"DEL", key}); },
        [this, key](const std::string&) {
            for (int row = 0; row < keys_->rowCount(); ++row)
                if (keys_->item(row, ColKey)->data(Qt::UserRole).toByteArray().toStdString() == key) {
                    keys_->removeRow(row);
                    break;
                }
            keyHeader_->setText("Deleted " + qs(key).toHtmlEscaped());
            keyCount_->setText(QString("%1 keys").arg(keys_->rowCount()));
        },
        [this](const QString& msg) {
            keyHeader_->setText("<font color=red>" + msg.toHtmlEscaped() + "</font>");
        });
}

void RedisTab::runCommand() {
    QString line = command_->text().trimmed();
    if (line.isEmpty()) return;
    std::vector<std::string> args;
    try {
        args = Redis::tokenize(line.toStdString());
    } catch (const DbError& e) {
        console_->appendPlainText("> " + line + "\n(error) " + e.what());
        return;
    }
    // commands that can block or wipe a production server need an explicit OK
    static const QStringList dangerous = {"SAVE", "FLUSHALL", "FLUSHDB", "KEYS", "SHUTDOWN", "DEBUG"};
    QString name = qs(args[0]).toUpper();
    if (name == "SELECT") { // the key browser is bound to this tab's DB
        console_->appendPlainText("> " + line + "\n(error) Open the other DB from the explorer instead\n");
        return;
    }
    if (dangerous.contains(name) &&
        QMessageBox::warning(this, "Dangerous command",
                             name + " can block or wipe the server. Run it anyway?",
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    command_->clear();
    command_->setEnabled(false);
    session_.run(
        this, [args](Redis& r) { return r.execute(args); },
        [this, line](const std::string& out) {
            command_->setEnabled(true);
            command_->setFocus();
            console_->appendPlainText("> " + line + "\n" + qs(out) + "\n");
        },
        [this, line](const QString& msg) {
            command_->setEnabled(true);
            command_->setFocus();
            console_->appendPlainText("> " + line + "\n(error) " + msg + "\n");
        });
}
