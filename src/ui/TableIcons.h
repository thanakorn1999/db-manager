#pragma once

#include <QIcon>
#include <QString>

class QWidget;

// Emoji icons for tables in the explorer: the user's pick, else one guessed from the name
// (users → 👤, orders → 🧾 …), else the plain file icon.
namespace TableIcons {

QString detect(const QString& table);                         // guessed emoji, "" if nothing fits
QString chosen(const QString& schema, const QString& table);  // user's pick, "" = automatic
QIcon icon(const QString& schema, const QString& table);
// Picker dialog; returns true when the choice changed.
bool pick(QWidget* parent, const QString& schema, const QString& table);

}
