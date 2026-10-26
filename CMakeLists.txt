cmake_minimum_required(VERSION 3.24)
project(ShuttleControl VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)
find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(ECMFindQmlModule)
find_package(KF6 REQUIRED COMPONENTS Kirigami)
ecm_find_qmlmodule(org.kde.kirigami REQUIRED)

add_executable(shuttle-control
    src/main.cpp
    src/tunnelcontroller.cpp
    src/tunnelcontroller.h
)

qt_add_resources(shuttle-control "qml_resources"
    PREFIX "/"
    FILES
        qml/Main.qml
)

target_link_libraries(shuttle-control PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
)

target_compile_options(shuttle-control PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

install(TARGETS shuttle-control RUNTIME DESTINATION bin)
install(FILES packaging/org.example.shuttle-control.desktop
    DESTINATION share/applications)
