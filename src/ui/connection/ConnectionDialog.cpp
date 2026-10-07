#include "ConnectionDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QUuid>

ConnectionDialog::ConnectionDialog(const ConnectionConfig& cfg, QWidget* parent)
    : QDialog(parent),
      id_(cfg.id.empty() ? QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() : cfg.id) {
    setWindowTitle(cfg.id.empty() ? "New Connection" : "Edit Connection");
    setMinimumWidth(420);

    type_ = new QComboBox;
    type_->addItem("PostgreSQL", int(DbType::PostgreSQL));
    type_->addItem("Redis", int(DbType::Redis));
    type_->setCurrentIndex(type_->findData(int(cfg.type)));
    type_->setEnabled(cfg.id.empty()); // type is fixed once created

    name_ = new QLineEdit(QString::fromStdString(cfg.name));
    host_ = new QLineEdit(QString::fromStdString(cfg.host));
    host_->setPlaceholderText("localhost, 192.168.1.100, db.example.com");
    port_ = new QSpinBox;
    port_->setRange(1, 65535);
    port_->setValue(cfg.port);
    database_ = new QLineEdit(QString::fromStdString(cfg.database));
    user_ = new QLineEdit(QString::fromStdString(cfg.username));
    password_ = new QLineEdit(QString::fromStdString(cfg.password));
    password_->setEchoMode(QLineEdit::Password);
    sslMode_ = new QComboBox;
    sslMode_->addItems({"disable", "allow", "prefer", "require", "verify-ca", "verify-full"});
    sslMode_->setCurrentText(QString::fromStdString(cfg.sslMode));
    tls_ = new QCheckBox("Use TLS");
    tls_->setChecked(cfg.tls);

    form_ = new QFormLayout;
    form_->addRow("Type", type_);
    form_->addRow("Name", name_);
    form_->addRow("Host", host_);
    form_->addRow("Port", port_);
    form_->addRow("Database", database_);
    form_->addRow("Username", user_);
    form_->addRow("Password", password_);
    form_->addRow("SSL Mode", sslMode_);
    form_->addRow("", tls_);

    testButton_ = new QPushButton("Test Connection");
    testResult_ = new QLabel;
    testResult_->setWordWrap(true);
    testResult_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* testRow = new QHBoxLayout;
    testRow->addWidget(testButton_);
    testRow->addWidget(testResult_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form_);
    layout->addLayout(testRow);
    layout->addWidget(buttons);

    connect(type_, &QComboBox::currentIndexChanged, this, &ConnectionDialog::typeChanged);
    connect(testButton_, &QPushButton::clicked, this, &ConnectionDialog::test);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (host_->text().trimmed().isEmpty()) {
            testResult_->setText("<font color=red>Host is required</font>");
            return;
        }
        accept();
    });
    typeChanged();
}

void ConnectionDialog::typeChanged() {
    bool redis = type_->currentData().toInt() == int(DbType::Redis);
    // swap default ports when the user hasn't customised them
    if (redis && port_->value() == 5432) port_->setValue(6379);
    if (!redis && port_->value() == 6379) port_->setValue(5432);
    database_->setPlaceholderText(redis ? "0" : "postgres");
    user_->setPlaceholderText(redis ? "default (Redis 6+ ACL, optional)" : "postgres");
    form_->setRowVisible(sslMode_, !redis);
    form_->setRowVisible(tls_, redis);
}

ConnectionConfig ConnectionDialog::config() const {
    ConnectionConfig c;
    c.id = id_;
    c.type = DbType(type_->currentData().toInt());
    c.host = host_->text().trimmed().toStdString();
    c.name = name_->text().trimmed().isEmpty() ? c.host : name_->text().trimmed().toStdString();
    c.port = port_->value();
    c.database = database_->text().trimmed().toStdString();
    c.username = user_->text().trimmed().toStdString();
    c.password = password_->text().toStdString();
    c.sslMode = sslMode_->currentText().toStdString();
    c.tls = tls_->isChecked();
    return c;
}

void ConnectionDialog::test() {
    testButton_->setEnabled(false);
    testResult_->setText("Connecting…");
    auto ok = [this] {
        testButton_->setEnabled(true);
        testResult_->setText("<font color=green>Connected ✓</font>");
    };
    auto fail = [this](const QString& msg) {
        testButton_->setEnabled(true);
        testResult_->setText("<font color=red>" + msg.toHtmlEscaped() + "</font>");
    };
    auto cfg = config();
    if (cfg.type == DbType::Redis) {
        redisTest_ = std::make_unique<Session<Redis>>(cfg);
        redisTest_->run(this, [](Redis& r) { r.ping(); }, ok, fail);
    } else {
        pgTest_ = std::make_unique<Session<PostgreSQL>>(cfg);
        pgTest_->run(this, [](PostgreSQL& p) { p.ping(); }, ok, fail);
    }
}
