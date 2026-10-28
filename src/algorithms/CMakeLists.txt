cmake_minimum_required(VERSION 3.28.3)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(P1AlgorithmsModel VERSION 0.1.0 LANGUAGES CXX)
    include(CTest)
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

foreach(model_target p1_algorithms_model p1_algorithms_model_tests)
    if(TARGET ${model_target})
        target_compile_options(${model_target} PRIVATE
            "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
            "$<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4>")
    endif()
endforeach()
