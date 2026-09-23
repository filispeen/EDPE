cmake_minimum_required(VERSION 3.25)
project(EDPE LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EDPE requires 64-bit Windows")
endif()

add_library(EDPE SHARED src/dllmain.cpp src/d3d11_proxy.cpp src/d3d11_proxy.def)
target_compile_features(EDPE PRIVATE cxx_std_20)
target_compile_definitions(EDPE PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(EDPE PROPERTIES OUTPUT_NAME EDPE)

add_library(EDPE_DXGI SHARED src/dllmain.cpp src/dxgi_proxy.cpp src/dxgi_observer.cpp src/dxgi_proxy.def)
target_compile_features(EDPE_DXGI PRIVATE cxx_std_20)
target_compile_definitions(EDPE_DXGI PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(EDPE_DXGI PROPERTIES OUTPUT_NAME dxgi)

enable_testing()
add_executable(proxy_smoke tests/proxy_smoke.cpp)
add_test(NAME proxy_smoke COMMAND proxy_smoke $<TARGET_FILE:EDPE> $<TARGET_FILE:EDPE_DXGI>)
