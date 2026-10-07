#pragma once

#include <QColor>
#include <QString>

class QWidget;

// User colours for result-grid values, stored in QSettings.
namespace Settings {

enum class CellRole { PrimaryKey, ForeignKey, Indexed, Unimportant, True, False, Null, Empty, Count };

struct CellStyle {
    QColor text;       // invalid = default text colour
    QColor background; // invalid = no background
};

const CellStyle& style(CellRole role);

// Coloured dots in the column header marking key / index columns.
enum class Tag { PrimaryKey, ForeignKey, Unique, Index, Count };
QColor tagColor(Tag tag); // invalid = tag hidden
QString tagName(Tag tag);

// Column name matches one of the user's "not important" regexes (case-insensitive, e.g. created_at).
bool isUnimportant(const QString& column);

// Modal settings window; repaints every view when saved.
void showDialog(QWidget* parent);

}
