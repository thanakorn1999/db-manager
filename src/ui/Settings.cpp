#include "Settings.h"

#include <QApplication>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QHBoxLayout>
#include <QHash>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QRegularExpression>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <array>

namespace Settings {
namespace {

constexpr int kRoles = int(CellRole::Count);
const char* kKeys[kRoles] = {"primaryKey", "foreignKey", "indexed", "unimportant", "true", "false", "null", "empty"};
const char* kNames[kRoles] = {"Primary key", "Foreign key", "Indexed column", "Not important", "TRUE", "FALSE", "NULL", "Empty text"};
const char* kSamples[kRoles] = {"1", "42", "abc", "2026-10-06", "true", "false", "NULL", "<EMPTY>"};
const char* kPatternsKey = "columns/unimportantPatterns";
const QStringList kDefaultPatterns = {"^(created|updated|deleted)_(at|by)$"};

CellStyle defaults(int role) {
    switch (CellRole(role)) {
    case CellRole::PrimaryKey: // keys are marked by header tags by default, not cell colours
    case CellRole::ForeignKey:
    case CellRole::Indexed: return {};
    case CellRole::Unimportant: return {QColor("#6e7681"), {}};
    case CellRole::True: return {QColor("#396c42"), {}};
    case CellRole::False: return {QColor("#80423a"), {}};
    case CellRole::Null: return {QColor(Qt::gray), {}};
    case CellRole::Empty: return {QColor(Qt::gray), {}};
    default: return {};
    }
}

// Stored as "#aarrggbb"; "" = none (the role was cleared on purpose), missing key = default.
QColor load(QSettings& s, const QString& key, QColor fallback) {
    if (!s.contains(key)) return fallback;
    return QColor(s.value(key).toString());
}
QString save(const QColor& c) { return c.isValid() ? c.name(QColor::HexArgb) : QString(); }
// What the code fields show: #rrggbb, or #aarrggbb when translucent; "" = none
QString code(const QColor& c) {
    return !c.isValid() ? QString() : c.alpha() == 255 ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb);
}

std::array<CellStyle, kRoles>& cache() {
    static std::array<CellStyle, kRoles> styles = [] {
        std::array<CellStyle, kRoles> out;
        QSettings s;
        for (int i = 0; i < kRoles; ++i) {
            auto d = defaults(i);
            out[i] = {load(s, QString("colors/%1/text").arg(kKeys[i]), d.text),
                      load(s, QString("colors/%1/background").arg(kKeys[i]), d.background)};
        }
        return out;
    }();
    return styles;
}

QStringList& patterns() {
    static QStringList p = QSettings().value(kPatternsKey, kDefaultPatterns).toStringList();
    return p;
}
// column name -> matches; cleared when the patterns change
QHash<QString, bool>& matchCache() {
    static QHash<QString, bool> c;
    return c;
}

void setPreview(QLabel* label, const CellStyle& st) {
    label->setStyleSheet(QString("padding: 2px 10px; border: 1px solid palette(mid); %1 %2")
                             .arg(st.text.isValid() ? "color: " + st.text.name(QColor::HexArgb) + ";" : "",
                                  st.background.isValid()
                                      ? "background: " + st.background.name(QColor::HexArgb) + ";"
                                      : ""));
}

} // namespace

const CellStyle& style(CellRole role) { return cache()[int(role)]; }

namespace {
constexpr int kTags = int(Tag::Count);
const char* kTagKeys[kTags] = {"primaryKey", "foreignKey", "unique", "index"};
const char* kTagNames[kTags] = {"PK", "FK", "UNIQUE", "INDEX"};
const QColor kTagDefaults[kTags] = {QColor("#d4a72c"), QColor("#4c9aff"), QColor("#a371f7"), QColor("#3fb9a0")};

std::array<QColor, kTags>& tagCache() {
    static std::array<QColor, kTags> tags = [] {
        std::array<QColor, kTags> out;
        QSettings s;
        for (int i = 0; i < kTags; ++i) out[i] = load(s, QString("tags/%1").arg(kTagKeys[i]), kTagDefaults[i]);
        return out;
    }();
    return tags;
}

void setDot(QLabel* label, const QColor& c) {
    label->setText(c.isValid() ? QString("<span style='color:%1; font-size:16px'>●</span>").arg(c.name(QColor::HexArgb))
                               : QString("<span style='color:gray'>hidden</span>"));
}
}

QColor tagColor(Tag tag) { return tagCache()[int(tag)]; }
QString tagName(Tag tag) { return kTagNames[int(tag)]; }

bool isUnimportant(const QString& column) {
    auto& c = matchCache();
    auto it = c.constFind(column);
    if (it != c.constEnd()) return *it;
    bool hit = std::any_of(patterns().begin(), patterns().end(), [&](const QString& p) {
        return QRegularExpression(p, QRegularExpression::CaseInsensitiveOption).match(column).hasMatch();
    });
    c.insert(column, hit);
    return hit;
}

void showDialog(QWidget* parent) {
    QDialog dlg(parent);
    dlg.setWindowTitle("Settings");
    auto edited = cache();

    auto* box = new QGroupBox("Result grid colours");
    auto* grid = new QGridLayout(box);
    grid->addWidget(new QLabel("<b>Value</b>"), 0, 0);
    grid->addWidget(new QLabel("<b>Preview</b>"), 0, 1);
    grid->addWidget(new QLabel("<b>Text</b>"), 0, 2, 1, 2);
    grid->addWidget(new QLabel("<b>Background</b>"), 0, 4, 1, 2);
    for (int i = 0; i < kRoles; ++i) {
        int row = i + 1;
        auto* preview = new QLabel(kSamples[i]);
        preview->setMinimumWidth(90);
        // colour code fields: type / paste a hex code (or a name like "red"); empty = none
        auto* textCode = new QLineEdit;
        auto* bgCode = new QLineEdit;
        for (auto* e : {textCode, bgCode}) {
            e->setPlaceholderText("none");
            e->setFixedWidth(100);
            e->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        }
        auto refresh = [&edited, preview, textCode, bgCode, i] {
            setPreview(preview, edited[i]);
            textCode->setText(code(edited[i].text));
            bgCode->setText(code(edited[i].background));
        };
        refresh();
        for (bool isText : {true, false}) {
            auto* e = isText ? textCode : bgCode;
            QObject::connect(e, &QLineEdit::textEdited, &dlg, [&edited, preview, e, i, isText](const QString& t) {
                QString v = t.trimmed();
                bool ok = v.isEmpty() || QColor::isValidColorName(v);
                e->setStyleSheet(ok ? QString() : "color: #e5534b;"); // red while not a colour yet
                if (!ok) return;
                (isText ? edited[i].text : edited[i].background) = v.isEmpty() ? QColor() : QColor(v);
                setPreview(preview, edited[i]);
            });
            QObject::connect(e, &QLineEdit::editingFinished, &dlg, [e, refresh] {
                e->setStyleSheet({});
                refresh(); // normalise (e.g. "red" -> #ff0000) or undo an invalid entry
            });
        }
        auto pick = [&dlg, &edited, refresh, i](bool text) {
            QColor& c = text ? edited[i].text : edited[i].background;
            QColor picked = QColorDialog::getColor(c.isValid() ? c : QColor(Qt::white), &dlg,
                                                   QString("%1 %2").arg(kNames[i], text ? "text" : "background"),
                                                   QColorDialog::ShowAlphaChannel);
            if (!picked.isValid()) return; // cancelled
            c = picked;
            refresh();
        };
        auto* text = new QPushButton("…");
        text->setToolTip("Pick the text colour");
        auto* background = new QPushButton("…");
        background->setToolTip("Pick the background colour");
        auto* clear = new QPushButton("None");
        clear->setToolTip("No colour: plain text, no background");
        auto* reset = new QPushButton("Default");
        QObject::connect(text, &QPushButton::clicked, &dlg, [pick] { pick(true); });
        QObject::connect(background, &QPushButton::clicked, &dlg, [pick] { pick(false); });
        QObject::connect(clear, &QPushButton::clicked, &dlg, [&edited, refresh, i] {
            edited[i] = {};
            refresh();
        });
        QObject::connect(reset, &QPushButton::clicked, &dlg, [&edited, refresh, i] {
            edited[i] = defaults(i);
            refresh();
        });
        grid->addWidget(new QLabel(kNames[i]), row, 0);
        grid->addWidget(preview, row, 1);
        grid->addWidget(textCode, row, 2);
        grid->addWidget(text, row, 3);
        grid->addWidget(bgCode, row, 4);
        grid->addWidget(background, row, 5);
        grid->addWidget(clear, row, 6);
        grid->addWidget(reset, row, 7);
    }
    // header tags: one colour each, shown as a dot before the column name
    auto editedTags = tagCache();
    auto* tagBox = new QGroupBox("Column tags (dots in the column header)");
    auto* tagGrid = new QGridLayout(tagBox);
    for (int i = 0; i < kTags; ++i) {
        auto* dot = new QLabel;
        auto* codeEdit = new QLineEdit;
        codeEdit->setPlaceholderText("hidden");
        codeEdit->setFixedWidth(100);
        codeEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        auto refresh = [&editedTags, dot, codeEdit, i] {
            setDot(dot, editedTags[i]);
            codeEdit->setText(code(editedTags[i]));
        };
        refresh();
        QObject::connect(codeEdit, &QLineEdit::textEdited, &dlg, [&editedTags, dot, codeEdit, i](const QString& t) {
            QString v = t.trimmed();
            bool ok = v.isEmpty() || QColor::isValidColorName(v);
            codeEdit->setStyleSheet(ok ? QString() : "color: #e5534b;");
            if (!ok) return;
            editedTags[i] = v.isEmpty() ? QColor() : QColor(v);
            setDot(dot, editedTags[i]);
        });
        QObject::connect(codeEdit, &QLineEdit::editingFinished, &dlg, [codeEdit, refresh] {
            codeEdit->setStyleSheet({});
            refresh();
        });
        auto* pick = new QPushButton("…");
        pick->setToolTip(QString("Pick the %1 tag colour").arg(kTagNames[i]));
        QObject::connect(pick, &QPushButton::clicked, &dlg, [&dlg, &editedTags, refresh, i] {
            QColor picked = QColorDialog::getColor(editedTags[i].isValid() ? editedTags[i] : kTagDefaults[i], &dlg,
                                                   QString("%1 tag").arg(kTagNames[i]));
            if (!picked.isValid()) return;
            editedTags[i] = picked;
            refresh();
        });
        auto* hide = new QPushButton("Hide");
        QObject::connect(hide, &QPushButton::clicked, &dlg, [&editedTags, refresh, i] {
            editedTags[i] = {};
            refresh();
        });
        auto* reset = new QPushButton("Default");
        QObject::connect(reset, &QPushButton::clicked, &dlg, [&editedTags, refresh, i] {
            editedTags[i] = kTagDefaults[i];
            refresh();
        });
        tagGrid->addWidget(new QLabel(kTagNames[i]), i, 0);
        tagGrid->addWidget(dot, i, 1);
        tagGrid->addWidget(codeEdit, i, 2);
        tagGrid->addWidget(pick, i, 3);
        tagGrid->addWidget(hide, i, 4);
        tagGrid->addWidget(reset, i, 5);
    }
    tagGrid->setColumnStretch(6, 1);

    auto* note = new QLabel("Edited / new / deleted rows keep their highlight on top of these colours.");
    note->setForegroundRole(QPalette::PlaceholderText);

    // "not important" columns: regexes on the column name, coloured with the Not important style
    auto* patternBox = new QGroupBox("Not important columns (regex on column name, case-insensitive)");
    auto* list = new QListWidget;
    auto addRow = [list](const QString& p) {
        auto* item = new QListWidgetItem(p, list);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        return item;
    };
    for (auto& p : patterns()) addRow(p);
    list->setMaximumHeight(120);
    auto* add = new QPushButton("+ Add");
    auto* remove = new QPushButton("− Remove");
    auto* restore = new QPushButton("Default");
    restore->setToolTip(kDefaultPatterns.join("\n"));
    auto* test = new QLineEdit;
    test->setPlaceholderText("Try a column name, e.g. created_at");
    auto* testResult = new QLabel;
    auto current = [list] {
        QStringList out;
        for (int i = 0; i < list->count(); ++i)
            if (auto t = list->item(i)->text().trimmed(); !t.isEmpty()) out << t;
        return out;
    };
    auto check = [list, test, testResult, current] {
        for (int i = 0; i < list->count(); ++i) { // invalid regex: red until fixed
            auto* item = list->item(i);
            bool ok = QRegularExpression(item->text()).isValid();
            item->setForeground(ok ? list->palette().text() : QBrush(QColor("#e5534b")));
            item->setToolTip(ok ? QString() : QRegularExpression(item->text()).errorString());
        }
        QString name = test->text().trimmed();
        QString hit;
        for (auto& p : current())
            if (QRegularExpression(p, QRegularExpression::CaseInsensitiveOption).match(name).hasMatch()) hit = p;
        testResult->setText(name.isEmpty() ? QString() : hit.isEmpty() ? "not matched" : "✓ matched by " + hit);
    };
    QObject::connect(add, &QPushButton::clicked, &dlg, [list, addRow] {
        auto* item = addRow("^column_name$");
        list->setCurrentItem(item);
        list->editItem(item);
    });
    QObject::connect(remove, &QPushButton::clicked, &dlg, [list] { delete list->currentItem(); });
    QObject::connect(restore, &QPushButton::clicked, &dlg, [list, addRow, check] {
        list->clear();
        for (auto& p : kDefaultPatterns) addRow(p);
        check();
    });
    QObject::connect(list, &QListWidget::itemChanged, &dlg, check);
    QObject::connect(test, &QLineEdit::textChanged, &dlg, check);
    check();
    auto* patternButtons = new QHBoxLayout;
    patternButtons->addWidget(add);
    patternButtons->addWidget(remove);
    patternButtons->addWidget(restore);
    patternButtons->addStretch();
    auto* testRow = new QHBoxLayout;
    testRow->addWidget(test);
    testRow->addWidget(testResult, 1);
    auto* patternLayout = new QVBoxLayout(patternBox);
    patternLayout->addWidget(list);
    patternLayout->addLayout(patternButtons);
    patternLayout->addLayout(testRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&dlg, current] {
        for (auto& p : current())
            if (QRegularExpression re(p); !re.isValid()) {
                QMessageBox::warning(&dlg, "Settings", QString("Not a valid regex: %1\n%2").arg(p, re.errorString()));
                return;
            }
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(tagBox);
    layout->addWidget(box);
    layout->addWidget(note);
    layout->addWidget(patternBox);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    QSettings s;
    for (int i = 0; i < kRoles; ++i) {
        s.setValue(QString("colors/%1/text").arg(kKeys[i]), save(edited[i].text));
        s.setValue(QString("colors/%1/background").arg(kKeys[i]), save(edited[i].background));
    }
    cache() = edited;
    for (int i = 0; i < kTags; ++i) s.setValue(QString("tags/%1").arg(kTagKeys[i]), save(editedTags[i]));
    tagCache() = editedTags;
    patterns() = current();
    s.setValue(kPatternsKey, patterns());
    matchCache().clear();
    for (auto* w : QApplication::allWidgets()) w->update(); // grids read colours at paint time
}

}
