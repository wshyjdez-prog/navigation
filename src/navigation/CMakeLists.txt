# 4 号：导航模块。本阶段内容：位置源契约 + 固定轨迹读取 + 可回放的模拟位置源。
# 由顶层 CMakeLists.txt 在 include(CTest) 之后 add_subdirectory(src/navigation) 引入，
# 依赖顶层的 C++17 与 CMAKE_AUTOMOC 设置。
find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Core)

add_library(p1_navigation STATIC
    MockLocationSource.cpp
    FixedTrackLoader.cpp
    # 含 Q_OBJECT 的公共契约头不在本目录，须显式列出才会被 AUTOMOC 处理并链接进本库。
    "${CMAKE_SOURCE_DIR}/include/contracts/LocationSource.h")
# 子目录中相对路径 src 会解析为 src/navigation/src，故使用绝对路径。
target_include_directories(p1_navigation PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(p1_navigation PUBLIC Qt6::Core)
target_compile_options(p1_navigation PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

set(P1_LOCATION_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/tests/fixtures/location")

# 命令行演示：加载固定轨迹 → start → 逐帧输出 → pause/seek/resume/倍速 → stop 后确认无回调。
qt_add_executable(p1-location-demo demo/LocationReplayDemo.cpp)
target_link_libraries(p1-location-demo PRIVATE p1_navigation)
target_compile_definitions(p1-location-demo PRIVATE
    P1_LOCATION_FIXTURE_DIR="${P1_LOCATION_FIXTURE_DIR}")
target_compile_options(p1-location-demo PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

if(BUILD_TESTING)
    find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Test)
    qt_add_executable(p1-navigation-tests tests/LocationReplayTests.cpp)
    target_link_libraries(p1-navigation-tests PRIVATE p1_navigation Qt6::Test)
    target_compile_definitions(p1-navigation-tests PRIVATE
        P1_LOCATION_FIXTURE_DIR="${P1_LOCATION_FIXTURE_DIR}")
    target_compile_options(p1-navigation-tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    add_test(NAME navigation-location COMMAND p1-navigation-tests)
    add_test(NAME navigation-location-demo COMMAND p1-location-demo)
endif()
