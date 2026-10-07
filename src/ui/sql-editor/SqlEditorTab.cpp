#include "SqlEditorTab.h"

#include "core/query/SqlCompleter.h"
#include "ui/ResultModel.h"

#include <QAbstractItemView>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QAction>
#include <QCompleter>
#include <QKeyEvent>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QStyle>
#include <QTimer>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// FK cells get a → in their right corner; clicking it opens the referenced row.
class ForeignKeyDelegate : public QStyledItemDelegate {
public:
    ForeignKeyDelegate(ResultModel* model, QObject* parent) : QStyledItemDelegate(parent), model_(model) {}
    std::function<void(const QModelIndex&)> follow;
    std::function<QString(const QModelIndex&)> describe; // tooltip for the arrow

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!hasArrow(index)) return QStyledItemDelegate::paint(p, option, index);
        QStyleOptionViewItem full(option);
        initStyleOption(&full, index);
        full.text.clear();
        full.widget->style()->drawControl(QStyle::CE_ItemViewItem, &full, p, full.widget); // background
        QStyleOptionViewItem text(option);
        text.rect.setRight(arrowRect(option.rect).left() - 1); // keep the value clear of the arrow
        QStyledItemDelegate::paint(p, text, index);
        p->save();
        p->setPen(option.palette.color(QPalette::Link));
        p->drawText(arrowRect(option.rect), Qt::AlignCenter, QStringLiteral("→"));
        p->restore();
    }

    bool editorEvent(QEvent* e, QAbstractItemModel* m, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override {
        auto t = e->type();
        if ((t == QEvent::MouseButtonPress || t == QEvent::MouseButtonRelease || t == QEvent::MouseButtonDblClick) &&
            hasArrow(index) && arrowRect(option.rect).contains(static_cast<QMouseEvent*>(e)->position().toPoint())) {
            if (t == QEvent::MouseButtonRelease && follow) follow(index);
            return true; // the arrow is a button: no selection change / editing
        }
        return QStyledItemDelegate::editorEvent(e, m, option, index);
    }

    bool helpEvent(QHelpEvent* e, QAbstractItemView* view, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override {
        if (hasArrow(index) && arrowRect(option.rect).contains(e->pos()) && describe) {
            QToolTip::showText(e->globalPos(), describe(index), view);
            return true;
        }
        return QStyledItemDelegate::helpEvent(e, view, option, index);
    }

private:
    static QRect arrowRect(const QRect& cell) { return QRect(cell.right() - 17, cell.top(), 18, cell.height()); }
    bool hasArrow(const QModelIndex& i) const { return model_->foreignRefFor(i.column()) >= 0 && !model_->isNull(i); }
    ResultModel* model_;
};

// "table_name" is selected after insert so typing replaces it.
const std::pair<const char*, const char*> kTemplates[] = {
    {"SELECT", "SELECT *\nFROM table_name\nWHERE condition\nLIMIT 100;"},
    {"INSERT", "INSERT INTO table_name (column1, column2)\nVALUES (value1, value2);"},
    {"UPDATE", "UPDATE table_name\nSET column1 = value1\nWHERE condition;"},
    {"DELETE", "DELETE FROM table_name\nWHERE condition;"},
    {"CREATE TABLE", "CREATE TABLE table_name (\n    id BIGSERIAL PRIMARY KEY,\n    name TEXT NOT NULL,\n"
                     "    created_at TIMESTAMPTZ NOT NULL DEFAULT now()\n);"},
    {"ALTER TABLE", "ALTER TABLE table_name\n    ADD COLUMN column_name TEXT;"},
    {"CREATE INDEX", "CREATE INDEX index_name ON table_name (column_name);"},
    {"DROP TABLE", "DROP TABLE table_name;"},
};

// Lets the completer popup own Enter / Tab / Esc while it's open (QCompleter forwards them here).
class SqlTextEdit : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<bool()> popupVisible;
    std::function<void(bool forced, const QString& typed)> afterKey;
    std::function<void()> afterDoubleClick;

protected:
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        QPlainTextEdit::mouseDoubleClickEvent(e); // selects the word
        afterDoubleClick();
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (popupVisible()) {
            switch (e->key()) {
            case Qt::Key_Enter:
            case Qt::Key_Return:
            case Qt::Key_Escape:
            case Qt::Key_Tab:
            case Qt::Key_Backtab: e->ignore(); return;
            default: break;
            }
        }
        // ⌃Space opens suggestions (⌘Space belongs to Spotlight on macOS)
        bool manual = e->key() == Qt::Key_Space && (e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier));
        if (!manual) QPlainTextEdit::keyPressEvent(e);
        afterKey(manual, e->text());
    }
};

class SqlHighlighter : public QSyntaxHighlighter {
public:
    explicit SqlHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc) {
        QTextCharFormat keyword, string, comment, number;
        keyword.setForeground(QColor("#1f6feb"));
        keyword.setFontWeight(QFont::Bold);
        string.setForeground(QColor("#2da44e"));
        comment.setForeground(Qt::gray);
        number.setForeground(QColor("#bc4c00"));
        rules_ = {
            {QRegularExpression(
                 R"(\b(SELECT|FROM|WHERE|AND|OR|NOT|IN|IS|NULL|AS|ON|JOIN|LEFT|RIGHT|INNER|OUTER|FULL|CROSS|)"
                 R"(GROUP|BY|ORDER|HAVING|LIMIT|OFFSET|INSERT|INTO|VALUES|UPDATE|SET|DELETE|CREATE|ALTER|)"
                 R"(DROP|TABLE|VIEW|INDEX|SCHEMA|DATABASE|PRIMARY|KEY|FOREIGN|REFERENCES|UNIQUE|DEFAULT|)"
                 R"(CASE|WHEN|THEN|ELSE|END|DISTINCT|UNION|ALL|EXISTS|BETWEEN|LIKE|ILIKE|ASC|DESC|WITH|)"
                 R"(RETURNING|BEGIN|COMMIT|ROLLBACK|TRUE|FALSE|EXPLAIN|ANALYZE|GRANT|REVOKE|TRUNCATE)\b)",
                 QRegularExpression::CaseInsensitiveOption),
             keyword},
            {QRegularExpression(R"(\b\d+(\.\d+)?\b)"), number},
            {QRegularExpression(R"('(?:[^']|'')*')"), string},
            {QRegularExpression(R"(--[^\n]*)"), comment},
        };
    }

protected:
    // ponytail: per-line rules only; multi-line strings and /* */ comments aren't coloured across lines
    void highlightBlock(const QString& text) override {
        for (auto& [re, fmt] : rules_)
            for (auto it = re.globalMatch(text); it.hasNext();) {
                auto m = it.next();
                setFormat(m.capturedStart(), m.capturedLength(), fmt);
            }
    }

private:
    std::vector<std::pair<QRegularExpression, QTextCharFormat>> rules_;
};

}

SqlEditorTab::SqlEditorTab(const ConnectionConfig& cfg, const QString& sql, bool runNow, QWidget* parent)
    : QWidget(parent), session_(cfg) {
    auto* editor = new SqlTextEdit(sql);
    editor_ = editor;
    editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor_->setPlaceholderText("SQL… (⌘↵ run, ⌘. cancel; select text to run only the selection)");
    new SqlHighlighter(editor_->document());

    model_ = new ResultModel(this);
    table_ = new QTableView;
    table_->setModel(model_);
    addCopyShortcut(table_);
    setZebra(table_);
    // FK arrow: SELECT the parent row(s) in a new tab
    auto* fkDelegate = new ForeignKeyDelegate(model_, table_);
    auto parentQuery = [this](const QModelIndex& index) -> QString {
        const auto& ref = model_->result().foreignRefs[model_->foreignRefFor(index.column())];
        auto q = [](const std::string& s) { return QString::fromStdString(PostgreSQL::quoteIdent(s)); };
        QStringList where;
        for (size_t i = 0; i < ref.columns.size(); ++i) {
            auto cell = model_->index(index.row(), ref.columns[i]);
            if (model_->isNull(cell)) return {}; // a NULL part references nothing
            QString v = cell.data(Qt::EditRole).toString();
            where << q(ref.targetColumns[i]) + " = '" + v.replace('\'', "''") + "'";
        }
        return QString("SELECT * FROM %1.%2 WHERE %3 LIMIT 100;").arg(q(ref.schema), q(ref.table), where.join(" AND "));
    };
    fkDelegate->follow = [this, parentQuery](const QModelIndex& index) {
        QString sql = parentQuery(index);
        if (sql.isEmpty()) status_->setText("This reference has a NULL part: no parent row");
        else if (openSql) openSql(sql);
    };
    fkDelegate->describe = [parentQuery](const QModelIndex& index) { return "Open " + parentQuery(index); };
    table_->setItemDelegate(fkDelegate);
    table_->setWordWrap(false);
    table_->horizontalHeader()->setDefaultSectionSize(140);
    table_->verticalHeader()->setDefaultSectionSize(22);

    status_ = new QLabel(QString("%1 / %2")
                             .arg(QString::fromStdString(cfg.host),
                                  QString::fromStdString(cfg.database.empty() ? "postgres" : cfg.database)));
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* toolbar = new QToolBar;
    run_ = toolbar->addAction("▶ Run");
    run_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    run_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    cancel_ = toolbar->addAction("■ Cancel");
    cancel_->setEnabled(false);
    cancel_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Period));
    cancel_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    auto* templates = new QToolButton;
    templates->setText("Template");
    templates->setPopupMode(QToolButton::InstantPopup);
    auto* templateMenu = new QMenu(templates);
    for (auto& [name, sql] : kTemplates)
        connect(templateMenu->addAction(name), &QAction::triggered, this, [this, s = QString(sql)] {
            auto c = editor_->textCursor();
            if (!c.atBlockStart()) c.insertText("\n");
            int start = c.position();
            c.insertText(s);
            int at = s.indexOf("table_name");
            if (at >= 0) {
                c.setPosition(start + at);
                c.setPosition(start + at + int(strlen("table_name")), QTextCursor::KeepAnchor);
            }
            editor_->setTextCursor(c);
            editor_->setFocus();
        });
    templates->setMenu(templateMenu);
    toolbar->addWidget(templates);
    toolbar->addSeparator();
    addRow_ = toolbar->addAction("+ Row");
    deleteRows_ = toolbar->addAction("− Row");
    deleteRows_->setToolTip("Mark selected rows for deletion (⌘⌫); again to unmark");
    deleteRows_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Backspace), QKeySequence::Delete});
    deleteRows_->setShortcutContext(Qt::WidgetShortcut); // grid only: ⌘⌫ means "delete line" in text fields
    table_->addAction(deleteRows_);
    setNull_ = toolbar->addAction("Set NULL");
    save_ = toolbar->addAction("Save");
    save_->setShortcut(QKeySequence::Save);
    discard_ = toolbar->addAction("Discard");
    toolbar->addSeparator();
    auto* exportResult = toolbar->addAction("Export…");
    exportResult->setToolTip("Save the result grid as CSV or INSERT statements");
    save_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addActions({run_, cancel_, save_}); // shortcut context is this tab, not just the toolbar

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(editor_);
    splitter->addWidget(table_);
    splitter->setStretchFactor(1, 2);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->addWidget(toolbar);
    layout->addWidget(splitter, 1);
    layout->addWidget(status_);

    connect(run_, &QAction::triggered, this, &SqlEditorTab::runQuery);
    connect(cancel_, &QAction::triggered, this, [this] { session_.cancel(); });
    connect(addRow_, &QAction::triggered, this, [this] {
        int r = model_->appendRow();
        table_->scrollToBottom();
        table_->setCurrentIndex(model_->index(r, 0));
    });
    connect(deleteRows_, &QAction::triggered, this, [this] {
        std::set<int> rows;
        for (auto& i : table_->selectionModel()->selectedIndexes()) rows.insert(i.row());
        for (int r : rows) model_->toggleDeleted(r);
    });
    connect(setNull_, &QAction::triggered, this, [this] {
        for (auto& i : table_->selectionModel()->selectedIndexes()) model_->setNull(i);
    });
    connect(save_, &QAction::triggered, this, &SqlEditorTab::save);
    connect(exportResult, &QAction::triggered, this, [this] {
        const auto& rs = model_->result();
        if (rs.columns.empty()) {
            QMessageBox::information(this, "Export", "Nothing to export yet: run a query first (⌘↵), "
                                                     "then Export saves its result grid.");
            return;
        }
        QString base = target_.table.empty() ? "result" : QString::fromStdString(target_.table);
        QString path = exportPath(this, base);
        if (path.isEmpty()) return;
        std::string table = target_.table.empty() ? "table_name"
                                                  : PostgreSQL::quoteIdent(target_.schema) + "." +
                                                        PostgreSQL::quoteIdent(target_.table);
        // ponytail: exports the rows as loaded, not pending grid edits
        auto finish = [this, path, table, rs](const std::string& structure) {
            QString err = writeExport(path, rs, table, structure);
            if (err.isEmpty()) showSaved(this, QString("Exported %1 rows.").arg(rs.rows.size()), path);
            else QMessageBox::warning(this, "Export failed", err);
        };
        if (!isJsonExport(path) || target_.table.empty()) {
            finish({}); // a query not on one table: JSON gets just the column names
            return;
        }
        session_.run(
            this, [table](PostgreSQL& pg) { return pg.tableStructureJson(table); }, finish,
            [this](const QString& msg) { QMessageBox::warning(this, "Export failed", msg); });
    });

    completions_ = new QStandardItemModel(this);
    completer_ = new QCompleter(completions_, this);
    completer_->setWidget(editor_);
    completer_->setCompletionMode(QCompleter::UnfilteredPopupCompletion); // SqlCompleter filters
    completer_->setMaxVisibleItems(12);
    connect(completer_, QOverload<const QModelIndex&>::of(&QCompleter::activated), this,
            [this](const QModelIndex& i) { insertCompletion(i.data(Qt::UserRole).toString()); });
    editor->popupVisible = [this] { return completer_->popup()->isVisible(); };
    editor->afterKey = [this](bool forced, const QString& typed) { updateCompletion(forced, typed); };
    editor->afterDoubleClick = [this] { pickWord(); };
    loadSchema();
    auto* lint = new QTimer(this);
    lint->setSingleShot(true);
    lint->setInterval(300);
    connect(lint, &QTimer::timeout, this, &SqlEditorTab::updateWarnings);
    connect(editor_, &QPlainTextEdit::textChanged, lint, qOverload<>(&QTimer::start));
    connect(discard_, &QAction::triggered, model_, &ResultModel::discard);
    connect(model_, &QAbstractItemModel::dataChanged, this, &SqlEditorTab::updateEditActions);
    connect(model_, &QAbstractItemModel::modelReset, this, &SqlEditorTab::updateEditActions);
    connect(model_, &QAbstractItemModel::rowsInserted, this, &SqlEditorTab::updateEditActions);
    updateEditActions();
    if (runNow) runQuery();
}

void SqlEditorTab::setRunning(bool running) {
    running_ = running;
    run_->setEnabled(!running);
    cancel_->setEnabled(running);
    updateEditActions();
}

void SqlEditorTab::updateEditActions() {
    bool editable = !running_ && target_.readOnlyReason.empty() && !target_.table.empty();
    bool dirty = editable && model_->hasChanges();
    for (auto* a : {addRow_, deleteRows_, setNull_}) a->setEnabled(editable);
    save_->setEnabled(dirty);
    discard_->setEnabled(dirty);
}

void SqlEditorTab::showError(const QString& msg) {
    status_->setText("<font color=red>" + msg.toHtmlEscaped().replace('\n', "<br>") + "</font>");
}

bool SqlEditorTab::confirmDiscard() {
    return !model_->hasChanges() ||
           QMessageBox::question(this, "Unsaved changes", "Discard unsaved changes to the result grid?") ==
               QMessageBox::Yes;
}

void SqlEditorTab::runQuery() {
    auto cursor = editor_->textCursor();
    // selectedText() uses U+2029 for line breaks
    QString sql = (cursor.hasSelection() ? cursor.selectedText().replace(QChar(0x2029), '\n')
                                         : editor_->toPlainText()).trimmed();
    if (sql.isEmpty() || !confirmDiscard()) return;
    execute(sql);
}

void SqlEditorTab::execute(const QString& sql, const QString& note) {
    lastSql_ = sql;
    target_ = {};
    setRunning(true);
    status_->setText("Running…");
    timer_.start();
    session_.run(
        this,
        [s = sql.toStdString()](PostgreSQL& pg) {
            auto rs = pg.execute(s);
            EditTarget t;
            try {
                t = pg.editTarget(rs);
                rs.keyFlags = pg.keyFlags(rs);
                rs.foreignRefs = pg.foreignRefs(rs);
            } catch (const std::exception& e) { // catalog lookups only cost editing / colours
                t.readOnlyReason = e.what();
            }
            return std::pair{rs, t};
        },
        [this, note](const std::pair<ResultSet, EditTarget>& res) {
            auto& [rs, target] = res;
            target_ = target;
            static const QRegularExpression ddl("^(CREATE|ALTER|DROP|COMMENT|RENAME)\\b");
            if (ddl.match(QString::fromStdString(rs.status)).hasMatch()) loadSchema();
            model_->setResult(rs);
            if (target_.readOnlyReason.empty()) {
                std::vector<bool> cols;
                for (auto& c : target_.columns) cols.push_back(!c.empty());
                model_->setEditable(cols, cols);
            }
            QString edit = rs.columns.empty() ? QString()
                           : target_.readOnlyReason.empty()
                               ? " · editable (double-click a cell)"
                               : " · read-only: " + QString::fromStdString(target_.readOnlyReason);
            status_->setText(QString("%1%2 · %3 rows · %4 ms%5")
                                 .arg(note.isEmpty() ? QString() : note + " · ", QString::fromStdString(rs.status))
                                 .arg(rs.rows.size())
                                 .arg(timer_.elapsed())
                                 .arg(edit));
            setRunning(false);
        },
        [this](const QString& msg) {
            setRunning(false);
            showError(msg);
        });
}

void SqlEditorTab::save() {
    auto changes = model_->changes();
    if (changes.empty() || !target_.readOnlyReason.empty()) return;
    const auto& rows = model_->result().rows;
    auto q = [](const std::string& s) { return PostgreSQL::quoteIdent(s); };
    std::string table = q(target_.schema) + "." + q(target_.table);
    // WHERE on the primary key's original values
    auto where = [&](int row, Params& params) {
        std::string w;
        for (int k : target_.keyColumns) {
            params.push_back(rows[row][k]);
            w += (w.empty() ? "" : " AND ") + q(target_.columns[k]) + " = $" + std::to_string(params.size());
        }
        return w;
    };

    std::vector<Statement> stmts;
    for (auto& ch : changes) {
        Statement st;
        st.expectOneRow = true;
        if (ch.deleted) {
            st.sql = "DELETE FROM " + table + " WHERE " + where(ch.row, st.params);
        } else if (ch.inserted) {
            std::string cols, vals;
            for (auto& [c, v] : ch.values) {
                st.params.push_back(v);
                cols += (cols.empty() ? "" : ", ") + q(target_.columns[c]);
                vals += (vals.empty() ? "$" : ", $") + std::to_string(st.params.size());
            }
            st.sql = "INSERT INTO " + table + (cols.empty() ? " DEFAULT VALUES" : " (" + cols + ") VALUES (" + vals + ")");
        } else {
            std::string set;
            for (auto& [c, v] : ch.values) {
                st.params.push_back(v);
                set += (set.empty() ? "" : ", ") + q(target_.columns[c]) + " = $" + std::to_string(st.params.size());
            }
            st.sql = "UPDATE " + table + " SET " + set + " WHERE " + where(ch.row, st.params);
        }
        stmts.push_back(std::move(st));
    }

    setRunning(true);
    status_->setText("Saving…");
    session_.run(
        this, [stmts](PostgreSQL& pg) { pg.executeInTransaction(stmts); },
        [this, n = stmts.size()] {
            setRunning(false);
            execute(lastSql_, QString("Saved %1 change(s)").arg(n)); // reload: defaults, triggers, ids
        },
        [this](const QString& msg) {
            setRunning(false);
            showError("Save failed, nothing was written: " + msg);
        });
}

void SqlEditorTab::loadSchema() {
    // failures only cost completion; the query itself reports connection errors
    session_.run(
        this, [](PostgreSQL& pg) { return pg.schemaInfo(); }, [this](const SchemaInfo& s) {
            schema_ = s;
            updateWarnings();
        },
        [](const QString&) {});
}

void SqlEditorTab::updateCompletion(bool forced, const QString& typed) {
    auto* popup = completer_->popup();
    QString text = editor_->toPlainText();
    int pos = editor_->textCursor().position();
    auto res = SqlCompleter::complete(text.toStdString(), size_t(text.left(pos).toUtf8().size()), schema_);

    bool typing = typed.size() == 1 && (typed[0].isLetterOrNumber() || typed == "_");
    bool show = !res.items.empty() &&
                (forced || typed == "." || (typed == " " && res.expectsName) ||
                 (!res.prefix.empty() && (typing || popup->isVisible())));
    // nothing left to complete: the word is already typed out
    if (res.items.size() == 1 && QString::fromStdString(res.items[0].insert).trimmed().compare(
                                     QString::fromStdString(res.prefix), Qt::CaseInsensitive) == 0)
        show = forced;
    if (!show) {
        popup->hide();
        return;
    }

    showCompletions(res.items, int(QString::fromStdString(res.prefix).size()));
}

void SqlEditorTab::showCompletions(const std::vector<SqlCompleter::Item>& items, int prefixLength) {
    auto* popup = completer_->popup();
    completions_->clear();
    for (auto& item : items) {
        auto* row = new QStandardItem(QString::fromStdString(item.label));
        row->setData(QString::fromStdString(item.insert), Qt::UserRole);
        static const QStyle::StandardPixmap icons[] = {QStyle::SP_CommandLink, QStyle::SP_FileIcon,
                                                       QStyle::SP_FileDialogDetailedView, QStyle::SP_ArrowRight};
        row->setIcon(style()->standardIcon(icons[item.kind]));
        static const char* kinds[] = {"keyword", "table", "column", "join on foreign key"};
        row->setToolTip(kinds[item.kind]);
        completions_->appendRow(row);
    }
    completionPrefixLength_ = prefixLength;
    QRect r = editor_->cursorRect();
    r.setWidth(std::min(640, popup->sizeHintForColumn(0) + popup->verticalScrollBar()->sizeHint().width() + 24));
    completer_->complete(r);
    popup->setCurrentIndex(completer_->completionModel()->index(0, 0));
}

void SqlEditorTab::insertCompletion(const QString& text) {
    auto cursor = editor_->textCursor();
    bool replacing = cursor.hasSelection(); // double-clicked word: swap it, keep what follows as is
    if (!replacing) cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, completionPrefixLength_);
    cursor.insertText(replacing ? text.trimmed() : text);
    editor_->setTextCursor(cursor);
    // e.g. after "FROM " go straight on to table names
    if (!replacing && text.endsWith(' ')) QTimer::singleShot(0, this, [this] { updateCompletion(false, " "); });
}

void SqlEditorTab::pickWord() {
    auto cursor = editor_->textCursor();
    if (!cursor.hasSelection()) return;
    QString text = editor_->toPlainText();
    // complete as if the word weren't typed yet: everything that fits this slot
    auto res = SqlCompleter::complete(text.toStdString(),
                                      size_t(text.left(cursor.selectionStart()).toUtf8().size()), schema_);
    std::erase_if(res.items, [](const SqlCompleter::Item& i) {
        return i.kind != SqlCompleter::Item::Table && i.kind != SqlCompleter::Item::Column;
    });
    if (!res.items.empty()) showCompletions(res.items, 0);
}

void SqlEditorTab::updateWarnings() {
    std::string sql = editor_->toPlainText().toStdString();
    QTextCharFormat warn;
    warn.setUnderlineStyle(QTextCharFormat::DashUnderline);
    warn.setUnderlineColor(QColor("#e0a800"));
    QList<QTextEdit::ExtraSelection> marks;
    for (auto [from, to] : SqlCompleter::unknownNames(sql, schema_)) {
        QTextEdit::ExtraSelection m{QTextCursor(editor_->document()), warn};
        m.cursor.setPosition(int(QString::fromUtf8(sql.data(), qsizetype(from)).size())); // bytes -> chars
        m.cursor.setPosition(int(QString::fromUtf8(sql.data(), qsizetype(to)).size()), QTextCursor::KeepAnchor);
        marks.append(m);
    }
    editor_->setExtraSelections(marks);
}
