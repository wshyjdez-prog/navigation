cmake_minimum_required(VERSION 3.28.3)
project(P1Navigation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Widgets)

add_library(p1_ui STATIC
    src/ui/common/Theme.cpp
    src/ui/common/Components.cpp
    src/app/ImportLanding.cpp
    src/app/MainWindow.cpp)
target_include_directories(p1_ui PUBLIC src)
target_link_libraries(p1_ui PUBLIC Qt6::Widgets)
target_compile_options(p1_ui PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

qt_add_executable(p1-navigation src/app/main.cpp)
target_link_libraries(p1-navigation PRIVATE p1_ui)
install(TARGETS p1-navigation RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Test)
    qt_add_executable(p1-ui-tests tests/UiTests.cpp)
    target_link_libraries(p1-ui-tests PRIVATE p1_ui Qt6::Test)
    add_test(NAME ui-contracts COMMAND p1-ui-tests)
    set_tests_properties(ui-contracts PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# 4 号：导航模块（位置源契约 + 固定轨迹读取 + 可回放模拟位置源）。
# 必须位于 include(CTest) 之后，子目录内的 add_test 才会注册到 ctest。
add_subdirectory(src/navigation)
