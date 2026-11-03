# 2 号：数据导入页面模块（独立目标）。
# 公开工厂头文件为 ImportFactory.h，页面固定路由 ID 为 "import"；页面只通过
# 公共契约把数据交给 1 号装配，不直接操作其他模块内部状态。
set(P1_APP_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

add_library(p1_import_page STATIC
    ImportFactory.cpp
    ImportFactory.h
    ImportPage.cpp
    ImportPage.h
    ImportTask.cpp
    ImportTask.h
    ImportParser.h
    ImportTypes.h
    OsmxImportAdapter.cpp
    OsmxImportAdapter.h
    OsmxDatasetLoader.cpp
    OsmxDatasetLoader.h)
target_include_directories(p1_import_page PUBLIC
    "${P1_APP_REPO_ROOT}/src"
    "${P1_APP_REPO_ROOT}/include")
target_link_libraries(p1_import_page
    PUBLIC p1_ui_common p1_data_import p1_data_osmx Qt6::Widgets
    PRIVATE Qt6::Concurrent)
target_compile_options(p1_import_page PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# Page-level integration test: imports a synthetic .osmx through the real page
# (background thread, cache publication, OsmxStore) and re-opens it from the
# cache after a simulated restart. Runs offscreen; no display or real data file.
if(BUILD_TESTING)
    qt_add_executable(p1-import-page-tests tests/ImportPageTests.cpp)
    target_link_libraries(p1-import-page-tests PRIVATE p1_import_page Qt6::Test)
    target_compile_options(p1-import-page-tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    add_test(NAME import-page-osmx COMMAND p1-import-page-tests)
    set_tests_properties(import-page-osmx PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
