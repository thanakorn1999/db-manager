#pragma once

#include "core/database/Database.h"

#include <vector>

// Connection list in QSettings; passwords in the OS keychain, never in the settings file.
namespace ConnectionStore {
std::vector<ConnectionConfig> load(); // passwords not filled in
void save(const ConnectionConfig& cfg);
void remove(const std::string& id);
std::string password(const std::string& id);
}
