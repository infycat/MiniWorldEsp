#include "esp/data_source.hpp"
#include "esp/diagnostics.hpp"
#include "esp/overlay_app.hpp"
#include "resource.h"

#include <shellapi.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
constexpr wchar_t kLauncherClass[] = L"MiniWorldEspLauncherWindow";
constexpr int kStartButton = 1001;
constexpr int kStopButton = 1002;
constexpr UINT kLaunchFinished = WM_APP + 10;

std::string utf8(const wchar_t* text) {
    if (!text) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}

struct LaunchOptions {
    enum class Source { Live, Demo, Json } source = Source::Live;
    std::string dataPath;
};

LaunchOptions launchOptions() {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    LaunchOptions options;
    for (int i = 1; args && i < count; ++i) {
        const std::wstring arg(args[i]);
        if (arg == L"--demo") {
            options.source = LaunchOptions::Source::Demo;
            options.dataPath = "sample_data.json";
        } else if (arg == L"--live") {
            options.source = LaunchOptions::Source::Live;
        } else if (arg == L"--data" && i + 1 < count) {
            options.source = LaunchOptions::Source::Json;
            options.dataPath = utf8(args[++i]);
        }
    }
    if (args) LocalFree(args);
    return options;
}

struct LauncherState {
    HINSTANCE instance = nullptr;
    LaunchOptions options;
    HWND window = nullptr;
    HWND status = nullptr;
    HWND start = nullptr;
    HWND stop = nullptr;
    std::thread worker;
    std::atomic_bool stopRequested{false};
    std::mutex appMutex;
    esp::OverlayApp* app = nullptr;
    bool active = false;
};

void setLauncherStatus(LauncherState& state, const wchar_t* text) {
    if (state.status) SetWindowTextW(state.status, text);
}

void stopOverlay(LauncherState& state) {
    state.stopRequested.store(true);
    std::lock_guard<std::mutex> lock(state.appMutex);
    if (state.app) state.app->requestStop();
}

void startOverlay(LauncherState& state) {
    if (state.active) return;
    if (state.worker.joinable()) state.worker.join();
    state.active = true;
    state.stopRequested.store(false);
    EnableWindow(state.start, FALSE);
    EnableWindow(state.stop, TRUE);
    setLauncherStatus(state, L"ESP 正在啟動… / Starting ESP…");

    state.worker = std::thread([&state]() {
        int result = 0;
        try {
            std::unique_ptr<esp::IDataSource> source;
            if (state.options.source == LaunchOptions::Source::Live)
                source = std::make_unique<esp::LiveProcessSource>();
            else
                source = std::make_unique<esp::JsonSnapshotSource>(state.options.dataPath);
            esp::SnapshotPoller poller(std::move(source));
            esp::OverlayApp app(poller);
            {
                std::lock_guard<std::mutex> lock(state.appMutex);
                state.app = &app;
                if (state.stopRequested.load()) app.requestStop();
            }
            result = app.run(state.instance);
            {
                std::lock_guard<std::mutex> lock(state.appMutex);
                state.app = nullptr;
            }
        } catch (const std::exception& error) {
            {
                std::lock_guard<std::mutex> lock(state.appMutex);
                state.app = nullptr;
            }
            esp::Diagnostics::instance().write("ERROR", "launcher", error.what());
            result = 1;
        }
        PostMessageW(state.window, kLaunchFinished, static_cast<WPARAM>(result), 0);
    });
}

LRESULT CALLBACK launcherProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<LauncherState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<LauncherState*>(create->lpCreateParams);
        state->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, message, wp, lp);

    switch (message) {
    case WM_CREATE:
        CreateWindowExW(0, L"STATIC", L"選擇操作 / Choose an action",
                        WS_CHILD | WS_VISIBLE, 22, 22, 380, 30, hwnd, nullptr,
                        state->instance, nullptr);
        state->start = CreateWindowExW(0, L"BUTTON", L"開啟 ESP / Start ESP",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        22, 67, 178, 42, hwnd, reinterpret_cast<HMENU>(kStartButton),
                        state->instance, nullptr);
        state->stop = CreateWindowExW(0, L"BUTTON", L"關閉 ESP / Close ESP",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        218, 67, 178, 42, hwnd, reinterpret_cast<HMENU>(kStopButton),
                        state->instance, nullptr);
        EnableWindow(state->stop, FALSE);
        state->status = CreateWindowExW(0, L"STATIC", L"ESP 尚未開啟 / ESP is stopped",
                        WS_CHILD | WS_VISIBLE | SS_LEFT, 22, 124, 380, 30, hwnd,
                        nullptr, state->instance, nullptr);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kStartButton) startOverlay(*state);
        else if (LOWORD(wp) == kStopButton && state->active) {
            EnableWindow(state->stop, FALSE);
            setLauncherStatus(*state, L"正在停止 ESP… / Stopping ESP…");
            stopOverlay(*state);
        }
        return 0;
    case kLaunchFinished: {
        if (state->worker.joinable()) state->worker.join();
        state->active = false;
        EnableWindow(state->start, TRUE);
        EnableWindow(state->stop, FALSE);
        if (wp == 0) setLauncherStatus(*state, L"ESP 已停止 / ESP is stopped");
        else setLauncherStatus(*state, L"ESP 啟動失敗，請查看 logs/esp.log / Startup failed; see logs/esp.log");
        return 0;
    }
    case WM_CLOSE:
        stopOverlay(*state);
        if (state->worker.joinable()) state->worker.join();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

int runLauncher(HINSTANCE instance, const LaunchOptions& options) {
    LauncherState state;
    state.instance = instance;
    state.options = options;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = launcherProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kLauncherClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    HWND window = CreateWindowExW(0, kLauncherClass, L"MiniWorld ESP 控制台 / Launcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 205, nullptr, nullptr, instance, &state);
    if (!window) return 2;
    RECT bounds{};
    GetWindowRect(window, &bounds);
    const int x = (GetSystemMetrics(SM_CXSCREEN) - (bounds.right - bounds.left)) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - (bounds.bottom - bounds.top)) / 2;
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (state.worker.joinable()) {
        stopOverlay(state);
        state.worker.join();
    }
    return static_cast<int>(message.wParam);
}
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetLastError(ERROR_SUCCESS);
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE, L"Local\\MiniWorldEsp.Singleton");
    if (!singleInstance) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"MiniWorld ESP 已經開啟。請使用現有視窗控制。\nMiniWorld ESP is already running.",
                    L"MiniWorld ESP", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        CloseHandle(singleInstance);
        return 0;
    }

    esp::Diagnostics::instance().initialize();
    const LaunchOptions options = launchOptions();
    esp::Diagnostics::instance().write("INFO", "startup", "launcher ready; source=" +
        std::string(options.source == LaunchOptions::Source::Live ? "live-process" :
                    options.source == LaunchOptions::Source::Demo ? "demo-json" : "json-file"));
    const int result = runLauncher(instance, options);
    CloseHandle(singleInstance);
    return result;
}
