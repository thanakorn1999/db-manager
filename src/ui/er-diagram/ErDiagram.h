#pragma once

#include "core/database/Database.h"

#include <QString>

class QWidget;

// Read-only ER diagram of schema's tables and foreign keys. Boxes can be dragged; ⌘-scroll / pinch zooms.
// focusTable (optional) is selected and centred.
QWidget* makeErDiagram(const SchemaInfo& schema, const QString& focusTable = {});
