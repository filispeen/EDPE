cmake_minimum_required(VERSION 3.25)
project(EDPE LANGUAGES CXX)

if(DEFINED CACHE{EDPE_VERSION} AND NOT "$CACHE{EDPE_VERSION}" STREQUAL "")
    set(EDPE_BUILD_VERSION "$CACHE{EDPE_VERSION}")
else()
    set(EDPE_BUILD_VERSION "dev")
endif()

if(NOT EDPE_BUILD_VERSION STREQUAL "dev")
    add_compile_definitions(EDPE_VERSION="${EDPE_BUILD_VERSION}")
endif()

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EDPE requires 64-bit Windows")
endif()

set(NGX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/DLSS")
if(EXISTS "${NGX_SDK_DIR}/include/nvsdk_ngx.h")
    set(EDPE_HAVE_NGX TRUE)
else()
    set(EDPE_HAVE_NGX FALSE)
endif()

set(FFX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffx-dx11")
if(EXISTS "${FFX_SDK_DIR}/include/FidelityFX/host/ffx_fsr3upscaler.h")
    set(EDPE_HAVE_FSR TRUE)
else()
    set(EDPE_HAVE_FSR FALSE)
endif()

add_library(EDPE_DXGI SHARED
    src/dllmain.cpp
    src/log.cpp
    src/d3d11_proxy.cpp
    src/dxgi_proxy.cpp
    src/dxgi_observer.cpp
    src/context_census.cpp
    src/dxbc_hash.cpp
    src/ui.cpp
    src/perf_overlay.cpp
    src/input_hook.cpp
    src/motion_pass.cpp
    src/ui_composite_pass.cpp
    src/ngx_context.cpp
    src/upscaler_backend.cpp
    src/taa_pass.cpp
    src/jitter_logger.cpp
    src/jitter_debug_pass.cpp
    src/jitter_view_recorder.cpp
    src/dxgi_proxy.def
)
target_compile_features(EDPE_DXGI PRIVATE cxx_std_20)
target_compile_definitions(EDPE_DXGI PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(EDPE_DXGI PROPERTIES OUTPUT_NAME dxgi)
add_library(EDPE ALIAS EDPE_DXGI)

add_library(EDPE_IMGUI STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_win32.cpp
    external/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(EDPE_IMGUI PUBLIC external/imgui external/imgui/backends)
target_compile_definitions(EDPE_IMGUI PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(EDPE_DXGI PRIVATE EDPE_IMGUI d3dcompiler dxguid)

if(EDPE_HAVE_NGX)
    target_include_directories(EDPE_DXGI PRIVATE "${NGX_SDK_DIR}/include")
    target_compile_definitions(EDPE_DXGI PRIVATE EDPE_ENABLE_NGX=1)
    target_link_libraries(EDPE_DXGI PRIVATE
        "$<IF:$<CONFIG:Debug>,${NGX_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib,${NGX_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib>"
        version)
    add_custom_command(TARGET EDPE_DXGI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NGX_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$<TARGET_FILE_DIR:EDPE_DXGI>/")
endif()

if(EDPE_HAVE_FSR)
    target_include_directories(EDPE_DXGI PRIVATE "${FFX_SDK_DIR}/include")
    target_compile_definitions(EDPE_DXGI PRIVATE EDPE_ENABLE_FSR=1)
    target_link_libraries(EDPE_DXGI PRIVATE
        "${FFX_SDK_DIR}/lib/ffx_fsr3upscaler_x64.lib"
        "${FFX_SDK_DIR}/lib/ffx_backend_dx11_x64.lib")
endif()

enable_testing()
add_executable(temporal_math_test tests/temporal_math.cpp)
target_compile_features(temporal_math_test PRIVATE cxx_std_20)
target_include_directories(temporal_math_test PRIVATE src)
add_test(NAME temporal_math COMMAND temporal_math_test)
add_executable(camera_delta_test tests/camera_delta.cpp)
target_compile_features(camera_delta_test PRIVATE cxx_std_20)
target_include_directories(camera_delta_test PRIVATE src)
add_test(NAME camera_delta COMMAND camera_delta_test)
add_executable(capture_format_test tests/capture_format.cpp)
target_compile_features(capture_format_test PRIVATE cxx_std_20)
target_include_directories(capture_format_test PRIVATE src)
add_test(NAME capture_format COMMAND capture_format_test)
add_executable(temporal_inputs_test tests/temporal_inputs.cpp src/upscaler_backend.cpp src/ngx_context.cpp src/log.cpp src/taa_pass.cpp)
target_compile_features(temporal_inputs_test PRIVATE cxx_std_20)
target_compile_definitions(temporal_inputs_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(temporal_inputs_test PRIVATE src)
target_link_libraries(temporal_inputs_test PRIVATE d3d11 d3dcompiler)
if(EDPE_HAVE_NGX)
    target_include_directories(temporal_inputs_test PRIVATE "${NGX_SDK_DIR}/include")
    target_compile_definitions(temporal_inputs_test PRIVATE EDPE_ENABLE_NGX=1)
    target_link_libraries(temporal_inputs_test PRIVATE
        "$<IF:$<CONFIG:Debug>,${NGX_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib,${NGX_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib>"
        version)
endif()
if(EDPE_HAVE_FSR)
    target_include_directories(temporal_inputs_test PRIVATE "${FFX_SDK_DIR}/include")
    target_compile_definitions(temporal_inputs_test PRIVATE EDPE_ENABLE_FSR=1)
    target_link_libraries(temporal_inputs_test PRIVATE
        "${FFX_SDK_DIR}/lib/ffx_fsr3upscaler_x64.lib"
        "${FFX_SDK_DIR}/lib/ffx_backend_dx11_x64.lib")
endif()
add_test(NAME temporal_inputs COMMAND temporal_inputs_test)
add_executable(motion_gpu_test tests/motion_gpu.cpp src/motion_pass.cpp)
target_compile_features(motion_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(motion_gpu_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(motion_gpu_test PRIVATE src)
target_link_libraries(motion_gpu_test PRIVATE d3d11 d3dcompiler)
add_test(NAME motion_gpu COMMAND motion_gpu_test)
add_executable(ui_composite_test tests/ui_composite_test.cpp src/ui_composite_pass.cpp)
target_compile_features(ui_composite_test PRIVATE cxx_std_20)
target_compile_definitions(ui_composite_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(ui_composite_test PRIVATE src)
target_link_libraries(ui_composite_test PRIVATE d3d11 d3dcompiler)
add_test(NAME ui_composite COMMAND ui_composite_test)
add_executable(dxbc_fanout_test tests/dxbc_fanout.cpp src/dxbc_hash.cpp)
target_compile_features(dxbc_fanout_test PRIVATE cxx_std_20)
target_compile_definitions(dxbc_fanout_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(dxbc_fanout_test PRIVATE src)
target_link_libraries(dxbc_fanout_test PRIVATE d3d11 d3dcompiler)
add_test(NAME dxbc_fanout COMMAND dxbc_fanout_test)
add_executable(jitter_view_report_test tests/jitter_view_report_test.cpp)
target_compile_features(jitter_view_report_test PRIVATE cxx_std_20)
target_include_directories(jitter_view_report_test PRIVATE src)
add_test(NAME jitter_view_report COMMAND jitter_view_report_test)
add_executable(proxy_smoke tests/proxy_smoke.cpp)
target_include_directories(proxy_smoke PRIVATE src)
target_link_libraries(proxy_smoke PRIVATE d3dcompiler)
add_test(NAME proxy_smoke COMMAND proxy_smoke $<TARGET_FILE:EDPE_DXGI> $<TARGET_FILE_DIR:proxy_smoke>/edpe.log)
add_test(NAME proxy_shader_probe COMMAND proxy_smoke $<TARGET_FILE:EDPE_DXGI> $<TARGET_FILE_DIR:proxy_smoke>/edpe.log shader-probe)
set_tests_properties(proxy_smoke proxy_shader_probe PROPERTIES RUN_SERIAL TRUE)

set(EDPE_STAGE "${CMAKE_CURRENT_BINARY_DIR}/stage")
add_custom_target(proxy_stage ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDPE_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:EDPE_DXGI> "${EDPE_STAGE}/dxgi.dll"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:proxy_smoke> "${EDPE_STAGE}/proxy_smoke.exe"
    DEPENDS EDPE_DXGI proxy_smoke)
if(EDPE_HAVE_NGX)
    add_custom_command(TARGET proxy_stage POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NGX_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" "${EDPE_STAGE}/nvngx_dlss.dll")
endif()
add_test(NAME proxy_alias_smoke COMMAND "${EDPE_STAGE}/proxy_smoke.exe"
    "${EDPE_STAGE}/dxgi.dll" "${EDPE_STAGE}/edpe.log")

# === NGX PROBE (standalone, never part of EDPE.dll or the proxy) ===
# ngx_probe       child: real NGX SDK calls on a hardware adapter, one log line per stage
# ngx_probe_host  parent: runs the child with a 30 s wall-clock timeout and kills it on hang
# The SDK lives in external/DLSS (gitignored); the probe is skipped when it is absent.
if(EDPE_HAVE_NGX)
    add_executable(ngx_probe tools/ngx_probe/ngx_probe.cpp)
    target_compile_features(ngx_probe PRIVATE cxx_std_20)
    target_compile_definitions(ngx_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(ngx_probe PRIVATE "${NGX_SDK_DIR}/include" tools/ngx_probe)
    # x64, dynamic CRT (/MD, /MDd): nvsdk_ngx_d*.lib are the dynamic-CRT variants.
    target_link_libraries(ngx_probe PRIVATE
        "$<IF:$<CONFIG:Debug>,${NGX_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib,${NGX_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib>"
        d3d11 dxgi version)
    add_custom_command(TARGET ngx_probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NGX_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$<TARGET_FILE_DIR:ngx_probe>/")

    add_executable(ngx_probe_host tools/ngx_probe/ngx_probe_host.cpp)
    target_compile_features(ngx_probe_host PRIVATE cxx_std_20)
    target_compile_definitions(ngx_probe_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(ngx_probe_host PRIVATE tools/ngx_probe)
    add_dependencies(ngx_probe_host ngx_probe)
else()
    message(STATUS "external/DLSS not found: ngx_probe and ngx_probe_host are not built")
endif()
