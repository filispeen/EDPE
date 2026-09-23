#include "ui.h"
#include "log.h"

#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <windows.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
struct UiState {
    IDXGISwapChain* swap_chain = nullptr; // Weak: the game's Release owns its lifetime.
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11RenderTargetView* backbuffer_rtv = nullptr;
    ImGuiContext* imgui = nullptr;
    HWND window = nullptr;
    WNDPROC original_wndproc = nullptr;
    bool visible = false;
    bool failed = false;
} ui;

bool isInputMessage(UINT message) {
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) return true;
    if (message >= WM_KEYFIRST && message <= WM_KEYLAST) return true;
    return message == WM_INPUT;
}

LRESULT CALLBACK edpeWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        wparam == VK_INSERT && !(lparam & (1LL << 30))) {
        ui.visible = !ui.visible;
        return 0;
    }
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wparam == VK_INSERT) return 0;
    if (ui.visible && ui.imgui) {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(ui.imgui);
        const LRESULT handled = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        ImGui::SetCurrentContext(previous);
        if (handled) return handled;
        if (message == WM_INPUT) return DefWindowProcW(window, message, wparam, lparam);
        if (isInputMessage(message)) return 1;
    }
    return ui.original_wndproc ? CallWindowProcW(ui.original_wndproc, window, message, wparam, lparam)
                               : DefWindowProcW(window, message, wparam, lparam);
}

void releaseBackbuffer() {
    if (ui.backbuffer_rtv) ui.backbuffer_rtv->Release();
    ui.backbuffer_rtv = nullptr;
}

void shutdownUi() {
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
    if (ui.context) ui.context->Release();
    if (ui.device) ui.device->Release();
    ui = {};
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
    EdpeLog(L"EDPE: Dear ImGui ready; Insert toggles menu");
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
    if (!ui.visible) return;
    if (!ui.backbuffer_rtv && !createBackbufferView(swap_chain)) {
        EdpeLog(L"EDPE: overlay backbuffer view unavailable; presenting original frame");
        return;
    }

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ui.imgui);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("EDPE \xE2\x80\x94 Elite Dangerous Performance Enhanced", &ui.visible);
    ImGui::TextUnformatted("Version: unreleased");
    ImGui::TextUnformatted("Rendering: original game output");
    ImGui::TextUnformatted("Upscaler: Native");
    ImGui::BeginDisabled();
    ImGui::TextUnformatted("DLAA / DLSS / FSR: temporal inputs not yet verified");
    ImGui::TextUnformatted("Frame Generation: unavailable (temporal path pending)");
    ImGui::EndDisabled();
    ImGui::TextUnformatted("Insert: hide menu");
    ImGui::End();
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

bool UiMenuVisible() { return ui.visible; }
