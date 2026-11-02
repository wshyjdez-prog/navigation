# 4 号：导航模块。本阶段内容：位置源契约 + 固定轨迹读取 + 可回放的模拟位置源
#       + 规划对接层（AsyncPlannerBridge 线程化对接 + StubRoutePlanner 确定性桩，第 2 次作业）。
# 由顶层 CMakeLists.txt 在 include(CTest) 之后 add_subdirectory(src/navigation) 引入，
# 依赖顶层的 C++17 与 CMAKE_AUTOMOC 设置。
find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Core)

add_library(p1_navigation STATIC
    MockLocationSource.cpp
    FixedTrackLoader.cpp
    AsyncPlannerBridge.cpp
    StubRoutePlanner.cpp
    # 含 Q_OBJECT 的公共契约头不在本目录，须显式列出才会被 AUTOMOC 处理并链接进本库。
    "${CMAKE_SOURCE_DIR}/include/contracts/LocationSource.h"
    # 导航控制器契约（含 Q_OBJECT），同样须显式列出；NavigationTypes.h 无 Q_OBJECT，无需列出。
    "${CMAKE_SOURCE_DIR}/include/contracts/NavigationController.h"
    # 对接层验证用最小控制器桩（Q_OBJECT，非第 3 次状态机，仅供演示与集成测试）。
    RecordingController.h)
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

# 规划对接层演示：start → 后台线程规划（可注入延迟）→ 队列回流 → 事件采纳；
# 含 stop() 取消在途后无回调的验证。退出码 0 即成功。
qt_add_executable(p1-planner-demo demo/PlannerBridgeDemo.cpp)
target_link_libraries(p1-planner-demo PRIVATE p1_navigation)
target_compile_options(p1-planner-demo PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
add_test(NAME navigation-planner-demo COMMAND p1-planner-demo)

# 规划对接层测试：端到端链路、回流线程归属、不阻塞（心跳）、stop 取消、
# 陈旧结果拒绝（故障注入 honorCancel=false）、析构静默、stub 输出确定性。
qt_add_executable(p1-navigation-integration-tests tests/PlannerBridgeTests.cpp)
target_link_libraries(p1-navigation-integration-tests PRIVATE p1_navigation Qt6::Test)
target_compile_options(p1-navigation-integration-tests PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
add_test(NAME navigation-integration COMMAND p1-navigation-integration-tests)
set_tests_properties(navigation-integration PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ── 4 号：3 号真实 A* 规划器对接（P1_NAVIGATION_REAL_PLANNER=ON 时参与构建）──
# 契约既定策略（DEC-20261101-4-planner-bridge-interface-01）：AsyncPlannerBridge 与控制器零改动，
# 仅把 StubRoutePlanner 换成 3 号 BoostRoutePlanner（contracts::RoutePlanner 即插即换）。
# 依赖根 CMakeLists：本开关开启时同步启用 P1_ALGORITHMS_BUILD_ASTAR / P1_ALGORITHMS_BUILD_QT_ADAPTER。
if(P1_NAVIGATION_REAL_PLANNER)
    if(NOT TARGET p1_algorithms_route_planner)
        message(FATAL_ERROR "P1_NAVIGATION_REAL_PLANNER=ON 需要 src/algorithms 的 Qt 适配层（检查根 CMakeLists 的选项传递）")
    endif()
    find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Concurrent)

    # 真实规划器对接演示：样例路网 + BoostRoutePlanner + GeoProjector，
    # 成功 / stop 取消静默 / 不可达 / 非节点端点四条链路全部经桥正确回流。退出码 0 即成功。
    qt_add_executable(p1-real-planner-demo demo/RealPlannerBridgeDemo.cpp)
    target_link_libraries(p1-real-planner-demo PRIVATE
        p1_navigation p1_algorithms_route_planner Qt6::Core Qt6::Concurrent)
    target_compile_options(p1-real-planner-demo PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    add_test(NAME navigation-real-planner-demo COMMAND p1-real-planner-demo)
    set_tests_properties(navigation-real-planner-demo PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    if(BUILD_TESTING)
        # 真实规划器对接测试：投影器往返、端到端采纳、"最新请求生效"、dataVersion 拒收、
        # 不可达 / 非节点端点失败路径、桥析构静默（覆盖条款见文件头注释）。
        qt_add_executable(p1-real-planner-tests tests/RealPlannerBridgeTests.cpp)
        target_link_libraries(p1-real-planner-tests PRIVATE
            p1_navigation p1_algorithms_route_planner Qt6::Test Qt6::Concurrent)
        target_compile_options(p1-real-planner-tests PRIVATE
            "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
            "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
        add_test(NAME navigation-real-planner COMMAND p1-real-planner-tests)
        set_tests_properties(navigation-real-planner PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

    # 契约测试：导航状态事件契约（转换表、非法事件、陈旧 requestId、快照字段）。
    qt_add_executable(p1-navigation-contract-tests tests/NavigationContractTests.cpp)
    target_link_libraries(p1-navigation-contract-tests PRIVATE p1_navigation Qt6::Test)
    target_compile_options(p1-navigation-contract-tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    add_test(NAME navigation-contracts COMMAND p1-navigation-contract-tests)
    set_tests_properties(navigation-contracts PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 五类模拟样例的加载与回放一致性（期望值见 tests/fixtures/location/expected/）。
    qt_add_executable(p1-location-fixture-tests tests/LocationFixtureTests.cpp)
    target_link_libraries(p1-location-fixture-tests PRIVATE p1_navigation Qt6::Test)
    target_compile_definitions(p1-location-fixture-tests PRIVATE
        P1_LOCATION_FIXTURE_DIR="${P1_LOCATION_FIXTURE_DIR}")
    target_compile_options(p1-location-fixture-tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    add_test(NAME navigation-location-fixtures COMMAND p1-location-fixture-tests)
    set_tests_properties(navigation-location-fixtures PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
