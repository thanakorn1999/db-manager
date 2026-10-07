cmake_minimum_required(VERSION 3.21)
project(db-manager LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ponytail: deps via Homebrew/system packages (find_package/pkg-config); add vcpkg.json when Windows/CI builds matter
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
find_package(Qt6Keychain REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(ZLIB REQUIRED) # xlsx export (deflate)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis hiredis_ssl)

# core: no Qt Widgets
add_library(dbcore STATIC
    src/core/database/Database.h
    src/core/database/postgres/PostgreSQL.cpp
    src/core/database/redis/Redis.cpp
    src/core/connection/Session.h
    src/core/query/SqlCompleter.cpp
    src/core/export/Export.cpp
)
target_include_directories(dbcore PUBLIC src)
target_link_libraries(dbcore PUBLIC Qt6::Core PostgreSQL::PostgreSQL PkgConfig::HIREDIS ZLIB::ZLIB)

add_executable(db-manager MACOSX_BUNDLE
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/ResultModel.cpp
    src/ui/Settings.cpp
    src/ui/TableIcons.cpp
    src/ui/connection/ConnectionDialog.cpp
    src/ui/connection/ConnectionStore.cpp
    src/ui/sql-editor/SqlEditorTab.cpp
    src/ui/redis/RedisTab.cpp
    src/ui/er-diagram/ErDiagram.cpp
)
target_link_libraries(db-manager PRIVATE dbcore Qt6::Widgets Qt6Keychain::Qt6Keychain)
# macOS won't show native open / save panels (Export, Backup) for an app with an empty bundle id
set_target_properties(db-manager PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER com.jamezarkk.db-manager
                                            MACOSX_BUNDLE_BUNDLE_NAME "DB Manager")

enable_testing()
add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE dbcore)
add_test(NAME core_test COMMAND core_test)
