#pragma once

#include "core/database/Database.h"

#include <QAbstractTableModel>
#include <optional>
#include <functional>
#include <map>
#include <set>

class QAbstractItemView;
class QAction;
namespace Settings { enum class CellRole; }

// ⌘C / Ctrl+C on the view copies the selected cells as TSV; returns that action.
QAction* addCopyShortcut(QAbstractItemView* view);
// Alternating row colours, a step stronger than the platform default (barely visible in dark mode).
void setZebra(QAbstractItemView* view);

// Save dialog for an export; empty if cancelled. .xlsx = Excel, .sql = INSERT statements, .json = data +
// structure, anything else CSV.
QString exportPath(QWidget* parent, const QString& baseName);
// Writes rs to path (format by extension; table is used for INSERTs, structureJson for JSON).
// Returns an error, empty on success. Plain file IO: safe on a worker thread.
QString writeExport(const QString& path, const ResultSet& rs, const std::string& table,
                    const std::string& structureJson = {});
// The JSON export carries the table structure; fetch it (PostgreSQL::tableStructureJson) first.
inline bool isJsonExport(const QString& path) { return path.endsWith(".json", Qt::CaseInsensitive); }
// "Saved" box with a Show in Finder button, so a finished export / backup can't go unnoticed.
void showSaved(QWidget* parent, const QString& message, const QString& path);

// Result grid. Editing is off until setEditable(); edits are then either recorded as pending
// changes (changes() / discard()) or, with a commit hook, handed straight to the hook.
class ResultModel : public QAbstractTableModel {
public:
    struct RowChange {
        int row;
        bool inserted = false;
        bool deleted = false;
        std::map<int, std::optional<std::string>> values; // edited columns
    };
    // Immediate mode: called on each cell edit instead of recording it; return false to reject.
    using CommitHook = std::function<bool(int row, int col, const std::optional<std::string>& value)>;

    using QAbstractTableModel::QAbstractTableModel;

    void setResult(ResultSet rs); // also clears edits and editability
    const ResultSet& result() const { return rs_; }
    // The selected rows x selected columns (current values, edits included) as a result of their own.
    ResultSet selection(const QModelIndexList& indexes) const;
    // index into result().foreignRefs for a column that's part of a navigable FK, else -1
    int foreignRefFor(int col) const;
    bool isNull(const QModelIndex& index) const { return !value(index.row(), index.column()); }
    // Editable columns for existing rows and for rows added with appendRow().
    void setEditable(std::vector<bool> existing, std::vector<bool> inserted);
    void setCommitHook(CommitHook hook) { hook_ = std::move(hook); }

    int appendRow();
    void toggleDeleted(const std::set<int>& rows);
    void setNull(const QModelIndexList& indexes);
    std::vector<RowChange> changes() const;
    bool hasChanges() const { return !changes().empty(); }
    void discard();
    // Steps back one pending change (an edit, Set NULL, + Row, − Row); false when there's none.
    bool undo();
    bool canUndo() const { return !undo_.empty(); }

    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    bool editable(int row, int col) const;
    std::optional<Settings::CellRole> cellRole(int row, int col) const;
    const std::optional<std::string>& value(int row, int col) const;
    bool store(const QModelIndex& index, const std::optional<std::string>& v);
    void remember();

    struct Snapshot { // pending state before one user action
        std::map<std::pair<int, int>, std::optional<std::string>> edits;
        std::set<int> deleted;
        size_t rows;
    };

    ResultSet rs_;
    size_t originalRows_ = 0;
    std::vector<bool> editExisting_, editInserted_;
    std::map<std::pair<int, int>, std::optional<std::string>> edits_;
    std::set<int> deleted_;
    std::vector<Snapshot> undo_; // ponytail: full copies of the pending edits; fine for hand-made changes
    CommitHook hook_;
};
