#pragma once

#include "data_source.hpp"
#include "types.hpp"
#include "draw_esp.hpp"
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <atomic>
#include <vector>

namespace esp {

class OverlayApp {
public:
    explicit OverlayApp(SnapshotPoller& poller);
    ~OverlayApp();
    OverlayApp(const OverlayApp&) = delete;
    OverlayApp& operator=(const OverlayApp&) = delete;
    int run(HINSTANCE instance);
    void requestStop() noexcept { stopRequested_.store(true); }

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static BOOL CALLBACK findGameWindowProc(HWND hwnd, LPARAM parameter);
    HWND findGameWindow(const WorldSnapshot& snapshot) const;
    void syncToGameWindow(const WorldSnapshot& snapshot);
    bool createDevice();
    void resize(UINT width, UINT height);
    void setInteractive(bool enabled);
    void updateMousePassThrough();
    void renderGui(const WorldSnapshot& snapshot);
    void renderFrame();

    SnapshotPoller& poller_;
    Settings settings_{};
    HWND hwnd_ = nullptr;
    HWND gameHwnd_ = nullptr;
    RECT lastGameClientRect_{};
    bool haveGameClientRect_ = false;
    bool warnedGameWindowMissing_ = false;
    std::vector<RECT> guiHitRects_; // Main panel plus active ImGui popups.
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain1* swapChain_ = nullptr;
    IDCompositionDevice* compositionDevice_ = nullptr;
    IDCompositionTarget* compositionTarget_ = nullptr;
    IDCompositionVisual* compositionVisual_ = nullptr;
    ID3D11RenderTargetView* target_ = nullptr;
    UINT width_ = 0, height_ = 0;
    bool running_ = true;
    std::atomic_bool stopRequested_{false};
    bool interactive_ = true;
    int appliedGuiTheme_ = -1;
    DrawStats drawStats_{};
    RadarStats radarStats_{};
    double renderFrameMs_ = 0.0;
    double renderFps_ = 0.0;
    std::chrono::steady_clock::time_point previousFrameAt_{};
};

} // namespace esp
