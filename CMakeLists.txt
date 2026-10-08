cmake_minimum_required(VERSION 3.21)
project(db-manager LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# deps: Homebrew / system packages on macOS & Linux, vcpkg (vcpkg.json) on Windows
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
find_package(Qt6Keychain REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(ZLIB REQUIRED) # xlsx export (deflate)
find_package(hiredis CONFIG QUIET) # vcpkg ships CMake configs; Homebrew / distros only pkg-config
if(hiredis_FOUND)
    find_package(hiredis_ssl CONFIG REQUIRED)
    set(REDIS_LIBS hiredis::hiredis hiredis::hiredis_ssl)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis hiredis_ssl)
    set(REDIS_LIBS PkgConfig::HIREDIS)
endif()

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
target_link_libraries(dbcore PUBLIC Qt6::Core PostgreSQL::PostgreSQL ${REDIS_LIBS} ZLIB::ZLIB)

add_executable(db-manager WIN32 MACOSX_BUNDLE # WIN32: no console window
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
    resources/icon.icns # app icon: python3 resources/make_icon.py redraws icon.png / .icns / .ico
    $<$<PLATFORM_ID:Windows>:resources/icon.rc>
)
set_source_files_properties(resources/icon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
target_link_libraries(db-manager PRIVATE dbcore Qt6::Widgets Qt6Keychain::Qt6Keychain)
# macOS won't show native open / save panels (Export, Backup) for an app with an empty bundle id
set_target_properties(db-manager PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER com.jamezarkk.db-manager
                                            MACOSX_BUNDLE_BUNDLE_NAME "DB Manager"
                                            MACOSX_BUNDLE_ICON_FILE icon.icns)

enable_testing()
add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE dbcore)
add_test(NAME core_test COMMAND core_test)
