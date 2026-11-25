// First-run launcher UI. See first_run_launcher.h.

#include "first_run_launcher.h"

#include "aurora_events.h"
#include "disc_import.h"
#include "game_ui.h"
#include "guest_abi/guest_region.h"
#include "runtime_config.h"

#include <SDL3/SDL_video.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#endif

namespace fs = std::filesystem;

namespace first_run_launcher {

bool JapanBuild() { return std::string_view(GuestRegion::CurrentFacts().gameId) == "R2VJ01"; }

const char* ManagedGameDirectoryName() { return JapanBuild() ? "Game-R2VJ01" : "Game"; }

fs::path ManagedGameDirectory() { return RuntimeConfigFile::ApplicationDataDirectory() / ManagedGameDirectoryName(); }

namespace {

bool LooksLikeGameRoot(const fs::path& root) {
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root / "files", ec) || !fs::is_regular_file(root / "sys" / "fst.bin", ec) ||
        !fs::is_regular_file(root / "sys" / "main.dol", ec)) {
        return false;
    }
    // Another region or game's extracted data is not usable by this regional
    // executable; treat it as missing so the player is asked for the matching disc.
    if (RuntimeConfigFile::DiscGameId(root) != GuestRegion::CurrentFacts().gameId) return false;
    return !fs::is_regular_file(root / "sys" / "boot.bin", ec) || RuntimeConfigFile::IsThisGamesDiscRoot(root);
}

void PointConfigAtManagedGame() {
    RuntimeConfigFile::Mutable().dvdRoot = ManagedGameDirectoryName();
    RuntimeConfigFile::WriteSetting("paths", "dvd_root", RuntimeConfigFile::FormatString(ManagedGameDirectoryName()));
}

}  // namespace

bool GameDataAvailable() {
    if (LooksLikeGameRoot(RuntimeConfigFile::ResolvedDvdRoot())) return true;
    // Imported earlier but the config lost its path: adopt the managed copy instead of asking again.
    if (RuntimeConfigFile::DvdRoot().empty() && LooksLikeGameRoot(ManagedGameDirectory())) {
        PointConfigAtManagedGame();
        return true;
    }
    return false;
}

#ifndef _WIN32
bool Run(PrepareFolder) { return false; }
#else

namespace {

using game_ui::Px;

enum class Phase { Idle, Importing, Done, Failed, NeedKey };

// A disc image or extracted folder found without asking.
struct FoundGame {
    fs::path path;
    std::string label;   // file name shown to the player
    std::string format;  // "RVZ", "WBFS", "Folder", ...
    bool folder = false;
};

struct Launcher {
    Phase phase = Phase::Idle;
    int focus = 0;

    // background search for discs in the usual places
    std::thread scanThread;
    std::atomic<bool> scanDone{false};
    std::vector<FoundGame> found;  // valid once scanDone
    size_t foundIndex = 0;

    // file dialog thread
    std::thread dialogThread;
    std::atomic<int> dialogState{0};  // 0 idle, 1 open, 2 finished
    std::mutex lock;
    fs::path picked, pickedKeys, currentImage;

    // import thread
    std::thread importThread;
    DiscImport::Progress progress;
    DiscImport::Summary summary;
    std::atomic<int> importState{0};  // 0 running, 1 ok, 2 failed, 3 cancelled, 4 needs key
    std::string error;
    std::chrono::steady_clock::time_point importStart;
    std::string imageName;

    bool dropHover = false;
    fs::path dropped;
};

HWND WindowHandle() {
    SDL_Window* window = aurora_get_window();
    if (!window) return nullptr;
    return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

fs::path PickFile(HWND owner, const wchar_t* title, const wchar_t* label, const wchar_t* pattern) {
    fs::path result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                   reinterpret_cast<void**>(&dialog))) && dialog) {
        const COMDLG_FILTERSPEC filters[] = {
            {label, pattern},
            {L"All files", L"*.*"},
        };
        if (pattern) dialog->SetFileTypes(2, filters);
        dialog->SetTitle(title);
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (pattern ? FOS_FILEMUSTEXIST : FOS_PICKFOLDERS));
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR text = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &text)) && text) {
                    result = fs::path(text);
                    CoTaskMemFree(text);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

// The dialogs run on a worker so the first-run screen stays visible.
enum class Picker { Disc, Folder, Key };
void PickInput(Launcher& launcher, HWND owner, Picker picker) {
    launcher.dialogState = 1;
    launcher.dialogThread = std::thread([&launcher, owner, picker] {
        if (picker == Picker::Disc) {
            launcher.picked = PickFile(owner, L"Select your Wii disc image", L"Wii disc images",
                                       L"*.iso;*.wbfs;*.rvz;*.wia;*.chd;*.ciso;*.cso;*.gcz;*.gcm;*.cue;*.bin;*.img;*.nkit;*.wbf1;*.001;*.part0.iso");
        } else if (picker == Picker::Folder) {
            launcher.picked = PickFile(owner, L"Select the game folder containing sys and files", nullptr, nullptr);
        } else {
            launcher.pickedKeys = PickFile(owner, L"Select a file containing your Wii key", L"Wii key files",
                                           L"*.bin;*.key;*.keys;*.hex;*.txt;*.dat;*.json;*.ini;*.cfg;*.xml;*.csv;*.h;*.7z;*.zip;*.rar;*.gz;*.xz;*.bz2;*.zst;*.tar;*.rvz;*.wia;common-key");
            if (!launcher.pickedKeys.empty()) launcher.picked = launcher.currentImage;
        }
        launcher.dialogState = 2;
    });
}

void JoinFinished(std::thread& t) {
    if (t.joinable()) t.join();
}

fs::path ExecutableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) return fs::path(buffer.substr(0, n)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
}

// Looks for the game next to the executable, in rom/roms/games folders beside it
// or its parents, in the working directory and in Downloads. Only file headers
// are read; nothing is decrypted or copied.
std::vector<FoundGame> FindGames() {
    std::vector<fs::path> dirs;
    auto addDir = [&](const fs::path& dir) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) return;
        const auto canonical = fs::weakly_canonical(dir, ec);
        if (std::find(dirs.begin(), dirs.end(), canonical) == dirs.end()) dirs.push_back(canonical);
    };
    auto addWithSubfolders = [&](const fs::path& base) {
        addDir(base);
        for (const char* sub : {"rom", "roms", "ROM", "ROMs", "Roms", "games", "Games", "iso", "ISO", "wii", "Wii", "disc", "Disc"})
            addDir(base / sub);
    };
    fs::path at = ExecutableDirectory();
    for (int level = 0; level < 4 && !at.empty(); ++level) {
        addWithSubfolders(at);
        const fs::path parent = at.parent_path();
        if (parent == at) break;
        at = parent;
    }
    std::error_code ec;
    addWithSubfolders(fs::current_path(ec));
    if (const wchar_t* profile = _wgetenv(L"USERPROFILE")) addDir(fs::path(profile) / "Downloads");

    std::vector<FoundGame> games;
    unsigned probed = 0;
    for (const auto& dir : dirs) {
        std::error_code it_ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, it_ec), end; !it_ec && it != end;
             it.increment(it_ec)) {
            std::error_code fileEc;
            const fs::path& path = it->path();
            if (it->is_directory(fileEc)) {
                // Dolphin extracts the game into <folder>/DATA (next to UPDATE).
                if (LooksLikeGameRoot(path)) games.push_back({path, path.filename().string(), "Folder", true});
                else if (LooksLikeGameRoot(path / "DATA")) games.push_back({path / "DATA", path.filename().string() + "/DATA", "Folder", true});
                continue;
            }
            if (!it->is_regular_file(fileEc) || !DiscImport::IsSupportedDiscExtension(path)) continue;
            // Wii images are hundreds of megabytes; skip small .bin files and similar.
            const std::string ext = path.extension().string();
            if (ext != ".cue" && it->file_size(fileEc) < (64ull << 20)) continue;
            if (++probed > 48) break;
            DiscImport::DiscInfo info;
            std::string error;
            if (DiscImport::ProbeDisc(path, info, error) && info.supported &&
                info.gameId == GuestRegion::CurrentFacts().gameId)
                games.push_back({path, path.filename().string(), info.format, false});
        }
    }
    // Extracted folders and RVZ/WIA (normally keyless) first.
    auto rank = [](const FoundGame& g) {
        if (g.folder) return 0;
        if (g.format == "RVZ" || g.format == "WIA") return 1;
        return 2;
    };
    std::stable_sort(games.begin(), games.end(), [&](const FoundGame& a, const FoundGame& b) { return rank(a) < rank(b); });
    return games;
}

void StartImport(Launcher& l, const fs::path& image, const fs::path& keys) {
    JoinFinished(l.importThread);
    std::string selectedId;
    std::error_code selectedEc;
    if (fs::is_directory(image, selectedEc)) {
        selectedId = RuntimeConfigFile::DiscGameId(image);
    } else {
        DiscImport::DiscInfo selectedInfo;
        std::string selectedError;
        if (DiscImport::ProbeDisc(image, selectedInfo, selectedError)) selectedId = selectedInfo.gameId;
    }
    if (!selectedId.empty() && selectedId != GuestRegion::CurrentFacts().gameId) {
        l.error = "This is " + selectedId + "; use the matching regional executable.";
        l.phase = Phase::Failed;
        return;
    }
    l.progress.cancel = false;
    l.progress.done = 0;
    l.progress.total = 1;
    l.summary = {};
    l.error.clear();
    l.currentImage = image;
    l.imageName = image.filename().string();
    l.importState = 0;
    l.importStart = std::chrono::steady_clock::now();
    l.phase = Phase::Importing;
    l.focus = 0;
    game_ui::Suppress(0.5f);
    l.importThread = std::thread([&l, image, keys] {
        std::string error;
        const auto result = DiscImport::ImportDisc(image, ManagedGameDirectory(), l.progress, l.summary, error, keys);
        {
            std::lock_guard<std::mutex> guard(l.lock);
            l.error = error;
        }
        l.importState = result == DiscImport::Result::Ok ? 1 : result == DiscImport::Result::Cancelled ? 3 :
                        result == DiscImport::Result::KeyRequired ? 4 : 2;
    });
}

std::string Megabytes(uint64_t bytes) {
    char text[48];
    std::snprintf(text, sizeof text, "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// Returns true when the button was activated by mouse. `hot` highlights it for keyboard / pad focus.
bool Button(ImDrawList* dl, game_ui::Frame& in, ImVec2 centre, float widthU, const char* label, bool focused, bool primary) {
    const ImVec2 half(Px(widthU) * 0.5f, Px(40.0f));
    const ImVec2 mn(centre.x - half.x, centre.y - half.y), mx(centre.x + half.x, centre.y + half.y);
    const bool hover = in.mouseValid && game_ui::PointIn(in.mouse, mn, mx);
    const bool hot = hover || focused;
    const ImU32 fill = primary ? (hot ? IM_COL32(74, 156, 255, 255) : IM_COL32(36, 112, 226, 255))
                               : (hot ? IM_COL32(120, 150, 186, 255) : IM_COL32(84, 114, 150, 255));
    game_ui::DrawPill(dl, mn, mx, fill, IM_COL32(244, 249, 253, 255), 1.0f, std::max(2.0f, Px(3.0f)));
    game_ui::TextCentered(dl, 30.0f, centre, game_ui::kWhite, label);
    return hover && in.mouseClicked;
}

void DrawBackground(ImDrawList* dl, const ImVec2& size) {
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(14, 28, 58, 255), IM_COL32(14, 28, 58, 255), IM_COL32(4, 8, 20, 255),
                                IM_COL32(4, 8, 20, 255));
}

}  // namespace

bool Run(PrepareFolder prepareFolder) {
    Launcher l;

    // The default 854x480 window renders the 1080p-based UI far too small; use 720p while the launcher is up.
    SDL_Window* window = aurora_get_window();
    int originalW = 0, originalH = 0;
    bool resized = false;
    if (window && aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED &&
        !(SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        SDL_GetWindowSize(window, &originalW, &originalH);
        if (originalH < 720) {
            SDL_SetWindowSize(window, 1280, 720);
            resized = true;
        }
    }

    game_ui::Suppress(0.4f);
    bool finished = false;

    // QA hooks: SIN_LAUNCHER_DROP=a;b;c feeds the paths through the same path as a window drop (one every SIN_LAUNCHER_DROP_GAP
    // seconds while no import runs); SIN_LAUNCHER_AUTOPLAY=1 presses Play when the import is done.
    std::vector<fs::path> hookDrops;
    if (const char* env = std::getenv("SIN_LAUNCHER_DROP")) {
        const std::string all = env;
        for (size_t at = 0; at <= all.size();) {
            const size_t semi = all.find(';', at);
            const std::string item = all.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
            if (!item.empty()) hookDrops.push_back(RuntimeConfigFile::PathFromUtf8(item));
            if (semi == std::string::npos) break;
            at = semi + 1;
        }
    }
    const char* gapEnv = std::getenv("SIN_LAUNCHER_DROP_GAP");
    const double hookGap = gapEnv ? std::atof(gapEnv) : 3.0;
    const bool hookAutoplay = std::getenv("SIN_LAUNCHER_AUTOPLAY") != nullptr;
    const char* cancelEnv = std::getenv("SIN_LAUNCHER_CANCEL_AFTER_MS");
    const double cancelAfterMs = cancelEnv ? std::atof(cancelEnv) : 0.0;
    size_t hookNext = 0;
    auto hookClock = std::chrono::steady_clock::now();

    auto stopWorkers = [&] {
        l.progress.cancel = true;
        JoinFinished(l.importThread);
        JoinFinished(l.dialogThread);
        JoinFinished(l.scanThread);
    };
    if (!std::getenv("SIN_LAUNCHER_NO_SCAN")) {
        l.scanThread = std::thread([&l] {
            auto games = FindGames();
            std::lock_guard<std::mutex> guard(l.lock);
            l.found = std::move(games);
            l.scanDone = true;
        });
    } else {
        l.scanDone = true;
    }

    while (!finished) {
        // ---- events
        const AuroraEvent* events = aurora_update();
        game_ui::InstallFont();
        for (const AuroraEvent* ev = events; ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_EXIT) {
                // Give an import in flight a moment to remove its half-written files.
                l.progress.cancel = true;
                for (int i = 0; i < 60 && l.importState.load() == 0 && l.phase == Phase::Importing; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                ExitForAuroraWindowClose();
            }
            if (ev->type != AURORA_SDL_EVENT) continue;
            switch (ev->sdl.type) {
            case SDL_EVENT_DROP_BEGIN: l.dropHover = true; break;
            case SDL_EVENT_DROP_COMPLETE: l.dropHover = false; break;
            case SDL_EVENT_DROP_FILE:
                l.dropHover = false;
                if (ev->sdl.drop.data) l.dropped = RuntimeConfigFile::PathFromUtf8(ev->sdl.drop.data);
                break;
            default: break;
            }
        }

        if (hookNext < hookDrops.size() && l.phase != Phase::Importing && l.phase != Phase::Done &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - hookClock).count() >= hookGap) {
            l.dropped = hookDrops[hookNext++];
            hookClock = std::chrono::steady_clock::now();
        }
        if (cancelAfterMs > 0 && l.phase == Phase::Importing &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - l.importStart).count() >= cancelAfterMs) {
            l.progress.cancel = true;
        }
        if (hookAutoplay && l.phase == Phase::Done &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - hookClock).count() >= hookGap) {
            finished = true;
        }

        // ---- state transitions
        if (l.dialogState.load() == 2) {
            JoinFinished(l.dialogThread);
            l.dialogState = 0;
            if (!l.picked.empty()) {
                std::error_code ec;
                if (fs::is_directory(l.picked, ec)) {
                    if (!LooksLikeGameRoot(l.picked) && LooksLikeGameRoot(l.picked / "DATA")) l.picked /= "DATA";
                    finished = prepareFolder(l.picked, l.error);
                    if (!finished) l.phase = Phase::Failed;
                } else {
                    StartImport(l, l.picked, l.pickedKeys);
                }
            }
            l.picked.clear();
            l.pickedKeys.clear();
            if (finished) break;
        }
        if (!l.dropped.empty()) {
            auto file = std::move(l.dropped);
            l.dropped.clear();
            if (l.dialogState.load() == 0 && l.phase != Phase::Importing && l.phase != Phase::Done) {
                // A file dropped on the key screen is the key for the current disc.
                if (l.phase == Phase::NeedKey && !DiscImport::IsSupportedDiscExtension(file)) {
                    l.pickedKeys = std::move(file);
                    l.picked = l.currentImage;
                } else {
                    l.picked = std::move(file);
                }
                l.dialogState = 2;
            }
        }
        if (l.phase == Phase::Importing && l.importState.load() != 0) {
            JoinFinished(l.importThread);
            const int state = l.importState.load();
            if (state == 1) {
                PointConfigAtManagedGame();
                l.phase = Phase::Done;
                hookClock = std::chrono::steady_clock::now();
            } else if (state == 4) {
                l.phase = Phase::NeedKey;
            } else {
                l.phase = Phase::Failed;
                if (state == 3) l.phase = Phase::Idle;
            }
            l.focus = 0;
            game_ui::Suppress(0.3f);
        }

        // ---- frame
        if (!BeginAuroraFrame()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        game_ui::Frame& in = game_ui::PollInput();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        DrawBackground(dl, size);

        const float cx = size.x * 0.5f;
        const float panelW = std::min(Px(1100.0f), size.x - Px(60.0f)), panelH = Px(640.0f);
        const float top = std::max(Px(20.0f), (size.y - panelH) * 0.5f - Px(20.0f));
        const ImVec2 pmin(cx - panelW * 0.5f, top), pmax(cx + panelW * 0.5f, top + panelH);
        game_ui::DrawGlassPanel(dl, pmin, pmax, 1.0f);
        if (l.dropHover) dl->AddRect(pmin, pmax, game_ui::kTitleYellow, Px(40.0f), 0, std::max(3.0f, Px(5.0f)));

        game_ui::TextCentered(dl, 76.0f, ImVec2(cx, top + Px(95.0f)), game_ui::kTitleYellow, "SIN & PUNISHMENT");
        game_ui::TextCentered(dl, 40.0f, ImVec2(cx, top + Px(165.0f)), game_ui::kWhite, "STAR SUCCESSOR");

        const float bodyX = pmin.x + Px(90.0f), bodyW = panelW - Px(180.0f), bodyY = top + Px(225.0f);
        const float btnY = top + panelH - Px(85.0f);
        bool primary = false, secondary = false;

        if (l.phase == Phase::Idle || l.phase == Phase::Failed) {
            // Discs found in the usual places become a one-press choice.
            std::vector<FoundGame> found;
            if (l.scanDone.load()) {
                std::lock_guard<std::mutex> guard(l.lock);
                found = l.found;
            }
            if (!found.empty()) l.foundIndex %= found.size();
            const FoundGame* pick = found.empty() ? nullptr : &found[l.foundIndex];
            if (l.phase == Phase::Failed) {
                std::string message;
                {
                    std::lock_guard<std::mutex> guard(l.lock);
                    message = l.error;
                }
                game_ui::DrawWrapped(dl, 28.0f, ImVec2(bodyX, bodyY), bodyW, game_ui::kBad, "Import failed", game_ui::Outline::Thin, 1.0f, true);
                game_ui::DrawWrapped(dl, pick ? 22.0f : 26.0f, ImVec2(bodyX, bodyY + Px(45.0f)), bodyW, game_ui::kWhite, message,
                                     game_ui::Outline::Thin, 1.0f, true);
            } else if (pick) {
                game_ui::DrawWrapped(dl, 30.0f, ImVec2(bodyX, bodyY), bodyW, game_ui::kWhite, "Welcome! Your game was found:",
                                     game_ui::Outline::Thin, 1.0f, true);
                std::string line = pick->label + "  (" + pick->format + ")";
                if (found.size() > 1) line = "<  " + line + "  >";
                game_ui::TextCentered(dl, 26.0f, ImVec2(cx, bodyY + Px(70.0f)), game_ui::kTitleYellow,
                                      game_ui::Ellipsize(line, 26.0f, bodyW, true).c_str(), game_ui::Outline::Thin);
                if (found.size() > 1) {
                    char more[96];
                    std::snprintf(more, sizeof more, "%zu of %zu found. Press left or right to choose another.", l.foundIndex + 1, found.size());
                    game_ui::TextCentered(dl, 20.0f, ImVec2(cx, bodyY + Px(104.0f)), game_ui::kTextDim, more, game_ui::Outline::Thin);
                }
            } else {
                game_ui::DrawWrapped(dl, 30.0f, ImVec2(bodyX, bodyY), bodyW, game_ui::kWhite,
                                     "Welcome! To get started, choose your North American Star Successor disc image or extracted game folder.",
                                     game_ui::Outline::Thin, 1.0f, true);
                game_ui::DrawWrapped(dl, 24.0f, ImVec2(bodyX, bodyY + Px(105.0f)), bodyW, game_ui::kTextDim,
                                     l.scanDone.load()
                                         ? "ISO, WBFS, RVZ, WIA, CHD, CISO, GCZ, split and CUE/BIN images work. Keys next to the disc are found automatically."
                                         : "Looking for your game...",
                                     game_ui::Outline::Thin, 1.0f, true);
            }
            // A dialog may finish during this frame. Consume its result and
            // join the worker before accepting another activation.
            const bool busy = l.dialogState.load() != 0;
            const int buttons = pick ? 3 : 2;
            if (l.focus >= buttons) l.focus = 0;
            if (!busy && in.up) l.focus = (l.focus + buttons - 1) % buttons;
            if (!busy && in.down) l.focus = (l.focus + 1) % buttons;
            if (!busy && pick && found.size() > 1 && (in.left || in.right)) {
                l.foundIndex = (l.foundIndex + (in.left ? found.size() - 1 : 1)) % found.size();
                l.focus = 0;
            }
            int activated = -1;
            float y = btnY - Px(80.0f) * float(buttons - 1);
            if (pick) {
                const std::string label = busy ? "Waiting for file dialog..." : "Use this " + std::string(pick->folder ? "folder" : "disc");
                if (Button(dl, in, ImVec2(cx, y), 560.0f, label.c_str(), l.focus == 0, true)) activated = 0;
                y += Px(80.0f);
            }
            const int discButton = pick ? 1 : 0;
            const char* discLabel = busy && !pick ? "Waiting for file dialog..."
                                    : l.phase == Phase::Failed ? "Select another disc image"
                                    : pick                     ? "Select a different disc image"
                                                               : "Select disc image";
            if (Button(dl, in, ImVec2(cx, y), 560.0f, discLabel, l.focus == discButton, !pick)) activated = discButton;
            y += Px(80.0f);
            if (Button(dl, in, ImVec2(cx, y), 560.0f, "Select extracted folder", l.focus == discButton + 1, false)) activated = discButton + 1;
            if (activated < 0 && in.accept) activated = l.focus;
            if (!busy && activated >= 0) {
                if (pick && activated == 0) {
                    l.picked = pick->path;
                    l.pickedKeys.clear();
                    l.dialogState = 2;
                } else {
                    PickInput(l, WindowHandle(), activated == discButton ? Picker::Disc : Picker::Folder);
                }
            }
        } else if (l.phase == Phase::NeedKey) {
            std::string message;
            {
                std::lock_guard<std::mutex> guard(l.lock);
                message = l.error;
            }
            game_ui::DrawWrapped(dl, 28.0f, ImVec2(bodyX, bodyY), bodyW, game_ui::kTitleYellow, "Wii key needed", game_ui::Outline::Thin, 1.0f, true);
            game_ui::DrawWrapped(dl, 20.0f, ImVec2(bodyX, bodyY + Px(42.0f)), bodyW, game_ui::kWhite, message, game_ui::Outline::Thin, 1.0f, false);
            const bool busy = l.dialogState.load() != 0;
            if (!busy && (in.up || in.down)) l.focus = 1 - l.focus;
            primary = Button(dl, in, ImVec2(cx, btnY - Px(80.0f)), 560.0f, busy ? "Waiting for file dialog..." : "Select key file", l.focus == 0, true);
            secondary = Button(dl, in, ImVec2(cx, btnY), 560.0f, "Choose a different disc", l.focus == 1, false);
            if (!busy && in.back) {
                l.phase = Phase::Idle;
                l.focus = 0;
            } else if (!busy && (primary || secondary || in.accept)) {
                if (secondary || (!primary && l.focus == 1)) {
                    l.phase = Phase::Idle;
                    l.focus = 0;
                } else {
                    PickInput(l, WindowHandle(), Picker::Key);
                }
            }
        } else if (l.phase == Phase::Importing) {
            std::string stage, detail;
            l.progress.Get(stage, detail);
            const uint64_t done = l.progress.done.load(), total = std::max<uint64_t>(1, l.progress.total.load());
            const float fraction = std::clamp(static_cast<float>(static_cast<double>(done) / static_cast<double>(total)), 0.0f, 1.0f);
            game_ui::TextCentered(dl, 32.0f, ImVec2(cx, bodyY + Px(10.0f)), game_ui::kWhite, (stage.empty() ? "Starting..." : stage + "...").c_str());
            if (!detail.empty()) {
                game_ui::TextCentered(dl, 22.0f, ImVec2(cx, bodyY + Px(60.0f)), game_ui::kTextDim,
                                      game_ui::Ellipsize(detail, 22.0f, bodyW, true).c_str(), game_ui::Outline::Thin);
            }
            const ImVec2 bmin(bodyX, bodyY + Px(110.0f)), bmax(bodyX + bodyW, bodyY + Px(150.0f));
            game_ui::DrawPill(dl, bmin, bmax, IM_COL32(10, 30, 66, 230), IM_COL32(244, 249, 253, 255), 1.0f, std::max(2.0f, Px(3.0f)));
            const float fillW = (bmax.x - bmin.x - Px(8.0f)) * fraction;
            if (fillW > Px(20.0f)) {
                game_ui::DrawPill(dl, ImVec2(bmin.x + Px(4.0f), bmin.y + Px(4.0f)), ImVec2(bmin.x + Px(4.0f) + fillW, bmax.y - Px(4.0f)),
                                  IM_COL32(74, 190, 255, 255), IM_COL32(0, 0, 0, 0), 1.0f);
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - l.importStart).count();
            char info[128];
            std::snprintf(info, sizeof info, "%.0f%%   %s of %s   %.0f MB/s", fraction * 100.0, Megabytes(done).c_str(), Megabytes(total).c_str(),
                          seconds > 0.5 ? static_cast<double>(done) / (1024.0 * 1024.0) / seconds : 0.0);
            game_ui::TextCentered(dl, 24.0f, ImVec2(cx, bodyY + Px(190.0f)), game_ui::kTextNormal, info, game_ui::Outline::Thin);
            secondary = Button(dl, in, ImVec2(cx, btnY), 320.0f, "Cancel", l.focus == 0, false);
            if (in.accept || in.back) secondary = true;
            if (secondary) l.progress.cancel = true;
        } else {  // Done
            const DiscImport::Summary& s = l.summary;
            game_ui::TextCentered(dl, 34.0f, ImVec2(cx, bodyY + Px(10.0f)), game_ui::kGood, "Import complete");
            char text[256];
            std::snprintf(text, sizeof text, "%u game files (%s) and %u stage modules prepared in %.0f seconds.", s.files,
                          Megabytes(s.bytes).c_str(), s.modules, s.seconds);
            std::string done = text;
            if (!s.keySource.empty()) done += "\nWii key: " + (s.keySource.rfind("not needed", 0) == 0 ? s.keySource : "found in " + s.keySource);
            game_ui::DrawWrapped(dl, 26.0f, ImVec2(bodyX, bodyY + Px(55.0f)), bodyW, game_ui::kWhite, done, game_ui::Outline::Thin, 1.0f, true);
            game_ui::DrawWrapped(dl, 24.0f, ImVec2(bodyX, bodyY + Px(175.0f)), bodyW, game_ui::kTextDim,
                                 "Your disc image is no longer needed. Game data lives in: " + RuntimeConfigFile::PathToUtf8(ManagedGameDirectory()),
                                 game_ui::Outline::Thin, 1.0f, true);
            primary = Button(dl, in, ImVec2(cx, btnY), 420.0f, "Play", l.focus == 0, true);
            if (in.accept) primary = true;
            if (primary) finished = true;
        }

        std::vector<std::pair<std::string, std::string>> hints;
        if (l.phase == Phase::Importing) hints = {{"B", "Cancel"}};
        else if (l.phase == Phase::Done) hints = {{"A", "Play"}};
        else if (l.phase == Phase::NeedKey) hints = {{"A", "Select"}, {"B", "Back"}};
        else hints = {{"A", "Select"}};
        game_ui::DrawHints(dl, ImVec2(cx, pmax.y + Px(70.0f)), hints, 1.0f, 1);

        EndAuroraFrame();
    }

    stopWorkers();
    if (resized && window) {
        SDL_SetWindowSize(window, originalW, originalH);
    }
    return true;
}

#endif  // _WIN32

}  // namespace first_run_launcher
