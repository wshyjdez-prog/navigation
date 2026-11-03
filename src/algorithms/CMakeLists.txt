cmake_minimum_required(VERSION 3.28.3)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(P1AlgorithmsModel VERSION 0.1.0 LANGUAGES CXX)
    include(CTest)
endif()

option(P1_ALGORITHMS_BUILD_ASTAR "Build the Boost.Graph A* integration" OFF)
option(P1_ALGORITHMS_BUILD_QT_ADAPTER "Build the RoutePlanner adapter and its tests" OFF)
option(P1_ALGORITHMS_BUILD_FMM_ADAPTER "Build the asynchronous FMM CLI MapMatcher" OFF)
set(P1_FMM_EXECUTABLE "" CACHE FILEPATH "Pinned external FMM executable (real sample test)")
set(P1_FMM_SAMPLE_DIR "" CACHE PATH "Pinned FMM example/data directory")
set(P1_FMM_SAMPLE_UBODT "" CACHE FILEPATH "Fresh UBODT generated from that sample network")
if(P1_ALGORITHMS_BUILD_QT_ADAPTER AND NOT P1_ALGORITHMS_BUILD_ASTAR)
    message(FATAL_ERROR "The Qt adapter requires P1_ALGORITHMS_BUILD_ASTAR=ON")
endif()

add_library(p1_algorithms_model STATIC model/Validation.cpp)
target_include_directories(p1_algorithms_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(p1_algorithms_model PUBLIC cxx_std_17)
set_target_properties(p1_algorithms_model PROPERTIES
    CXX_EXTENSIONS OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

if(BUILD_TESTING)
    add_executable(p1_algorithms_model_tests tests/ModelTests.cpp)
    target_link_libraries(p1_algorithms_model_tests PRIVATE p1_algorithms_model)
    set_target_properties(p1_algorithms_model_tests PROPERTIES
        CXX_EXTENSIONS OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_test(NAME member3-models COMMAND p1_algorithms_model_tests)
endif()

if(P1_ALGORITHMS_BUILD_ASTAR)
    find_package(Boost 1.74 REQUIRED)
    message(STATUS "Member 3 Boost version: ${Boost_VERSION_STRING}")
    add_library(p1_algorithms_astar STATIC core/BoostAStar.cpp)
    target_link_libraries(p1_algorithms_astar PUBLIC p1_algorithms_model PRIVATE Boost::headers)
    set_target_properties(p1_algorithms_astar PROPERTIES
        CXX_EXTENSIONS OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_executable(p1_algorithms_astar_demo demo/AStarDemo.cpp)
    target_link_libraries(p1_algorithms_astar_demo PRIVATE p1_algorithms_astar Boost::headers)
    set_target_properties(p1_algorithms_astar_demo PROPERTIES
        CXX_EXTENSIONS OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    if(BUILD_TESTING)
        add_executable(p1_algorithms_astar_tests tests/AStarTests.cpp)
        target_link_libraries(p1_algorithms_astar_tests PRIVATE p1_algorithms_astar)
        set_target_properties(p1_algorithms_astar_tests PROPERTIES
            CXX_EXTENSIONS OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
        add_test(NAME member3-astar COMMAND p1_algorithms_astar_tests)
    endif()
endif()

if(P1_ALGORITHMS_BUILD_QT_ADAPTER OR P1_ALGORITHMS_BUILD_FMM_ADAPTER)
    find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Core Concurrent)
    get_filename_component(member3_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    # The existing navigation target owns the public QObject moc definitions.
    # Standalone builds provide them locally, without modifying public headers.
    if(TARGET p1_navigation)
        set(member3_contracts_target p1_navigation)
    else()
        add_library(p1_algorithms_contracts STATIC
            "${member3_repo}/include/contracts/NavigationController.h")
        set_target_properties(p1_algorithms_contracts PROPERTIES AUTOMOC ON LINKER_LANGUAGE CXX)
        target_compile_features(p1_algorithms_contracts PUBLIC cxx_std_17)
        target_include_directories(p1_algorithms_contracts PUBLIC "${member3_repo}/include")
        target_link_libraries(p1_algorithms_contracts PUBLIC Qt6::Core)
        set(member3_contracts_target p1_algorithms_contracts)
    endif()
endif()

if(P1_ALGORITHMS_BUILD_QT_ADAPTER)
    add_library(p1_algorithms_route_planner STATIC qt/BoostRoutePlanner.cpp qt/BoostRoutePlanner.h)
    set_target_properties(p1_algorithms_route_planner PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_include_directories(p1_algorithms_route_planner PUBLIC "${member3_repo}/include")
    target_link_libraries(p1_algorithms_route_planner PUBLIC
        p1_algorithms_astar ${member3_contracts_target} Qt6::Core PRIVATE Qt6::Concurrent)
    if(BUILD_TESTING)
        find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Test)
        add_executable(p1_algorithms_route_planner_tests tests/RoutePlannerTests.cpp)
        set_target_properties(p1_algorithms_route_planner_tests PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(p1_algorithms_route_planner_tests PRIVATE
            p1_algorithms_route_planner Qt6::Test Qt6::Concurrent)
        add_test(NAME member3-route-planner COMMAND p1_algorithms_route_planner_tests)
    endif()
endif()

if(P1_ALGORITHMS_BUILD_FMM_ADAPTER)
    add_library(p1_algorithms_fmm STATIC
        fmm/FmmClient.cpp fmm/FmmClient.h qt/FmmMapMatcher.cpp qt/FmmMapMatcher.h)
    set_target_properties(p1_algorithms_fmm PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_compile_features(p1_algorithms_fmm PUBLIC cxx_std_17)
    target_include_directories(p1_algorithms_fmm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(p1_algorithms_fmm PUBLIC
        ${member3_contracts_target} p1_algorithms_model Qt6::Core PRIVATE Qt6::Concurrent)
    if(BUILD_TESTING)
        find_package(Qt6 6.4.2 EXACT REQUIRED COMPONENTS Test)
        # This helper tests subprocess scheduling only, NEVER HMM correctness.
        add_executable(p1_algorithms_fmm_process_fixture tests/FmmProcessFixture.cpp)
        target_link_libraries(p1_algorithms_fmm_process_fixture PRIVATE Qt6::Core)
        target_compile_features(p1_algorithms_fmm_process_fixture PRIVATE cxx_std_17)
        set_target_properties(p1_algorithms_fmm_process_fixture PROPERTIES AUTOMOC OFF)
        add_executable(p1_algorithms_fmm_tests tests/FmmTests.cpp)
        set_target_properties(p1_algorithms_fmm_tests PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(p1_algorithms_fmm_tests PRIVATE p1_algorithms_fmm Qt6::Test)
        add_dependencies(p1_algorithms_fmm_tests p1_algorithms_fmm_process_fixture)
        add_test(NAME member3-fmm-adapter COMMAND p1_algorithms_fmm_tests
            parseResults invalidBatch windowAndInvalidRequests asyncLifecycle failureDelivery)
        set_tests_properties(member3-fmm-adapter PROPERTIES ENVIRONMENT
            "P1_FMM_PROCESS_FIXTURE=$<TARGET_FILE:p1_algorithms_fmm_process_fixture>")
        if(P1_FMM_EXECUTABLE OR P1_FMM_SAMPLE_DIR OR P1_FMM_SAMPLE_UBODT)
            if(NOT EXISTS "${P1_FMM_EXECUTABLE}" OR
               NOT EXISTS "${P1_FMM_SAMPLE_DIR}/trips.csv" OR
               NOT EXISTS "${P1_FMM_SAMPLE_DIR}/edges.shp" OR
               NOT EXISTS "${P1_FMM_SAMPLE_UBODT}")
                message(FATAL_ERROR "Real FMM test requires executable, sample data and generated UBODT paths")
            endif()
            add_test(NAME member3-fmm-official COMMAND p1_algorithms_fmm_tests officialSample)
            set_tests_properties(member3-fmm-official PROPERTIES TIMEOUT 180 ENVIRONMENT
                "P1_FMM_EXECUTABLE=${P1_FMM_EXECUTABLE};P1_FMM_SAMPLE_DIR=${P1_FMM_SAMPLE_DIR};P1_FMM_SAMPLE_UBODT=${P1_FMM_SAMPLE_UBODT};P1_FMM_RESULT_DIR=${CMAKE_CURRENT_BINARY_DIR}/fmm-results")
        else()
            message(STATUS "Real FMM HMM test NOT registered: supply P1_FMM_* paths")
        endif()
    endif()
endif()

foreach(model_target p1_algorithms_model p1_algorithms_model_tests
        p1_algorithms_astar p1_algorithms_astar_tests p1_algorithms_astar_demo
        p1_algorithms_route_planner p1_algorithms_route_planner_tests
        p1_algorithms_fmm p1_algorithms_fmm_tests p1_algorithms_fmm_process_fixture)
    if(TARGET ${model_target})
        target_compile_options(${model_target} PRIVATE
            "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
            "$<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4>")
    endif()
endforeach()
