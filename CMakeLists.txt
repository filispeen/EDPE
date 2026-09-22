cmake_minimum_required(VERSION 3.25)
project(EDPE LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EDPE requires 64-bit Windows")
endif()

add_library(EDPE SHARED src/dllmain.cpp)
target_compile_features(EDPE PRIVATE cxx_std_20)
target_compile_definitions(EDPE PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(EDPE PROPERTIES OUTPUT_NAME EDPE)
