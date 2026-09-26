cmake_minimum_required(VERSION 3.25)
project(EDPE LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EDPE requires 64-bit Windows")
endif()

add_library(EDPE SHARED src/dllmain.cpp src/log.cpp src/d3d11_proxy.cpp src/d3d11_proxy.def)
target_compile_features(EDPE PRIVATE cxx_std_20)
target_compile_definitions(EDPE PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(EDPE PROPERTIES OUTPUT_NAME EDPE)

add_library(EDPE_DXGI SHARED src/dllmain.cpp src/log.cpp src/dxgi_proxy.cpp src/dxgi_observer.cpp src/context_census.cpp src/ui.cpp src/dxgi_proxy.def)
target_compile_features(EDPE_DXGI PRIVATE cxx_std_20)
target_compile_definitions(EDPE_DXGI PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(EDPE_DXGI PROPERTIES OUTPUT_NAME dxgi)

add_library(EDPE_IMGUI STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_win32.cpp
    external/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(EDPE_IMGUI PUBLIC external/imgui external/imgui/backends)
target_compile_definitions(EDPE_IMGUI PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(EDPE_DXGI PRIVATE EDPE_IMGUI d3dcompiler)

enable_testing()
add_executable(temporal_math_test tests/temporal_math.cpp)
target_compile_features(temporal_math_test PRIVATE cxx_std_20)
target_include_directories(temporal_math_test PRIVATE src)
add_test(NAME temporal_math COMMAND temporal_math_test)
add_executable(motion_gpu_test tests/motion_gpu.cpp)
target_compile_features(motion_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(motion_gpu_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(motion_gpu_test PRIVATE src)
target_link_libraries(motion_gpu_test PRIVATE d3d11 d3dcompiler)
add_test(NAME motion_gpu COMMAND motion_gpu_test)
add_executable(proxy_smoke tests/proxy_smoke.cpp)
target_link_libraries(proxy_smoke PRIVATE d3dcompiler)
add_test(NAME proxy_smoke COMMAND proxy_smoke $<TARGET_FILE:EDPE> $<TARGET_FILE:EDPE_DXGI> $<TARGET_FILE_DIR:proxy_smoke>/edpe.log)

set(EDPE_STAGE "${CMAKE_CURRENT_BINARY_DIR}/stage")
add_custom_target(proxy_stage ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDPE_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:EDPE> "${EDPE_STAGE}/d3d11.dll"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:EDPE_DXGI> "${EDPE_STAGE}/dxgi.dll"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:proxy_smoke> "${EDPE_STAGE}/proxy_smoke.exe"
    DEPENDS EDPE EDPE_DXGI proxy_smoke)
add_test(NAME proxy_alias_smoke COMMAND "${EDPE_STAGE}/proxy_smoke.exe"
    "${EDPE_STAGE}/d3d11.dll" "${EDPE_STAGE}/dxgi.dll" "${EDPE_STAGE}/edpe.log")
