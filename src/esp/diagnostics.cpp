#include "diagnostics.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace esp {

Diagnostics& Diagnostics::instance() {
    static Diagnostics logger;
    return logger;
}

void Diagnostics::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!path_.empty()) return;

    std::filesystem::path base;
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        base = std::filesystem::path(executable).parent_path();
    if (base.empty()) base = std::filesystem::current_path();

    auto tryPath = [&](const std::filesystem::path& candidate) {
        std::error_code ec;
        std::filesystem::create_directories(candidate.parent_path(), ec);
        if (ec) return false;
        std::ofstream test(candidate, std::ios::app);
        if (!test) return false;
        path_ = candidate;
        return true;
    };

    const auto preferred = base / L"logs" / L"esp.log";
    if (!tryPath(preferred)) {
        wchar_t temp[MAX_PATH]{};
        const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
        const auto fallback = tempLength > 0 && tempLength < MAX_PATH
            ? std::filesystem::path(temp) / L"MiniWorldEsp" / L"esp.log"
            : std::filesystem::current_path() / L"esp.log";
        tryPath(fallback);
    }
}

void Diagnostics::write(const char* level, const char* component, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path_.empty()) {
        // Avoid recursive locking: initialization is deliberately simple and
        // normally called once by WinMain before worker threads start.
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream line;
    line << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << " ["
         << (level ? level : "INFO") << "] ["
         << (component ? component : "app") << "] " << message;
    const std::string text = line.str();

    std::error_code ec;
    constexpr std::uintmax_t kMaxLogSize = 5u * 1024u * 1024u;
    if (std::filesystem::exists(path_, ec) && !ec &&
        std::filesystem::file_size(path_, ec) >= kMaxLogSize && !ec) {
        std::filesystem::remove(path_.wstring() + L".1", ec);
        ec.clear();
        std::filesystem::rename(path_, path_.wstring() + L".1", ec);
    }
    std::ofstream file(path_, std::ios::app | std::ios::binary);
    if (file) file << text << "\r\n";
    OutputDebugStringA((text + "\n").c_str());
}

std::filesystem::path Diagnostics::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

} // namespace esp
