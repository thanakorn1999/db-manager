#include "ResultModel.h"

#include "core/export/Export.h"

#include "ui/Settings.h"

#include <QAbstractItemView>
#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDesktopServices>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <QClipboard>
#include <QColor>
#include <QFont>
#include <QPainter>
#include <QStyle>
#include <QPalette>
#include <QPixmap>
#include <QApplication>
#include <QGuiApplication>
#include <algorithm>

QString exportPath(QWidget* parent, const QString& baseName) {
    return QFileDialog::getSaveFileName(parent, "Export", QDir::home().filePath(baseName + ".csv"),
                                        "CSV (*.csv);;Excel (*.xlsx);;JSON: data + structure (*.json);;SQL INSERT statements (*.sql)");
}

QString writeExport(const QString& path, const ResultSet& rs, const std::string& table,
                    const std::string& structureJson) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return f.errorString();
    std::string data;
    try {
        if (path.endsWith(".sql", Qt::CaseInsensitive)) data = Export::inserts(rs, table);
        else if (isJsonExport(path)) data = Export::json(rs, structureJson);
        else if (path.endsWith(".xlsx", Qt::CaseInsensitive)) data = Export::xlsx(rs, QFileInfo(path).completeBaseName().toStdString());
        else data = Export::csv(rs);
    } catch (const std::exception& e) {
        return e.what();
    }
    if (f.write(data.data(), qint64(data.size())) != qint64(data.size())) return f.errorString();
    return {};
}

void showSaved(QWidget* parent, const QString& message, const QString& path) {
    QMessageBox box(QMessageBox::Information, "Saved", message, QMessageBox::Ok, parent);
    box.setInformativeText(path);
    auto* reveal = box.addButton("Show in Finder", QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() != reveal) return;
#ifdef Q_OS_MACOS
    QProcess::startDetached("open", {"-R", path});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

void setZebra(QAbstractItemView* view) {
    // ponytail: computed once from the current palette; a light/dark switch needs a new tab
    QPalette pal = view->palette();
    QColor base = pal.color(QPalette::Base);
    pal.setColor(QPalette::AlternateBase, base.lightness() < 128 ? base.lighter(160) : base.darker(108));
    view->setPalette(pal);
    view->setAlternatingRowColors(true);
}

void addCopyShortcut(QAbstractItemView* view) {
    auto* copy = new QAction("Copy", view);
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetShortcut);
    QObject::connect(copy, &QAction::triggered, view, [view] {
        auto cells = view->selectionModel()->selectedIndexes();
        if (cells.isEmpty()) return;
        std::sort(cells.begin(), cells.end(), [](auto& a, auto& b) {
            return a.row() != b.row() ? a.row() < b.row() : a.column() < b.column();
        });
        QString out;
        for (int i = 0; i < cells.size(); ++i) {
            if (i) out += cells[i].row() != cells[i - 1].row() ? '\n' : '\t';
            out += cells[i].data(Qt::EditRole).toString();
        }
        QGuiApplication::clipboard()->setText(out);
    });
    view->addAction(copy);
}

void ResultModel::setResult(ResultSet rs) {
    beginResetModel();
    rs_ = std::move(rs);
    originalRows_ = rs_.rows.size();
    editExisting_.clear();
    editInserted_.clear();
    edits_.clear();
    deleted_.clear();
    undo_.clear();
    endResetModel();
}

void ResultModel::setEditable(std::vector<bool> existing, std::vector<bool> inserted) {
    editExisting_ = std::move(existing);
    editInserted_ = std::move(inserted);
}

bool ResultModel::editable(int row, int col) const {
    const auto& cols = size_t(row) < originalRows_ ? editExisting_ : editInserted_;
    return size_t(col) < cols.size() && cols[col] && !deleted_.count(row);
}

const std::optional<std::string>& ResultModel::value(int row, int col) const {
    auto it = edits_.find({row, col});
    return it != edits_.end() ? it->second : rs_.rows[row][col];
}

int ResultModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(rs_.rows.size());
}

int ResultModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(rs_.columns.size());
}

namespace {
constexpr unsigned kBoolOid = 16;

std::optional<bool> boolValue(const std::string& v) {
    QString s = QString::fromStdString(v).toLower();
    if (s == "t" || s == "true") return true;
    if (s == "f" || s == "false") return false;
    return std::nullopt;
}
}

// Which user colour applies: value kinds (NULL / empty / TRUE / FALSE), then "not important" columns, then keys.
std::optional<Settings::CellRole> ResultModel::cellRole(int row, int col) const {
    using R = Settings::CellRole;
    const auto& cell = value(row, col);
    if (!cell) return R::Null;
    if (cell->empty()) return R::Empty;
    if (size_t(col) < rs_.columnType.size() && rs_.columnType[col] == kBoolOid)
        if (auto b = boolValue(*cell)) return *b ? R::True : R::False;
    // a user rule beats the automatic key colours
    if (size_t(col) < rs_.columns.size() && Settings::isUnimportant(QString::fromStdString(rs_.columns[col])))
        return R::Unimportant;
    uint8_t keys = size_t(col) < rs_.keyFlags.size() ? rs_.keyFlags[col] : 0;
    if (keys & KeyPrimary) return R::PrimaryKey;
    if (keys & KeyForeign) return R::ForeignKey;
    if (keys & KeyIndexed) return R::Indexed;
    return std::nullopt;
}

QVariant ResultModel::data(const QModelIndex& index, int role) const {
    int r = index.row(), c = index.column();
    const auto& cell = value(r, c);
    auto cellStyle = [&]() -> Settings::CellStyle {
        auto cr = cellRole(r, c);
        return cr ? Settings::style(*cr) : Settings::CellStyle{};
    };
    switch (role) {
    case Qt::DisplayRole: {
        if (!cell) return QStringLiteral("NULL");
        if (cell->empty()) return QStringLiteral("<EMPTY>"); // placeholder only; copy / edit see ""
        if (auto cr = cellRole(r, c); cr == Settings::CellRole::True || cr == Settings::CellRole::False)
            return cr == Settings::CellRole::True ? QStringLiteral("true") : QStringLiteral("false");
        // keep the grid single-line; full value is in the tooltip
        QString s = QString::fromStdString(*cell);
        s.replace('\n', QChar(0x21B5));
        return s.size() > 300 ? s.left(300) + QChar(0x2026) : s;
    }
    case Qt::ToolTipRole:
        if (cell && cell->empty()) return QStringLiteral("Empty string ('')");
        return cell ? QString::fromStdString(cell->substr(0, 4000)) : QVariant();
    case Qt::ForegroundRole: {
        auto color = cellStyle().text;
        return color.isValid() ? QVariant(color) : QVariant();
    }
    case Qt::EditRole: return cell ? QString::fromStdString(*cell) : QString();
    case Qt::BackgroundRole:
        if (deleted_.count(r)) return QColor(220, 60, 60, 70);
        if (size_t(r) >= originalRows_) return QColor(60, 180, 90, 60);
        if (edits_.count({r, c})) return QColor(230, 180, 40, 80);
        if (auto bg = cellStyle().background; bg.isValid()) return bg;
        return {};
    case Qt::FontRole: {
        if (!deleted_.count(r)) return {};
        QFont f;
        f.setStrikeOut(true);
        return f;
    }
    default: return {};
    }
}

Qt::ItemFlags ResultModel::flags(const QModelIndex& index) const {
    auto f = QAbstractTableModel::flags(index);
    return editable(index.row(), index.column()) ? f | Qt::ItemIsEditable : f;
}

bool ResultModel::setData(const QModelIndex& index, const QVariant& v, int role) {
    if (role != Qt::EditRole) return false;
    std::string s = v.toString().toStdString();
    // an editor opened on NULL and closed untouched hands back "": keep the NULL
    if (!value(index.row(), index.column()) && s.empty()) return false;
    remember();
    if (store(index, s)) return true;
    if (!hook_) undo_.pop_back(); // nothing changed
    return false;
}

void ResultModel::setNull(const QModelIndexList& indexes) {
    remember();
    bool changed = false;
    for (auto& i : indexes) changed |= store(i, std::nullopt);
    if (!changed && !hook_) undo_.pop_back();
}

void ResultModel::remember() {
    if (!hook_) undo_.push_back({edits_, deleted_, rs_.rows.size()}); // immediate mode: already written
}

bool ResultModel::undo() {
    if (undo_.empty()) return false;
    auto s = std::move(undo_.back());
    undo_.pop_back();
    if (s.rows < rs_.rows.size()) { // only + Row adds rows
        beginRemoveRows({}, int(s.rows), int(rs_.rows.size()) - 1);
        rs_.rows.resize(s.rows);
        endRemoveRows();
    }
    edits_ = std::move(s.edits);
    deleted_ = std::move(s.deleted);
    if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
    return true;
}

bool ResultModel::store(const QModelIndex& index, const std::optional<std::string>& v) {
    int r = index.row(), c = index.column();
    if (!editable(r, c) || value(r, c) == v) return false;
    if (hook_) {
        if (!hook_(r, c, v)) return false;
        rs_.rows[r][c] = v; // optimistic; the owner reloads after the write lands
    } else if (size_t(r) < originalRows_ && rs_.rows[r][c] == v) {
        edits_.erase({r, c}); // edited back to the original
    } else {
        edits_[{r, c}] = v;
    }
    emit dataChanged(index, index);
    return true;
}

int ResultModel::appendRow() {
    remember();
    int r = int(rs_.rows.size());
    beginInsertRows({}, r, r);
    rs_.rows.emplace_back(rs_.columns.size(), std::nullopt);
    endInsertRows();
    return r;
}

void ResultModel::toggleDeleted(const std::set<int>& rows) {
    if (rows.empty()) return;
    remember();
    for (int row : rows) {
        if (!deleted_.erase(row)) deleted_.insert(row);
        emit dataChanged(index(row, 0), index(row, columnCount() - 1));
    }
}

std::vector<ResultModel::RowChange> ResultModel::changes() const {
    std::map<int, RowChange> byRow;
    for (auto& [rc, v] : edits_) byRow[rc.first].values[rc.second] = v;
    for (size_t r = originalRows_; r < rs_.rows.size(); ++r) byRow[int(r)].inserted = true;
    for (int r : deleted_) byRow[r].deleted = true;
    std::vector<RowChange> out;
    for (auto& [r, ch] : byRow) {
        if (ch.inserted && ch.deleted) continue; // added then removed: nothing to do
        ch.row = r;
        out.push_back(std::move(ch));
    }
    return out;
}

void ResultModel::discard() {
    beginResetModel();
    rs_.rows.resize(originalRows_);
    edits_.clear();
    deleted_.clear();
    undo_.clear();
    endResetModel();
}

int ResultModel::foreignRefFor(int col) const {
    for (size_t i = 0; i < rs_.foreignRefs.size(); ++i)
        for (int c : rs_.foreignRefs[i].columns)
            if (c == col) return int(i);
    return -1;
}

// Header tags of a column, in display order: PK, FK, UNIQUE, INDEX (index only when not implied).
static std::vector<Settings::Tag> columnTags(uint8_t keys) {
    using T = Settings::Tag;
    std::vector<T> tags;
    if (keys & KeyPrimary) tags.push_back(T::PrimaryKey);
    if (keys & KeyForeign) tags.push_back(T::ForeignKey);
    if ((keys & KeyUnique) && !(keys & KeyPrimary)) tags.push_back(T::Unique);
    if ((keys & KeyIndexed) && !(keys & (KeyPrimary | KeyUnique))) tags.push_back(T::Index);
    return tags;
}

QVariant ResultModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Vertical)
        return role == Qt::DisplayRole ? (size_t(section) < originalRows_ ? QVariant(section + 1) : QVariant("+"))
                                       : QVariant();
    uint8_t keys = size_t(section) < rs_.keyFlags.size() ? rs_.keyFlags[section] : 0;
    switch (role) {
    case Qt::DisplayRole: return QString::fromStdString(rs_.columns[section]);
    case Qt::DecorationRole: { // one dot per tag, in the user's tag colours
        std::vector<QColor> dots;
        for (auto t : columnTags(keys))
            if (auto c = Settings::tagColor(t); c.isValid()) dots.push_back(c);
        if (dots.empty()) return {};
        // full icon height so the header doesn't scale the 8px dots up
        const qreal dpr = qApp->devicePixelRatio(), d = 8, gap = 3;
        const qreal h = qApp->style()->pixelMetric(QStyle::PM_SmallIconSize);
        QPixmap pm(QSize(int((dots.size() * (d + gap) - gap) * dpr), int(h * dpr)));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        for (size_t i = 0; i < dots.size(); ++i) {
            p.setBrush(dots[i]);
            p.drawEllipse(QRectF(i * (d + gap), (h - d) / 2, d, d));
        }
        return pm;
    }
    case Qt::ToolTipRole: {
        QStringList parts;
        for (auto t : columnTags(keys)) {
            QString part = Settings::tagName(t);
            if (int fk = foreignRefFor(section); t == Settings::Tag::ForeignKey && fk >= 0) {
                const auto& ref = rs_.foreignRefs[fk];
                QStringList cols;
                for (auto& c : ref.targetColumns) cols << QString::fromStdString(c);
                part += QString(" → %1.%2(%3)").arg(QString::fromStdString(ref.schema), QString::fromStdString(ref.table),
                                                   cols.join(", "));
            }
            parts << part;
        }
        return parts.isEmpty() ? QVariant() : parts.join(" · ");
    }
    default: return {};
    }
}
