#include "overlay_app.hpp"
#include "draw_esp.hpp"
#include "diagnostics.hpp"
#include "projection.hpp"
#include "resource.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cwctype>

// Dear ImGui intentionally leaves this declaration disabled in its Win32
// backend header to avoid including windows.h from that header.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace esp {
namespace {
// Avoid magenta: it is a common user-selected ESP color and must not be keyed
// out as transparent. The tiny near-black key is reserved for the clear pass.

struct GameWindowSearch {
    DWORD targetPid = 0;
    HWND found = nullptr;
};

ImFont* loadUiFont(ImGuiIO& io) {
    struct FontSource { const char* path; const char* label; bool merge; };
    // ImGui's embedded default font only contains a small Latin set. Names
    // arrive from the game as UTF-8, so load Windows' script-capable fonts and
    // merge their glyphs into one font. The DX11 backend supports ImGui 1.92's
    // dynamic glyph texture path, avoiding enormous eager CJK atlases.
    constexpr FontSource sources[] = {
        {"C:/Windows/Fonts/segoeui.ttf", "Segoe UI", false},
        {"C:/Windows/Fonts/arial.ttf", "Arial", true},
        {"C:/Windows/Fonts/tahoma.ttf", "Tahoma", true},
        {"C:/Windows/Fonts/Nirmala.ttc", "Nirmala UI", true},
        {"C:/Windows/Fonts/seguisym.ttf", "Segoe UI Symbol", true},
        {"C:/Windows/Fonts/msyh.ttc", "Microsoft YaHei", true},
        {"C:/Windows/Fonts/mingliu.ttc", "MingLiU", true},
        {"C:/Windows/Fonts/simsun.ttc", "SimSun", true},
        {"C:/Windows/Fonts/malgun.ttf", "Malgun Gothic", true},
        {"C:/Windows/Fonts/msgothic.ttc", "MS Gothic", true},
    };
    ImFont* primary = nullptr;
    for (const FontSource& source : sources) {
        ImFontConfig config{};
        config.MergeMode = source.merge;
        config.FontNo = 0;
        config.OversampleH = 1;
        config.OversampleV = 1;
        ImFont* loaded = io.Fonts->AddFontFromFileTTF(source.path, 17.0f, &config);
        if (!loaded) {
            Diagnostics::instance().write("WARN", "fonts", std::string("could not load ") + source.label + " from " + source.path);
            continue;
        }
        if (!primary) primary = loaded;
        Diagnostics::instance().write("INFO", "fonts", std::string("loaded glyph source: ") + source.label);
    }
    if (!primary) {
        primary = io.Fonts->AddFontDefault();
        Diagnostics::instance().write("WARN", "fonts", "using ImGui embedded font; non-Latin glyph coverage is limited");
    }
    io.FontDefault = primary;
    return primary;
}

void colorEdit(const char* label, std::array<float, 4>& value) {
    ImGui::ColorEdit4(label, value.data(), ImGuiColorEditFlags_AlphaBar);
}

std::filesystem::path uiPreferencesPath() {
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return L"mcp-esp-ui.ini";
    return std::filesystem::path(executable).parent_path() / L"mcp-esp-ui.ini";
}

void loadUiPreferences(Settings& settings) {
    std::ifstream input(uiPreferencesPath());
    std::string key;
    int value = settings.guiTheme;
    while (input >> key >> value) {
        if (key == "theme") settings.guiTheme = std::clamp(value, 0, 1);
    }
}

void saveUiPreferences(const Settings& settings) {
    std::ofstream output(uiPreferencesPath(), std::ios::trunc);
    if (output) output << "theme " << std::clamp(settings.guiTheme, 0, 1) << '\n';
}

void applyGuiTheme(int theme) {
    if (theme == 0) {
        ImGui::StyleColorsDark();
        return;
    }
    ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.08f, 0.14f, 0.24f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.40f, 0.48f, 0.59f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.94f, 0.97f, 1.00f, 0.94f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.91f, 0.95f, 0.99f, 0.80f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.98f, 0.99f, 1.00f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.55f, 0.68f, 0.83f, 0.75f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.84f, 0.90f, 0.97f, 0.88f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.72f, 0.83f, 0.96f, 0.95f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.61f, 0.76f, 0.94f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.78f, 0.86f, 0.96f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.18f, 0.39f, 0.68f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.78f, 0.86f, 0.96f, 0.80f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.86f, 0.91f, 0.97f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.86f, 0.91f, 0.97f, 0.65f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.49f, 0.64f, 0.83f, 0.82f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.32f, 0.52f, 0.75f, 0.95f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.20f, 0.41f, 0.67f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.08f, 0.39f, 0.78f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.18f, 0.48f, 0.82f, 0.90f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.08f, 0.34f, 0.70f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.20f, 0.46f, 0.78f, 0.78f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.16f, 0.42f, 0.76f, 0.95f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.10f, 0.32f, 0.63f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.31f, 0.53f, 0.79f, 0.52f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.24f, 0.48f, 0.78f, 0.78f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.16f, 0.39f, 0.70f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.47f, 0.61f, 0.78f, 0.65f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.18f, 0.48f, 0.82f, 0.85f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.10f, 0.38f, 0.75f, 1.00f);
    style.WindowRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 5.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 5.0f;
    style.WindowBorderSize = 1.0f;
}
}

OverlayApp::OverlayApp(SnapshotPoller& poller) : poller_(poller) {}

OverlayApp::~OverlayApp() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    if (target_) target_->Release();
    if (swapChain_) swapChain_->Release();
    if (compositionVisual_) compositionVisual_->Release();
    if (compositionTarget_) compositionTarget_->Release();
    if (compositionDevice_) compositionDevice_->Release();
    if (context_) context_->Release();
    if (device_) device_->Release();
}

LRESULT CALLBACK OverlayApp::windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<OverlayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<OverlayApp*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self && ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (self && msg == WM_SIZE && wp != SIZE_MINIMIZED) {
        self->resize(LOWORD(lp), HIWORD(lp));
        return 0;
    }
    if (self && msg == WM_NCHITTEST) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &point);
        for (const RECT& rect : self->guiHitRects_) {
            if (PtInRect(&rect, point)) {
                const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                if (ex & WS_EX_TRANSPARENT)
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
                return HTCLIENT;
            }
        }
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (!(ex & WS_EX_TRANSPARENT))
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
        return HTTRANSPARENT;
    }
    // The overlay may receive clicks over its control panel, but never take
    // foreground focus away from the game window.
    if (self && msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL CALLBACK OverlayApp::findGameWindowProc(HWND hwnd, LPARAM parameter) {
    auto* search = reinterpret_cast<GameWindowSearch*>(parameter);
    if (!search || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || (search->targetPid && pid != search->targetPid)) return TRUE;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;
    wchar_t path[MAX_PATH]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const BOOL readPath = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    if (!readPath) return TRUE;
    const wchar_t* basename = wcsrchr(path, L'\\');
    basename = basename ? basename + 1 : path;
    std::wstring name(basename);
    for (wchar_t& ch : name) ch = static_cast<wchar_t>(std::towlower(ch));
    if (name == L"minigameapp.exe") {
        search->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND OverlayApp::findGameWindow(const WorldSnapshot& snapshot) const {
    GameWindowSearch search;
    search.targetPid = snapshot.live.processId;
    EnumWindows(&OverlayApp::findGameWindowProc, reinterpret_cast<LPARAM>(&search));
    if (search.found) return search.found;
    search.targetPid = 0;
    EnumWindows(&OverlayApp::findGameWindowProc, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

void OverlayApp::syncToGameWindow(const WorldSnapshot& snapshot) {
    HWND candidate = gameHwnd_;
    if (candidate && IsWindow(candidate) && snapshot.live.processId) {
        DWORD currentPid = 0;
        GetWindowThreadProcessId(candidate, &currentPid);
        if (currentPid != snapshot.live.processId) candidate = nullptr;
    }
    if (!candidate || !IsWindow(candidate)) candidate = findGameWindow(snapshot);
    if (candidate != gameHwnd_) {
        gameHwnd_ = candidate;
        haveGameClientRect_ = false;
        if (gameHwnd_) {
            Diagnostics::instance().write("INFO", "window", "attached overlay to MiniGameApp.exe HWND=" +
                std::to_string(reinterpret_cast<std::uintptr_t>(gameHwnd_)));
        }
    }
    if (!gameHwnd_ || !IsWindow(gameHwnd_) || IsIconic(gameHwnd_) || !IsWindowVisible(gameHwnd_)) {
        if (IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
        if (!warnedGameWindowMissing_) {
            Diagnostics::instance().write("WARN", "window", "game window unavailable/minimized; overlay hidden");
            warnedGameWindowMissing_ = true;
        }
        return;
    }
    warnedGameWindowMissing_ = false;

    HWND foreground = GetForegroundWindow();
    const bool gameIsForeground = foreground == gameHwnd_ ||
        (foreground && GetAncestor(foreground, GA_ROOT) == gameHwnd_);
    if (!gameIsForeground) {
        if (IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
        return;
    }

    RECT client{};
    POINT origin{0, 0};
    if (!GetClientRect(gameHwnd_, &client) || !ClientToScreen(gameHwnd_, &origin)) return;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    RECT target{origin.x, origin.y, origin.x + width, origin.y + height};
    const bool changed = !haveGameClientRect_ ||
        target.left != lastGameClientRect_.left || target.top != lastGameClientRect_.top ||
        target.right != lastGameClientRect_.right || target.bottom != lastGameClientRect_.bottom;
    if (changed) {
        width_ = static_cast<UINT>(width);
        height_ = static_cast<UINT>(height);
        SetWindowPos(hwnd_, HWND_TOPMOST, target.left, target.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        lastGameClientRect_ = target;
        haveGameClientRect_ = true;
    } else if (!IsWindowVisible(hwnd_)) {
        SetWindowPos(hwnd_, HWND_TOPMOST, target.left, target.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

bool OverlayApp::createDevice() {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
        &device_, &selected, &context_);
    if (FAILED(hr)) {
        Diagnostics::instance().write("ERROR", "graphics", "D3D11CreateDeviceAndSwapChain failed; HRESULT=" +
            std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        if (factory) factory->Release();
        if (adapter) adapter->Release();
        if (dxgiDevice) dxgiDevice->Release();
        Diagnostics::instance().write("ERROR", "graphics", "Could not create the DXGI composition factory");
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_; desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    HRESULT compositionHr = factory->CreateSwapChainForComposition(device_, &desc, nullptr, &swapChain_);
    if (SUCCEEDED(compositionHr)) compositionHr = DCompositionCreateDevice(
        dxgiDevice, IID_PPV_ARGS(&compositionDevice_));
    if (SUCCEEDED(compositionHr)) compositionHr = compositionDevice_->CreateTargetForHwnd(
        hwnd_, TRUE, &compositionTarget_);
    if (SUCCEEDED(compositionHr)) compositionHr = compositionDevice_->CreateVisual(&compositionVisual_);
    if (SUCCEEDED(compositionHr)) compositionHr = compositionVisual_->SetContent(swapChain_);
    if (SUCCEEDED(compositionHr)) compositionHr = compositionTarget_->SetRoot(compositionVisual_);
    if (SUCCEEDED(compositionHr)) compositionHr = compositionDevice_->Commit();
    factory->Release(); adapter->Release(); dxgiDevice->Release();
    if (FAILED(compositionHr)) {
        Diagnostics::instance().write("ERROR", "graphics", "DirectComposition initialization failed; HRESULT=" +
            std::to_string(static_cast<unsigned long>(compositionHr)));
        return false;
    }
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) {
        Diagnostics::instance().write("ERROR", "graphics", "DXGI back-buffer acquisition failed");
        return false;
    }
    const HRESULT targetHr = device_->CreateRenderTargetView(backBuffer, nullptr, &target_);
    backBuffer->Release();
    if (FAILED(targetHr)) Diagnostics::instance().write("ERROR", "graphics", "render-target creation failed");
    return SUCCEEDED(targetHr);
}

void OverlayApp::resize(UINT width, UINT height) {
    if (!swapChain_ || width == 0 || height == 0) return;
    width_ = width; height_ = height;
    if (target_) { target_->Release(); target_ = nullptr; }
    if (SUCCEEDED(swapChain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0))) {
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
            device_->CreateRenderTargetView(backBuffer, nullptr, &target_);
            backBuffer->Release();
        }
    }
}

void OverlayApp::setInteractive(bool enabled) {
    interactive_ = enabled;
    settings_.guiVisible = enabled;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    // Keep the composition overlay interactive only over actual ImGui windows;
    // the ESP canvas remains click-through everywhere else.
    ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

void OverlayApp::updateMousePassThrough() {
    if (!hwnd_) return;
    POINT cursor{};
    bool overInteractiveUi = false;
    if (GetCursorPos(&cursor) && ScreenToClient(hwnd_, &cursor)) {
        for (const RECT& rect : guiHitRects_) {
            if (PtInRect(&rect, cursor)) {
                overInteractiveUi = true;
                break;
            }
        }
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    const LONG_PTR desired = overInteractiveUi
        ? (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT))
        : (ex | static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    if (desired != ex) {
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, desired);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

void OverlayApp::renderGui(const WorldSnapshot& snapshot) {
    ImGui::SetNextWindowSize(ImVec2(390.0f, 620.0f), ImGuiCond_FirstUseEver);
    const ImVec2 viewport = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(1.0f, 1.0f),
                                        ImVec2(std::max(1.0f, viewport.x),
                                               std::max(1.0f, viewport.y)));
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("MCP ESP Control", &settings_.guiVisible);
    ImVec2 panelSize = ImGui::GetWindowSize();
    const ImVec2 clampedSize(std::min(panelSize.x, viewport.x),
                             std::min(panelSize.y, viewport.y));
    if (panelSize.x != clampedSize.x || panelSize.y != clampedSize.y) {
        ImGui::SetWindowSize(clampedSize);
        panelSize = clampedSize;
    }
    ImVec2 panelPos = ImGui::GetWindowPos();
    const ImVec2 clampedPos(std::clamp(panelPos.x, 0.0f, std::max(0.0f, viewport.x - panelSize.x)),
                            std::clamp(panelPos.y, 0.0f, std::max(0.0f, viewport.y - panelSize.y)));
    if (panelPos.x != clampedPos.x || panelPos.y != clampedPos.y) {
        ImGui::SetWindowPos(clampedPos);
        panelPos = clampedPos;
    }
    guiHitRects_.push_back({static_cast<LONG>(panelPos.x), static_cast<LONG>(panelPos.y),
                            static_cast<LONG>(panelPos.x + panelSize.x),
                            static_cast<LONG>(panelPos.y + panelSize.y)});
    if (ImGui::CollapsingHeader("Overview", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Source: %s | %s", snapshot.source.c_str(), snapshot.backend.c_str());
    ImGui::Text("Sequence: %llu | Entities: %zu | Objects: %zu",
                static_cast<unsigned long long>(snapshot.sequence), snapshot.entities.size(), snapshot.objects.size());
    const ImVec4 perfColor = (snapshot.snapshotAgeMs > 250.0 || (snapshot.updateHz > 0.0 && snapshot.updateHz < 15.0) || renderFps_ < 30.0)
        ? ImVec4(1.0f, 0.55f, 0.25f, 1.0f) : ImVec4(0.45f, 0.9f, 0.55f, 1.0f);
    ImGui::TextColored(perfColor, "Performance: poll %.1f Hz | read %.1f ms | data age %.1f ms | render %.1f FPS (%.2f ms/frame)",
                       snapshot.updateHz, snapshot.sourceReadMs, snapshot.snapshotAgeMs,
                       renderFps_, renderFrameMs_);
    if (!poller_.connected()) ImGui::TextColored(ImVec4(1,0.35f,0.3f,1), "%s", poller_.lastError().c_str());
    }
    if (snapshot.live.available) {
        if (ImGui::CollapsingHeader("Live process details")) {
        ImGui::Separator();
        ImGui::Text("Live process: MiniGameApp.exe (PID %u)", snapshot.live.processId);
        ImGui::Text("Player controller: 0x%08X | motion: 0x%08X",
                    snapshot.live.playerController, snapshot.live.positionComponent);
        ImGui::Text("Player position (engine units): %d, %d, %d",
                    snapshot.live.playerPositionRaw[0], snapshot.live.playerPositionRaw[1],
                    snapshot.live.playerPositionRaw[2]);
        ImGui::Text("World: 0x%08X | ActorManager: 0x%08X",
                    snapshot.live.world, snapshot.live.actorManager);
        ImGui::Text("Verified actor positions: %u | list: 0x%08X..0x%08X",
                    snapshot.live.liveActorCount, snapshot.live.actorListBegin,
                    snapshot.live.actorListEnd);
        ImGui::Text("ClientActorMgr list: %u/%u | bounds: 0x%08X..0x%08X | %s",
                    snapshot.live.clientActorCount, snapshot.live.clientActorSlots,
                    snapshot.live.clientActorListBegin, snapshot.live.clientActorListEnd,
                    snapshot.live.clientActorListValid ? "stable" : "unavailable / changing");
        ImGui::Text("Renderer: 0x%08X | camera: 0x%08X",
                    snapshot.live.worldRenderer, snapshot.live.renderCamera);
        ImGui::Text("Camera chain: %s | view: %s | projection: %s | eye: %.1f, %.1f, %.1f",
                    snapshot.live.gameCameraRead ? "valid" : "unavailable",
                    snapshot.live.viewMatrixRead ? "valid" : "unavailable",
                    snapshot.live.projectionMatrixRead ? "valid" : "unavailable",
                    snapshot.cameraPosition.x, snapshot.cameraPosition.y, snapshot.cameraPosition.z);
        ImGui::Text("Actor slots: %u | players: %u | names read: %u (failed: %u) | creatures: %u | unknown: %u",
                    snapshot.live.actorSlots, snapshot.live.playerCount,
                    snapshot.live.playerNamesRead, snapshot.live.playerNameReadFailures,
                    snapshot.live.creatureCount, snapshot.live.unknownCount);
        std::size_t bonePointCount = 0;
        for (const auto& entity : snapshot.entities) bonePointCount += entity.bones.size();
        ImGui::Text("Health records: %u | ActorBody pointers: %u | live bone points: %zu",
                    snapshot.live.healthReadCount, snapshot.live.bodyPointerCount, bonePointCount);
        ImGui::Text("Invalid pointers: %u | missing motion: %u",
                    snapshot.live.invalidActorPointers, snapshot.live.missingPositionComponents);
        ImGui::Text("Position read failures: %u | invalid positions: %u | look angles: %s",
                    snapshot.live.positionReadFailures, snapshot.live.invalidPositions,
                    snapshot.live.lookAnglesAvailable ? "valid" : "unavailable");
        if (snapshot.live.lookAnglesAvailable)
            ImGui::Text("Look raw (yaw, pitch): %.3f, %.3f | forward: %.3f, %.3f, %.3f",
                        snapshot.live.playerLookAnglesRaw[0], snapshot.live.playerLookAnglesRaw[1],
                        snapshot.cameraForward.x, snapshot.cameraForward.y, snapshot.cameraForward.z);
        ImGui::Text("Coordinate scale: %.0f source units per meter | camera matrices: %s",
                    snapshot.unitsPerMeter, snapshot.live.matrixAvailable ? "live / valid" : "unavailable");
        if (snapshot.live.available)
            ImGui::TextColored(ImVec4(1,0.75f,0.25f,1),
                "Team-side metadata is unavailable; player and creature records are classified from their runtime type labels.");
        if (!snapshot.entities.empty() && ImGui::TreeNode("Live actor positions")) {
            for (const auto& entity : snapshot.entities) {
                const auto screen = snapshot.live.matrixAvailable
                    ? worldToScreen(snapshot, entity.position, static_cast<int>(ImGui::GetIO().DisplaySize.x),
                                    static_cast<int>(ImGui::GetIO().DisplaySize.y))
                    : std::optional<Vec2>{};
                ImGui::Text("%s [%s] %s  %.0f, %.0f, %.0f  distance=%.1fm  screen=%s%s",
                    entity.id.c_str(), entity.kind.c_str(), entity.name.c_str(), entity.position.x,
                    entity.position.y, entity.position.z,
                    std::sqrt(std::pow(entity.position.x - snapshot.cameraPosition.x, 2.0f) +
                              std::pow(entity.position.y - snapshot.cameraPosition.y, 2.0f) +
                              std::pow(entity.position.z - snapshot.cameraPosition.z, 2.0f)) /
                              std::max(1.0f, snapshot.unitsPerMeter),
                    screen ? "visible" : (snapshot.live.matrixAvailable ? "clipped/behind" : "no-matrix"),
                    (entity.kind == "player" && !entity.team && entity.relation == "neutral")
                        ? " team=unknown" : "");
            }
            ImGui::TreePop();
        }
        }
    }
    if (ImGui::CollapsingHeader("Draw diagnostics")) {
    ImGui::Text("Draw diagnostics: entities %zu (players %zu / creatures %zu / unknown %zu), drawn %zu",
                snapshot.entities.size(), drawStats_.playerEntities, drawStats_.creatureEntities,
                drawStats_.unknownEntities, drawStats_.drawn);
    ImGui::Text("Skipped: kind %zu, relation %zu, visibility %zu, distance %zu, no bounds %zu, no matrix %zu, clipped %zu",
                drawStats_.filteredKind, drawStats_.filteredRelation, drawStats_.filteredVisibility,
                drawStats_.filteredDistance, drawStats_.missingBounds, drawStats_.missingMatrix,
                drawStats_.clipped);
    ImGui::Text("Radar: entities %zu/%zu, objects %zu/%zu, heading %s%s",
                radarStats_.entitiesPlotted, snapshot.entities.size(), radarStats_.objectsPlotted,
                snapshot.objects.size(), radarStats_.headingAvailable ? "valid" : "unavailable",
                radarStats_.headingFallbackUsed ? " (world-axis fallback)" : "");
    const auto logPath = Diagnostics::instance().path().wstring();
    ImGui::Text("Log: %ls", logPath.c_str());
    }
    ImGui::Separator();
    if (ImGui::BeginTabBar("ESP tabs")) {
        if (ImGui::BeginTabItem("Entities")) {
            ImGui::Checkbox("Players", &settings_.showPlayers); ImGui::SameLine();
            ImGui::Checkbox("Creatures", &settings_.showCreatures); ImGui::SameLine();
            ImGui::Checkbox("Unknown", &settings_.showUnknown);
            ImGui::Checkbox("Friends", &settings_.showFriends); ImGui::SameLine();
            ImGui::Checkbox("Enemies", &settings_.showEnemies); ImGui::SameLine();
            ImGui::Checkbox("Neutral", &settings_.showNeutral);
            ImGui::Checkbox("Only visible (unknown stays visible)", &settings_.visibleOnly);
            ImGui::SliderFloat("Max distance in meters (0 = unlimited)",
                               &settings_.maxDistance, 0.0f, 100000.0f, "%.0f m");
            ImGui::InputFloat("Exact max distance (meters)", &settings_.maxDistance, 1.0f, 100.0f, "%.1f m");
            settings_.maxDistance = std::max(0.0f, settings_.maxDistance);
            ImGui::Separator();
            ImGui::Checkbox("Box outline", &settings_.showBox); ImGui::SameLine();
            ImGui::Checkbox("Box fill", &settings_.showBoxFill);
            const char* boxTypes[] = {"2D", "3D"};
            int boxMode = settings_.use3dBoxes ? 1 : 0;
            if (ImGui::Combo("Box type", &boxMode, boxTypes, 2)) settings_.use3dBoxes = boxMode == 1;
            ImGui::SliderFloat("Box opacity / concentration", &settings_.boxOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat("Face opacity", &settings_.boxFillOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat("Box line thickness", &settings_.boxThickness, 0.5f, 8.0f, "%.1f px");
            ImGui::Checkbox("Name", &settings_.showName); ImGui::SameLine();
            ImGui::Checkbox("Distance", &settings_.showDistance);
            ImGui::Checkbox("Health", &settings_.showHealth); ImGui::SameLine();
            ImGui::Checkbox("Health as number", &settings_.healthAsNumber);
            ImGui::SliderFloat("Text/health opacity", &settings_.textOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat("Health opacity", &settings_.healthOpacity, 0.0f, 1.0f);
            ImGui::Checkbox("Antenna", &settings_.showAntenna);
            const char* anchors[] = {"Top", "Bottom", "Screen center"};
            ImGui::Combo("Antenna anchor", &settings_.antennaAnchor, anchors, 3);
            ImGui::SliderFloat("Antenna opacity", &settings_.antennaOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat("Antenna thickness", &settings_.antennaThickness, 0.5f, 8.0f, "%.1f px");
            ImGui::Checkbox("Detailed skeleton (source bones only)", &settings_.showSkeleton);
            ImGui::SliderFloat("Skeleton opacity", &settings_.skeletonOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat("Skeleton thickness", &settings_.skeletonThickness, 0.5f, 8.0f, "%.1f px");
            ImGui::Checkbox("Default rig when bones missing (demo only)", &settings_.applyDefaultRigWhenMissing);
            ImGui::Checkbox("Entity count", &settings_.showEntityCount);
            colorEdit("Player color", settings_.playerColor);
            colorEdit("Friend color", settings_.friendColor);
            colorEdit("Enemy color", settings_.enemyColor);
            colorEdit("Creature color", settings_.creatureColor);
            colorEdit("Skeleton color", settings_.skeletonColor);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Objects")) {
            ImGui::Checkbox("Object ESP", &settings_.showObjects);
            ImGui::Checkbox("Object box outline", &settings_.showObjectBox);
            ImGui::Checkbox("Object face fill", &settings_.showObjectFill);
            ImGui::Checkbox("Object name", &settings_.showObjectName); ImGui::SameLine();
            ImGui::Checkbox("Object distance", &settings_.showObjectDistance);
            ImGui::Checkbox("Object box follows 2D/3D selection", &settings_.use3dBoxes);
            colorEdit("Object color", settings_.objectColor);
            ImGui::SliderFloat("Object opacity", &settings_.objectOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat("Object face opacity", &settings_.objectFillOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat("Object line thickness", &settings_.objectThickness, 0.5f, 8.0f, "%.1f px");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Radar")) {
            ImGui::Checkbox("Radar", &settings_.showRadar);
            ImGui::SliderFloat("Radar radius", &settings_.radarRadius, 40.0f, 240.0f, "%.0f px");
            ImGui::SliderFloat("Radar range (meters)", &settings_.radarRange,
                               1.0f, 100000.0f, "%.0f m");
            ImGui::InputFloat("Exact radar range (meters)", &settings_.radarRange, 1.0f, 100.0f, "%.1f m");
            settings_.radarRange = std::max(1.0f, settings_.radarRange);
            if (!radarStats_.headingAvailable)
                ImGui::TextColored(ImVec4(1,0.75f,0.25f,1),
                    "Heading is unverified; radar uses world axes while raw look angles are logged.");
            const bool radarXChanged = ImGui::SliderFloat("Radar X", &settings_.radarX, 0.0f, std::max(1.0f, width_ - settings_.radarRadius * 2.0f));
            const bool radarYChanged = ImGui::SliderFloat("Radar Y", &settings_.radarY, 0.0f, std::max(1.0f, height_ - settings_.radarRadius * 2.0f));
            settings_.radarPositionOverride = radarXChanged || radarYChanged;
            ImGui::TextUnformatted("You can also drag the radar window directly.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Crosshair")) {
            ImGui::Checkbox("Crosshair", &settings_.showCrosshair);
            const char* styles[] = {"Cross", "Circle", "Four corners", "Dynamic"};
            ImGui::Combo("Style", &settings_.crosshairStyle, styles, 4);
            ImGui::SliderFloat("Size", &settings_.crosshairSize, 2.0f, 80.0f, "%.0f px");
            ImGui::SliderFloat("Gap", &settings_.crosshairGap, 0.0f, 30.0f, "%.0f px");
            ImGui::SliderFloat("Thickness", &settings_.crosshairThickness, 0.5f, 8.0f, "%.1f px");
            ImGui::SliderFloat("Opacity", &settings_.crosshairOpacity, 0.0f, 1.0f);
            colorEdit("Crosshair color", settings_.crosshairColor);
            ImGui::Separator();
            ImGui::Checkbox("Outer circle", &settings_.showCrosshairRing);
            ImGui::SliderFloat("Outer circle radius", &settings_.ringRadius, 2.0f, 500.0f, "%.0f px");
            ImGui::SliderFloat("Outer circle opacity", &settings_.ringOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat("Outer circle thickness", &settings_.ringThickness, 0.5f, 8.0f, "%.1f px");
            colorEdit("Outer circle color", settings_.ringColor);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            ImGui::TextUnformatted("Interface theme / 界面風格");
            const char* themes[] = {"Dark / 深色", "White + Blue / 白色 + 藍色"};
            int selectedTheme = settings_.guiTheme;
            if (ImGui::Combo("Theme / 主題", &selectedTheme, themes, 2)) {
                settings_.guiTheme = selectedTheme;
                saveUiPreferences(settings_);
            }
            ImGui::TextWrapped("The theme is saved beside the executable and applied when the panel refreshes. / 主題設定會保存於執行檔旁，並在面板刷新時套用。");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Insert: toggle menu / click-through  |  End: quit");
    if (!settings_.guiVisible && interactive_) setInteractive(false);
    ImGui::End();
}

void OverlayApp::renderFrame() {
    if (GetAsyncKeyState(VK_INSERT) & 1) setInteractive(!interactive_);
    if (GetAsyncKeyState(VK_END) & 1) { running_ = false; return; }
    const auto frameStarted = std::chrono::steady_clock::now();
    if (previousFrameAt_ != std::chrono::steady_clock::time_point{}) {
        const double frameMs = std::chrono::duration<double, std::milli>(frameStarted - previousFrameAt_).count();
        renderFrameMs_ = renderFrameMs_ == 0.0 ? frameMs : renderFrameMs_ * 0.9 + frameMs * 0.1;
        renderFps_ = renderFrameMs_ > 0.0 ? 1000.0 / renderFrameMs_ : 0.0;
    }
    previousFrameAt_ = frameStarted;
    const WorldSnapshot snapshot = poller_.latest();
    syncToGameWindow(snapshot);
    guiHitRects_.clear();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (appliedGuiTheme_ != settings_.guiTheme) {
        applyGuiTheme(settings_.guiTheme);
        appliedGuiTheme_ = settings_.guiTheme;
    }
    if (settings_.guiVisible) renderGui(snapshot);
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    drawStats_ = drawWorld(background, snapshot, settings_, static_cast<int>(width_),
                           static_cast<int>(height_), static_cast<float>(ImGui::GetTime()));
    drawCrosshair(background, settings_, static_cast<int>(width_), static_cast<int>(height_),
                  static_cast<float>(ImGui::GetTime()));
    radarStats_ = drawRadar(snapshot, settings_);
    if (ImGuiContext* context = ImGui::GetCurrentContext()) {
        for (ImGuiWindow* window : context->Windows) {
            if (!window || !window->WasActive) continue;
            const bool isRadar = window->Name && std::strcmp(window->Name, "Radar") == 0;
            const bool isPopup = (window->Flags & ImGuiWindowFlags_Popup) != 0;
            if (!isRadar && !isPopup) continue;
            const ImVec2 pos = window->Pos;
            const ImVec2 size = window->Size;
            guiHitRects_.push_back({static_cast<LONG>(pos.x), static_cast<LONG>(pos.y),
                                    static_cast<LONG>(pos.x + size.x),
                                    static_cast<LONG>(pos.y + size.y)});
        }
    }
    updateMousePassThrough();
    static auto lastDiagnosticLog = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastDiagnosticLog >= std::chrono::seconds(1)) {
        lastDiagnosticLog = now;
        std::ostringstream line;
        line << "source=" << snapshot.source << " seq=" << snapshot.sequence
             << " entities=" << snapshot.entities.size() << " players=" << drawStats_.playerEntities
             << " creatures=" << drawStats_.creatureEntities << " unknown=" << drawStats_.unknownEntities
             << " drawn=" << drawStats_.drawn << " skipKind=" << drawStats_.filteredKind
             << " skipRelation=" << drawStats_.filteredRelation
             << " skipVisibility=" << drawStats_.filteredVisibility
             << " skipDistance=" << drawStats_.filteredDistance
             << " missingBounds=" << drawStats_.missingBounds
             << " missingMatrix=" << drawStats_.missingMatrix << " clipped=" << drawStats_.clipped
             << " objects=" << snapshot.objects.size() << " objectsDrawn=" << drawStats_.objectsDrawn
             << " objectSkipDistance=" << drawStats_.objectsFilteredDistance
             << " objectMissingMatrix=" << drawStats_.objectsMissingMatrix
             << " objectClipped=" << drawStats_.objectsClipped
             << " unitsPerMeter=" << snapshot.unitsPerMeter
             << " maxDistanceMeters=" << settings_.maxDistance
             << " radarRangeMeters=" << settings_.radarRange
             << " radarPlotted=" << radarStats_.entitiesPlotted << '/' << radarStats_.objectsPlotted
             << " radarHeading=" << (radarStats_.headingAvailable ? "valid" : "missing")
             << " radarFallback=" << (radarStats_.headingFallbackUsed ? "world-axis" : "none")
             << " pollHz=" << snapshot.updateHz << " sourceReadMs=" << snapshot.sourceReadMs
             << " snapshotAgeMs=" << snapshot.snapshotAgeMs << " renderFps=" << renderFps_
             << " renderFrameMs=" << renderFrameMs_;
        Diagnostics::instance().write("INFO", "render", line.str());
    }
    ImGui::Render();

    const float clear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetRenderTargets(1, &target_, nullptr);
    context_->ClearRenderTargetView(target_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);
}

int OverlayApp::run(HINSTANCE instance) {
    width_ = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
    height_ = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &OverlayApp::windowProc; wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = L"McpEspOverlayWindow";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Diagnostics::instance().write("ERROR", "window", "RegisterClassExW failed; Win32 error=" +
            std::to_string(GetLastError()));
        return 1;
    }
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"MCP ESP Overlay", WS_POPUP, 0, 0, width_, height_,
        nullptr, nullptr, instance, this);
    if (!hwnd_) {
        Diagnostics::instance().write("ERROR", "window", "CreateWindowExW failed; Win32 error=" +
            std::to_string(GetLastError()));
        return 2;
    }
    if (!createDevice()) { DestroyWindow(hwnd_); return 3; }
    Diagnostics::instance().write("INFO", "graphics", "DirectX 11 overlay initialized at " +
        std::to_string(width_) + "x" + std::to_string(height_));

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = "mcp-esp.ini";
    loadUiPreferences(settings_);
    loadUiFont(io);
    applyGuiTheme(settings_.guiTheme);
    appliedGuiTheme_ = settings_.guiTheme;
    ImGui_ImplWin32_Init(hwnd_); ImGui_ImplDX11_Init(device_, context_);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    setInteractive(true); // Start with a visible control panel; Insert toggles click-through ESP mode.

    MSG message{};
    while (running_) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { running_ = false; break; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (running_) renderFrame();
    }
    return 0;
}

} // namespace esp
