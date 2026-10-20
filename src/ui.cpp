#include "ui.h"
#include "log.h"
#include "context_census.h"
#include "upscaler_backend.h"
#include "ngx_context.h"
#include "capture_format.h"
#include "config.h"
#include "perf_overlay.h"
#include "input_hook.h"
#include "jitter_logger.h"


#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <windows.h>
#include <algorithm>
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
    enum class CaptureKind {
        Depth,
        Motion,
        SceneColor,
        EarlyHdr,
        Backbuffer,
        ComparisonBefore,
        ComparisonAfter
    };

    struct TextureCaptureRequest {
        ID3D11Texture2D* staging = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT width = 0;
        UINT height = 0;
        unsigned attempts = 0;
        CaptureKind kind = CaptureKind::Depth;
        int index = -1;
        SYSTEMTIME timestamp{};
    };

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
    std::vector<TextureCaptureRequest> pending_captures;
    unsigned capture_serial = 0;
    bool pending_backbuffer_capture = false;
    bool pending_backbuffer_capture_on_motion = false;
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
    wchar_t last_comparison_saved[128]{};
} ui;
std::atomic<bool> menu_visible{false};
std::atomic<bool> all_captures_requested{false};
std::mutex input_mutex;
std::vector<InputMessage> pending_input;
std::vector<InputMessage> render_input;

bool isInputMessage(UINT message) {
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) return true;
    if (message >= WM_KEYFIRST && message <= WM_KEYLAST) return true;
    return message == WM_INPUT;
}
} // namespace

bool UiProcessInput(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        wparam == VK_F5 && !(lparam & (1LL << 30))) {
        const bool new_visible = !menu_visible.load();
        menu_visible.store(new_visible);
        if (new_visible) {
            ClipCursor(nullptr);
            while (ShowCursor(TRUE) < 0) {}
        } else {
            while (ShowCursor(FALSE) >= 0) {}
        }
        return true;
    }
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wparam == VK_F5) return true;
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        wparam == VK_F7 && !(lparam & (1LL << 30))) {
        auto& cfg = edpe::Config::instance();
        cfg.showPerfOverlay = !cfg.showPerfOverlay;
        cfg.save();
        EdpeLog(cfg.showPerfOverlay ? L"EDPE: F7 pressed — Performance HUD enabled" : L"EDPE: F7 pressed — Performance HUD disabled");
        return true;
    }
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wparam == VK_F7) return true;
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        wparam == VK_F8 && !(lparam & (1LL << 30))) {
        ContextCensusToggleJitterDebugView();
        const bool active = ContextCensusGetJitterDebugView();
        auto& cfg = edpe::Config::instance();
        cfg.jitterDebugView = active;
        cfg.save();
        EdpeLog(active ? L"EDPE: F8 pressed — Jitter Debug View enabled" : L"EDPE: F8 pressed — Jitter Debug View disabled");
        return true;
    }
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wparam == VK_F8) return true;
    if (menu_visible.load()) {
        if (message == WM_INPUT) return true;
        if (message == WM_SETCURSOR) {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return true;
        }
        if (message == WM_MOUSEACTIVATE) {
            return true;
        }
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
            return true;
        }
    }
    return false;
}

namespace {
LRESULT CALLBACK edpeWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (UiProcessInput(window, message, wparam, lparam)) {
        if (message == WM_SETCURSOR) return TRUE;
        if (message == WM_MOUSEACTIVATE) return MA_ACTIVATE;
        return 0;
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

void queueTextureCapture(ID3D11Texture2D* source, UiState::CaptureKind kind, int index = -1,
    const SYSTEMTIME* customTime = nullptr) {
    if (!source || !ui.device || !ui.context) return;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (!desc.Width || !desc.Height || desc.SampleDesc.Count != 1 ||
        static_cast<unsigned long long>(desc.Width) * desc.Height > 7680ull * 4320) {
        EdpeLog(L"EDPE: texture capture skipped (unsupported layout or dimensions)");
        return;
    }
    if (ui.pending_captures.size() >= 8) {
        EdpeLog(L"EDPE: texture capture skipped (too many pending captures)");
        return;
    }
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(ui.device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
        ui.context->CopyResource(staging, source);
        SYSTEMTIME t{};
        if (customTime) {
            t = *customTime;
        } else {
            GetLocalTime(&t);
        }
        ui.pending_captures.push_back({staging, desc.Format, desc.Width, desc.Height, 0, kind, index, t});
        wchar_t msg[128];
        swprintf_s(msg, L"EDPE: queued %ls capture %ux%u format=%u",
            kind == UiState::CaptureKind::Depth ? L"depth" :
            kind == UiState::CaptureKind::Motion ? L"motion" :
            kind == UiState::CaptureKind::SceneColor ? L"scene color" :
            kind == UiState::CaptureKind::EarlyHdr ? L"early HDR" :
            kind == UiState::CaptureKind::ComparisonBefore ? L"comparison before" :
            kind == UiState::CaptureKind::ComparisonAfter ? L"comparison after" : L"backbuffer",
            desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
        EdpeLog(msg);
    } else {
        EdpeLog(L"EDPE: texture capture staging creation failed");
    }
}

void queueTextureCaptureFromSrv(ID3D11ShaderResourceView* srv, UiState::CaptureKind kind, int index = -1) {
    if (!srv) return;
    ID3D11Resource* resource = nullptr;
    srv->GetResource(&resource);
    if (resource) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
            queueTextureCapture(tex, kind, index);
            tex->Release();
        }
        resource->Release();
    }
}

void queueComparisonCaptureInternal(ID3D11DeviceContext* context, ID3D11Texture2D* beforeTex, ID3D11Texture2D* afterTex) {
    if (!beforeTex || !afterTex || !context) return;
    if (!ui.device) {
        context->GetDevice(&ui.device);
    }
    if (!ui.context) {
        ui.context = context;
        ui.context->AddRef();
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    queueTextureCapture(beforeTex, UiState::CaptureKind::ComparisonBefore, -1, &now);
    queueTextureCapture(afterTex, UiState::CaptureKind::ComparisonAfter, -1, &now);
}

void pollFrameCapture() {
    if (ui.pending_captures.empty()) return;

    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    std::filesystem::path folder;
    if (length && length < MAX_PATH) {
        std::error_code error;
        folder = std::filesystem::path(executable).parent_path() / L"edpe-captures";
        std::filesystem::create_directories(folder, error);
        if (error) {
            EdpeLog(L"EDPE: capture directory unavailable");
            folder.clear();
        }
    }

    for (auto it = ui.pending_captures.begin(); it != ui.pending_captures.end();) {
        auto& req = *it;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = ui.context->Map(req.staging, 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);

        if (result == DXGI_ERROR_WAS_STILL_DRAWING && ++req.attempts < 120) {
            ++it;
            continue;
        }

        if (SUCCEEDED(result)) {
            if (!folder.empty()) {
                wchar_t name[128];
                switch (req.kind) {
                case UiState::CaptureKind::Depth:
                    if (req.index >= 0)
                        swprintf_s(name, L"depth-dsv%d-%04u%02u%02u-%02u%02u%02u-%03u-%u.bmp",
                            req.index, req.timestamp.wYear, req.timestamp.wMonth, req.timestamp.wDay, req.timestamp.wHour, req.timestamp.wMinute,
                            req.timestamp.wSecond, req.timestamp.wMilliseconds, ++ui.capture_serial);
                    else
                        swprintf_s(name, L"depth-%04u%02u%02u-%02u%02u%02u-%03u-%u.bmp",
                            req.timestamp.wYear, req.timestamp.wMonth, req.timestamp.wDay, req.timestamp.wHour, req.timestamp.wMinute,
                            req.timestamp.wSecond, req.timestamp.wMilliseconds, ++ui.capture_serial);
                    break;
                case UiState::CaptureKind::Motion:
                    swprintf_s(name, L"motion-%04u%02u%02u-%02u%02u%02u-%03u-%u.bmp",
                        req.timestamp.wYear, req.timestamp.wMonth, req.timestamp.wDay, req.timestamp.wHour, req.timestamp.wMinute,
                        req.timestamp.wSecond, req.timestamp.wMilliseconds, ++ui.capture_serial);
                    break;
                case UiState::CaptureKind::SceneColor:
                    if (req.index >= 0)
                        swprintf_s(name, L"color-dsv%d-%04u%02u%02u-%02u%02u%02u-%03u-%u.bmp",
                            req.index, req.timestamp.wYear, req.timestamp.wMonth, req.timestamp.wDay, req.timestamp.wHour, req.timestamp.wMinute,
                            req.timestamp.wSecond, req.timestamp.wMilliseconds, ++ui.capture_serial);
                    else
                        swprintf_s(name, L"color-%04u%02u%02u-%02u%02u%02u-%03u-%u.bmp",
                            req.timestamp.wYear, req.timestamp.wMonth, req.timestamp.wDay, req.timestamp.wHour, req.timestamp.wMinute,
                            req.timestamp.wSecond, req.timestamp.wMilliseconds, ++ui.capture_serial);
                    break;
                case UiState::CaptureKind::EarlyHdr:
                    swprintf_s(name, L"early-color-%04u%02u%02u-%02u%02u%02u-%03u-%u.bmp",
                        req.timestamp.wYear, req.timestamp.wMonth, req.timestamp.wDay, req.timestamp.wHour, req.timestamp.wMinute,
                        req.timestamp.wSecond, req.timestamp.wMilliseconds, ++ui.capture_serial);
                    break;
                case UiState::CaptureKind::ComparisonBefore:
                    swprintf_s(name, L"capture-%04u%02u%02u-%02u%02u%02u-before.bmp",
                        req.timestamp.wYear, req.timestamp.wMonth, req.timestamp.wDay,
                        req.timestamp.wHour, req.timestamp.wMinute, req.timestamp.wSecond);
                    break;
                case UiState::CaptureKind::ComparisonAfter:
                    swprintf_s(name, L"capture-%04u%02u%02u-%02u%02u%02u-after.bmp",
                        req.timestamp.wYear, req.timestamp.wMonth, req.timestamp.wDay,
                        req.timestamp.wHour, req.timestamp.wMinute, req.timestamp.wSecond);
                    break;
                case UiState::CaptureKind::Backbuffer:
                default:
                    swprintf_s(name, L"frame-%04u%02u%02u-%02u%02u%02u-%03u-%u.bmp",
                        req.timestamp.wYear, req.timestamp.wMonth, req.timestamp.wDay, req.timestamp.wHour, req.timestamp.wMinute,
                        req.timestamp.wSecond, req.timestamp.wMilliseconds, ++ui.capture_serial);
                    break;
                }

                const auto path = folder / name;
                std::ofstream file(path, std::ios::binary);
                const DWORD image_bytes = req.width * req.height * 4;
                BITMAPFILEHEADER header{};
                header.bfType = 0x4D42;
                header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
                header.bfSize = header.bfOffBits + image_bytes;
                BITMAPINFOHEADER info{};
                info.biSize = sizeof(info);
                info.biWidth = static_cast<LONG>(req.width);
                info.biHeight = -static_cast<LONG>(req.height); // Top-down
                info.biPlanes = 1;
                info.biBitCount = 32;
                info.biCompression = BI_RGB;
                info.biSizeImage = image_bytes;
                file.write(reinterpret_cast<const char*>(&header), sizeof(header));
                file.write(reinterpret_cast<const char*>(&info), sizeof(info));

                std::vector<unsigned char> row(req.width * 4);
                for (UINT y = 0; y < req.height && file; ++y) {
                    const auto* source = static_cast<const unsigned char*>(mapped.pData) +
                        static_cast<size_t>(y) * mapped.RowPitch;

                    if (req.kind == UiState::CaptureKind::Depth) {
                        for (UINT x = 0; x < req.width; ++x) {
                            float depth = 0.0f;
                            if (req.format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                                req.format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) {
                                std::memcpy(&depth, source + 8 * x, sizeof(depth));
                            } else if (req.format == DXGI_FORMAT_R24G8_TYPELESS ||
                                       req.format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
                                       req.format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
                                uint32_t packed = 0;
                                std::memcpy(&packed, source + 4 * x, sizeof(packed));
                                depth = static_cast<float>(packed & 0x00FFFFFFu) / 16777215.0f;
                            } else if (req.format == DXGI_FORMAT_R32_TYPELESS ||
                                       req.format == DXGI_FORMAT_R32_FLOAT ||
                                       req.format == DXGI_FORMAT_D32_FLOAT) {
                                std::memcpy(&depth, source + 4 * x, sizeof(depth));
                            } else if (req.format == DXGI_FORMAT_R16_TYPELESS ||
                                       req.format == DXGI_FORMAT_R16_UNORM ||
                                       req.format == DXGI_FORMAT_D16_UNORM) {
                                uint16_t packed = 0;
                                std::memcpy(&packed, source + 2 * x, sizeof(packed));
                                depth = static_cast<float>(packed) / 65535.0f;
                            }
                            const float contrast = edpe::depthContrast(depth);
                            const auto gray = static_cast<unsigned char>(contrast * 255.0f + 0.5f);
                            row[4 * x + 0] = gray;
                            row[4 * x + 1] = gray;
                            row[4 * x + 2] = gray;
                            row[4 * x + 3] = 255;
                        }
                    } else if (req.kind == UiState::CaptureKind::Motion) {
                        for (UINT x = 0; x < req.width; ++x) {
                            float mx = 0.0f, my = 0.0f;
                            if (req.format == DXGI_FORMAT_R16G16_FLOAT) {
                                const auto* px = reinterpret_cast<const uint16_t*>(source + 4 * x);
                                mx = edpe::halfToFloat(px[0]);
                                my = edpe::halfToFloat(px[1]);
                            } else if (req.format == DXGI_FORMAT_R32G32_FLOAT) {
                                const auto* px = reinterpret_cast<const float*>(source + 8 * x);
                                mx = px[0];
                                my = px[1];
                            }
                            uint8_t mb = 128, mg = 128, mr = 128;
                            edpe::motionToBgr(mx, my, mb, mg, mr);
                            row[4 * x + 0] = mb;
                            row[4 * x + 1] = mg;
                            row[4 * x + 2] = mr;
                            row[4 * x + 3] = 255;
                        }
                    } else if (req.kind == UiState::CaptureKind::SceneColor ||
                               req.kind == UiState::CaptureKind::EarlyHdr) {
                        for (UINT x = 0; x < req.width; ++x) {
                            if (req.format == DXGI_FORMAT_R11G11B10_FLOAT) {
                                uint32_t val = 0;
                                std::memcpy(&val, source + 4 * x, sizeof(val));
                                const float cr = edpe::decodeFloat11(val & 0x7FF);
                                const float cg = edpe::decodeFloat11((val >> 11) & 0x7FF);
                                const float cb = edpe::decodeFloat10((val >> 22) & 0x3FF);
                                row[4 * x + 0] = edpe::toneMapSrgb(cb);
                                row[4 * x + 1] = edpe::toneMapSrgb(cg);
                                row[4 * x + 2] = edpe::toneMapSrgb(cr);
                                row[4 * x + 3] = 255;
                            } else if (req.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                                uint32_t val = 0;
                                std::memcpy(&val, source + 4 * x, sizeof(val));
                                row[4 * x + 0] = static_cast<unsigned char>(((val >> 20) & 0x3FF) >> 2);
                                row[4 * x + 1] = static_cast<unsigned char>(((val >> 10) & 0x3FF) >> 2);
                                row[4 * x + 2] = static_cast<unsigned char>((val & 0x3FF) >> 2);
                                row[4 * x + 3] = 255;
                            } else if (req.format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                                const auto* px = source + 4 * x;
                                row[4 * x + 0] = px[0];
                                row[4 * x + 1] = px[1];
                                row[4 * x + 2] = px[2];
                                row[4 * x + 3] = 255;
                            } else {
                                const auto* px = source + 4 * x;
                                row[4 * x + 0] = px[2];
                                row[4 * x + 1] = px[1];
                                row[4 * x + 2] = px[0];
                                row[4 * x + 3] = 255;
                            }
                        }
                    } else { // Backbuffer, ComparisonBefore, ComparisonAfter
                        for (UINT x = 0; x < req.width; ++x) {
                            if (req.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                                uint32_t val = 0;
                                std::memcpy(&val, source + 4 * x, sizeof(val));
                                row[4 * x + 0] = static_cast<unsigned char>(((val >> 20) & 0x3FF) >> 2);
                                row[4 * x + 1] = static_cast<unsigned char>(((val >> 10) & 0x3FF) >> 2);
                                row[4 * x + 2] = static_cast<unsigned char>((val & 0x3FF) >> 2);
                                row[4 * x + 3] = 255;
                            } else {
                                const auto* pixel = source + 4 * x;
                                row[4 * x + 0] = req.format == DXGI_FORMAT_R8G8B8A8_UNORM ? pixel[2] : pixel[0];
                                row[4 * x + 1] = pixel[1];
                                row[4 * x + 2] = req.format == DXGI_FORMAT_R8G8B8A8_UNORM ? pixel[0] : pixel[2];
                                row[4 * x + 3] = 255;
                            }
                        }
                    }
                    file.write(reinterpret_cast<const char*>(row.data()), row.size());
                }
                file.flush();
                if (file) {
                    wchar_t log_msg[256];
                    swprintf_s(log_msg, L"EDPE: %ls capture saved %ls",
                        req.kind == UiState::CaptureKind::Depth ? L"depth" :
                        req.kind == UiState::CaptureKind::Motion ? L"motion" :
                        req.kind == UiState::CaptureKind::SceneColor ? L"scene color" :
                        req.kind == UiState::CaptureKind::EarlyHdr ? L"early HDR" :
                        req.kind == UiState::CaptureKind::ComparisonBefore ? L"comparison before" :
                        req.kind == UiState::CaptureKind::ComparisonAfter ? L"comparison after" : L"frame",
                        path.wstring().c_str());
                    EdpeLog(log_msg);
                    if (req.kind == UiState::CaptureKind::ComparisonAfter) {
                        swprintf_s(ui.last_comparison_saved, L"Saved %ls", name);
                    }
                } else {
                    EdpeLog(L"EDPE: capture file write failed");
                }
            }
            ui.context->Unmap(req.staging, 0);
        } else {
            EdpeLog(L"EDPE: capture readback failed or timed out");
        }
        req.staging->Release();
        it = ui.pending_captures.erase(it);
    }
}

void placeSnapshotWindow(int column, int row = 0) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const bool depth_row = ui.depth_srv &&
        (ui.early_color_srv || ui.motion_srv || ui.color_srv);
    const float width = std::min(700.0f, std::max(1.0f, (display.x - 40.0f) / 3.0f));
    const float left = std::max(0.0f, (display.x - 3.0f * width - 20.0f) * 0.5f);
    const float top = display.y > 600.0f ? 80.0f : 10.0f;
    const float height = std::min(440.0f,
        std::max(1.0f, (display.y - top - (depth_row ? 30.0f : 10.0f)) /
            (depth_row ? 2.0f : 1.0f)));
    const ImGuiCond condition = ui.snapshot_ready ? ImGuiCond_Always : ImGuiCond_Appearing;
    ImGui::SetNextWindowPos(ImVec2(left + column * (width + 10.0f),
        top + row * (height + 10.0f)), condition);
    ImGui::SetNextWindowSize(ImVec2(width, height), condition);
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
        queueTextureCapture(copy, UiState::CaptureKind::Depth, static_cast<int>(index));
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
        queueTextureCapture(copy, UiState::CaptureKind::SceneColor, static_cast<int>(index));
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
    for (auto& req : ui.pending_captures) {
        if (req.staging) req.staging->Release();
    }
    ui.pending_captures.clear();
    if (ui.motion_preview_shader) ui.motion_preview_shader->Release();
    if (ui.depth_contrast_shader) ui.depth_contrast_shader->Release();
    if (ui.context) ui.context->Release();
    if (ui.device) ui.device->Release();
    edpe::UpscalerManager::instance().shutdown();
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
    ImGui::GetIO().MouseDrawCursor = false;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
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
    edpe::UpscalerManager::instance().initialize(ui.device, ui.context);
    edpe::installInputHooks();
    {
        auto& cfg = edpe::Config::instance();
        cfg.load();
        edpe::NgxContext::set_preset(static_cast<uint32_t>(cfg.dlssPreset));
        if (cfg.upscalerBackend == 1 &&
            edpe::UpscalerManager::instance().isBackendAvailable(edpe::UpscalerBackend::NvidiaDlss)) {
            edpe::UpscalerManager::instance().setBackend(edpe::UpscalerBackend::NvidiaDlss);
            const int m = (cfg.upscaleMode >= 1 && cfg.upscaleMode <= 5) ? cfg.upscaleMode : 1;
            edpe::UpscalerManager::instance().setMode(static_cast<edpe::UpscaleMode>(m));
        } else if (cfg.upscalerBackend == 2 &&
            edpe::UpscalerManager::instance().isBackendAvailable(edpe::UpscalerBackend::AmdFsr)) {
            edpe::UpscalerManager::instance().setBackend(edpe::UpscalerBackend::AmdFsr);
            const int m = (cfg.upscaleMode >= 1 && cfg.upscaleMode <= 5) ? cfg.upscaleMode : 1;
            edpe::UpscalerManager::instance().setMode(static_cast<edpe::UpscaleMode>(m));
        } else if (cfg.upscalerBackend == 3 &&
            edpe::UpscalerManager::instance().isBackendAvailable(edpe::UpscalerBackend::Taa)) {
            edpe::UpscalerManager::instance().setBackend(edpe::UpscalerBackend::Taa);
            edpe::UpscalerManager::instance().setMode(edpe::UpscaleMode::NativeAA);
        }
    }
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
    if (all_captures_requested.exchange(false, std::memory_order_acq_rel)) {
        int candidate = ContextCensusSceneDepthCandidate();
        if (candidate < 0 && ui.depth_candidate >= 0 &&
            ContextCensusDepthSnapshotAvailable(static_cast<unsigned>(ui.depth_candidate))) {
            candidate = ui.depth_candidate;
        }
        if (candidate < 0 && ContextCensusDepthSnapshotAvailable(2)) {
            candidate = 2;
        }
        if (candidate >= 0) {
            ui.depth_candidate = candidate;
            ContextCensusRequestMotionPair(static_cast<unsigned>(candidate));
            ui.pending_backbuffer_capture_on_motion = true;
            releaseMotionSnapshot();
            releaseColorSnapshot();
            releaseEarlyColorSnapshot();
            releaseDepthSnapshot();
            wchar_t msg[160];
            swprintf_s(msg, L"EDPE: all-captures armed on DSV #%d", candidate);
            EdpeLog(msg);
        } else {
            EdpeLog(L"EDPE: all-captures armed backbuffer only (no depth candidate yet)");
            ui.pending_backbuffer_capture = true;
        }
    }
    bool motion_batch_ready = false;
    if (auto* motion = ContextCensusTakeMotionSnapshot(&ui.motion_width, &ui.motion_height)) {
        releaseMotionSnapshot();
        ui.motion_srv = motion;
        ui.motion_window_open = true;
        ui.snapshot_ready = true;
        ensureMotionPreviewShader();
        queueTextureCaptureFromSrv(ui.motion_srv, UiState::CaptureKind::Motion);
        EdpeLog(L"EDPE: motion snapshot handed to ImGui");
        motion_batch_ready = true;
    }
    UINT motion_color_width = 0, motion_color_height = 0;
    if (auto* color = ContextCensusTakeMotionColorSnapshot(
            &motion_color_width, &motion_color_height)) {
        releaseColorSnapshot();
        ui.color_srv = color;
        ui.color_width = motion_color_width;
        ui.color_height = motion_color_height;
        ui.color_window_open = true;
        queueTextureCaptureFromSrv(ui.color_srv, UiState::CaptureKind::SceneColor);
        EdpeLog(L"EDPE: same-frame HDR color handed to ImGui");
    }
    if (auto* early = ContextCensusTakeMotionEarlyColorSnapshot(
            &ui.early_color_width, &ui.early_color_height)) {
        releaseEarlyColorSnapshot();
        ui.early_color_srv = early;
        ui.early_color_window_open = true;
        queueTextureCaptureFromSrv(ui.early_color_srv, UiState::CaptureKind::EarlyHdr);
        EdpeLog(L"EDPE: early HDR color handed to ImGui");
    }
    UINT motion_depth_width = 0, motion_depth_height = 0;
    unsigned motion_depth_idx = 0;
    if (auto* depth_view = ContextCensusTakeMotionDepthSnapshot(
            &motion_depth_width, &motion_depth_height, &motion_depth_idx)) {
        releaseDepthSnapshot();
        ui.depth_srv = depth_view;
        ui.depth_width = motion_depth_width;
        ui.depth_height = motion_depth_height;
        ui.depth_snapshot_index = static_cast<int>(motion_depth_idx);
        ui.depth_window_open = true;
        ui.snapshot_ready = true;
        ensureDepthContrastShader();
        queueTextureCaptureFromSrv(ui.depth_srv, UiState::CaptureKind::Depth, static_cast<int>(motion_depth_idx));
        EdpeLog(L"EDPE: same-frame depth handed to ImGui");
    }
    if (motion_batch_ready && ui.pending_backbuffer_capture_on_motion) {
        ui.pending_backbuffer_capture_on_motion = false;
        ui.pending_backbuffer_capture = true;
    }

    // Manual single depth snapshot (if triggered separately via menu button)
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

    // Clean backbuffer capture (captured before ImGui overlay is drawn)
    if (ui.pending_backbuffer_capture) {
        ui.pending_backbuffer_capture = false;
        ID3D11Texture2D* backbuffer = nullptr;
        if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
            queueTextureCapture(backbuffer, UiState::CaptureKind::Backbuffer);
            backbuffer->Release();
            EdpeLog(L"EDPE: clean game frame backbuffer queued for capture");
        }
    }

    auto& cfg = edpe::Config::instance();
    const bool comparison_pending = ContextCensusIsComparisonCapturePending();
    const bool show_menu = menu_visible.load();
    const bool show_perf = cfg.showPerfOverlay || comparison_pending;

    if (!show_menu && !show_perf && !comparison_pending) {
        std::lock_guard lock(input_mutex);
        pending_input.clear();
        return;
    }
    if (!ui.backbuffer_rtv && !createBackbufferView(swap_chain)) {
        EdpeLog(L"EDPE: overlay backbuffer view unavailable; presenting original frame");
        return;
    }

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ui.imgui);

    const bool draw_menu = show_menu && !comparison_pending;
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = draw_menu;
    if (draw_menu) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
    if (draw_menu) {
        std::lock_guard lock(input_mutex);
        pending_input.swap(render_input);
        if (!render_input.empty() && !ui.input_logged) {
            EdpeLog(L"EDPE: queued input routed to Dear ImGui");
            ui.input_logged = true;
        }
        for (const auto& input : render_input) {
            ImGui_ImplWin32_WndProcHandler(input.window, input.message, input.wparam, input.lparam);
        }
        render_input.clear();
    } else {
        std::lock_guard lock(input_mutex);
        pending_input.clear();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (show_perf) {
        edpe::PerfTracker::instance().renderOverlay(draw_menu, cfg.perfOverlayCorner, cfg.perfOverlayScale);
    }

    bool window_open = true;
    if (draw_menu) {
        ImGui::Begin("EDPE \xE2\x80\x94 Elite Dangerous Performance Enhanced", &window_open);
        ImGui::Text("Version: %s", edpe::kVersion);
        auto& upscaler = edpe::UpscalerManager::instance();
        const bool nvidia_available = upscaler.isBackendAvailable(edpe::UpscalerBackend::NvidiaDlss);
        const bool amd_available = upscaler.isBackendAvailable(edpe::UpscalerBackend::AmdFsr);

    // Performance HUD Controls
    ImGui::Separator();
    ImGui::TextUnformatted("Performance HUD Overlay");
    if (ImGui::Checkbox("Show Performance HUD (FPS, Frametimes, GPU, VRAM)", &cfg.showPerfOverlay)) {
        cfg.save();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Hotkey: F7)");

    const char* hud_corners[] = { "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right" };
    if (ImGui::Combo("HUD Position", &cfg.perfOverlayCorner, hud_corners, IM_ARRAYSIZE(hud_corners))) {
        cfg.perfOverlayPosX = -1.0f;
        cfg.perfOverlayPosY = -1.0f;
        cfg.save();
    }
    if (cfg.perfOverlayPosX >= 0.0f && cfg.perfOverlayPosY >= 0.0f) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset Dragged Position")) {
            cfg.perfOverlayPosX = -1.0f;
            cfg.perfOverlayPosY = -1.0f;
            cfg.save();
        }
    }
    if (ImGui::SliderFloat("HUD Scale", &cfg.perfOverlayScale, 0.5f, 2.5f, "%.2fx")) {
        cfg.save();
    }

    const auto& live_tel = edpe::PerfTracker::instance().getTelemetry();
    ImGui::Text("Stats: %.1f FPS | Total: %.2f ms (Game: %.2f ms, Upscaler: %.2f ms)",
        live_tel.fps, live_tel.totalFrametimeMs, live_tel.gameFrametimeMs, live_tel.upscalerFrametimeMs);

    if (upscaler.isReconstructionActive()) {
        const bool isAmd = (upscaler.getBackend() == edpe::UpscalerBackend::AmdFsr);
        const bool isTaa = (upscaler.getBackend() == edpe::UpscalerBackend::Taa);
        const bool isSr = (live_tel.renderWidth < live_tel.outputWidth || live_tel.renderHeight < live_tel.outputHeight);
        const char* uName = isTaa ? "TAA" :
                            isAmd ? (isSr ? "AMD FSR SR" : "AMD FSR Native AA")
                                  : (isSr ? "NVIDIA DLSS" : "NVIDIA DLAA");
        char pBuf[64]{};
        if (isTaa || isAmd) {
            snprintf(pBuf, sizeof(pBuf), "(%s)", isSr ? "Super Resolution" : "Native AA");
        } else {
            const uint32_t p = edpe::NgxContext::get_preset();
            const char* pName = (p == 10) ? "Preset J" :
                                (p == 11) ? "Preset K" :
                                (p == 12) ? "Preset L" :
                                (p == 13) ? "Preset M" : "Auto (Preset K)";
            snprintf(pBuf, sizeof(pBuf), "(%s)", pName);
        }
        ImGui::Text("Reconstruction: %s %s [%ux%u -> %ux%u]",
            uName, pBuf, live_tel.renderWidth, live_tel.renderHeight, live_tel.outputWidth, live_tel.outputHeight);
    } else {
        ImGui::Text("Reconstruction: Bypass (Native) [%ux%u -> %ux%u]",
            live_tel.outputWidth, live_tel.outputHeight, live_tel.outputWidth, live_tel.outputHeight);
    }

    ImGui::Text("Hardware: %ls | Load: %u%%",
        live_tel.gpuName.empty() ? L"Direct3D 11 Adapter" : live_tel.gpuName.c_str(), live_tel.gpuUsagePct);
    if (live_tel.vramTotalBytes > 0) {
        ImGui::Text("VRAM: %.2f / %.2f GB (%u%%)",
            static_cast<double>(live_tel.vramUsedBytes) / (1024.0 * 1024.0 * 1024.0),
            static_cast<double>(live_tel.vramTotalBytes) / (1024.0 * 1024.0 * 1024.0),
            static_cast<unsigned>(100.0 * live_tel.vramUsedBytes / live_tel.vramTotalBytes));
    }
    ImGui::Separator();

    // Upscaler backend selector
    const char* upscaler_items[] = {
        "Native (Bypass)",
        nvidia_available ? "NVIDIA DLSS / DLAA" : "NVIDIA DLSS / DLAA (unavailable)",
        amd_available ? "AMD FSR (Native AA / SR)" : "AMD FSR (unavailable)",
        "TAA (Temporal Anti-Aliasing)"
    };
    int current_backend_idx = (upscaler.getBackend() == edpe::UpscalerBackend::NvidiaDlss) ? 1 :
                              (upscaler.getBackend() == edpe::UpscalerBackend::AmdFsr) ? 2 :
                              (upscaler.getBackend() == edpe::UpscalerBackend::Taa) ? 3 : 0;

    if (ImGui::Combo("Upscaler Backend", &current_backend_idx, upscaler_items, IM_ARRAYSIZE(upscaler_items))) {
        if (current_backend_idx == 1) {
            if (nvidia_available) {
                upscaler.setBackend(edpe::UpscalerBackend::NvidiaDlss);
                upscaler.setMode(edpe::UpscaleMode::NativeAA);
                cfg.upscalerBackend = 1;
                cfg.save();
            }
        } else if (current_backend_idx == 2) {
            if (amd_available) {
                upscaler.setBackend(edpe::UpscalerBackend::AmdFsr);
                upscaler.setMode(edpe::UpscaleMode::NativeAA);
                cfg.upscalerBackend = 2;
                cfg.save();
            }
        } else if (current_backend_idx == 3) {
            upscaler.setBackend(edpe::UpscalerBackend::Taa);
            upscaler.setMode(edpe::UpscaleMode::NativeAA);
            cfg.upscalerBackend = 3;
            cfg.save();
        } else {
            upscaler.setBackend(edpe::UpscalerBackend::None);
            upscaler.setMode(edpe::UpscaleMode::Native);
            cfg.upscalerBackend = 0;
            cfg.save();
        }
    }

    const bool backend_active = (upscaler.getBackend() != edpe::UpscalerBackend::None);
    const bool nvidia_active = (upscaler.getBackend() == edpe::UpscalerBackend::NvidiaDlss);
    const bool amd_active = (upscaler.getBackend() == edpe::UpscalerBackend::AmdFsr);

    if (!nvidia_active) ImGui::BeginDisabled();

    // NVIDIA DLSS Preset selector
    const char* preset_items[] = {
        "Auto (Preset K)",
        "Preset J (Transformer)",
        "Preset K (Transformer)",
        "Preset L",
        "Preset M"
    };
    const uint32_t curPreset = edpe::NgxContext::get_preset();
    int preset_idx = (curPreset >= 10u && curPreset <= 13u) ? static_cast<int>(curPreset - 9u) : 0;
    if (ImGui::Combo("NVIDIA DLSS Preset", &preset_idx, preset_items, IM_ARRAYSIZE(preset_items))) {
        const uint32_t newPreset = (preset_idx == 0) ? 0u : static_cast<uint32_t>(preset_idx + 9);
        edpe::NgxContext::set_preset(newPreset);
        cfg.dlssPreset = static_cast<int>(newPreset);
        cfg.save();
    }
    if (!nvidia_active) ImGui::EndDisabled();

    DXGI_SWAP_CHAIN_DESC scDesc{};
    swap_chain->GetDesc(&scDesc);
    const UINT outW = scDesc.BufferDesc.Width;
    const UINT outH = scDesc.BufferDesc.Height;
    UINT sceneW = 0, sceneH = 0;
    ContextCensusGetSceneRenderSize(&sceneW, &sceneH);
    if (!sceneW || !sceneH) {
        sceneW = outW;
        sceneH = outH;
    }
    const float activeScalePct = outW ? (100.0f * static_cast<float>(sceneW) / static_cast<float>(outW)) : 100.0f;
    const bool isSr = (sceneW < outW || sceneH < outH);

    const bool taa_active = (upscaler.getBackend() == edpe::UpscalerBackend::Taa);
    const auto& diag = upscaler.getDiagnostics();
    if (backend_active) {
        const char* backendName = taa_active ? "Native D3D11" : (amd_active ? "AMD" : "NVIDIA");
        const char* techName = taa_active
            ? (isSr ? "TAA Super Resolution" : "TAA Native AA")
            : (amd_active ? (isSr ? "FSR Super Resolution" : "FSR Native AA")
                          : (isSr ? "DLSS Super Resolution" : "DLAA"));
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
            "Reconstruction: %s %s Active",
            backendName, techName);
        ImGui::Text("Resolution: Scene %ux%u -> Output %ux%u (%.1f%%)",
            sceneW, sceneH, outW, outH, activeScalePct);
        ImGui::Text("Active Mode: %ls",
            edpe::getUpscaleModeName(upscaler.getMode()));
        if (isSr) {
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                "Scaling is automatically driven by game Supersampling (< 1.0x).");
        } else {
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                "Native 1:1 render detected -> %s antialiasing active.",
                taa_active ? "TAA" : (amd_active ? "FSR Native AA" : "DLAA"));
        }
    } else {
        ImGui::Text("Reconstruction: Inactive (Scene %ux%u -> Output %ux%u)",
            sceneW, sceneH, outW, outH);
    }

    ImGui::Text("Status: %ls", diag.statusMessage);
    if (diag.didRun) {
        ImGui::Text("GPU Reconstruction Time: %.3f ms", diag.gpuTimeMs);
    }

    // Comparison Screenshot Button
    ImGui::Separator();
    ImGui::TextUnformatted("Reconstruction Screenshot Comparison");
    const bool can_compare = backend_active && upscaler.isReconstructionActive();
    if (!can_compare) ImGui::BeginDisabled();
    if (ImGui::Button("Capture Before & After Reconstruction (2x BMP)")) {
        ContextCensusRequestComparisonCapture();
    }
    if (!can_compare) {
        ImGui::EndDisabled();
        ImGui::TextDisabled("Enable DLSS, DLAA or FSR to capture comparison");
    } else if (ContextCensusIsComparisonCapturePending()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Comparison capture queued for next frame...");
    } else if (ui.last_comparison_saved[0] != L'\0') {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%ls (and -before.bmp)", ui.last_comparison_saved);
    }

    if (!nvidia_available) {
        ImGui::TextDisabled("NVIDIA DLSS/DLAA reason: %ls",
            upscaler.getBackendUnavailableReason(edpe::UpscalerBackend::NvidiaDlss));
    }
    if (!amd_available) {
        ImGui::TextDisabled("AMD FSR reason: %ls",
            upscaler.getBackendUnavailableReason(edpe::UpscalerBackend::AmdFsr));
    }
    ImGui::BeginDisabled();
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
        ImGui::Text("Emissive (Early HDR) candidate: %ux%u", ui.early_color_width,
            ui.early_color_height);
        if (!ui.early_color_window_open && ImGui::Button("Show emissive / glow snapshot"))
            ui.early_color_window_open = true;
    }
    if (ui.motion_srv) {
        ImGui::Text("Motion candidate: %ux%u, current to previous pixels",
            ui.motion_width, ui.motion_height);
        if (!ui.motion_window_open && ImGui::Button("Show motion snapshot"))
            ui.motion_window_open = true;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Projection Jitter & Motion Tuning (DLAA Stability)");
    bool jitter_enabled = ContextCensusGetJitterEnabled();
    if (ImGui::Checkbox("Enable Projection Jitter (Halton 2,3)", &jitter_enabled)) {
        ContextCensusSetJitterEnabled(jitter_enabled);
    }
    bool jitter_override = ContextCensusGetJitterTestOverride();
    if (ImGui::Checkbox("Force Jitter test override (subpixel vibration test)", &jitter_override)) {
        ContextCensusSetJitterTestOverride(jitter_override);
    }

    bool j_inv_x = ContextCensusGetJitterInvertX();
    if (ImGui::Checkbox("Invert Jitter X (pass -JitterX to DLSS)", &j_inv_x)) {
        ContextCensusSetJitterInvertX(j_inv_x);
        edpe::Config::instance().save();
    }
    ImGui::SameLine();
    bool j_inv_y = ContextCensusGetJitterInvertY();
    if (ImGui::Checkbox("Invert Jitter Y (pass -JitterY to DLSS)", &j_inv_y)) {
        ContextCensusSetJitterInvertY(j_inv_y);
        edpe::Config::instance().save();
    }

    bool j_swap = ContextCensusGetJitterSwapXY();
    if (ImGui::Checkbox("Swap Jitter X/Y", &j_swap)) {
        ContextCensusSetJitterSwapXY(j_swap);
        edpe::Config::instance().save();
    }

    float j_scale = ContextCensusGetJitterScale();
    if (ImGui::SliderFloat("Jitter Scale (1.0 = standard, 0.0 = disable)", &j_scale, 0.0f, 2.0f, "%.2f")) {
        ContextCensusSetJitterScale(j_scale);
        edpe::Config::instance().save();
    }

    int j_phases = static_cast<int>(ContextCensusGetJitterPhaseCount());
    if (ImGui::SliderInt("Jitter Phase Cycle (default: 8, standard Halton)", &j_phases, 0, 128)) {
        ContextCensusSetJitterPhaseCount(static_cast<unsigned>(j_phases));
        edpe::Config::instance().save();
    }

    bool preserve_ui = ContextCensusGetPreserveNativeUi();
    if (ImGui::Checkbox("Preserve Native UI (Composite in Super Resolution)", &preserve_ui)) {
        ContextCensusSetPreserveNativeUi(preserve_ui);
        edpe::Config::instance().save();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Motion Vectors Tuning");
    bool zero_mv = ContextCensusGetZeroMotion();
    if (ImGui::Checkbox("Zero Motion Vectors (Force stationary 0,0)", &zero_mv)) {
        ContextCensusSetZeroMotion(zero_mv);
        edpe::Config::instance().save();
    }
    bool m_inv_x = ContextCensusGetMotionInvertX();
    if (ImGui::Checkbox("Invert Motion X", &m_inv_x)) {
        ContextCensusSetMotionInvertX(m_inv_x);
        edpe::Config::instance().save();
    }
    ImGui::SameLine();
    bool m_inv_y = ContextCensusGetMotionInvertY();
    if (ImGui::Checkbox("Invert Motion Y", &m_inv_y)) {
        ContextCensusSetMotionInvertY(m_inv_y);
        edpe::Config::instance().save();
    }
    float m_scale = ContextCensusGetMotionScale();
    if (ImGui::SliderFloat("Motion Scale", &m_scale, 0.0f, 2.0f, "%.2f")) {
        ContextCensusSetMotionScale(m_scale);
        edpe::Config::instance().save();
    }

    if (ImGui::Button("Save Tuning to edpe.ini")) {
        edpe::Config::instance().save();
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "Subpixel Jitter Debug View (Hotkey: F8)");
    bool debug_view = ContextCensusGetJitterDebugView();
    if (ImGui::Checkbox("Enable Jitter Debug View (Grayscale Scene + Green Jitter Pixels)", &debug_view)) {
        ContextCensusSetJitterDebugView(debug_view);
        auto& cfg = edpe::Config::instance();
        cfg.jitterDebugView = debug_view;
        cfg.save();
    }
    if (debug_view) {
        float sens = ContextCensusGetJitterDebugSensitivity();
        if (ImGui::SliderFloat("Jitter Sensitivity", &sens, 0.1f, 5.0f, "%.2fx")) {
            ContextCensusSetJitterDebugSensitivity(sens);
            auto& cfg = edpe::Config::instance();
            cfg.jitterDebugSensitivity = sens;
            cfg.save();
        }
        float thresh = ContextCensusGetJitterDebugThreshold();
        if (ImGui::SliderFloat("Jitter Threshold", &thresh, 0.002f, 0.1f, "%.3f")) {
            ContextCensusSetJitterDebugThreshold(thresh);
            auto& cfg = edpe::Config::instance();
            cfg.jitterDebugThreshold = thresh;
            cfg.save();
        }
        int mode = ContextCensusGetJitterDebugMode();
        const char* const modes[] = {
            "Hybrid (Spatial Shift + Temporal Pulse)",
            "Spatial Jitter Shift Only",
            "Temporal Pulse Only",
            "Binary Mask Only"
        };
        if (ImGui::Combo("Debug Mode", &mode, modes, IM_ARRAYSIZE(modes))) {
            ContextCensusSetJitterDebugMode(mode);
            auto& cfg = edpe::Config::instance();
            cfg.jitterDebugMode = mode;
            cfg.save();
        }
        ImGui::TextDisabled("Grayscale = game scene  |  Green pixels = subpixel jitter affected");
    }

    ImGui::Separator();
    if (ImGui::Button("Capture ALL buffers (diagnostic dump)")) {
        all_captures_requested.store(true, std::memory_order_release);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("(color, motion, depth, emission, frame)");
    if (ImGui::Button("Capture clean game frame (backbuffer BMP)")) {
        ui.pending_backbuffer_capture = true;
    }
    ImGui::TextUnformatted("F5: toggle menu | F7: toggle performance HUD | F8: toggle jitter debug view");
    ImGui::End();
    if (draw_menu && ui.depth_srv && ui.depth_window_open) {
        placeSnapshotWindow(0, ui.early_color_srv || ui.motion_srv || ui.color_srv ? 1 : 0);
        if (ImGui::Begin("EDPE Depth Snapshot", &ui.depth_window_open)) {
            ImGui::Text("DSV #%d (%ux%u)",
                ui.depth_snapshot_index, ui.depth_width, ui.depth_height);
            if (ui.depth_range_valid) ImGui::Text("8x8 sample range: %.6g .. %.6g",
                ui.depth_min, ui.depth_max);
            if (ui.depth_center_valid) ImGui::Text("Center raw depth: %.9g", ui.depth_center);
            if (ui.depth_contrast_shader) ImGui::Checkbox("Contrast preview", &ui.depth_contrast);
            else ImGui::TextDisabled("Contrast preview unavailable; showing raw red depth");
            if (ui.depth_copy && ImGui::Button("Save depth snapshot to BMP")) {
                queueTextureCapture(ui.depth_copy, UiState::CaptureKind::Depth, ui.depth_snapshot_index);
            }
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
    if (draw_menu && ui.color_srv && ui.color_window_open) {
        placeSnapshotWindow(2);
        if (ImGui::Begin("EDPE Scene Color Snapshot", &ui.color_window_open)) {
            if (ui.color_snapshot_index >= 0)
                ImGui::Text("DSV #%d / HDR RTV0 (%ux%u)",
                    ui.color_snapshot_index, ui.color_width, ui.color_height);
            else
                ImGui::Text("Motion frame / HDR RTV3 (%ux%u)",
                    ui.color_width, ui.color_height);
            if (ui.color_snapshot_index < 0)
                ImGui::TextUnformatted("Present-time HDR color; may include HUD");
            if (ui.color_copy && ImGui::Button("Save scene color to BMP")) {
                queueTextureCapture(ui.color_copy, UiState::CaptureKind::SceneColor, ui.color_snapshot_index);
            } else if (ui.color_srv && ImGui::Button("Save scene color to BMP")) {
                queueTextureCaptureFromSrv(ui.color_srv, UiState::CaptureKind::SceneColor, ui.color_snapshot_index);
            }
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
    if (draw_menu && ui.early_color_srv && ui.early_color_window_open) {
        placeSnapshotWindow(0);
        if (ImGui::Begin("EDPE Emissive / Glow (Early HDR) Snapshot", &ui.early_color_window_open)) {
            ImGui::TextUnformatted("Emissive / illumination pass: lamps, dials, lights");
            ImGui::TextUnformatted("Direct native candidate for upscaler Reactive Mask");
            if (ui.early_color_srv && ImGui::Button("Save emissive to BMP")) {
                queueTextureCaptureFromSrv(ui.early_color_srv, UiState::CaptureKind::EarlyHdr);
            }
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
    if (draw_menu && ui.motion_srv && ui.motion_window_open) {
        placeSnapshotWindow(1);
        if (ImGui::Begin("EDPE Motion Snapshot", &ui.motion_window_open)) {
            ImGui::TextUnformatted("Camera + depth motion; HUD is not represented");
            ImGui::TextUnformatted("Neutral gray = 0 pixels; red = horizontal, green = vertical");
            ImGui::TextUnformatted("Nonlinear preview: 10 pixels shifts a channel by 0.25");
            if (ui.motion_srv && ImGui::Button("Save motion to BMP")) {
                queueTextureCaptureFromSrv(ui.motion_srv, UiState::CaptureKind::Motion);
            }
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
    } // if (draw_menu)
    if (draw_menu && !window_open) {
        menu_visible.store(false);
        while (ShowCursor(FALSE) >= 0) {}
    }
    ImGui::Render();

    ID3D11RenderTargetView* previous_rtv = nullptr;
    ID3D11DepthStencilView* previous_dsv = nullptr;
    ui.context->OMGetRenderTargets(1, &previous_rtv, &previous_dsv);

    ID3D11Texture2D* copy_tex = ContextCensusGetBackbufferCopyTexture();
    ID3D11RenderTargetView* copy_rtv = ContextCensusGetBackbufferCopyRtv();

    if (comparison_pending && copy_rtv && copy_tex) {
        ui.context->OMSetRenderTargets(1, &copy_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    ui.context->OMSetRenderTargets(1, &ui.backbuffer_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    if (comparison_pending && copy_tex) {
        ID3D11Texture2D* backbuffer = nullptr;
        if (SUCCEEDED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)))) {
            ContextCensusConsumeComparisonCaptureRequest();
            UiQueueComparisonCapture(ui.context, copy_tex, backbuffer);
            backbuffer->Release();
        }
    }

    ui.context->OMSetRenderTargets(1, &previous_rtv, previous_dsv);
    if (previous_rtv) previous_rtv->Release();
    if (previous_dsv) previous_dsv->Release();
    ui.snapshot_ready = false;
    ImGui::SetCurrentContext(previous);
}

void UiOnResize(IDXGISwapChain* swap_chain) {
    if (ui.swap_chain == swap_chain) {
        EdpeLog(L"EDPE: releasing overlay backbuffer for resize");
        releaseBackbuffer();
    }
}

void UiOnRelease(IUnknown* object) {
    if (ui.swap_chain == object) {
        EdpeLog(L"EDPE: swap chain destroyed; shutting overlay down");
        shutdownUi();
    }
}

bool UiMenuVisible() { return menu_visible.load(); }

void UiQueueComparisonCapture(ID3D11DeviceContext* context, ID3D11Texture2D* beforeTex, ID3D11Texture2D* afterTex) {
    queueComparisonCaptureInternal(context, beforeTex, afterTex);
}
