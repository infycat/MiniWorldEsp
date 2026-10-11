#pragma once

#include <filesystem>
#include <mutex>
#include <string>

namespace esp {

class Diagnostics {
public:
    static Diagnostics& instance();
    void initialize();
    void write(const char* level, const char* component, const std::string& message);
    std::filesystem::path path() const;

private:
    Diagnostics() = default;
    mutable std::mutex mutex_;
    std::filesystem::path path_;
};

} // namespace esp
