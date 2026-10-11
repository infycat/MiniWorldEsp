#pragma once

#include "types.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace esp {

class IDataSource {
public:
    virtual ~IDataSource() = default;
    virtual bool read(WorldSnapshot& out, std::string& error) = 0;
    // Optional render-rate camera refresh; slow entity enumeration stays on
    // the background polling thread.
    virtual bool refreshCamera(WorldSnapshot&) { return false; }
};

// Reads a normalized JSON snapshot. It intentionally does not invent bones,
// health, teams, or visibility when a source does not provide those values.
class JsonSnapshotSource final : public IDataSource {
public:
    explicit JsonSnapshotSource(std::string path);
    bool read(WorldSnapshot& out, std::string& error) override;
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

// Read-only, build-specific telemetry reader. Offsets are documented against
// the CE-verified MiniWorld build; unavailable actor/projection data is left
// empty instead of being synthesized.
class LiveProcessSource final : public IDataSource {
public:
    explicit LiveProcessSource(std::wstring processName = L"MiniGameApp.exe");
    ~LiveProcessSource() override;
    bool read(WorldSnapshot& out, std::string& error) override;
    bool refreshCamera(WorldSnapshot& snapshot) override;
private:
    bool attach(std::string& error);
    void close();
    std::wstring processName_;
    void* processHandle_ = nullptr;
    std::uint32_t processId_ = 0;
    std::uintptr_t moduleBase_ = 0;
    // Independent handle/state so render-rate matrix reads never race the
    // snapshot worker's attach/detach lifecycle.
    void* cameraProcessHandle_ = nullptr;
    std::uint32_t cameraProcessId_ = 0;
    std::uintptr_t cameraModuleBase_ = 0;
    std::chrono::steady_clock::time_point nextCameraAttachAttempt_{};
};

// MCP calls and file reads stay off the render thread. The render loop only
// copies the most recent immutable snapshot, so a slow data provider cannot
// hitch DirectX rendering.
class SnapshotPoller {
public:
    explicit SnapshotPoller(std::unique_ptr<IDataSource> source,
                            std::chrono::milliseconds interval = std::chrono::milliseconds(16));
    ~SnapshotPoller();
    SnapshotPoller(const SnapshotPoller&) = delete;
    SnapshotPoller& operator=(const SnapshotPoller&) = delete;
    WorldSnapshot latest() const;
    std::string lastError() const;
    bool connected() const;
    bool refreshCamera(WorldSnapshot& snapshot);
private:
    void run();
    std::unique_ptr<IDataSource> source_;
    std::chrono::milliseconds interval_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    mutable std::mutex mutex_;
    WorldSnapshot latest_{};
    std::string lastError_ = "Waiting for snapshot";
    bool connected_ = false;
    std::uint64_t sequence_ = 0;
};

} // namespace esp
