# 2 号：统一数据（OSMX 解析库）独立目标。
# 该目标不依赖 Qt Widgets、libosmium 或 SQLite，可被算法/渲染/测试单独链接。
set(P1_OSMX_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

add_library(p1_data_osmx STATIC
    OsmxTypes.cpp
    OsmxTypes.h
    OsmxDataset.cpp
    OsmxDataset.h
    OsmxParser.cpp
    OsmxParser.h
    OsmxCache.cpp
    OsmxCache.h
    OsmxStore.cpp
    OsmxStore.h)
target_include_directories(p1_data_osmx PUBLIC
    "${P1_OSMX_REPO_ROOT}/src"
    "${P1_OSMX_REPO_ROOT}/include")
target_link_libraries(p1_data_osmx PUBLIC Qt6::Core)
target_compile_options(p1_data_osmx PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

add_executable(p1-osmx-cli tools/OsmxCli.cpp)
target_link_libraries(p1-osmx-cli PRIVATE p1_data_osmx)
target_compile_options(p1-osmx-cli PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

if(BUILD_TESTING)
    qt_add_executable(p1-osmx-tests tests/OsmxTests.cpp)
    target_link_libraries(p1-osmx-tests PRIVATE p1_data_osmx Qt6::Test)
    add_test(NAME data-osmx COMMAND p1-osmx-tests)
endif()
