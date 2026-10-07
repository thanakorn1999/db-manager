#include "TableIcons.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QSettings>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace TableIcons {
namespace {

// word in a table name -> emoji; words are lower-case and singular
const QHash<QString, QString>& words() {
    static const QHash<QString, QString> map = [] {
        QHash<QString, QString> m;
        auto add = [&m](const char* emoji, QStringList keys) {
            for (auto& k : keys) m.insert(k, QString::fromUtf8(emoji));
        };
        add("🧑", {"user", "account", "member", "customer", "person", "people", "employee", "staff", "profile",
                   "author", "client", "owner", "contact", "admin"});
        add("🧑‍🤝‍🧑", {"group", "team", "follower", "friend"});
        add("🏢", {"company", "organization", "organisation", "org", "department", "tenant", "workspace"});
        add("🧾", {"order", "purchase", "receipt"});
        add("📦", {"product", "item", "sku", "inventory", "stock", "package", "good"});
        add("🛒", {"cart", "basket"});
        add("💳", {"payment", "transaction", "invoice", "billing", "charge", "refund", "card", "subscription"});
        add("💰", {"price", "wallet", "balance", "fee", "salary", "revenue", "currency"});
        add("🏷️", {"category", "tag", "label", "type"});
        add("📝", {"post", "article", "blog", "page", "note", "document", "doc", "content", "draft"});
        add("💬", {"message", "chat", "comment", "reply", "conversation", "review", "feedback", "thread"});
        add("✉️", {"email", "mail", "newsletter", "invitation", "invite"});
        add("🔔", {"notification", "alert", "reminder"});
        add("📜", {"log", "audit", "history", "event", "activity", "change"});
        add("📊", {"report", "stat", "statistic", "metric", "analytic", "dashboard"});
        add("⚙️", {"setting", "config", "configuration", "option", "preference", "parameter", "param", "feature"});
        add("🔑", {"session", "token", "auth", "credential", "permission", "role", "key", "secret", "password",
                   "login", "otp", "privilege"});
        add("📍", {"address", "location", "place", "geo", "coordinate", "branch"});
        add("🌍", {"country", "city", "region", "province", "language", "locale", "timezone"});
        add("📅", {"schedule", "calendar", "booking", "appointment", "reservation", "holiday", "shift"});
        add("⏰", {"job", "task", "queue", "cron", "worker"});
        add("🖼️", {"image", "photo", "media", "picture", "avatar", "video", "attachment", "asset", "banner"});
        add("📁", {"file", "upload", "folder", "directory"});
        add("🔗", {"link", "url", "relation", "mapping", "redirect"});
        add("⭐", {"favorite", "favourite", "rating", "like", "star", "bookmark", "wishlist"});
        add("🎮", {"game", "player", "score", "match", "level", "achievement"});
        add("🚚", {"shipment", "shipping", "delivery", "courier"});
        add("🏪", {"store", "shop", "vendor", "supplier", "merchant", "seller"});
        add("🎫", {"ticket", "coupon", "voucher", "promo", "promotion", "discount"});
        add("🛠️", {"migration", "version", "schema", "maintenance"});
        add("🎓", {"course", "lesson", "student", "teacher", "class", "exam", "enrollment"});
        add("🏥", {"patient", "doctor", "hospital", "clinic", "prescription"});
        add("🍔", {"menu", "food", "recipe", "restaurant", "dish", "ingredient"});
        add("📚", {"book", "library", "chapter"});
        return m;
    }();
    return map;
}

// users -> user, categories -> category, addresses -> address
QString singular(const QString& w) {
    if (words().contains(w)) return w;
    if (w.endsWith("ies")) return w.chopped(3) + "y";
    if (w.endsWith("sses") || w.endsWith("xes") || w.endsWith("ches") || w.endsWith("shes")) return w.chopped(2);
    if (w.endsWith('s') && !w.endsWith("ss")) return w.chopped(1);
    return w;
}

QString settingsKey(const QString& schema, const QString& table) {
    return "tableIcons/" + QString::fromLatin1(QUrl::toPercentEncoding(schema + "." + table));
}

QIcon emojiIcon(const QString& emoji) {
    static QHash<QString, QIcon> cache;
    if (auto it = cache.constFind(emoji); it != cache.constEnd()) return *it;
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap pm(QSize(32, 32) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    QFont f;
    f.setPixelSize(28);
    p.setFont(f);
    p.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, emoji);
    p.end();
    return cache[emoji] = QIcon(pm);
}

} // namespace

QString detect(const QString& table) {
    // split snake_case / kebab / camelCase into words; the last word (the noun) counts most
    QString spaced = table;
    spaced.replace(QRegularExpression("([a-z0-9])([A-Z])"), "\\1 \\2");
    QStringList parts = spaced.toLower().split(QRegularExpression("[^a-z0-9]+"), Qt::SkipEmptyParts);
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        if (auto hit = words().find(singular(*it)); hit != words().end()) return *hit;
    return {};
}

QString chosen(const QString& schema, const QString& table) {
    return QSettings().value(settingsKey(schema, table)).toString();
}

QIcon icon(const QString& schema, const QString& table) {
    QString e = chosen(schema, table);
    if (e.isEmpty()) e = detect(table);
    return e.isEmpty() ? qApp->style()->standardIcon(QStyle::SP_FileIcon) : emojiIcon(e);
}

bool pick(QWidget* parent, const QString& schema, const QString& table) {
    QDialog dlg(parent);
    dlg.setWindowTitle("Icon for " + table);
    QString before = chosen(schema, table), result = before;

    auto* custom = new QLineEdit(before);
    custom->setPlaceholderText("or type / paste any emoji (⌃⌘Space), saved to My icons");
    static const char* palette[] = {"🧑", "🧑‍🤝‍🧑", "👤", "👥", "🏢", "🧾", "📦", "🛒", "💳", "💰", "🏷️", "📝", "💬", "✉️",
                                    "🔔", "📜", "📊", "⚙️", "🔑", "🔒", "📍", "🌍", "📅", "⏰", "🖼️", "📁",
                                    "🔗", "⭐", "🎮", "🚚", "🏪", "🎫", "🛠️", "🎓", "🏥", "🍔", "📚", "🧪",
                                    "🐞", "❤️", "🚀", "📄"};
    QStringList builtIn;
    for (auto* e : palette) builtIn << QString::fromUtf8(e);
    // emoji the user typed before, kept at the top of the picker; right-click removes
    const QString mineKey = "iconPalette/mine";
    QStringList mine = QSettings().value(mineKey).toStringList();

    auto makeGrid = [&](const QStringList& emojis, bool removable) {
        auto* grid = new QGridLayout;
        for (int n = 0; n < emojis.size(); ++n) {
            QString e = emojis[n];
            auto* b = new QToolButton;
            b->setText(e);
            b->setAutoRaise(true);
            b->setStyleSheet("font-size: 20px; padding: 2px;");
            QObject::connect(b, &QToolButton::clicked, &dlg, [&dlg, &result, e] {
                result = e;
                dlg.accept();
            });
            if (removable) {
                auto* remove = new QAction("Remove from My icons", b);
                QObject::connect(remove, &QAction::triggered, b, [b, e, mineKey] {
                    QSettings s;
                    QStringList list = s.value(mineKey).toStringList();
                    list.removeAll(e);
                    s.setValue(mineKey, list);
                    b->hide();
                });
                b->addAction(remove);
                b->setContextMenuPolicy(Qt::ActionsContextMenu);
            }
            grid->addWidget(b, n / 10, n % 10);
        }
        return grid;
    };
    QString guess = detect(table);
    auto* autoButton = new QToolButton;
    autoButton->setText(guess.isEmpty() ? "Automatic (plain file icon)" : "Automatic: " + guess + " from the name");
    QObject::connect(autoButton, &QToolButton::clicked, &dlg, [&dlg, &result] {
        result.clear();
        dlg.accept();
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&dlg, &result, custom] {
        result = custom->text().trimmed();
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(QString("<b>%1.%2</b>").arg(schema.toHtmlEscaped(), table.toHtmlEscaped())));
    if (!mine.isEmpty()) {
        layout->addWidget(new QLabel("My icons"));
        layout->addLayout(makeGrid(mine, true));
        layout->addWidget(new QLabel("Icons"));
    }
    layout->addLayout(makeGrid(builtIn, false));
    layout->addWidget(autoButton);
    layout->addWidget(custom);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted || result == before) return false;

    QSettings s;
    if (!result.isEmpty() && !builtIn.contains(result) && !mine.contains(result)) {
        mine.prepend(result); // ponytail: no cap; add one if the list ever gets unwieldy
        s.setValue(mineKey, mine);
    }
    if (result.isEmpty()) s.remove(settingsKey(schema, table));
    else s.setValue(settingsKey(schema, table), result);
    return true;
}

}
