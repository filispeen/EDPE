cmake_minimum_required(VERSION 3.25)
project(EDPE LANGUAGES CXX)

# Ensure compatibility with Wine/Proton builtin MSVCP140.dll and pre-14.40 VC++ runtimes
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

if(DEFINED CACHE{EDPE_VERSION} AND NOT "$CACHE{EDPE_VERSION}" STREQUAL "")
    set(EDPE_BUILD_VERSION "$CACHE{EDPE_VERSION}")
else()
    set(EDPE_BUILD_VERSION "dev")
endif()

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EDPE requires 64-bit Windows")
endif()

option(EDPE_ENABLE_ASAN "Enable MSVC AddressSanitizer (/fsanitize=address)" OFF)
if(EDPE_ENABLE_ASAN AND MSVC)
    add_compile_options(/fsanitize=address)
    # Prebuilt SDK static libraries (FidelityFX DX11) are compiled without ASan STL annotations.
    add_compile_definitions(_DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION _DISABLE_OPTIONAL_ANNOTATION)
    add_link_options(/INCREMENTAL:NO)
endif()

set(NGX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/DLSS")
if(EXISTS "${NGX_SDK_DIR}/include/nvsdk_ngx.h")
    set(EDPE_HAVE_NGX TRUE)
else()
    set(EDPE_HAVE_NGX FALSE)
endif()

set(FFX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffx-dx11")
if(NOT EXISTS "${FFX_SDK_DIR}/include/FidelityFX/host/ffx_fsr3upscaler.h")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        message(STATUS "Fetching FidelityFX DX11 SDK...")
        execute_process(
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch_ffx_dx11.py"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE FFX_FETCH_RES
        )
    endif()
endif()
if(EXISTS "${FFX_SDK_DIR}/include/FidelityFX/host/ffx_fsr3upscaler.h")
    set(EDPE_HAVE_FSR TRUE)
else()
    set(EDPE_HAVE_FSR FALSE)
endif()

set(FFX_FG_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffx-sdk/Kits/FidelityFX")
if(EXISTS "${FFX_FG_SDK_DIR}/framegeneration/include/ffx_framegeneration.h")
    set(EDPE_HAVE_FFX_FG TRUE)
else()
    set(EDPE_HAVE_FFX_FG FALSE)
endif()

# FSR 4 (ML upscaler) through the D3D11/D3D12 bridge: needs the FidelityFX SDK headers and the signed
# amd_fidelityfx_upscaler_dx12.dll. Optional D3D12 Agility SDK runtime (D3D12Core.dll) in external/agility
# is staged under D3D12/ and used when present.
set(EDPE_AGILITY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/agility")
option(EDPE_REQUIRE_FSR4 "Fail the configure step when the FSR 4 inputs are missing instead of building the stub" OFF)
set(EDPE_FSR4_INPUTS
    "${FFX_FG_SDK_DIR}/framegeneration/include/ffx_framegeneration.h"
    "${FFX_FG_SDK_DIR}/upscalers/include/ffx_upscale.h"
    "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_upscaler_dx12.dll")
set(EDPE_HAVE_FFX_FSR4 TRUE)
set(EDPE_FSR4_MISSING "")
foreach(input IN LISTS EDPE_FSR4_INPUTS)
    if(NOT EXISTS "${input}")
        set(EDPE_HAVE_FFX_FSR4 FALSE)
        string(APPEND EDPE_FSR4_MISSING "\n  ${input}")
    endif()
endforeach()
if(EDPE_HAVE_FFX_FSR4)
    message(STATUS "FSR 4: enabled (FidelityFX upscaler headers and signed upscaler DLL found)")
elseif(EDPE_REQUIRE_FSR4)
    message(FATAL_ERROR "EDPE_REQUIRE_FSR4 is ON but the FSR 4 inputs are missing (run tools/fetch_fsr4_runtime.py):${EDPE_FSR4_MISSING}")
else()
    message(STATUS "FSR 4: disabled, building the stub (missing:${EDPE_FSR4_MISSING})")
endif()

set(STREAMLINE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/streamline")
if(EXISTS "${STREAMLINE_SDK_DIR}/include/sl_dlss_g.h")
    set(EDPE_HAVE_STREAMLINE TRUE)
else()
    set(EDPE_HAVE_STREAMLINE FALSE)
endif()

add_library(EDPE_DXGI SHARED
    src/dllmain.cpp
    src/crash_logger.cpp
    src/log.cpp
    src/d3d11_proxy.cpp
    src/dxgi_proxy.cpp
    src/dxgi_observer.cpp
    src/context_census.cpp
    src/dxbc_hash.cpp
    src/ui.cpp
    src/perf_overlay.cpp
    src/sampler_bias_manager.cpp
    src/input_hook.cpp
    src/motion_pass.cpp
    src/weapon_motion.cpp
    src/celestial_motion.cpp
    src/ui_layer.cpp
    src/ui_layer.h
    src/shaders/ui_layer_composite_cs.hlsl
    src/ui_composite_pass.cpp
    src/ngx_context.cpp
    src/upscaler_backend.cpp
    src/taa_pass.cpp
    src/sharpen_pass.cpp
    src/jitter_logger.cpp
    src/jitter_debug_pass.cpp
    src/motion_class_debug_pass.cpp
    src/jitter_view_recorder.cpp
    src/hud_debug_pass.cpp
    src/renderdoc_support.cpp
    src/d3d12_bridge.cpp
    src/d3d12_presenter.cpp
    src/fg_backend.cpp
    src/fg_ui_pack_pass.cpp
    src/amd_fsr_fg_backend.cpp
    src/amd_fsr4_engine.cpp
    src/nvidia_sl_fg_backend.cpp
    src/fg_integration.cpp
    src/frame_limiter.cpp
    src/version.cpp
    src/dxgi_proxy.def
)
if(NOT EDPE_BUILD_VERSION STREQUAL "dev")
    set_source_files_properties(src/version.cpp PROPERTIES
        COMPILE_DEFINITIONS EDPE_VERSION="${EDPE_BUILD_VERSION}")
endif()
target_compile_features(EDPE_DXGI PRIVATE cxx_std_20)
target_compile_definitions(EDPE_DXGI PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_source_files_properties(src/shaders/ui_layer_composite_cs.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
set_target_properties(EDPE_DXGI PROPERTIES OUTPUT_NAME dxgi)
add_library(EDPE ALIAS EDPE_DXGI)

option(EDPE_CRASH_TEST "Trigger intentional crash after startup to test logger" OFF)
if(EDPE_CRASH_TEST)
    target_compile_definitions(EDPE_DXGI PRIVATE EDPE_CRASH_TEST=1)
endif()

add_library(EDPE_IMGUI STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_win32.cpp
    external/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(EDPE_IMGUI PUBLIC external/imgui external/imgui/backends)
target_compile_definitions(EDPE_IMGUI PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(EDPE_DXGI PRIVATE EDPE_IMGUI d3dcompiler dxguid dbghelp shell32)

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

if(EDPE_HAVE_FFX_FG)
    target_include_directories(EDPE_DXGI PRIVATE
        "${FFX_FG_SDK_DIR}/api/include"
        "${FFX_FG_SDK_DIR}/framegeneration/include")
    target_compile_definitions(EDPE_DXGI PRIVATE EDPE_ENABLE_FFX_FG=1)
    if(EXISTS "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll")
        add_custom_command(TARGET EDPE_DXGI POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll" "$<TARGET_FILE_DIR:EDPE_DXGI>/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_framegeneration_dx12.dll" "$<TARGET_FILE_DIR:EDPE_DXGI>/")
    endif()
endif()

# Copies the signed FidelityFX loader and upscaler DLLs, and the optional Agility SDK D3D12Core.dll (under D3D12/),
# next to the built binary of `target` so it can run FSR 4.
function(edpe_stage_fsr4_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll" "$<TARGET_FILE_DIR:${target}>/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_upscaler_dx12.dll" "$<TARGET_FILE_DIR:${target}>/")
    if(EXISTS "${EDPE_AGILITY_DIR}/D3D12Core.dll")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EDPE_AGILITY_DIR}/D3D12Core.dll" "$<TARGET_FILE_DIR:${target}>/D3D12/")
    endif()
endfunction()

if(EDPE_HAVE_FFX_FSR4)
    target_include_directories(EDPE_DXGI PRIVATE "${FFX_FG_SDK_DIR}/upscalers/include")
    target_compile_definitions(EDPE_DXGI PRIVATE EDPE_ENABLE_FFX_FSR4=1)
    edpe_stage_fsr4_runtime(EDPE_DXGI)
endif()

if(EDPE_HAVE_STREAMLINE)
    target_include_directories(EDPE_DXGI PRIVATE "${STREAMLINE_SDK_DIR}/include")
    target_compile_definitions(EDPE_DXGI PRIVATE EDPE_ENABLE_STREAMLINE=1)
    if(EXISTS "${STREAMLINE_SDK_DIR}/bin/x64/sl.interposer.dll")
        foreach(_sl_dll
                sl.interposer.dll
                sl.common.dll
                sl.dlss_g.dll
                sl.reflex.dll
                sl.pcl.dll
                nvngx_dlssg.dll)
            if(EXISTS "${STREAMLINE_SDK_DIR}/bin/x64/${_sl_dll}")
                add_custom_command(TARGET EDPE_DXGI POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${STREAMLINE_SDK_DIR}/bin/x64/${_sl_dll}" "$<TARGET_FILE_DIR:EDPE_DXGI>/")
            endif()
        endforeach()
    endif()
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
add_executable(motion_stage2_test tests/motion_stage2.cpp)
target_compile_features(motion_stage2_test PRIVATE cxx_std_20)
target_compile_definitions(motion_stage2_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(motion_stage2_test PRIVATE src)
add_test(NAME motion_stage2 COMMAND motion_stage2_test)
add_executable(capture_format_test tests/capture_format.cpp)
target_compile_features(capture_format_test PRIVATE cxx_std_20)
target_include_directories(capture_format_test PRIVATE src)

add_test(NAME capture_format COMMAND capture_format_test)
add_executable(cb_map_bounce_test tests/cb_map_bounce_test.cpp)
target_compile_features(cb_map_bounce_test PRIVATE cxx_std_20)
target_include_directories(cb_map_bounce_test PRIVATE src)
add_test(NAME cb_map_bounce COMMAND cb_map_bounce_test)
add_executable(gpu_vendor_test tests/gpu_vendor_test.cpp)
target_compile_features(gpu_vendor_test PRIVATE cxx_std_20)
target_include_directories(gpu_vendor_test PRIVATE src)
add_test(NAME gpu_vendor COMMAND gpu_vendor_test)
add_executable(sampler_bias_test tests/sampler_bias_test.cpp src/sampler_bias_manager.cpp src/log.cpp)
target_compile_features(sampler_bias_test PRIVATE cxx_std_20)
target_compile_definitions(sampler_bias_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(sampler_bias_test PRIVATE src)
target_link_libraries(sampler_bias_test PRIVATE d3d11)
add_test(NAME sampler_bias COMMAND sampler_bias_test)
add_executable(atomic_file_test tests/atomic_file_test.cpp src/renderdoc_support.cpp src/log.cpp)
target_compile_features(atomic_file_test PRIVATE cxx_std_20)
target_compile_definitions(atomic_file_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(atomic_file_test PRIVATE src)
target_link_libraries(atomic_file_test PRIVATE d3d11 dxgi)
add_test(NAME atomic_file COMMAND atomic_file_test)
add_executable(frame_limiter_test tests/frame_limiter_test.cpp src/frame_limiter.cpp src/log.cpp)
target_compile_features(frame_limiter_test PRIVATE cxx_std_20)
target_compile_definitions(frame_limiter_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(frame_limiter_test PRIVATE src)
add_test(NAME frame_limiter COMMAND frame_limiter_test)
add_executable(temporal_inputs_test tests/temporal_inputs.cpp src/upscaler_backend.cpp src/amd_fsr4_engine.cpp src/ngx_context.cpp src/log.cpp src/taa_pass.cpp src/sharpen_pass.cpp)
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
add_executable(upscaler_dejitter_test tests/upscaler_dejitter_test.cpp src/upscaler_backend.cpp src/amd_fsr4_engine.cpp src/ngx_context.cpp src/log.cpp src/taa_pass.cpp src/sharpen_pass.cpp)
target_compile_features(upscaler_dejitter_test PRIVATE cxx_std_20)
target_compile_definitions(upscaler_dejitter_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(upscaler_dejitter_test PRIVATE src)
target_link_libraries(upscaler_dejitter_test PRIVATE d3d11 d3dcompiler)
if(EDPE_HAVE_NGX)
    target_include_directories(upscaler_dejitter_test PRIVATE "${NGX_SDK_DIR}/include")
    target_compile_definitions(upscaler_dejitter_test PRIVATE EDPE_ENABLE_NGX=1)
    target_link_libraries(upscaler_dejitter_test PRIVATE
        "$<IF:$<CONFIG:Debug>,${NGX_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib,${NGX_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib>"
        version)
endif()
if(EDPE_HAVE_FSR)
    target_include_directories(upscaler_dejitter_test PRIVATE "${FFX_SDK_DIR}/include")
    target_compile_definitions(upscaler_dejitter_test PRIVATE EDPE_ENABLE_FSR=1)
    target_link_libraries(upscaler_dejitter_test PRIVATE
        "${FFX_SDK_DIR}/lib/ffx_fsr3upscaler_x64.lib"
        "${FFX_SDK_DIR}/lib/ffx_backend_dx11_x64.lib")
endif()
add_test(NAME upscaler_dejitter COMMAND upscaler_dejitter_test)
add_executable(motion_gpu_test tests/motion_gpu.cpp src/motion_pass.cpp src/weapon_motion.cpp src/renderdoc_support.cpp src/log.cpp)
target_compile_features(motion_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(motion_gpu_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(motion_gpu_test PRIVATE src)
target_link_libraries(motion_gpu_test PRIVATE d3d11 d3dcompiler)
add_test(NAME motion_gpu COMMAND motion_gpu_test)
add_executable(ui_composite_test tests/ui_composite_test.cpp src/ui_composite_pass.cpp)
target_compile_features(ui_composite_test PRIVATE cxx_std_20)
target_compile_definitions(ui_composite_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX EDPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(ui_composite_test PRIVATE src)
target_link_libraries(ui_composite_test PRIVATE d3d11 d3dcompiler)
add_test(NAME ui_composite COMMAND ui_composite_test)
add_executable(hud_debug_test tests/hud_debug_test.cpp src/hud_debug_pass.cpp)
target_compile_features(hud_debug_test PRIVATE cxx_std_20)
target_compile_definitions(hud_debug_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(hud_debug_test PRIVATE src)
target_link_libraries(hud_debug_test PRIVATE d3d11 d3dcompiler)
add_test(NAME hud_debug COMMAND hud_debug_test)
add_executable(sharpen_pass_test tests/sharpen_pass_test.cpp src/sharpen_pass.cpp src/log.cpp)
target_compile_features(sharpen_pass_test PRIVATE cxx_std_20)
target_compile_definitions(sharpen_pass_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(sharpen_pass_test PRIVATE src)
target_link_libraries(sharpen_pass_test PRIVATE d3d11 d3dcompiler)
add_test(NAME sharpen_pass COMMAND sharpen_pass_test)
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
add_executable(crash_logger_test tests/crash_logger_test.cpp src/crash_logger.cpp src/log.cpp src/version.cpp)
target_compile_features(crash_logger_test PRIVATE cxx_std_20)
target_compile_definitions(crash_logger_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(crash_logger_test PRIVATE src)
target_link_libraries(crash_logger_test PRIVATE dbghelp dxgi)
add_test(NAME crash_logger COMMAND crash_logger_test)
if(EDPE_ENABLE_ASAN AND MSVC)
    # crash_logger_test intentionally triggers an access violation to verify SEH crash reporting.
    set_tests_properties(crash_logger PROPERTIES ENVIRONMENT "ASAN_OPTIONS=handle_segv=0")
endif()
add_executable(ui_menu_size_test tests/ui_menu_size_test.cpp)
target_compile_features(ui_menu_size_test PRIVATE cxx_std_20)
target_include_directories(ui_menu_size_test PRIVATE external/imgui)
target_link_libraries(ui_menu_size_test PRIVATE EDPE_IMGUI)
add_test(NAME ui_menu_size COMMAND ui_menu_size_test)

add_executable(vtable_hook_test tests/vtable_hook_test.cpp)
target_compile_features(vtable_hook_test PRIVATE cxx_std_20)
target_compile_definitions(vtable_hook_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(vtable_hook_test PRIVATE src)
add_test(NAME vtable_hook COMMAND vtable_hook_test)

add_executable(renderdoc_support_test tests/renderdoc_support_test.cpp src/renderdoc_support.cpp src/log.cpp)
target_compile_features(renderdoc_support_test PRIVATE cxx_std_20)
target_compile_definitions(renderdoc_support_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(renderdoc_support_test PRIVATE src)
target_link_libraries(renderdoc_support_test PRIVATE d3d11 dxgi)
add_test(NAME renderdoc_support COMMAND renderdoc_support_test)

add_executable(d3d11_d3d12_bridge_test tests/d3d11_d3d12_bridge_test.cpp src/d3d12_bridge.cpp src/renderdoc_support.cpp src/log.cpp)
target_compile_features(d3d11_d3d12_bridge_test PRIVATE cxx_std_20)
target_compile_definitions(d3d11_d3d12_bridge_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(d3d11_d3d12_bridge_test PRIVATE src)
target_link_libraries(d3d11_d3d12_bridge_test PRIVATE d3d11 dxgi d3dcompiler)
add_test(NAME d3d11_d3d12_bridge COMMAND d3d11_d3d12_bridge_test)

add_executable(d3d12_presenter_test tests/d3d12_presenter_test.cpp src/d3d12_presenter.cpp src/d3d12_bridge.cpp src/renderdoc_support.cpp src/log.cpp)
target_compile_features(d3d12_presenter_test PRIVATE cxx_std_20)
target_compile_definitions(d3d12_presenter_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(d3d12_presenter_test PRIVATE src)
target_link_libraries(d3d12_presenter_test PRIVATE d3d11 dxgi)
add_test(NAME d3d12_presenter COMMAND d3d12_presenter_test)

add_executable(fg_backend_test tests/fg_backend_test.cpp src/fg_backend.cpp src/fg_ui_pack_pass.cpp src/d3d12_presenter.cpp src/d3d12_bridge.cpp src/renderdoc_support.cpp src/log.cpp)
target_compile_features(fg_backend_test PRIVATE cxx_std_20)
target_compile_definitions(fg_backend_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(fg_backend_test PRIVATE src)
target_link_libraries(fg_backend_test PRIVATE d3d11 dxgi d3dcompiler)
add_test(NAME fg_backend COMMAND fg_backend_test)

add_executable(amd_fsr_fg_test tests/amd_fsr_fg_test.cpp src/amd_fsr_fg_backend.cpp src/fg_backend.cpp src/fg_ui_pack_pass.cpp src/d3d12_presenter.cpp src/d3d12_bridge.cpp src/renderdoc_support.cpp src/log.cpp)
target_compile_features(amd_fsr_fg_test PRIVATE cxx_std_20)
target_compile_definitions(amd_fsr_fg_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(amd_fsr_fg_test PRIVATE src)
target_link_libraries(amd_fsr_fg_test PRIVATE d3d11 dxgi d3dcompiler)
if(EDPE_HAVE_FFX_FG)
    target_include_directories(amd_fsr_fg_test PRIVATE
        "${FFX_FG_SDK_DIR}/api/include"
        "${FFX_FG_SDK_DIR}/framegeneration/include")
    target_compile_definitions(amd_fsr_fg_test PRIVATE EDPE_ENABLE_FFX_FG=1)
    if(EXISTS "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll")
        add_custom_command(TARGET amd_fsr_fg_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll" "$<TARGET_FILE_DIR:amd_fsr_fg_test>/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_framegeneration_dx12.dll" "$<TARGET_FILE_DIR:amd_fsr_fg_test>/")
    endif()
endif()
add_test(NAME amd_fsr_fg COMMAND amd_fsr_fg_test)

# FSR 4 pure logic (failure gate, provider selection, input mapping): CPU only, no GPU or SDK needed.
add_executable(fsr4_logic_test tests/fsr4_logic_test.cpp)
target_compile_features(fsr4_logic_test PRIVATE cxx_std_20)
target_compile_definitions(fsr4_logic_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(fsr4_logic_test PRIVATE src)
target_link_libraries(fsr4_logic_test PRIVATE d3d11 dxgi)
add_test(NAME fsr4_logic COMMAND fsr4_logic_test)

# Guard that turns a throwing provider call into an error value: CPU only, no GPU or SDK needed.
add_executable(guarded_call_test tests/guarded_call_test.cpp)
target_compile_features(guarded_call_test PRIVATE cxx_std_20)
target_include_directories(guarded_call_test PRIVATE src)
add_test(NAME guarded_call COMMAND guarded_call_test)

# FSR 4 engine smoke test (skips cleanly when the signed DLLs are not staged or the GPU does not offer FSR 4).
add_executable(amd_fsr4_engine_test tests/amd_fsr4_engine_test.cpp src/amd_fsr4_engine.cpp src/d3d12_bridge.cpp src/renderdoc_support.cpp src/log.cpp)
target_compile_features(amd_fsr4_engine_test PRIVATE cxx_std_20)
target_compile_definitions(amd_fsr4_engine_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(amd_fsr4_engine_test PRIVATE src)
target_link_libraries(amd_fsr4_engine_test PRIVATE d3d11 dxgi d3dcompiler dxguid)
if(EDPE_HAVE_FFX_FSR4)
    target_include_directories(amd_fsr4_engine_test PRIVATE
        "${FFX_FG_SDK_DIR}/api/include"
        "${FFX_FG_SDK_DIR}/upscalers/include")
    target_compile_definitions(amd_fsr4_engine_test PRIVATE EDPE_ENABLE_FFX_FSR4=1)
    edpe_stage_fsr4_runtime(amd_fsr4_engine_test)
endif()
add_test(NAME amd_fsr4_engine COMMAND amd_fsr4_engine_test)

add_executable(nvidia_sl_fg_test tests/nvidia_sl_fg_test.cpp src/nvidia_sl_fg_backend.cpp src/fg_backend.cpp src/fg_ui_pack_pass.cpp src/d3d12_presenter.cpp src/d3d12_bridge.cpp src/renderdoc_support.cpp src/log.cpp)
target_compile_features(nvidia_sl_fg_test PRIVATE cxx_std_20)
target_compile_definitions(nvidia_sl_fg_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(nvidia_sl_fg_test PRIVATE src)
target_link_libraries(nvidia_sl_fg_test PRIVATE d3d11 dxgi d3dcompiler)
if(EDPE_HAVE_STREAMLINE)
    target_include_directories(nvidia_sl_fg_test PRIVATE "${STREAMLINE_SDK_DIR}/include")
    target_compile_definitions(nvidia_sl_fg_test PRIVATE EDPE_ENABLE_STREAMLINE=1)
    if(EXISTS "${STREAMLINE_SDK_DIR}/bin/x64/sl.interposer.dll")
        foreach(_sl_dll
                sl.interposer.dll
                sl.common.dll
                sl.dlss_g.dll
                sl.reflex.dll
                sl.pcl.dll
                nvngx_dlssg.dll)
            if(EXISTS "${STREAMLINE_SDK_DIR}/bin/x64/${_sl_dll}")
                add_custom_command(TARGET nvidia_sl_fg_test POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${STREAMLINE_SDK_DIR}/bin/x64/${_sl_dll}" "$<TARGET_FILE_DIR:nvidia_sl_fg_test>/")
            endif()
        endforeach()
    endif()
endif()
add_test(NAME nvidia_sl_fg COMMAND nvidia_sl_fg_test)

add_executable(fg_integration_test
    tests/fg_integration_test.cpp
    src/fg_integration.cpp
    src/amd_fsr_fg_backend.cpp
    src/nvidia_sl_fg_backend.cpp
    src/fg_backend.cpp
    src/fg_ui_pack_pass.cpp
    src/d3d12_presenter.cpp
    src/d3d12_bridge.cpp
    src/renderdoc_support.cpp
    src/log.cpp)
target_compile_features(fg_integration_test PRIVATE cxx_std_20)
target_compile_definitions(fg_integration_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(fg_integration_test PRIVATE src)
target_link_libraries(fg_integration_test PRIVATE d3d11 dxgi d3dcompiler)
if(EDPE_HAVE_FFX_FG)
    target_include_directories(fg_integration_test PRIVATE
        "${FFX_FG_SDK_DIR}/api/include"
        "${FFX_FG_SDK_DIR}/framegeneration/include")
    target_compile_definitions(fg_integration_test PRIVATE EDPE_ENABLE_FFX_FG=1)
    if(EXISTS "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll")
        add_custom_command(TARGET fg_integration_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll" "$<TARGET_FILE_DIR:fg_integration_test>/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_framegeneration_dx12.dll" "$<TARGET_FILE_DIR:fg_integration_test>/")
    endif()
endif()
if(EDPE_HAVE_STREAMLINE)
    target_include_directories(fg_integration_test PRIVATE "${STREAMLINE_SDK_DIR}/include")
    target_compile_definitions(fg_integration_test PRIVATE EDPE_ENABLE_STREAMLINE=1)
    if(EXISTS "${STREAMLINE_SDK_DIR}/bin/x64/sl.interposer.dll")
        foreach(_sl_dll
                sl.interposer.dll
                sl.common.dll
                sl.dlss_g.dll
                sl.reflex.dll
                sl.pcl.dll
                nvngx_dlssg.dll)
            if(EXISTS "${STREAMLINE_SDK_DIR}/bin/x64/${_sl_dll}")
                add_custom_command(TARGET fg_integration_test POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${STREAMLINE_SDK_DIR}/bin/x64/${_sl_dll}" "$<TARGET_FILE_DIR:fg_integration_test>/")
            endif()
        endforeach()
    endif()
endif()
add_test(NAME fg_integration COMMAND fg_integration_test)

if(EDPE_HAVE_NGX AND EDPE_HAVE_STREAMLINE)
    add_executable(ngx_streamline_interop_test
        tests/ngx_streamline_interop_test.cpp
        src/ngx_context.cpp
        src/nvidia_sl_fg_backend.cpp
        src/d3d12_bridge.cpp
        src/d3d12_presenter.cpp
        src/fg_backend.cpp
        src/fg_ui_pack_pass.cpp
        src/renderdoc_support.cpp
        src/log.cpp)
    target_compile_features(ngx_streamline_interop_test PRIVATE cxx_std_20)
    target_compile_definitions(ngx_streamline_interop_test PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX EDPE_ENABLE_NGX=1 EDPE_ENABLE_STREAMLINE=1)
    target_include_directories(ngx_streamline_interop_test PRIVATE
        src
        "${NGX_SDK_DIR}/include"
        "${STREAMLINE_SDK_DIR}/include")
    target_link_libraries(ngx_streamline_interop_test PRIVATE
        "$<IF:$<CONFIG:Debug>,${NGX_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib,${NGX_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib>"
        d3d11 dxgi d3dcompiler version advapi32 user32)
    add_custom_command(TARGET ngx_streamline_interop_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NGX_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$<TARGET_FILE_DIR:ngx_streamline_interop_test>/")
    if(EXISTS "${STREAMLINE_SDK_DIR}/bin/x64/sl.interposer.dll")
        foreach(_sl_dll
                sl.interposer.dll
                sl.common.dll
                sl.dlss.dll
                sl.dlss_g.dll
                sl.reflex.dll
                sl.pcl.dll
                nvngx_dlssg.dll)
            if(EXISTS "${STREAMLINE_SDK_DIR}/bin/x64/${_sl_dll}")
                add_custom_command(TARGET ngx_streamline_interop_test POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${STREAMLINE_SDK_DIR}/bin/x64/${_sl_dll}" "$<TARGET_FILE_DIR:ngx_streamline_interop_test>/")
            endif()
        endforeach()
    endif()
    add_test(NAME ngx_streamline_interop COMMAND ngx_streamline_interop_test)
endif()

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
if(EDPE_HAVE_FFX_FG AND EXISTS "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll")
    add_custom_command(TARGET proxy_stage POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll" "${EDPE_STAGE}/amd_fidelityfx_loader_dx12.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFX_FG_SDK_DIR}/signedbin/amd_fidelityfx_framegeneration_dx12.dll" "${EDPE_STAGE}/amd_fidelityfx_framegeneration_dx12.dll")
endif()
if(EDPE_HAVE_STREAMLINE AND EXISTS "${STREAMLINE_SDK_DIR}/bin/x64/sl.interposer.dll")
    foreach(_sl_dll
            sl.interposer.dll
            sl.common.dll
            sl.dlss_g.dll
            sl.reflex.dll
            sl.pcl.dll
            nvngx_dlssg.dll)
        if(EXISTS "${STREAMLINE_SDK_DIR}/bin/x64/${_sl_dll}")
            add_custom_command(TARGET proxy_stage POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${STREAMLINE_SDK_DIR}/bin/x64/${_sl_dll}" "${EDPE_STAGE}/${_sl_dll}")
        endif()
    endforeach()
endif()
add_test(NAME proxy_alias_smoke COMMAND "${EDPE_STAGE}/proxy_smoke.exe"
    "${EDPE_STAGE}/dxgi.dll" "${EDPE_STAGE}/edpe.log")
set_tests_properties(proxy_alias_smoke PROPERTIES RUN_SERIAL TRUE)

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
