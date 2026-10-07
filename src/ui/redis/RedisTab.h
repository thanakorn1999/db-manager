#pragma once

#include "core/connection/Session.h"
#include "core/database/redis/Redis.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTableView;
class QTableWidget;
class ResultModel;

// Key browser + value viewer + command console for one Redis logical database.
class RedisTab : public QWidget {
public:
    RedisTab(const ConnectionConfig& cfg, QWidget* parent = nullptr);

private:
    void scan(bool reset);
    void showSelectedKey();
    void deleteSelectedKey();
    void runCommand();
    std::string selectedKey() const;
    int rowOf(const std::string& key) const;
    // Runs f on the worker, then `after` and a reload of the selected key; errors pop up.
    void mutate(std::function<void(Redis&)> f, std::function<void()> after = {});
    bool commitCell(int row, int col, const std::optional<std::string>& value);
    void newKey();
    void renameKey();
    void editTtl();
    void addItem();
    void removeItems();
    void saveString();

    std::string cursor_ = "0";
    std::string currentKey_;  // key shown in the value viewer
    std::string currentType_;
    QLineEdit* pattern_;
    QPushButton* more_;
    QLabel* keyCount_;
    QTableWidget* keys_;
    QLabel* keyHeader_;
    QPushButton* delete_;
    QPushButton* rename_;
    QPushButton* ttl_;
    QPushButton* addItem_;
    QPushButton* removeItems_;
    QPushButton* saveString_;
    QStackedWidget* valueStack_;
    QPlainTextEdit* stringValue_;
    QTableView* valueTable_;
    ResultModel* valueModel_;
    QPlainTextEdit* console_;
    QLineEdit* command_;
    Session<Redis> session_;
};
