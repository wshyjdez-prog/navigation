cmake_minimum_required(VERSION 3.28.3)
project(P1Navigation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Widgets Concurrent)
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Test)
endif()
add_subdirectory(src/data/import)
add_subdirectory(src/pal/linux/renderer)

add_library(p1_ui STATIC
    src/ui/common/Theme.cpp
    src/ui/common/Components.cpp
    src/ui/map/MapDemoWidget.cpp
    src/ui/map/MapDemoWidget.h
    src/ui/map/MapPage.cpp
    src/ui/map/MapPage.h
    src/app/ImportFactory.cpp
    src/app/ImportPage.cpp
    src/app/ImportTask.cpp
    src/app/MainWindow.cpp)
target_include_directories(p1_ui PUBLIC src)
target_link_libraries(p1_ui PUBLIC Qt6::Widgets p1_data_import p1_renderer PRIVATE Qt6::Concurrent)
target_compile_options(p1_ui PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

qt_add_executable(p1-navigation src/app/main.cpp)
target_link_libraries(p1-navigation PRIVATE p1_ui)
install(TARGETS p1-navigation RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    qt_add_executable(p1-ui-tests tests/UiTests.cpp)
    target_link_libraries(p1-ui-tests PRIVATE p1_ui Qt6::Test)
    add_test(NAME ui-contracts COMMAND p1-ui-tests)
    set_tests_properties(ui-contracts PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# 模块装配必须位于 include(CTest) 之后，统一注册各模块已有测试。
# 3 号当前仅提供基础模型；本目标不代表 A*/HMM 服务已经接入应用。
add_subdirectory(src/algorithms)
# 4 号：位置源契约、固定轨迹读取与 Mock 回放。
add_subdirectory(src/navigation)
