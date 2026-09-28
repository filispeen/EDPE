#include "ui.h"
#include "log.h"
#include "context_census.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
struct InputMessage {
    HWND window;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
};

struct UiState {
    IDXGISwapChain* swap_chain = nullptr; // Weak: the game's Release owns its lifetime.
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11RenderTargetView* backbuffer_rtv = nullptr;
    ID3D11Texture2D* depth_copy = nullptr;
    ID3D11ShaderResourceView* depth_srv = nullptr;
    ID3D11Texture2D* color_copy = nullptr;
    ID3D11ShaderResourceView* color_srv = nullptr;
    ID3D11ShaderResourceView* early_color_srv = nullptr;
    ID3D11ShaderResourceView* motion_srv = nullptr;
    ID3D11Texture2D* frame_staging = nullptr;
    DXGI_FORMAT frame_format = DXGI_FORMAT_UNKNOWN;
    UINT frame_width = 0;
    UINT frame_height = 0;
    unsigned frame_attempts = 0;
    unsigned frame_serial = 0;
    bool snapshot_ready = false;
    UINT motion_width = 0;
    UINT motion_height = 0;
    bool motion_window_open = true;
    ID3D11PixelShader* motion_preview_shader = nullptr;
    bool motion_shader_failed = false;
    UINT color_width = 0;
    UINT color_height = 0;
    int color_snapshot_index = -1;
    bool color_window_open = true;
    bool color_image_logged = false;
    UINT early_color_width = 0;
    UINT early_color_height = 0;
    bool early_color_window_open = true;
    ID3D11PixelShader* depth_contrast_shader = nullptr;
    bool depth_shader_failed = false;
    bool depth_contrast = true;
    bool depth_shader_logged = false;
    ID3D11Texture2D* depth_samples = nullptr;
    DXGI_FORMAT depth_sample_format = DXGI_FORMAT_UNKNOWN;
    unsigned depth_sample_attempts = 0;
    bool depth_range_valid = false;
    bool depth_center_valid = false;
    float depth_min = 0;
    float depth_max = 0;
    float depth_center = 0;
    UINT depth_width = 0;
    UINT depth_height = 0;
    int depth_candidate = 1;
    int depth_snapshot_index = -1;
    bool depth_window_open = true;
    bool depth_image_logged = false;
    ImGuiContext* imgui = nullptr;
    HWND window = nullptr;
    WNDPROC original_wndproc = nullptr;
    bool failed = false;
    bool input_logged = false;
} ui;
std::atomic<bool> menu_visible{false};
std::mutex input_mutex;
std::vector<InputMessage> pending_input;
std::vector<InputMessage> render_input;

bool isInputMessage(UINT message) {
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) return true;
    if (message >= WM_KEYFIRST && message <= WM_KEYLAST) return true;
    return message == WM_INPUT;
}

LRESULT CALLBACK edpeWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        wparam == VK_F5 && !(lparam & (1LL << 30))) {
        menu_visible.store(!menu_visible.load());
        return 0;
    }
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wparam == VK_F5) return 0;
    if (menu_visible.load()) {
        if (message == WM_INPUT) return DefWindowProcW(window, message, wparam, lparam);
        if (isInputMessage(message)) {
            std::lock_guard lock(input_mutex);
            const InputMessage input{window, message, wparam, lparam};
            if (message == WM_MOUSEMOVE && !pending_input.empty() &&
                pending_input.back().message == WM_MOUSEMOVE) {
                pending_input.back() = input;
            } else {
                if (pending_input.size() == 256) pending_input.erase(pending_input.begin());
                pending_input.push_back(input);
            }
            return 1;
        }
    }
    return ui.original_wndproc ? CallWindowProcW(ui.original_wndproc, window, message, wparam, lparam)
                               : DefWindowProcW(window, message, wparam, lparam);
}

void releaseBackbuffer() {
    if (ui.backbuffer_rtv) ui.backbuffer_rtv->Release();
    ui.backbuffer_rtv = nullptr;
}

void releaseDepthSnapshot() {
    if (ui.depth_samples) ui.depth_samples->Release();
    if (ui.depth_srv) ui.depth_srv->Release();
    if (ui.depth_copy) ui.depth_copy->Release();
    ui.depth_srv = nullptr;
    ui.depth_copy = nullptr;
    ui.depth_samples = nullptr;
    ui.depth_range_valid = false;
    ui.depth_center_valid = false;
    ui.depth_snapshot_index = -1;
    ui.depth_image_logged = false;
}

void releaseColorSnapshot() {
    if (ui.color_srv) ui.color_srv->Release();
    if (ui.color_copy) ui.color_copy->Release();
    ui.color_srv = nullptr;
    ui.color_copy = nullptr;
    ui.color_snapshot_index = -1;
    ui.color_image_logged = false;
}

void releaseEarlyColorSnapshot() {
    if (ui.early_color_srv) ui.early_color_srv->Release();
    ui.early_color_srv = nullptr;
}

void releaseMotionSnapshot() {
    if (ui.motion_srv) ui.motion_srv->Release();
    ui.motion_srv = nullptr;
}

void pollFrameCapture() {
    if (!ui.frame_staging) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = ui.context->Map(ui.frame_staging, 0, D3D11_MAP_READ,
        D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING && ++ui.frame_attempts < 120) return;
    if (SUCCEEDED(result)) {
        wchar_t executable[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        std::error_code error;
        if (length && length < MAX_PATH) {
            const auto folder = std::filesystem::path(executable).parent_path() /
                L"edpe-captures";
            std::filesystem::create_directories(folder, error);
            if (!error) {
                SYSTEMTIME now{};
                GetLocalTime(&now);
                wchar_t name[96];
                swprintf_s(name, L"capture-%04u%02u%02u-%02u%02u%02u-%03u-%u.bmp",
                    now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                    now.wSecond, now.wMilliseconds, ++ui.frame_serial);
                const auto path = folder / name;
                std::ofstream file(path, std::ios::binary);
                const DWORD image_bytes = ui.frame_width * ui.frame_height * 4;
                BITMAPFILEHEADER header{};
                header.bfType = 0x4D42;
                header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
                header.bfSize = header.bfOffBits + image_bytes;
                BITMAPINFOHEADER info{};
                info.biSize = sizeof(info);
                info.biWidth = static_cast<LONG>(ui.frame_width);
                info.biHeight = -static_cast<LONG>(ui.frame_height); // Top-down D3D rows.
                info.biPlanes = 1;
                info.biBitCount = 32;
                info.biCompression = BI_RGB;
                info.biSizeImage = image_bytes;
                file.write(reinterpret_cast<const char*>(&header), sizeof(header));
                file.write(reinterpret_cast<const char*>(&info), sizeof(info));
                std::vector<unsigned char> row(ui.frame_width * 4);
                for (UINT y = 0; y < ui.frame_height && file; ++y) {
                    const auto* source = static_cast<const unsigned char*>(mapped.pData) +
                        static_cast<size_t>(y) * mapped.RowPitch;
                    for (UINT x = 0; x < ui.frame_width; ++x) {
                        const auto* pixel = source + 4 * x;
                        auto* output = row.data() + 4 * x;
                        output[0] = ui.frame_format == DXGI_FORMAT_R8G8B8A8_UNORM ? pixel[2] : pixel[0];
                        output[1] = pixel[1];
                        output[2] = ui.frame_format == DXGI_FORMAT_R8G8B8A8_UNORM ? pixel[0] : pixel[2];
                        output[3] = 255;
                    }
                    file.write(reinterpret_cast<const char*>(row.data()), row.size());
                }
                file.flush();
                if (file) EdpeLog((L"EDPE: frame capture saved " + path.wstring()).c_str());
                else EdpeLog(L"EDPE: frame capture file write failed");
            }
        }
        if (error || !length || length >= MAX_PATH)
            EdpeLog(L"EDPE: frame capture directory unavailable");
        ui.context->Unmap(ui.frame_staging, 0);
    } else {
        EdpeLog(L"EDPE: frame capture readback unavailable");
    }
    ui.frame_staging->Release();
    ui.frame_staging = nullptr;
}

void queueFrameCapture(IDXGISwapChain* swap_chain) {
    if (ui.frame_staging) return;
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return;
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    if ((desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) ||
        !desc.Width || !desc.Height || desc.SampleDesc.Count != 1 ||
        static_cast<unsigned long long>(desc.Width) * desc.Height > 3840ull * 2160) {
        EdpeLog(L"EDPE: frame capture skipped (unsupported backbuffer)");
        backbuffer->Release();
        return;
    }
    ui.frame_format = desc.Format;
    ui.frame_width = desc.Width;
    ui.frame_height = desc.Height;
    ui.frame_attempts = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (SUCCEEDED(ui.device->CreateTexture2D(&desc, nullptr, &ui.frame_staging))) {
        ui.context->CopyResource(ui.frame_staging, backbuffer);
    } else {
        EdpeLog(L"EDPE: frame capture staging texture unavailable");
    }
    backbuffer->Release();
}

void bindMotionPreviewShader(const ImDrawList*, const ImDrawCmd*) {
    auto* state = static_cast<ImGui_ImplDX11_RenderState*>(
        ImGui::GetPlatformIO().Renderer_RenderState);
    if (state && ui.motion_preview_shader)
        state->DeviceContext->PSSetShader(ui.motion_preview_shader, nullptr, 0);
}

void ensureMotionPreviewShader() {
    if (ui.motion_preview_shader || ui.motion_shader_failed) return;
    static constexpr char source[] = R"(
        struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
        Texture2D<float2> texture0 : register(t0);
        SamplerState sampler0 : register(s0);
        float4 main(PS_INPUT input) : SV_Target {
            float2 motion = texture0.Sample(sampler0, input.uv);
            float2 color = 0.5 + 0.5 * motion / (abs(motion) + 10.0);
            return float4(color, 0.5, 1.0) * input.col;
        }
    )";
    ID3DBlob* bytecode = nullptr;
    HRESULT result = D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr,
        "main", "ps_4_0", 0, 0, &bytecode, nullptr);
    if (SUCCEEDED(result)) result = ui.device->CreatePixelShader(bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(), nullptr, &ui.motion_preview_shader);
    if (bytecode) bytecode->Release();
    if (FAILED(result)) {
        ui.motion_shader_failed = true;
        EdpeLog(L"EDPE: motion preview shader unavailable");
    } else {
        EdpeLog(L"EDPE: motion preview shader ready");
    }
}

void bindDepthContrastShader(const ImDrawList*, const ImDrawCmd*) {
    auto* state = static_cast<ImGui_ImplDX11_RenderState*>(
        ImGui::GetPlatformIO().Renderer_RenderState);
    if (!state || !ui.depth_contrast_shader) return;
    state->DeviceContext->PSSetShader(ui.depth_contrast_shader, nullptr, 0);
    if (!ui.depth_shader_logged) {
        EdpeLog(L"EDPE: depth contrast shader active in ImGui");
        ui.depth_shader_logged = true;
    }
}

void ensureDepthContrastShader() {
    if (ui.depth_contrast_shader || ui.depth_shader_failed) return;
    static constexpr char source[] = R"(
        struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
        Texture2D<float> texture0 : register(t0);
        SamplerState sampler0 : register(s0);
        float4 main(PS_INPUT input) : SV_Target {
            float depth = texture0.Sample(sampler0, input.uv);
            float value = saturate(depth / (depth + 0.001));
            return float4(value, value, value, 1.0) * input.col;
        }
    )";
    ID3DBlob* bytecode = nullptr;
    HRESULT result = D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr,
        "main", "ps_4_0", 0, 0, &bytecode, nullptr);
    if (SUCCEEDED(result)) {
        result = ui.device->CreatePixelShader(bytecode->GetBufferPointer(),
            bytecode->GetBufferSize(), nullptr, &ui.depth_contrast_shader);
    }
    if (bytecode) bytecode->Release();
    if (FAILED(result)) {
        ui.depth_shader_failed = true;
        wchar_t message[128];
        swprintf_s(message, L"EDPE: depth contrast unavailable HRESULT=0x%08X",
            static_cast<unsigned>(result));
        EdpeLog(message);
    }
}

void queueDepthSamples(const D3D11_TEXTURE2D_DESC& source_desc) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 8;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = source_desc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ui.device->CreateTexture2D(&desc, nullptr, &ui.depth_samples))) {
        EdpeLog(L"EDPE: depth sample grid unavailable (staging creation failed)");
        return;
    }
    ui.depth_sample_format = desc.Format;
    ui.depth_sample_attempts = 0;
    for (UINT y = 0; y < 8; ++y) {
        for (UINT x = 0; x < 8; ++x) {
            const UINT sx = x == 4 && y == 4 ? source_desc.Width / 2
                : (2 * x + 1) * source_desc.Width / 16;
            const UINT sy = x == 4 && y == 4 ? source_desc.Height / 2
                : (2 * y + 1) * source_desc.Height / 16;
            const D3D11_BOX box{sx, sy, 0, sx + 1, sy + 1, 1};
            ui.context->CopySubresourceRegion(ui.depth_samples, 0, x, y, 0,
                ui.depth_copy, 0, &box);
        }
    }
}

void pollDepthSamples() {
    if (!ui.depth_samples) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = ui.context->Map(ui.depth_samples, 0, D3D11_MAP_READ,
        D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING && ++ui.depth_sample_attempts < 120) return;
    if (FAILED(result)) {
        EdpeLog(L"EDPE: depth sample grid unavailable (nonblocking map failed or timed out)");
    } else {
        float minimum = 1.0f;
        float maximum = 0.0f;
        unsigned valid = 0;
        unsigned nonzero = 0;
        for (UINT y = 0; y < 8; ++y) {
            const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < 8; ++x) {
                float depth = 0;
                if (ui.depth_sample_format == DXGI_FORMAT_R32G8X24_TYPELESS) {
                    std::memcpy(&depth, row + 8 * x, sizeof(depth));
                } else {
                    uint32_t packed = 0;
                    std::memcpy(&packed, row + 4 * x, sizeof(packed));
                    depth = static_cast<float>(packed & 0x00FFFFFFu) / 16777215.0f;
                }
                if (!std::isfinite(depth)) continue;
                if (x == 4 && y == 4) {
                    ui.depth_center = depth;
                    ui.depth_center_valid = true;
                }
                if (depth < minimum) minimum = depth;
                if (depth > maximum) maximum = depth;
                if (depth > 0.0f) ++nonzero;
                ++valid;
            }
        }
        ui.context->Unmap(ui.depth_samples, 0);
        ui.depth_range_valid = valid != 0;
        ui.depth_min = minimum;
        ui.depth_max = maximum;
        wchar_t message[220];
        swprintf_s(message, L"EDPE: depth sample grid valid=%u nonzero=%u min=%.9g max=%.9g centerValid=%u center=%.9g",
            valid, nonzero, minimum, maximum,
            static_cast<unsigned>(ui.depth_center_valid), ui.depth_center);
        EdpeLog(message);
    }
    ui.depth_samples->Release();
    ui.depth_samples = nullptr;
}

void captureDepthSnapshot(ID3D11DepthStencilView* view, unsigned index) {
    ID3D11DepthStencilView* bound = nullptr;
    ui.context->OMGetRenderTargets(0, nullptr, &bound);
    if (bound) {
        bound->Release();
        EdpeLog(L"EDPE: depth snapshot skipped (depth still bound at Present)");
        return;
    }
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    ID3D11Texture2D* source = nullptr;
    if (resource) {
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
        resource->Release();
    }
    if (!source) {
        EdpeLog(L"EDPE: depth snapshot skipped (not a Texture2D)");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    DXGI_FORMAT read_format = DXGI_FORMAT_UNKNOWN;
    if (desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS) read_format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    if (desc.Format == DXGI_FORMAT_R24G8_TYPELESS) read_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    if (!desc.Width || !desc.Height || desc.MipLevels != 1 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 || read_format == DXGI_FORMAT_UNKNOWN) {
        source->Release();
        EdpeLog(L"EDPE: depth snapshot skipped (unsupported texture layout or format)");
        return;
    }
    D3D11_TEXTURE2D_DESC copy_desc = desc;
    copy_desc.Usage = D3D11_USAGE_DEFAULT;
    copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy_desc.CPUAccessFlags = 0;
    copy_desc.MiscFlags = 0;
    ID3D11Texture2D* copy = nullptr;
    HRESULT result = ui.device->CreateTexture2D(&copy_desc, nullptr, &copy);
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(result)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = read_format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        result = ui.device->CreateShaderResourceView(copy, &srv_desc, &srv);
    }
    if (SUCCEEDED(result)) {
        ui.context->CopyResource(copy, source);
        releaseDepthSnapshot();
        ui.depth_copy = copy;
        ui.depth_srv = srv;
        ui.depth_width = desc.Width;
        ui.depth_height = desc.Height;
        ui.depth_snapshot_index = static_cast<int>(index);
        ui.depth_window_open = true;
        ui.snapshot_ready = true;
        ensureDepthContrastShader();
        queueDepthSamples(desc);
        wchar_t message[160];
        swprintf_s(message, L"EDPE: depth snapshot #%u copied %ux%u format=%u",
            index, desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
        EdpeLog(message);
    } else {
        if (srv) srv->Release();
        if (copy) copy->Release();
        wchar_t message[128];
        swprintf_s(message, L"EDPE: depth snapshot unavailable HRESULT=0x%08X",
            static_cast<unsigned>(result));
        EdpeLog(message);
    }
    source->Release();
}

void captureColorSnapshot(ID3D11RenderTargetView* view, unsigned index) {
    ID3D11RenderTargetView* bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ui.context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, nullptr);
    bool still_bound = false;
    for (auto*& target : bound) {
        if (target == view) still_bound = true;
        if (target) target->Release();
    }
    EdpeLog(still_bound ? L"EDPE: scene color RTV still bound at Present"
                        : L"EDPE: scene color RTV unbound at Present");
    D3D11_RENDER_TARGET_VIEW_DESC target{};
    view->GetDesc(&target);
    if (target.Format != DXGI_FORMAT_R11G11B10_FLOAT) {
        EdpeLog(L"EDPE: scene color snapshot skipped (last RTV0 is not R11G11B10_FLOAT)");
        return;
    }
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    ID3D11Texture2D* source = nullptr;
    if (resource) {
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
        resource->Release();
    }
    if (!source) {
        EdpeLog(L"EDPE: scene color snapshot skipped (not a Texture2D)");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (!desc.Width || !desc.Height || desc.MipLevels != 1 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.Format != target.Format) {
        source->Release();
        EdpeLog(L"EDPE: scene color snapshot skipped (unsupported texture layout)");
        return;
    }
    D3D11_TEXTURE2D_DESC copy_desc = desc;
    copy_desc.Usage = D3D11_USAGE_DEFAULT;
    copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy_desc.CPUAccessFlags = 0;
    copy_desc.MiscFlags = 0;
    ID3D11Texture2D* copy = nullptr;
    HRESULT result = ui.device->CreateTexture2D(&copy_desc, nullptr, &copy);
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(result)) result = ui.device->CreateShaderResourceView(copy, nullptr, &srv);
    if (SUCCEEDED(result)) {
        ui.context->CopyResource(copy, source);
        releaseColorSnapshot();
        ui.color_copy = copy;
        ui.color_srv = srv;
        ui.color_width = desc.Width;
        ui.color_height = desc.Height;
        ui.color_snapshot_index = static_cast<int>(index);
        ui.color_window_open = true;
        wchar_t message[160];
        swprintf_s(message, L"EDPE: scene color snapshot #%u copied %ux%u format=%u",
            index, desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
        EdpeLog(message);
    } else {
        if (srv) srv->Release();
        if (copy) copy->Release();
        wchar_t message[128];
        swprintf_s(message, L"EDPE: scene color snapshot unavailable HRESULT=0x%08X",
            static_cast<unsigned>(result));
        EdpeLog(message);
    }
    source->Release();
}

void shutdownUi() {
    menu_visible.store(false);
    if (ui.window && ui.original_wndproc &&
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(ui.window, GWLP_WNDPROC)) == edpeWndProc) {
        SetWindowLongPtrW(ui.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ui.original_wndproc));
    }
    if (ui.imgui) {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(ui.imgui);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(ui.imgui);
        ImGui::SetCurrentContext(previous == ui.imgui ? nullptr : previous);
    }
    releaseBackbuffer();
    releaseDepthSnapshot();
    releaseColorSnapshot();
    releaseEarlyColorSnapshot();
    releaseMotionSnapshot();
    if (ui.frame_staging) ui.frame_staging->Release();
    if (ui.motion_preview_shader) ui.motion_preview_shader->Release();
    if (ui.depth_contrast_shader) ui.depth_contrast_shader->Release();
    if (ui.context) ui.context->Release();
    if (ui.device) ui.device->Release();
    ui = {};
    std::lock_guard lock(input_mutex);
    pending_input.clear();
    render_input.clear();
}

bool initializeUi(IDXGISwapChain* swap_chain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap_chain->GetDesc(&desc)) || !desc.OutputWindow ||
        FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&ui.device)))) return false;
    ui.device->GetImmediateContext(&ui.context);
    if (!ui.context) return false;
    ui.window = desc.OutputWindow;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ui.imgui = ImGui::CreateContext();
    if (!ui.imgui) return false;
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().MouseDrawCursor = true;
    const bool win32_ready = ImGui_ImplWin32_Init(ui.window);
    const bool dx11_ready = win32_ready && ImGui_ImplDX11_Init(ui.device, ui.context);
    ImGui::SetCurrentContext(previous);
    if (!dx11_ready) {
        if (win32_ready) {
            ImGui::SetCurrentContext(ui.imgui);
            ImGui_ImplWin32_Shutdown();
            ImGui::SetCurrentContext(previous);
        }
        ImGui::DestroyContext(ui.imgui);
        ui.imgui = nullptr;
        return false;
    }
    SetLastError(0);
    ui.original_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(ui.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edpeWndProc)));
    if (!ui.original_wndproc) return false;
    ui.swap_chain = swap_chain;
    ID3D11Device1* device1 = nullptr;
    ID3D11DeviceContext1* context1 = nullptr;
    ID3DDeviceContextState* motion_state = nullptr;
    HRESULT state_result = ui.device->QueryInterface(__uuidof(ID3D11Device1),
        reinterpret_cast<void**>(&device1));
    if (SUCCEEDED(state_result)) state_result = ui.context->QueryInterface(
        __uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1));
    if (SUCCEEDED(state_result)) {
        const auto level = ui.device->GetFeatureLevel();
        const UINT state_flags = ui.device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED
            ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        state_result = device1->CreateDeviceContextState(state_flags, &level, 1,
            D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &motion_state);
    }
    wchar_t state_message[120];
    swprintf_s(state_message, L"EDPE: D3D11 context state available=%u HRESULT=0x%08X",
        SUCCEEDED(state_result) && motion_state, static_cast<unsigned>(state_result));
    EdpeLog(state_message);
    if (motion_state) motion_state->Release();
    if (context1) context1->Release();
    if (device1) device1->Release();
    EdpeLog(L"EDPE: Dear ImGui ready; F5 toggles menu");
    return true;
}

bool createBackbufferView(IDXGISwapChain* swap_chain) {
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&backbuffer)))) return false;
    const HRESULT result = ui.device->CreateRenderTargetView(backbuffer, nullptr, &ui.backbuffer_rtv);
    backbuffer->Release();
    return SUCCEEDED(result);
}
} // namespace

void UiOnPresent(IDXGISwapChain* swap_chain, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return;
    if (ui.swap_chain && ui.swap_chain != swap_chain) return;
    if (ui.failed) return;
    if (!ui.imgui && !initializeUi(swap_chain)) {
        shutdownUi();
        ui.failed = true;
        EdpeLog(L"EDPE: Dear ImGui unavailable; presenting original frame");
        return;
    }
    pollDepthSamples();
    pollFrameCapture();
    if (auto* motion = ContextCensusTakeMotionSnapshot(&ui.motion_width, &ui.motion_height)) {
        releaseMotionSnapshot();
        ui.motion_srv = motion;
        ui.motion_window_open = true;
        ui.snapshot_ready = true;
        ensureMotionPreviewShader();
        EdpeLog(L"EDPE: motion snapshot handed to ImGui");
    }
    UINT motion_color_width = 0, motion_color_height = 0;
    if (auto* color = ContextCensusTakeMotionColorSnapshot(
            &motion_color_width, &motion_color_height)) {
        releaseColorSnapshot();
        ui.color_srv = color;
        ui.color_width = motion_color_width;
        ui.color_height = motion_color_height;
        ui.color_window_open = true;
        EdpeLog(L"EDPE: same-frame HDR color handed to ImGui");
    }
    if (auto* early = ContextCensusTakeMotionEarlyColorSnapshot(
            &ui.early_color_width, &ui.early_color_height)) {
        releaseEarlyColorSnapshot();
        ui.early_color_srv = early;
        ui.early_color_window_open = true;
        EdpeLog(L"EDPE: early HDR color handed to ImGui");
    }
    if (!menu_visible.load()) {
        unsigned ignored = 0;
        ID3D11RenderTargetView* color = nullptr;
        if (auto* view = ContextCensusTakeDepthSnapshot(&ignored, &color)) view->Release();
        if (color) color->Release();
        std::lock_guard lock(input_mutex);
        pending_input.clear();
        return;
    }
    if (!ui.backbuffer_rtv && !createBackbufferView(swap_chain)) {
        EdpeLog(L"EDPE: overlay backbuffer view unavailable; presenting original frame");
        return;
    }
    unsigned captured_index = 0;
    ID3D11RenderTargetView* color = nullptr;
    if (auto* view = ContextCensusTakeDepthSnapshot(&captured_index, &color)) {
        captureDepthSnapshot(view, captured_index);
        view->Release();
    }
    if (color) {
        captureColorSnapshot(color, captured_index);
        color->Release();
    }

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ui.imgui);
    {
        std::lock_guard lock(input_mutex);
        pending_input.swap(render_input);
    }
    if (!render_input.empty() && !ui.input_logged) {
        EdpeLog(L"EDPE: queued input routed to Dear ImGui");
        ui.input_logged = true;
    }
    for (const auto& input : render_input) {
        ImGui_ImplWin32_WndProcHandler(input.window, input.message, input.wparam, input.lparam);
    }
    render_input.clear();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    bool window_open = true;
    ImGui::Begin("EDPE \xE2\x80\x94 Elite Dangerous Performance Enhanced", &window_open);
    ImGui::TextUnformatted("Version: unreleased");
    ImGui::TextUnformatted("Rendering: original game output");
    ImGui::TextUnformatted("Upscaler: Native");
    ImGui::BeginDisabled();
    ImGui::TextUnformatted("DLAA / DLSS / FSR: temporal inputs not yet verified");
    ImGui::TextUnformatted("Frame Generation: unavailable (temporal path pending)");
    ImGui::EndDisabled();
    const bool sequence_available = ContextCensusBindSequenceAvailable();
    if (!sequence_available) ImGui::BeginDisabled();
    if (ImGui::Button("Capture DSV bind order (one frame)")) ContextCensusRequestBindSequence();
    if (!sequence_available) ImGui::EndDisabled();
    if (!sequence_available) ImGui::TextDisabled("Waiting for DSV observer or capture completion");
    ImGui::InputInt("Depth candidate index", &ui.depth_candidate);
    const bool depth_available = ui.depth_candidate >= 0 &&
        ContextCensusDepthSnapshotAvailable(static_cast<unsigned>(ui.depth_candidate));
    if (!depth_available) ImGui::BeginDisabled();
    if (ImGui::Button("Capture depth candidate (one frame)"))
        ContextCensusRequestDepthSnapshot(static_cast<unsigned>(ui.depth_candidate));
    if (!depth_available) ImGui::EndDisabled();
    if (!depth_available) ImGui::TextDisabled("Choose a bound R32G8X24/R24G8 single-sample DSV, or wait");
    const int scene_candidate = ContextCensusSceneDepthCandidate();
    if (scene_candidate < 0) ImGui::BeginDisabled();
    if (ImGui::Button("Capture scene candidate (experimental)")) {
        ui.depth_candidate = scene_candidate;
        ContextCensusRequestDepthSnapshot(static_cast<unsigned>(scene_candidate));
    }
    if (ImGui::Button("Capture adjacent camera frames (experimental)"))
        ContextCensusRequestCameraPair(static_cast<unsigned>(scene_candidate));
    if (ImGui::Button("Capture motion candidate (experimental)") &&
        ContextCensusRequestMotionPair(static_cast<unsigned>(scene_candidate))) {
        releaseMotionSnapshot();
        releaseColorSnapshot();
        releaseEarlyColorSnapshot();
    }
    if (scene_candidate < 0) ImGui::EndDisabled();
    if (scene_candidate < 0) ImGui::TextDisabled("No unique recent scene MRT/HDR signature; use manual index");
    if (ui.depth_srv) {
        ImGui::Text("Depth snapshot: DSV #%d (%ux%u)",
            ui.depth_snapshot_index, ui.depth_width, ui.depth_height);
        if (!ui.depth_window_open && ImGui::Button("Show depth snapshot")) ui.depth_window_open = true;
    }
    if (ui.color_srv) {
        ImGui::Text("Scene color candidate: DSV #%d (%ux%u)",
            ui.color_snapshot_index, ui.color_width, ui.color_height);
        if (!ui.color_window_open && ImGui::Button("Show scene color snapshot")) ui.color_window_open = true;
    }
    if (ui.early_color_srv) {
        ImGui::Text("Early HDR candidate: %ux%u", ui.early_color_width,
            ui.early_color_height);
        if (!ui.early_color_window_open && ImGui::Button("Show early HDR snapshot"))
            ui.early_color_window_open = true;
    }
    if (ui.motion_srv) {
        ImGui::Text("Motion candidate: %ux%u, current to previous pixels",
            ui.motion_width, ui.motion_height);
        if (!ui.motion_window_open && ImGui::Button("Show motion snapshot"))
            ui.motion_window_open = true;
    }
    ImGui::TextUnformatted("F5: hide menu");
    ImGui::End();
    if (ui.depth_srv && ui.depth_window_open) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x > 700.0f ? (display.x - 700.0f) * 0.5f : 0.0f,
            display.y > 500.0f ? 80.0f : 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(700.0f, 440.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("EDPE Depth Snapshot", &ui.depth_window_open)) {
            ImGui::Text("DSV #%d (%ux%u)",
                ui.depth_snapshot_index, ui.depth_width, ui.depth_height);
            if (ui.depth_range_valid) ImGui::Text("8x8 sample range: %.6g .. %.6g",
                ui.depth_min, ui.depth_max);
            if (ui.depth_center_valid) ImGui::Text("Center raw depth: %.9g", ui.depth_center);
            if (ui.depth_contrast_shader) ImGui::Checkbox("Contrast preview", &ui.depth_contrast);
            else ImGui::TextDisabled("Contrast preview unavailable; showing raw red depth");
            float width = ImGui::GetContentRegionAvail().x;
            if (width > 640.0f) width = 640.0f;
            if (width < 1.0f) width = 1.0f;
            float height = width * static_cast<float>(ui.depth_height) / ui.depth_width;
            if (height > 360.0f) { width *= 360.0f / height; height = 360.0f; }
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const bool contrast = ui.depth_contrast && ui.depth_contrast_shader;
            if (contrast) draw_list->AddCallback(bindDepthContrastShader);
            ImGui::Image(reinterpret_cast<ImTextureID>(ui.depth_srv), ImVec2(width, height));
            if (contrast) draw_list->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState);
            if (!ui.depth_image_logged) {
                EdpeLog(L"EDPE: depth snapshot image submitted to ImGui");
                ui.depth_image_logged = true;
            }
        }
        ImGui::End();
    }
    if (ui.color_srv && ui.color_window_open) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x > 1400.0f ? display.x - 700.0f : 0.0f,
            display.y > 500.0f ? 80.0f : 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(700.0f, 440.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("EDPE Scene Color Snapshot", &ui.color_window_open)) {
            if (ui.color_snapshot_index >= 0)
                ImGui::Text("DSV #%d / HDR RTV0 (%ux%u)",
                    ui.color_snapshot_index, ui.color_width, ui.color_height);
            else
                ImGui::Text("Motion frame / HDR RTV3 (%ux%u)",
                    ui.color_width, ui.color_height);
            if (ui.color_snapshot_index < 0)
                ImGui::TextUnformatted("Present-time HDR color; may include HUD");
            float width = ImGui::GetContentRegionAvail().x;
            if (width > 640.0f) width = 640.0f;
            if (width < 1.0f) width = 1.0f;
            float height = width * static_cast<float>(ui.color_height) / ui.color_width;
            if (height > 360.0f) { width *= 360.0f / height; height = 360.0f; }
            ImGui::Image(reinterpret_cast<ImTextureID>(ui.color_srv), ImVec2(width, height));
            if (!ui.color_image_logged) {
                EdpeLog(L"EDPE: scene color snapshot image submitted to ImGui");
                ui.color_image_logged = true;
            }
        }
        ImGui::End();
    }
    if (ui.early_color_srv && ui.early_color_window_open) {
        ImGui::SetNextWindowSize(ImVec2(700.0f, 440.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("EDPE Early HDR Snapshot", &ui.early_color_window_open)) {
            ImGui::TextUnformatted("Early HDR candidate; capture point in edpe.log");
            float width = ImGui::GetContentRegionAvail().x;
            if (width > 640.0f) width = 640.0f;
            if (width < 1.0f) width = 1.0f;
            float height = width * static_cast<float>(ui.early_color_height) /
                ui.early_color_width;
            if (height > 360.0f) { width *= 360.0f / height; height = 360.0f; }
            ImGui::Image(reinterpret_cast<ImTextureID>(ui.early_color_srv),
                ImVec2(width, height));
        }
        ImGui::End();
    }
    if (ui.motion_srv && ui.motion_window_open) {
        ImGui::SetNextWindowSize(ImVec2(700.0f, 440.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("EDPE Motion Snapshot", &ui.motion_window_open)) {
            ImGui::TextUnformatted("Camera + depth motion; HUD is not represented");
            ImGui::TextUnformatted("Neutral gray = 0 pixels; red = horizontal, green = vertical");
            ImGui::TextUnformatted("Nonlinear preview: 10 pixels shifts a channel by 0.25");
            float width = ImGui::GetContentRegionAvail().x;
            if (width > 640.0f) width = 640.0f;
            if (width < 1.0f) width = 1.0f;
            float height = width * static_cast<float>(ui.motion_height) / ui.motion_width;
            if (height > 360.0f) { width *= 360.0f / height; height = 360.0f; }
            if (ui.motion_preview_shader) {
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                draw_list->AddCallback(bindMotionPreviewShader);
                ImGui::Image(reinterpret_cast<ImTextureID>(ui.motion_srv), ImVec2(width, height));
                draw_list->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState);
            } else {
                ImGui::TextDisabled("Motion preview unavailable (shader creation failed)");
            }
        }
        ImGui::End();
    }
    if (!window_open) menu_visible.store(false);
    ImGui::Render();

    ID3D11RenderTargetView* previous_rtv = nullptr;
    ID3D11DepthStencilView* previous_dsv = nullptr;
    ui.context->OMGetRenderTargets(1, &previous_rtv, &previous_dsv);
    ui.context->OMSetRenderTargets(1, &ui.backbuffer_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ui.context->OMSetRenderTargets(1, &previous_rtv, previous_dsv);
    if (previous_rtv) previous_rtv->Release();
    if (previous_dsv) previous_dsv->Release();
    if (ui.snapshot_ready && !ui.frame_staging) {
        queueFrameCapture(swap_chain);
        ui.snapshot_ready = false;
    }
    ImGui::SetCurrentContext(previous);
}

void UiOnResize(IDXGISwapChain* swap_chain) {
    if (ui.swap_chain == swap_chain) {
        EdpeLog(L"EDPE: releasing overlay backbuffer for resize");
        releaseBackbuffer();
    }
}

void UiOnRelease(IUnknown* object) {
    if (ui.swap_chain == object) shutdownUi();
}

bool UiMenuVisible() { return menu_visible.load(); }
