#include "ErDiagram.h"

#include <QApplication>
#include <QFontMetrics>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QNativeGestureEvent>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace {

constexpr qreal kHeaderH = 26, kRowH = 20, kPad = 8, kGapX = 90, kGapY = 30;
constexpr qreal kMaxColumnH = 1400; // a layer taller than this wraps into another column

class Edge;

class Box : public QGraphicsRectItem {
public:
    std::vector<Edge*> edges;

    Box(const SchemaInfo::Table& t, const QString& title, const std::set<std::string>& fkCols, const QPalette& pal) {
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        QFont bold = QApplication::font();
        bold.setBold(true);
        QFontMetrics fm(QApplication::font()), fmb(bold);
        qreal gutter = fm.horizontalAdvance("PK") + 8, nameW = 0, typeW = 0;
        for (size_t i = 0; i < t.columns.size(); ++i) {
            nameW = std::max<qreal>(nameW, fm.horizontalAdvance(QString::fromStdString(t.columns[i])));
            if (i < t.types.size())
                typeW = std::max<qreal>(typeW, fm.horizontalAdvance(QString::fromStdString(t.types[i])));
        }
        qreal w = std::max(fmb.horizontalAdvance(title) + 2 * kPad, kPad + gutter + nameW + 20 + typeW + kPad);
        setRect(0, 0, w, kHeaderH + qreal(t.columns.size()) * kRowH + 4);
        setBrush(pal.base());
        setPen(QPen(pal.mid().color()));

        auto* header = new QGraphicsRectItem(0, 0, w, kHeaderH, this);
        header->setBrush(pal.highlight());
        header->setPen(Qt::NoPen);
        auto* name = new QGraphicsSimpleTextItem(title, header);
        name->setFont(bold);
        name->setBrush(pal.highlightedText());
        name->setPos(kPad, (kHeaderH - fmb.height()) / 2);

        for (size_t i = 0; i < t.columns.size(); ++i) {
            qreal y = kHeaderH + qreal(i) * kRowH + (kRowH - fm.height()) / 2 + 2;
            bool pk = i < t.primary.size() && t.primary[i], fk = fkCols.count(t.columns[i]);
            if (pk || fk) {
                auto* key = new QGraphicsSimpleTextItem(pk ? "PK" : "FK", this);
                key->setBrush(pk ? QBrush(QColor("#e0a800")) : pal.placeholderText());
                key->setPos(kPad, y);
            }
            auto* col = new QGraphicsSimpleTextItem(QString::fromStdString(t.columns[i]), this);
            col->setBrush(pal.text());
            col->setPos(kPad + gutter, y);
            if (i < t.types.size()) {
                QString type = QString::fromStdString(t.types[i]);
                auto* ty = new QGraphicsSimpleTextItem(type, this);
                ty->setBrush(pal.placeholderText());
                ty->setPos(w - kPad - fm.horizontalAdvance(type), y);
            }
        }
    }

    // y of a column row's middle, in box coordinates; -1 = header
    qreal rowY(int row) const { return row < 0 ? kHeaderH / 2 : kHeaderH + (row + 0.5) * kRowH; }

protected:
    QVariant itemChange(GraphicsItemChange c, const QVariant& v) override;
};

// FK line from the referencing column to the referenced one (dot end), redrawn when a box moves.
class Edge : public QGraphicsPathItem {
public:
    Edge(Box* from, int fromRow, Box* to, int toRow, const QColor& color)
        : from_(from), to_(to), fromRow_(fromRow), toRow_(toRow), dot_(new QGraphicsEllipseItem(this)) {
        setPen(QPen(color, 1.4));
        setZValue(-1); // under the boxes
        dot_->setBrush(color);
        dot_->setPen(Qt::NoPen);
        from->edges.push_back(this);
        to->edges.push_back(this);
        reroute();
    }

    void reroute() {
        QRectF a = from_->sceneBoundingRect(), b = to_->sceneBoundingRect();
        qreal ya = from_->scenePos().y() + from_->rowY(fromRow_), yb = to_->scenePos().y() + to_->rowY(toRow_);
        QPainterPath p;
        qreal xb;
        if (a.left() < b.right() && b.left() < a.right()) { // stacked or same table: loop out on the right
            qreal xa = a.right(), out = std::max(a.right(), b.right()) + 40;
            xb = b.right();
            p.moveTo(xa, ya);
            p.cubicTo(out, ya, out, yb, xb, yb);
        } else {
            bool right = b.center().x() > a.center().x();
            qreal xa = right ? a.right() : a.left();
            xb = right ? b.left() : b.right();
            qreal d = std::max(40.0, std::abs(xb - xa) / 2) * (right ? 1 : -1);
            p.moveTo(xa, ya);
            p.cubicTo(xa + d, ya, xb - d, yb, xb, yb);
        }
        setPath(p);
        dot_->setRect(xb - 3.5, yb - 3.5, 7, 7);
    }

private:
    Box *from_, *to_;
    int fromRow_, toRow_;
    QGraphicsEllipseItem* dot_;
};

QVariant Box::itemChange(GraphicsItemChange c, const QVariant& v) {
    if (c == ItemPositionHasChanged)
        for (auto* e : edges) e->reroute();
    return QGraphicsRectItem::itemChange(c, v);
}

class View : public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;

protected:
    void wheelEvent(QWheelEvent* e) override {
        if (e->modifiers() & Qt::ControlModifier) zoom(std::pow(1.0015, e->angleDelta().y())); // ⌘-scroll
        else QGraphicsView::wheelEvent(e);
    }
    bool viewportEvent(QEvent* e) override {
        if (e->type() == QEvent::NativeGesture) { // trackpad pinch
            auto* g = static_cast<QNativeGestureEvent*>(e);
            if (g->gestureType() == Qt::ZoomNativeGesture) {
                zoom(1 + g->value());
                return true;
            }
        }
        return QGraphicsView::viewportEvent(e);
    }

private:
    void zoom(qreal f) {
        qreal s = transform().m11() * f;
        if (s > 0.1 && s < 4) scale(f, f);
    }
};

int columnIndex(const SchemaInfo::Table& t, const std::string& col) {
    auto it = std::find(t.columns.begin(), t.columns.end(), col);
    return it == t.columns.end() ? -1 : int(it - t.columns.begin());
}

} // namespace

QWidget* makeErDiagram(const SchemaInfo& schema, const QString& focusTable) {
    auto* scene = new QGraphicsScene;
    auto* view = new View(scene);
    scene->setParent(view);
    QPalette pal = QApplication::palette();
    view->setBackgroundBrush(pal.window());
    view->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    view->setDragMode(QGraphicsView::ScrollHandDrag); // drag empty space to pan, a box to move it
    view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);

    const auto& tables = schema.tables;
    if (tables.empty()) {
        scene->addSimpleText("No tables")->setBrush(pal.placeholderText());
        return view;
    }
    std::map<std::pair<std::string, std::string>, size_t> index;
    for (size_t i = 0; i < tables.size(); ++i) index[{tables[i].schema, tables[i].name}] = i;
    bool oneSchema = std::all_of(tables.begin(), tables.end(), [&](auto& t) { return t.schema == tables[0].schema; });

    // fk endpoints that are both in the diagram
    struct Link { size_t from, to; const SchemaInfo::ForeignKey* fk; };
    std::vector<Link> links;
    std::vector<std::set<std::string>> fkCols(tables.size());
    std::vector<std::set<size_t>> parents(tables.size());
    for (auto& fk : schema.foreignKeys) {
        auto f = index.find({fk.fromSchema, fk.fromTable}), t = index.find({fk.toSchema, fk.toTable});
        if (f == index.end() || t == index.end()) continue;
        links.push_back({f->second, t->second, &fk});
        fkCols[f->second].insert(fk.fromColumns.begin(), fk.fromColumns.end());
        if (f->second != t->second) parents[f->second].insert(t->second);
    }

    // ponytail: layered layout (referenced tables left, referencing right), no crossing minimisation;
    // drag boxes to tidy up. Add a real graph layout if big schemas get unreadable.
    std::vector<int> layer(tables.size(), -1);
    std::function<int(size_t)> depth = [&](size_t i) {
        if (layer[i] >= 0) return layer[i];
        layer[i] = 0; // cycle guard
        int d = 0;
        for (size_t p : parents[i]) d = std::max(d, depth(p) + 1);
        return layer[i] = d;
    };
    std::map<int, std::vector<size_t>> layers;
    for (size_t i = 0; i < tables.size(); ++i) layers[depth(i)].push_back(i);

    std::vector<Box*> boxes(tables.size());
    Box* focus = nullptr;
    qreal x = 0;
    for (auto& [l, members] : layers) {
        qreal y = 0, colW = 0;
        for (size_t i : members) {
            auto& t = tables[i];
            QString title = QString::fromStdString(oneSchema ? t.name : t.schema + "." + t.name);
            auto* box = new Box(t, title, fkCols[i], pal);
            if (y > 0 && y + box->rect().height() > kMaxColumnH) {
                x += colW + kGapX;
                y = colW = 0;
            }
            box->setPos(x, y);
            scene->addItem(box);
            boxes[i] = box;
            y += box->rect().height() + kGapY;
            colW = std::max(colW, box->rect().width());
            if (QString::fromStdString(t.name) == focusTable) focus = box;
        }
        x += colW + kGapX;
    }
    QColor edgeColor = pal.placeholderText().color();
    for (auto& l : links) {
        auto& fk = *l.fk;
        int fromRow = fk.fromColumns.empty() ? -1 : columnIndex(tables[l.from], fk.fromColumns[0]);
        int toRow = fk.toColumns.empty() ? -1 : columnIndex(tables[l.to], fk.toColumns[0]);
        scene->addItem(new Edge(boxes[l.from], fromRow, boxes[l.to], toRow, edgeColor));
    }
    scene->setSceneRect(scene->itemsBoundingRect().adjusted(-200, -200, 200, 200));

    QTimer::singleShot(0, view, [view, scene, focus] { // needs the real viewport size
        if (focus) {
            focus->setSelected(true);
            view->centerOn(focus);
            return;
        }
        QRectF all = scene->itemsBoundingRect().adjusted(-20, -20, 20, 20);
        qreal fit = std::min(view->viewport()->width() / all.width(), view->viewport()->height() / all.height());
        qreal s = std::clamp(fit, 0.5, 1.0); // fit small schemas, start big ones top-left at half size
        view->scale(s, s);
        QPointF halfView = QPointF(view->viewport()->width(), view->viewport()->height()) / (2 * s);
        view->centerOn(fit >= 0.5 ? all.center() : all.topLeft() + halfView);
    });
    return view;
}
