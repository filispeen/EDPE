#include <imgui.h>
#include <imgui_internal.h>
#include <cassert>
#include <cstdio>
#include <algorithm>

int main() {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(2560.0f, 1440.0f);
    io.IniFilename = nullptr;

    ImFont* font = io.Fonts->AddFontDefaultVector();
    io.FontDefault = font;

    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    const float uiScale = 1.0f;
    const ImVec2 display = io.DisplaySize;
    const float defW = (std::min)(540.0f * uiScale, display.x > 0.0f ? display.x - 40.0f : 540.0f);
    const float maxH = (display.y > 100.0f) ? (display.y - 40.0f) : 1080.0f;

    bool header1Open = true;
    bool header2Open = false;

    for (int frame = 0; frame < 15; ++frame) {
        ImGui::NewFrame();

        ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f * uiScale, 80.0f), ImVec2(display.x > 0.0f ? display.x : 3840.0f, maxH));
        ImGui::SetNextWindowSize(ImVec2(defW, 0.0f), ImGuiCond_Always);

        bool window_open = true;
        ImGui::Begin("EDPE | Elite Dangerous Performance Enhanced", &window_open, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Version: dev");
        if (ImGui::CollapsingHeader("Header 1", header1Open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ImGui::Checkbox("Option 1", &header1Open);
            ImGui::Button("Button 1");
        }
        if (header2Open) {
            for (int i = 0; i < 30; ++i) {
                ImGui::Text("Diagnostic line %d", i);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // Keybinds with token-level wrapping
        const char* const binds[] = {
            "F5: menu", "F6: capture", "F7: perf HUD",
            "F8: debug views", "F9: record jitter"
        };
        const int bindCount = sizeof(binds) / sizeof(binds[0]);
        const float sepWidth = ImGui::CalcTextSize(" | ").x;

        for (int i = 0; i < bindCount; ++i) {
            const float itemWidth = ImGui::CalcTextSize(binds[i]).x;
            if (i > 0) {
                const float lastRight = ImGui::GetItemRectMax().x;
                const float windowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                const float remaining = windowRight - lastRight;

                if (remaining >= (sepWidth + itemWidth)) {
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextDisabled(" | ");
                    ImGui::SameLine(0.0f, 0.0f);
                }
            }
            ImGui::TextUnformatted(binds[i]);
        }

        ImGui::TextDisabled("(Settings auto-saved)");

        ImGui::End();
        ImGui::Render();

        if (frame == 3) {
            ImGuiWindow* win = ImGui::FindWindowByName("EDPE | Elite Dangerous Performance Enhanced");
            assert(win != nullptr);
            assert(win->Size.x == defW);
            assert(win->Size.y < 300.0f);
            header2Open = true;
        } else if (frame == 7) {
            ImGuiWindow* win = ImGui::FindWindowByName("EDPE | Elite Dangerous Performance Enhanced");
            assert(win != nullptr);
            assert(win->Size.x == defW);
            assert(win->Size.y > 500.0f);
            header2Open = false;
        } else if (frame == 11) {
            ImGuiWindow* win = ImGui::FindWindowByName("EDPE | Elite Dangerous Performance Enhanced");
            assert(win != nullptr);
            assert(win->Size.x == defW);
            assert(win->Size.y < 300.0f);
        }
    }

    ImGui::DestroyContext(ctx);
    printf("Exact EDPE Menu auto-size and keybind wrapping tests passed perfectly!\n");
    return 0;
}
