find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Gui Concurrent)

add_library(p1_renderer STATIC
    MapScene.cpp
    MockMapScene.cpp
    LinuxRenderer.cpp
    LinuxRenderer.h)
target_include_directories(p1_renderer PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(p1_renderer PUBLIC Qt6::Gui PRIVATE Qt6::Concurrent)
target_compile_options(p1_renderer PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4>")

option(P1_BUILD_RENDERER_BENCHMARK "Build the LinuxRenderer internal benchmark" OFF)
if(P1_BUILD_RENDERER_BENCHMARK)
    qt_add_executable(p1-renderer-benchmark benchmark/RendererBenchmark.cpp)
    target_link_libraries(p1-renderer-benchmark PRIVATE p1_renderer)
    target_compile_options(p1-renderer-benchmark PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4>")
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Test)
    qt_add_executable(p1-renderer-tests tests/RendererTests.cpp)
    target_link_libraries(p1-renderer-tests PRIVATE p1_renderer Qt6::Test)
    target_compile_options(p1-renderer-tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4>")
    add_test(NAME renderer-contracts COMMAND p1-renderer-tests)
    set_tests_properties(renderer-contracts PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
