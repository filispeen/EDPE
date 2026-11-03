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
        ImGui::Text("F5: menu | F7: perf HUD");
        ImGui::TextDisabled("(Settings auto-saved)");

        ImGui::End();
        ImGui::Render();

        if (frame == 3) {
            ImGuiWindow* win = ImGui::FindWindowByName("EDPE | Elite Dangerous Performance Enhanced");
            printf("Frame 3 (Default): Width=%.1f (expected %.1f), Height=%.1f (expected < 250)\n", win->Size.x, defW, win->Size.y);
            assert(win->Size.x == defW);
            assert(win->Size.y < 250.0f);

            // Open header 2
            header2Open = true;
        } else if (frame == 7) {
            ImGuiWindow* win = ImGui::FindWindowByName("EDPE | Elite Dangerous Performance Enhanced");
            printf("Frame 7 (Expanded): Width=%.1f, Height=%.1f (expected > 500)\n", win->Size.x, win->Size.y);
            assert(win->Size.x == defW);
            assert(win->Size.y > 500.0f);

            // Close header 2
            header2Open = false;
        } else if (frame == 11) {
            ImGuiWindow* win = ImGui::FindWindowByName("EDPE | Elite Dangerous Performance Enhanced");
            printf("Frame 11 (Collapsed back): Width=%.1f, Height=%.1f (expected < 250)\n", win->Size.x, win->Size.y);
            assert(win->Size.x == defW);
            assert(win->Size.y < 250.0f);
        }
    }

    ImGui::DestroyContext(ctx);
    printf("Exact EDPE Menu auto-size test passed perfectly!\n");
    return 0;
}
