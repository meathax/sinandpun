// Offline preview of the in-game options menu. Links the real settings_overlay.cpp / game_ui.cpp / controls_menu.cpp
// against ImGui and the stubs in stubs.cpp, drives the menu with scripted input and writes a software-rasterized
// premultiplied RGBA dump per screenshot (tools/ui_preview/run.ps1 converts it to PNG over a game screenshot).
// No game, GPU or window is needed.
//
//   ui_preview WIDTH HEIGHT OUTPREFIX command...
//   commands: open | wait N | key NAME | hold NAME N | shot LABEL | mouse X Y | click | tab N | rclose
//   key names: Up Down Left Right Enter Esc E Q Delete
#include "settings_overlay.h"
#include "game_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<float> px;  // premultiplied RGBA
};

float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

void Sample(const unsigned char* tex, int tw, int th, float u, float v, float out[4]) {
    const float x = u * tw - 0.5f, y = v * th - 0.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    for (int c = 0; c < 4; ++c) out[c] = 0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int xi = std::clamp(x0 + i, 0, tw - 1), yi = std::clamp(y0 + j, 0, th - 1);
            const float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            for (int c = 0; c < 4; ++c) out[c] += wgt * tex[(yi * tw + xi) * 4 + c] / 255.0f;
        }
    }
}

void Raster(Image& img, const ImDrawData* dd, const unsigned char* tex, int tw, int th) {
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* list = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const int cx0 = std::max(0, static_cast<int>(cmd.ClipRect.x)), cy0 = std::max(0, static_cast<int>(cmd.ClipRect.y));
            const int cx1 = std::min(img.w, static_cast<int>(std::ceil(cmd.ClipRect.z))), cy1 = std::min(img.h, static_cast<int>(std::ceil(cmd.ClipRect.w)));
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; ++k) v[k] = &list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset + i + k] + cmd.VtxOffset];
                const float minx = std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}), maxx = std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x});
                const float miny = std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}), maxy = std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y});
                const int x0 = std::max(cx0, static_cast<int>(std::floor(minx))), x1 = std::min(cx1, static_cast<int>(std::ceil(maxx)));
                const int y0 = std::max(cy0, static_cast<int>(std::floor(miny))), y1 = std::min(cy1, static_cast<int>(std::ceil(maxy)));
                const float det = (v[1]->pos.y - v[2]->pos.y) * (v[0]->pos.x - v[2]->pos.x) + (v[2]->pos.x - v[1]->pos.x) * (v[0]->pos.y - v[2]->pos.y);
                if (std::fabs(det) < 1e-8f) continue;
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const float px = x + 0.5f, py = y + 0.5f;
                        const float l0 = ((v[1]->pos.y - v[2]->pos.y) * (px - v[2]->pos.x) + (v[2]->pos.x - v[1]->pos.x) * (py - v[2]->pos.y)) / det;
                        const float l1 = ((v[2]->pos.y - v[0]->pos.y) * (px - v[2]->pos.x) + (v[0]->pos.x - v[2]->pos.x) * (py - v[2]->pos.y)) / det;
                        const float l2 = 1 - l0 - l1;
                        if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
                        float t[4];
                        Sample(tex, tw, th, l0 * v[0]->uv.x + l1 * v[1]->uv.x + l2 * v[2]->uv.x, l0 * v[0]->uv.y + l1 * v[1]->uv.y + l2 * v[2]->uv.y, t);
                        float col[4];
                        for (int c = 0; c < 4; ++c) {
                            const int sh = c * 8;
                            col[c] = (l0 * ((v[0]->col >> sh) & 0xFF) + l1 * ((v[1]->col >> sh) & 0xFF) + l2 * ((v[2]->col >> sh) & 0xFF)) / 255.0f;
                        }
                        const float a = Clamp01(col[3] * t[3]);
                        float* d = &img.px[(static_cast<size_t>(y) * img.w + x) * 4];
                        for (int c = 0; c < 3; ++c) d[c] = col[c] * t[c] * a + d[c] * (1 - a);
                        d[3] = a + d[3] * (1 - a);
                    }
                }
            }
        }
    }
}

ImGuiKey KeyByName(const std::string& n) {
    if (n == "Up") return ImGuiKey_UpArrow;
    if (n == "Down") return ImGuiKey_DownArrow;
    if (n == "Left") return ImGuiKey_LeftArrow;
    if (n == "Right") return ImGuiKey_RightArrow;
    if (n == "Enter") return ImGuiKey_Enter;
    if (n == "Esc") return ImGuiKey_Escape;
    if (n == "E") return ImGuiKey_E;
    if (n == "Q") return ImGuiKey_Q;
    if (n == "Delete") return ImGuiKey_Delete;
    if (n == "PageDown") return ImGuiKey_PageDown;
    return ImGuiKey_None;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: ui_preview W H OUTPREFIX commands...\n");
        return 2;
    }
    const int W = std::atoi(argv[1]), H = std::atoi(argv[2]);
    const std::string prefix = argv[3];
    std::vector<std::string> cmds(argv + 4, argv + argc);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(W), static_cast<float>(H));
    game_ui::InstallFont();
    unsigned char* tex = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&tex, &tw, &th);
    settings_overlay::InitializeRuntimeSettings();
    settings_overlay::NotifyStrapInputAccepted();
    for (int i = 0; i < 400; ++i) settings_overlay::AdvancePresentedFrame();

    size_t next = 0;
    int waitFrames = 0;
    int holdFrames = 0;
    ImGuiKey heldKey = ImGuiKey_None;
    bool pendingShot = false;
    std::string shotLabel;
    bool pendingClick = false;
    for (int frame = 0; frame < 4000; ++frame) {
        io.DeltaTime = 1.0f / 60.0f;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // the menu's input suppression uses wall-clock time
        io.AddMousePosEvent(io.MousePos.x, io.MousePos.y);
        if (heldKey != ImGuiKey_None && holdFrames > 0 && --holdFrames == 0) {
            io.AddKeyEvent(heldKey, false);
            heldKey = ImGuiKey_None;
        }
        if (pendingClick) {
            io.AddMouseButtonEvent(0, false);
            pendingClick = false;
        }
        while (waitFrames == 0 && holdFrames == 0 && next < cmds.size() && !pendingShot) {
            const std::string c = cmds[next++];
            if (c == "open") settings_overlay::OpenFromGame();
            else if (c == "wait") waitFrames = std::atoi(cmds[next++].c_str());
            else if (c == "key") {
                heldKey = KeyByName(cmds[next++]);
                io.AddKeyEvent(heldKey, true);
                holdFrames = 2;
            } else if (c == "hold") {
                heldKey = KeyByName(cmds[next++]);
                io.AddKeyEvent(heldKey, true);
                holdFrames = std::atoi(cmds[next++].c_str());
            } else if (c == "shot") {
                shotLabel = cmds[next++];
                pendingShot = true;
            } else if (c == "mouse") {
                io.AddMousePosEvent(static_cast<float>(std::atof(cmds[next].c_str())), static_cast<float>(std::atof(cmds[next + 1].c_str())));
                next += 2;
            } else if (c == "click") {
                io.AddMouseButtonEvent(0, true);
                pendingClick = true;
                break;
            }
        }
        if (waitFrames > 0) --waitFrames;
        ImGui::NewFrame();
        if (std::getenv("UIP_DEBUG") && (ImGui::IsKeyPressed(ImGuiKey_E, false) || ImGui::IsKeyDown(ImGuiKey_E))) std::printf("frame %d E pressed=%d down=%d\n", frame, ImGui::IsKeyPressed(ImGuiKey_E, false), ImGui::IsKeyDown(ImGuiKey_E));
        settings_overlay::Draw();
        ImGui::Render();
        if (pendingShot && waitFrames == 0) {
            Image img;
            img.w = W;
            img.h = H;
            img.px.assign(static_cast<size_t>(W) * H * 4, 0.0f);
            Raster(img, ImGui::GetDrawData(), tex, tw, th);
            const std::string path = prefix + "_" + shotLabel + ".rgba";
            FILE* f = std::fopen(path.c_str(), "wb");
            std::fwrite(&W, 4, 1, f);
            std::fwrite(&H, 4, 1, f);
            for (float v : img.px) {
                const unsigned char b = static_cast<unsigned char>(Clamp01(v) * 255.0f + 0.5f);
                std::fwrite(&b, 1, 1, f);
            }
            std::fclose(f);
            std::printf("wrote %s\n", path.c_str());
            pendingShot = false;
        }
        if (next >= cmds.size() && waitFrames == 0 && !pendingShot && holdFrames == 0) break;
    }
    return 0;
}
