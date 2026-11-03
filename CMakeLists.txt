cmake_minimum_required(VERSION 3.28.3)
project(P1Navigation VERSION 0.2.0 LANGUAGES CXX)

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
add_subdirectory(src/data/osmx)
add_subdirectory(src/pal/linux/renderer)

add_library(p1_ui_common STATIC
    src/ui/common/Theme.cpp
    src/ui/common/Components.cpp)
target_include_directories(p1_ui_common PUBLIC src include)
target_link_libraries(p1_ui_common PUBLIC Qt6::Widgets)
add_subdirectory(src/app)

add_library(p1_ui STATIC
    src/ui/map/MapDemoWidget.cpp
    src/ui/map/MapDemoWidget.h
    src/ui/map/MapPage.cpp
    src/ui/map/MapView.cpp
    src/ui/map/TilePyramid.cpp
    src/ui/map/TilePyramid.h
    src/ui/map/MapPage.h
    src/ui/map/MapRegionLoader.cpp
    src/ui/map/MapRegionLoader.h
    src/app/NavigationWorkflow.cpp
    src/app/NavigationWorkflow.h
    src/app/MainWindow.cpp)
target_include_directories(p1_ui PUBLIC src)
target_link_libraries(p1_ui PUBLIC p1_ui_common p1_import_page Qt6::Widgets p1_renderer PRIVATE Qt6::Concurrent)
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
# 3 号模型、Boost A*、Qt 适配与 FMM 调用层统一装配；真实 FMM 仍需外部程序和数据。
# 4 号：真实规划器对接联调开关——开启后同时启用 3 号 A* 与 Qt 适配层（见 src/algorithms/CMakeLists.txt）。
# 缺少交付源码时在配置阶段明确报错。
option(P1_NAVIGATION_REAL_PLANNER "Build member-4 real planner integration (enables member-3 A* core and Qt adapter)" ON)
option(P1_ALGORITHMS_BUILD_FMM_ADAPTER "Build the asynchronous FMM CLI MapMatcher" ON)
if(P1_NAVIGATION_REAL_PLANNER)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms/qt/BoostRoutePlanner.h")
        message(FATAL_ERROR
            "P1_NAVIGATION_REAL_PLANNER=ON 需要 3 号算法源码（src/algorithms/qt/BoostRoutePlanner.h 等），\n"
            "请核对 3 号算法源码是否完整，或显式关闭 P1_NAVIGATION_REAL_PLANNER。")
    endif()
    set(P1_ALGORITHMS_BUILD_ASTAR ON CACHE BOOL "" FORCE)
    set(P1_ALGORITHMS_BUILD_QT_ADAPTER ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(src/algorithms)
# 4 号：位置源契约、固定轨迹读取与 Mock 回放。
add_subdirectory(src/navigation)
if(NOT P1_NAVIGATION_REAL_PLANNER OR NOT P1_ALGORITHMS_BUILD_FMM_ADAPTER)
    message(FATAL_ERROR "The business workflow requires the selected A* and FMM adapters")
endif()
target_link_libraries(p1_ui PUBLIC p1_algorithms_astar p1_algorithms_fmm)
if(BUILD_TESTING)
    qt_add_executable(p1-pyramid-tests tests/TilePyramidTests.cpp)
    target_link_libraries(p1-pyramid-tests PRIVATE p1_ui Qt6::Test)
    add_test(NAME tile-pyramid COMMAND p1-pyramid-tests)
    set_tests_properties(tile-pyramid PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(p1-business-tests tests/BusinessTests.cpp)
    target_link_libraries(p1-business-tests PRIVATE p1_ui Qt6::Test)
    add_test(NAME business-workflow COMMAND p1-business-tests regionalRoads routeAndCancel uiPipeline automaticMapRegions panZoomKeepsVisibleMapBeforeWorkerReturns newestMapRegionWins malformedTrajectory)
    set_tests_properties(business-workflow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
