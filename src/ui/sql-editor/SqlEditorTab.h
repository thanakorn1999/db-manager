#pragma once

#include "core/connection/Session.h"
#include "core/database/postgres/PostgreSQL.h"
#include "core/query/SqlCompleter.h"

#include <QElapsedTimer>
#include <QWidget>

class QAction;
class QComboBox;
class QCompleter;
class QLineEdit;
class QStandardItemModel;
class QLabel;
class QPlainTextEdit;
class QTableView;
class ResultModel;

// SQL editor + result grid on its own PostgreSQL connection.
class SqlEditorTab : public QWidget {
public:
    SqlEditorTab(const ConnectionConfig& cfg, const QString& sql, bool runNow, QWidget* parent = nullptr);

    // Asks before throwing away unsaved grid edits; true = OK to proceed.
    bool confirmDiscard();

    // Opens SQL in a new tab on the same database (set by the owner); used to follow FK arrows.
    std::function<void(const QString& sql)> openSql;

private:
    void runQuery();
    // replaces the editor text as one undo step
    void setSql(const QString& sql);
    // ANDs "column op value" into the SQL's WHERE and runs it (see SqlCompleter::addFilter)
    void applyFilter(int column, const QString& op, const QString& value);
    void showFilterBar(int column);
    void fillFilterColumns();
    void execute(const QString& sql, const QString& note = {});
    void save();
    void setRunning(bool running);
    void updateEditActions();
    void showError(const QString& msg);
    void loadSchema();
    // forced: ⌃Space; typed: the text of the key just pressed
    void updateCompletion(bool forced, const QString& typed);
    void showCompletions(const std::vector<SqlCompleter::Item>& items, int prefixLength);
    void insertCompletion(const QString& text);
    // double-click on a table / WHERE column name: pick a replacement from the schema
    void pickWord();
    // dashed yellow underline under tables / columns the schema doesn't have
    void updateWarnings();

    QPlainTextEdit* editor_;
    QTableView* table_;
    ResultModel* model_;
    QLabel* status_;
    QAction* run_;
    QAction* cancel_;
    QAction* rerun_;
    QAction* addRow_;
    QAction* deleteRows_;
    QAction* setNull_;
    QAction* save_;
    QAction* discard_;
    QAction* undo_;
    QWidget* filterBar_;
    QComboBox* filterColumn_; // result column index as item data
    QComboBox* filterOp_;
    QLineEdit* filterValue_;
    QAction* clearFilter_;
    QString filterBase_; // the SQL before the first filter; null = not filtered
    bool running_ = false;
    QString lastSql_;
    EditTarget target_;
    SchemaInfo schema_;
    QCompleter* completer_;
    QStandardItemModel* completions_;
    int completionPrefixLength_ = 0;
    QElapsedTimer timer_;
    Session<PostgreSQL> session_;
};
