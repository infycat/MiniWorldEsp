#include "esp/data_source.hpp"
#include "esp/diagnostics.hpp"
#include "esp/overlay_app.hpp"

#include <shellapi.h>
#include <memory>
#include <string>

namespace {
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
    LaunchOptions options; // live process is the normal, non-demo source
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
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    esp::Diagnostics::instance().initialize();
    const LaunchOptions options = launchOptions();
    esp::Diagnostics::instance().write("INFO", "startup", "MCP ESP diagnostics started; source=" +
        std::string(options.source == LaunchOptions::Source::Live ? "live-process" :
                    options.source == LaunchOptions::Source::Demo ? "demo-json" : "json-file"));
    std::unique_ptr<esp::IDataSource> source;
    if (options.source == LaunchOptions::Source::Live)
        source = std::make_unique<esp::LiveProcessSource>();
    else
        source = std::make_unique<esp::JsonSnapshotSource>(options.dataPath);
    esp::SnapshotPoller poller(std::move(source));
    esp::OverlayApp app(poller);
    return app.run(instance);
}
