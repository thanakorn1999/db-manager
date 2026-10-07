#include "ui/MainWindow.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QTextEdit>

namespace {

// macOS-standard line delete in every text field: ⌘⌫ to line start, ⌘⌦ to line end (⌥⌫ word delete is Qt-native).
class LineDeleteFilter : public QObject {
protected:
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() != QEvent::KeyPress) return false;
        auto* k = static_cast<QKeyEvent*>(e);
        bool back = k->key() == Qt::Key_Backspace;
        if ((!back && k->key() != Qt::Key_Delete) || k->modifiers() != Qt::ControlModifier) return false; // Control = ⌘
        if (auto* le = qobject_cast<QLineEdit*>(obj)) {
            if (le->isReadOnly()) return true;
            if (!le->hasSelectedText()) back ? le->home(true) : le->end(true);
            if (le->hasSelectedText()) le->del();
            return true;
        }
        QTextCursor c;
        if (auto* pe = qobject_cast<QPlainTextEdit*>(obj)) {
            if (pe->isReadOnly()) return true;
            c = pe->textCursor();
        } else if (auto* te = qobject_cast<QTextEdit*>(obj)) {
            if (te->isReadOnly()) return true;
            c = te->textCursor();
        } else {
            return false;
        }
        if (!c.hasSelection()) c.movePosition(back ? QTextCursor::StartOfLine : QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
        if (c.hasSelection()) c.removeSelectedText();
        else back ? c.deletePreviousChar() : c.deleteChar(); // already at the edge: join lines, like macOS
        return true;
    }
};

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("db-manager");
    QApplication::setApplicationName("DB Manager");
    LineDeleteFilter lineDelete;
    app.installEventFilter(&lineDelete);

    MainWindow w;
    w.resize(1280, 800);
    w.show();
    return app.exec();
}
