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
const char* uiText(bool chinese, const char* zh, const char* en) {
    return chinese ? zh : en;
}

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
    int value = 0;
    while (input >> key >> value) {
        if (key == "theme") settings.guiTheme = std::clamp(value, 0, 1);
        else if (key == "language") settings.guiLanguage = std::clamp(value, 0, 1);
    }
}

void saveUiPreferences(const Settings& settings) {
    std::ofstream output(uiPreferencesPath(), std::ios::trunc);
    if (output) {
        output << "theme " << std::clamp(settings.guiTheme, 0, 1) << '\n';
        output << "language " << std::clamp(settings.guiLanguage, 0, 1) << '\n';
    }
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
    const bool chinese = settings_.guiLanguage == 0;
    ImGui::SetNextWindowSize(ImVec2(390.0f, 620.0f), ImGuiCond_FirstUseEver);
    const ImVec2 viewport = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(1.0f, 1.0f),
                                        ImVec2(std::max(1.0f, viewport.x),
                                               std::max(1.0f, viewport.y)));
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(uiText(chinese, "MCP ESP 控制台###MCP ESP Control", "MCP ESP Control###MCP ESP Control"), &settings_.guiVisible);
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
    if (ImGui::CollapsingHeader(uiText(chinese, "概覽", "Overview"), ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text(uiText(chinese, "資料來源：%s | %s", "Source: %s | %s"), snapshot.source.c_str(), snapshot.backend.c_str());
    ImGui::Text(uiText(chinese, "序號：%llu | 目標：%zu | 物件：%zu", "Sequence: %llu | Entities: %zu | Objects: %zu"),
                static_cast<unsigned long long>(snapshot.sequence), snapshot.entities.size(), snapshot.objects.size());
    const ImVec4 perfColor = (snapshot.snapshotAgeMs > 250.0 || (snapshot.updateHz > 0.0 && snapshot.updateHz < 15.0) || renderFps_ < 30.0)
        ? ImVec4(1.0f, 0.55f, 0.25f, 1.0f) : ImVec4(0.45f, 0.9f, 0.55f, 1.0f);
    ImGui::TextColored(perfColor, uiText(chinese,
                       "效能：資料 %.1f Hz | 讀取 %.1f ms | 目標資料 %.1f ms | 相機 %.1f ms | 畫面 %.1f FPS (%.2f ms/幀)",
                       "Performance: poll %.1f Hz | read %.1f ms | target data %.1f ms | camera %.1f ms | render %.1f FPS (%.2f ms/frame)"),
                       snapshot.updateHz, snapshot.sourceReadMs, snapshot.snapshotAgeMs, snapshot.cameraAgeMs,
                       renderFps_, renderFrameMs_);
    if (!poller_.connected()) ImGui::TextColored(ImVec4(1,0.35f,0.3f,1), "%s", poller_.lastError().c_str());
    }
    if (snapshot.live.available) {
        if (ImGui::CollapsingHeader(uiText(chinese, "即時進程資訊", "Live process details"))) {
        ImGui::Separator();
        ImGui::Text(uiText(chinese, "遊戲進程：MiniGameApp.exe (PID %u)", "Live process: MiniGameApp.exe (PID %u)"), snapshot.live.processId);
        ImGui::Text(uiText(chinese, "玩家控制器：0x%08X | 動作元件：0x%08X", "Player controller: 0x%08X | motion: 0x%08X"),
                    snapshot.live.playerController, snapshot.live.positionComponent);
        ImGui::Text(uiText(chinese, "玩家座標（引擎單位）：%d, %d, %d", "Player position (engine units): %d, %d, %d"),
                    snapshot.live.playerPositionRaw[0], snapshot.live.playerPositionRaw[1],
                    snapshot.live.playerPositionRaw[2]);
        ImGui::Text(uiText(chinese, "世界：0x%08X | ActorManager：0x%08X", "World: 0x%08X | ActorManager: 0x%08X"),
                    snapshot.live.world, snapshot.live.actorManager);
        ImGui::Text(uiText(chinese, "有效目標座標：%u | 清單：0x%08X..0x%08X", "Verified actor positions: %u | list: 0x%08X..0x%08X"),
                    snapshot.live.liveActorCount, snapshot.live.actorListBegin,
                    snapshot.live.actorListEnd);
        ImGui::Text(uiText(chinese, "ClientActorMgr 清單：%u/%u | 範圍：0x%08X..0x%08X | %s", "ClientActorMgr list: %u/%u | bounds: 0x%08X..0x%08X | %s"),
                    snapshot.live.clientActorCount, snapshot.live.clientActorSlots,
                    snapshot.live.clientActorListBegin, snapshot.live.clientActorListEnd,
                    snapshot.live.clientActorListValid ? "stable" : "unavailable / changing");
        ImGui::Text(uiText(chinese, "渲染器：0x%08X | 相機：0x%08X", "Renderer: 0x%08X | camera: 0x%08X"),
                    snapshot.live.worldRenderer, snapshot.live.renderCamera);
        ImGui::Text(uiText(chinese, "相機鏈：%s | 視角矩陣：%s | 投影矩陣：%s | 眼睛位置：%.1f, %.1f, %.1f", "Camera chain: %s | view: %s | projection: %s | eye: %.1f, %.1f, %.1f"),
                    snapshot.live.gameCameraRead ? "valid" : "unavailable",
                    snapshot.live.viewMatrixRead ? "valid" : "unavailable",
                    snapshot.live.projectionMatrixRead ? "valid" : "unavailable",
                    snapshot.cameraPosition.x, snapshot.cameraPosition.y, snapshot.cameraPosition.z);
        ImGui::Text(uiText(chinese, "目標槽位：%u | 玩家：%u | 名稱成功：%u（失敗：%u）| 生物：%u | 未分類：%u", "Actor slots: %u | players: %u | names read: %u (failed: %u) | creatures: %u | unknown: %u"),
                    snapshot.live.actorSlots, snapshot.live.playerCount,
                    snapshot.live.playerNamesRead, snapshot.live.playerNameReadFailures,
                    snapshot.live.creatureCount, snapshot.live.unknownCount);
        std::size_t bonePointCount = 0;
        for (const auto& entity : snapshot.entities) bonePointCount += entity.bones.size();
        ImGui::Text(uiText(chinese, "血量資料：%u | ActorBody 指標：%u | 即時骨骼點：%zu", "Health records: %u | ActorBody pointers: %u | live bone points: %zu"),
                    snapshot.live.healthReadCount, snapshot.live.bodyPointerCount, bonePointCount);
        ImGui::Text(uiText(chinese, "無效指標：%u | 缺少動作元件：%u", "Invalid pointers: %u | missing motion: %u"),
                    snapshot.live.invalidActorPointers, snapshot.live.missingPositionComponents);
        ImGui::Text(uiText(chinese, "座標讀取失敗：%u | 無效座標：%u | 視角角度：%s", "Position read failures: %u | invalid positions: %u | look angles: %s"),
                    snapshot.live.positionReadFailures, snapshot.live.invalidPositions,
                    snapshot.live.lookAnglesAvailable ? "valid" : "unavailable");
        if (snapshot.live.lookAnglesAvailable)
            ImGui::Text(uiText(chinese, "原始視角（yaw, pitch）：%.3f, %.3f | 前向：%.3f, %.3f, %.3f", "Look raw (yaw, pitch): %.3f, %.3f | forward: %.3f, %.3f, %.3f"),
                        snapshot.live.playerLookAnglesRaw[0], snapshot.live.playerLookAnglesRaw[1],
                        snapshot.cameraForward.x, snapshot.cameraForward.y, snapshot.cameraForward.z);
        ImGui::Text(uiText(chinese, "座標比例：每米 %.0f 單位 | 相機矩陣：%s", "Coordinate scale: %.0f source units per meter | camera matrices: %s"),
                    snapshot.unitsPerMeter, snapshot.live.matrixAvailable ? "live / valid" : "unavailable");
        if (snapshot.live.available)
            ImGui::TextColored(ImVec4(1,0.75f,0.25f,1),
                uiText(chinese, "遊戲未提供隊伍資料；玩家與生物依執行時類型標籤分類。", "Team-side metadata is unavailable; player and creature records are classified from their runtime type labels."));
        if (!snapshot.entities.empty() && ImGui::TreeNode(uiText(chinese, "即時目標座標", "Live actor positions"))) {
            for (const auto& entity : snapshot.entities) {
                const auto screen = snapshot.live.matrixAvailable
                    ? worldToScreen(snapshot, entity.position, static_cast<int>(ImGui::GetIO().DisplaySize.x),
                                    static_cast<int>(ImGui::GetIO().DisplaySize.y))
                    : std::optional<Vec2>{};
                ImGui::Text(uiText(chinese, "%s [%s] %s  %.0f, %.0f, %.0f  距離=%.1fm  螢幕=%s%s", "%s [%s] %s  %.0f, %.0f, %.0f  distance=%.1fm  screen=%s%s"),
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
    if (ImGui::CollapsingHeader(uiText(chinese, "繪製診斷", "Draw diagnostics"))) {
    ImGui::Text(uiText(chinese, "繪製診斷：目標 %zu（玩家 %zu / 生物 %zu / 未分類 %zu），已繪製 %zu", "Draw diagnostics: entities %zu (players %zu / creatures %zu / unknown %zu), drawn %zu"),
                snapshot.entities.size(), drawStats_.playerEntities, drawStats_.creatureEntities,
                drawStats_.unknownEntities, drawStats_.drawn);
    ImGui::Text(uiText(chinese, "略過：類型 %zu、關係 %zu、可見性 %zu、距離 %zu、無範圍 %zu、無矩陣 %zu、超出視野 %zu", "Skipped: kind %zu, relation %zu, visibility %zu, distance %zu, no bounds %zu, no matrix %zu, clipped %zu"),
                drawStats_.filteredKind, drawStats_.filteredRelation, drawStats_.filteredVisibility,
                drawStats_.filteredDistance, drawStats_.missingBounds, drawStats_.missingMatrix,
                drawStats_.clipped);
    ImGui::Text(uiText(chinese, "雷達：目標 %zu/%zu、物件 %zu/%zu、方向 %s%s", "Radar: entities %zu/%zu, objects %zu/%zu, heading %s%s"),
                radarStats_.entitiesPlotted, snapshot.entities.size(), radarStats_.objectsPlotted,
                snapshot.objects.size(), radarStats_.headingAvailable ? "valid" : "unavailable",
                radarStats_.headingFallbackUsed ? " (world-axis fallback)" : "");
    const auto logPath = Diagnostics::instance().path().wstring();
    ImGui::Text(uiText(chinese, "日誌：%ls", "Log: %ls"), logPath.c_str());
    }
    ImGui::Separator();
    if (ImGui::BeginTabBar("ESP tabs")) {
        if (ImGui::BeginTabItem(uiText(chinese, "目標", "Entities"))) {
            ImGui::Checkbox(uiText(chinese, "玩家", "Players"), &settings_.showPlayers); ImGui::SameLine();
            ImGui::Checkbox(uiText(chinese, "生物", "Creatures"), &settings_.showCreatures); ImGui::SameLine();
            ImGui::Checkbox(uiText(chinese, "未分類", "Unknown"), &settings_.showUnknown);
            ImGui::Checkbox(uiText(chinese, "隊友", "Friends"), &settings_.showFriends); ImGui::SameLine();
            ImGui::Checkbox(uiText(chinese, "敵人", "Enemies"), &settings_.showEnemies); ImGui::SameLine();
            ImGui::Checkbox(uiText(chinese, "中立", "Neutral"), &settings_.showNeutral);
            ImGui::Checkbox(uiText(chinese, "只顯示可見目標（未分類仍顯示）", "Only visible (unknown stays visible)"), &settings_.visibleOnly);
            ImGui::SliderFloat(uiText(chinese, "最大距離（米，0=不限）", "Max distance in meters (0 = unlimited)"),
                               &settings_.maxDistance, 0.0f, 100000.0f, "%.0f m");
            ImGui::InputFloat(uiText(chinese, "精確最大距離（米）", "Exact max distance (meters)"), &settings_.maxDistance, 1.0f, 100.0f, "%.1f m");
            settings_.maxDistance = std::max(0.0f, settings_.maxDistance);
            ImGui::Separator();
            ImGui::Checkbox(uiText(chinese, "方框邊線", "Box outline"), &settings_.showBox); ImGui::SameLine();
            ImGui::Checkbox(uiText(chinese, "方框填色", "Box fill"), &settings_.showBoxFill);
            const char* boxTypes[] = {"2D", "3D"};
            int boxMode = settings_.use3dBoxes ? 1 : 0;
            if (ImGui::Combo(uiText(chinese, "方框類型", "Box type"), &boxMode, boxTypes, 2)) settings_.use3dBoxes = boxMode == 1;
            ImGui::SliderFloat(uiText(chinese, "方框不透明度", "Box opacity"), &settings_.boxOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat(uiText(chinese, "面透明度", "Face opacity"), &settings_.boxFillOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat(uiText(chinese, "方框線條粗細", "Box line thickness"), &settings_.boxThickness, 0.5f, 8.0f, "%.1f px");
            ImGui::Checkbox(uiText(chinese, "名稱", "Name"), &settings_.showName); ImGui::SameLine();
            ImGui::Checkbox(uiText(chinese, "距離", "Distance"), &settings_.showDistance);
            ImGui::Checkbox(uiText(chinese, "血量", "Health"), &settings_.showHealth); ImGui::SameLine();
            ImGui::Checkbox(uiText(chinese, "血量數字", "Health as number"), &settings_.healthAsNumber);
            ImGui::SliderFloat(uiText(chinese, "文字透明度", "Text opacity"), &settings_.textOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat(uiText(chinese, "血量透明度", "Health opacity"), &settings_.healthOpacity, 0.0f, 1.0f);
            ImGui::Checkbox(uiText(chinese, "天線", "Antenna"), &settings_.showAntenna);
            const char* anchors[] = {"頂部", "底部", "螢幕中心"};
            if (!chinese) { anchors[0] = "Top"; anchors[1] = "Bottom"; anchors[2] = "Screen center"; }
            ImGui::Combo(uiText(chinese, "天線起點", "Antenna anchor"), &settings_.antennaAnchor, anchors, 3);
            ImGui::SliderFloat(uiText(chinese, "天線透明度", "Antenna opacity"), &settings_.antennaOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat(uiText(chinese, "天線粗細", "Antenna thickness"), &settings_.antennaThickness, 0.5f, 8.0f, "%.1f px");
            ImGui::Checkbox(uiText(chinese, "詳細骨架（僅使用來源骨骼）", "Detailed skeleton (source bones only)"), &settings_.showSkeleton);
            ImGui::SliderFloat(uiText(chinese, "骨架透明度", "Skeleton opacity"), &settings_.skeletonOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat(uiText(chinese, "骨架線條粗細", "Skeleton thickness"), &settings_.skeletonThickness, 0.5f, 8.0f, "%.1f px");
            ImGui::Checkbox(uiText(chinese, "缺少骨骼時套用預設骨架（僅示範）", "Default rig when bones missing (demo only)"), &settings_.applyDefaultRigWhenMissing);
            ImGui::Checkbox(uiText(chinese, "目標數量", "Entity count"), &settings_.showEntityCount);
            if (settings_.showEntityCount) {
                ImGui::SliderFloat(uiText(chinese, "數量字體大小", "Count text size"),
                                   &settings_.entityCountFontSize, 10.0f, 48.0f, "%.0f px");
                ImGui::SliderFloat(uiText(chinese, "數量文字 X 位置", "Count text X position"),
                                   &settings_.entityCountX, 0.0f, std::max(0.0f, width_ - 160.0f), "%.0f px");
                ImGui::SliderFloat(uiText(chinese, "數量文字 Y 位置", "Count text Y position"),
                                   &settings_.entityCountY, 0.0f, std::max(0.0f, height_ - 24.0f), "%.0f px");
            }
            colorEdit(uiText(chinese, "玩家顏色", "Player color"), settings_.playerColor);
            colorEdit(uiText(chinese, "隊友顏色", "Friend color"), settings_.friendColor);
            colorEdit(uiText(chinese, "敵人顏色", "Enemy color"), settings_.enemyColor);
            colorEdit(uiText(chinese, "生物顏色", "Creature color"), settings_.creatureColor);
            colorEdit(uiText(chinese, "骨架顏色", "Skeleton color"), settings_.skeletonColor);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(uiText(chinese, "物件", "Objects"))) {
            ImGui::Checkbox(uiText(chinese, "物件 ESP", "Object ESP"), &settings_.showObjects);
            ImGui::Checkbox(uiText(chinese, "物件方框邊線", "Object box outline"), &settings_.showObjectBox);
            ImGui::Checkbox(uiText(chinese, "物件表面填色", "Object face fill"), &settings_.showObjectFill);
            ImGui::Checkbox(uiText(chinese, "物件名稱", "Object name"), &settings_.showObjectName); ImGui::SameLine();
            ImGui::Checkbox(uiText(chinese, "物件距離", "Object distance"), &settings_.showObjectDistance);
            ImGui::Checkbox(uiText(chinese, "物件方框沿用 2D/3D 選擇", "Object box follows 2D/3D selection"), &settings_.use3dBoxes);
            colorEdit(uiText(chinese, "物件顏色", "Object color"), settings_.objectColor);
            ImGui::SliderFloat(uiText(chinese, "物件透明度", "Object opacity"), &settings_.objectOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat(uiText(chinese, "物件面透明度", "Object face opacity"), &settings_.objectFillOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat(uiText(chinese, "物件線條粗細", "Object line thickness"), &settings_.objectThickness, 0.5f, 8.0f, "%.1f px");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(uiText(chinese, "雷達", "Radar"))) {
            ImGui::Checkbox(uiText(chinese, "顯示雷達", "Radar"), &settings_.showRadar);
            ImGui::SliderFloat(uiText(chinese, "雷達半徑", "Radar radius"), &settings_.radarRadius, 40.0f, 240.0f, "%.0f px");
            ImGui::SliderFloat(uiText(chinese, "雷達範圍（米）", "Radar range (meters)"), &settings_.radarRange,
                               1.0f, 100000.0f, "%.0f m");
            ImGui::InputFloat(uiText(chinese, "精確雷達範圍（米）", "Exact radar range (meters)"), &settings_.radarRange, 1.0f, 100.0f, "%.1f m");
            settings_.radarRange = std::max(1.0f, settings_.radarRange);
            if (!radarStats_.headingAvailable)
                ImGui::TextColored(ImVec4(1,0.75f,0.25f,1),
                    uiText(chinese, "方向尚未確認；雷達目前依世界座標軸顯示，原始視角已寫入日誌。", "Heading is unverified; radar uses world axes while raw look angles are logged."));
            const bool radarXChanged = ImGui::SliderFloat(uiText(chinese, "雷達 X 位置", "Radar X"), &settings_.radarX, 0.0f, std::max(1.0f, width_ - settings_.radarRadius * 2.0f));
            const bool radarYChanged = ImGui::SliderFloat(uiText(chinese, "雷達 Y 位置", "Radar Y"), &settings_.radarY, 0.0f, std::max(1.0f, height_ - settings_.radarRadius * 2.0f));
            settings_.radarPositionOverride = radarXChanged || radarYChanged;
            ImGui::TextUnformatted(uiText(chinese, "也可以直接拖曳雷達視窗調整位置。", "You can also drag the radar window directly."));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(uiText(chinese, "準星", "Crosshair"))) {
            ImGui::Checkbox(uiText(chinese, "顯示準星", "Crosshair"), &settings_.showCrosshair);
            const char* styles[] = {"十字", "圓形", "四角", "動態"};
            if (!chinese) { styles[0] = "Cross"; styles[1] = "Circle"; styles[2] = "Four corners"; styles[3] = "Dynamic"; }
            ImGui::Combo(uiText(chinese, "樣式", "Style"), &settings_.crosshairStyle, styles, 4);
            ImGui::SliderFloat(uiText(chinese, "尺寸", "Size"), &settings_.crosshairSize, 2.0f, 80.0f, "%.0f px");
            ImGui::SliderFloat(uiText(chinese, "間距", "Gap"), &settings_.crosshairGap, 0.0f, 30.0f, "%.0f px");
            ImGui::SliderFloat(uiText(chinese, "粗細", "Thickness"), &settings_.crosshairThickness, 0.5f, 8.0f, "%.1f px");
            ImGui::SliderFloat(uiText(chinese, "透明度", "Opacity"), &settings_.crosshairOpacity, 0.0f, 1.0f);
            colorEdit(uiText(chinese, "準星顏色", "Crosshair color"), settings_.crosshairColor);
            ImGui::Separator();
            ImGui::Checkbox(uiText(chinese, "外圈", "Outer circle"), &settings_.showCrosshairRing);
            ImGui::SliderFloat(uiText(chinese, "外圈半徑", "Outer circle radius"), &settings_.ringRadius, 2.0f, 500.0f, "%.0f px");
            ImGui::SliderFloat(uiText(chinese, "外圈透明度", "Outer circle opacity"), &settings_.ringOpacity, 0.0f, 1.0f);
            ImGui::SliderFloat(uiText(chinese, "外圈線條粗細", "Outer circle thickness"), &settings_.ringThickness, 0.5f, 8.0f, "%.1f px");
            colorEdit(uiText(chinese, "外圈顏色", "Outer circle color"), settings_.ringColor);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(uiText(chinese, "設定", "Settings"))) {
            const char* languages[] = {"繁體中文", "English"};
            int selectedLanguage = settings_.guiLanguage;
            if (ImGui::Combo(uiText(chinese, "介面語言", "Interface language"), &selectedLanguage, languages, 2)) {
                settings_.guiLanguage = selectedLanguage;
                saveUiPreferences(settings_);
            }
            const char* themes[] = {"深色", "白色 + 藍色"};
            if (!chinese) { themes[0] = "Dark"; themes[1] = "White + Blue"; }
            int selectedTheme = settings_.guiTheme;
            if (ImGui::Combo(uiText(chinese, "介面風格", "Interface theme"), &selectedTheme, themes, 2)) {
                settings_.guiTheme = selectedTheme;
                saveUiPreferences(settings_);
            }
            ImGui::TextWrapped(uiText(chinese, "語言與主題設定會保存在執行檔旁的 mcp-esp-ui.ini。", "Language and theme settings are saved in mcp-esp-ui.ini beside the executable."));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    ImGui::TextUnformatted(uiText(chinese, "Insert：開關選單／穿透點擊  |  End：結束程式", "Insert: toggle menu / click-through  |  End: quit"));
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
    WorldSnapshot snapshot = poller_.latest();
    poller_.refreshCamera(snapshot);
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
             << " snapshotAgeMs=" << snapshot.snapshotAgeMs << " cameraAgeMs=" << snapshot.cameraAgeMs
             << " renderFps=" << renderFps_
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
