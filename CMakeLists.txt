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
)

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
