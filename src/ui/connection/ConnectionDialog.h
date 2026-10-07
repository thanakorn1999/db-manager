#pragma once

#include "core/connection/Session.h"
#include "core/database/postgres/PostgreSQL.h"
#include "core/database/redis/Redis.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class ConnectionDialog : public QDialog {
public:
    explicit ConnectionDialog(const ConnectionConfig& cfg, QWidget* parent = nullptr);
    ConnectionConfig config() const;

private:
    void typeChanged();
    void test();

    std::string id_;
    QFormLayout* form_;
    QComboBox* type_;
    QLineEdit* name_;
    QLineEdit* host_;
    QSpinBox* port_;
    QLineEdit* database_;
    QLineEdit* user_;
    QLineEdit* password_;
    QComboBox* sslMode_;
    QCheckBox* tls_;
    QPushButton* testButton_;
    QLabel* testResult_;

    std::unique_ptr<Session<PostgreSQL>> pgTest_;
    std::unique_ptr<Session<Redis>> redisTest_;
};
