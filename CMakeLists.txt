cmake_minimum_required(VERSION 3.21)
project(VGRPresenterUI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Quick QuickControls2 QuickShapes)

qt_standard_project_setup(REQUIRES 6.11)

file(GLOB_RECURSE APP_QML_FILES
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.qml")

file(GLOB_RECURSE APP_ASSET_FILES
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.png")

qt_add_executable(appVGRPresenterUI
    src/main.cpp
    src/SlideListModel.h
    src/SlideListModel.cpp
    src/OutputListModel.h
    src/OutputListModel.cpp
)

# Theme.qml has `pragma Singleton`, but qt_add_qml_module doesn't infer that
# from the file content on its own — without this, the generated qmldir
# lists it as an ordinary type ("Theme 1.0 ...") instead of a singleton
# ("singleton Theme 1.0 ..."), which is exactly what made Theme.* resolve to
# undefined at deeper nesting and forced literal hex colors everywhere.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)

qt_add_qml_module(appVGRPresenterUI
    URI VGRPresenterUI
    VERSION 1.0
    QML_FILES
        ${APP_QML_FILES}
    RESOURCES
        ${APP_ASSET_FILES}
    RESOURCE_PREFIX /qt/qml
)

set_target_properties(appVGRPresenterUI PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

target_link_libraries(appVGRPresenterUI
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::QuickShapes
)

# The generated qmltyperegistrations.cpp does `#if __has_include(<SlideListModel.h>)`
# — without src/ on the include path the include is skipped and the generated
# references to QAbstractListModel / SlideListModel::Role fail to compile.
target_include_directories(appVGRPresenterUI PRIVATE src)
