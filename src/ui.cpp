#include "ui.h"
#include "log.h"
#include "context_census.h"

#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <windows.h>
#include <atomic>
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
    UINT depth_width = 0;
    UINT depth_height = 0;
    int depth_candidate = 1;
    int depth_snapshot_index = -1;
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
    if (ui.depth_srv) ui.depth_srv->Release();
    if (ui.depth_copy) ui.depth_copy->Release();
    ui.depth_srv = nullptr;
    ui.depth_copy = nullptr;
    ui.depth_snapshot_index = -1;
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
    if (!menu_visible.load()) {
        unsigned ignored = 0;
        if (auto* view = ContextCensusTakeDepthSnapshot(&ignored)) view->Release();
        std::lock_guard lock(input_mutex);
        pending_input.clear();
        return;
    }
    if (!ui.backbuffer_rtv && !createBackbufferView(swap_chain)) {
        EdpeLog(L"EDPE: overlay backbuffer view unavailable; presenting original frame");
        return;
    }
    unsigned captured_index = 0;
    if (auto* view = ContextCensusTakeDepthSnapshot(&captured_index)) {
        captureDepthSnapshot(view, captured_index);
        view->Release();
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
    if (ui.depth_srv) {
        ImGui::Text("DSV #%d: raw depth in red channel (%ux%u)",
            ui.depth_snapshot_index, ui.depth_width, ui.depth_height);
        float width = ImGui::GetContentRegionAvail().x;
        if (width > 640.0f) width = 640.0f;
        if (width < 1.0f) width = 1.0f;
        float height = width * static_cast<float>(ui.depth_height) / ui.depth_width;
        if (height > 360.0f) { width *= 360.0f / height; height = 360.0f; }
        ImGui::Image(reinterpret_cast<ImTextureID>(ui.depth_srv), ImVec2(width, height));
    }
    ImGui::TextUnformatted("F5: hide menu");
    ImGui::End();
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
