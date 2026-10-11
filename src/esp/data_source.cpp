#include "data_source.hpp"
#include "diagnostics.hpp"

#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <utility>

namespace esp {
namespace {
using json = nlohmann::json;

std::optional<float> optionalNumber(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_number()) return std::nullopt;
    return it->get<float>();
}

Vec3 vec3(const json& j, Vec3 fallback = {}) {
    if (j.is_array() && j.size() >= 3 && j[0].is_number() && j[1].is_number() && j[2].is_number())
        return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    if (j.is_object() && j.contains("x") && j.contains("y") && j.contains("z") &&
        j["x"].is_number() && j["y"].is_number() && j["z"].is_number())
        return {j["x"].get<float>(), j["y"].get<float>(), j["z"].get<float>()};
    return fallback;
}

bool isVec3(const json& j) {
    return (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) ||
           (j.is_object() && j.contains("x") && j.contains("y") && j.contains("z") &&
            j["x"].is_number() && j["y"].is_number() && j["z"].is_number());
}

float length3(Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3 normalized(Vec3 v) {
    const float length = length3(v);
    if (length <= 1.0e-6f) return {};
    return {v.x / length, v.y / length, v.z / length};
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

std::string stringOr(const json& j, const char* key, std::string fallback = {}) {
    const auto it = j.find(key);
    if (it == j.end()) return fallback;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return fallback;
}

void readMatrix(const json& j, Mat4& out) {
    if (!j.is_array() || j.size() != 16) return;
    for (std::size_t i = 0; i < 16; ++i) {
        if (!j[i].is_number()) return;
    }
    for (std::size_t i = 0; i < 16; ++i) out.v[i] = j[i].get<float>();
}

bool matrixPresent(const Mat4& m) {
    for (float v : m.v) if (v != 0.0f) return true;
    return false;
}

Entity readEntity(const json& j, const char* forcedKind = nullptr) {
    Entity e;
    e.id = stringOr(j, "id");
    e.name = stringOr(j, "name", "(unnamed)");
    e.kind = forcedKind ? forcedKind : stringOr(j, "kind", stringOr(j, "type", "player"));
    e.relation = stringOr(j, "relation", "neutral");
    e.position = vec3(j.value("position", json::object()));
    e.velocity = vec3(j.value("velocity", json::object()));
    if (j.contains("halfExtents")) {
        e.halfExtents = vec3(j["halfExtents"], e.halfExtents);
    } else if (j.contains("bounds")) {
        // Legacy sample_data.json defines bounds as half extents.
        e.halfExtents = vec3(j["bounds"], e.halfExtents);
    } else if (j.contains("size")) {
        const Vec3 size = vec3(j["size"], {0.6f, 1.8f, 0.6f});
        e.halfExtents = {size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
    }
    e.health = optionalNumber(j, "health");
    e.maxHealth = optionalNumber(j, "maxHealth");
    if (!e.maxHealth) e.maxHealth = optionalNumber(j, "max_health");
    if (j.contains("team") && j["team"].is_number_integer()) e.team = j["team"].get<int>();
    if (j.contains("visible") && j["visible"].is_boolean()) e.visible = j["visible"].get<bool>();
    if (j.contains("bones") && j["bones"].is_object()) {
        for (auto it = j["bones"].begin(); it != j["bones"].end(); ++it)
            e.bones.emplace(it.key(), vec3(it.value()));
    }
    if (j.contains("bone_links") && j["bone_links"].is_array()) {
        for (const auto& link : j["bone_links"]) {
            if (link.is_array() && link.size() >= 2 && link[0].is_string() && link[1].is_string())
                e.boneLinks.emplace_back(link[0].get<std::string>(), link[1].get<std::string>());
        }
    }
    return e;
}

WorldObject readObject(const json& j) {
    WorldObject o;
    o.id = stringOr(j, "id");
    o.name = stringOr(j, "name", "Object");
    o.kind = stringOr(j, "kind", stringOr(j, "type", "object"));
    o.position = vec3(j.value("position", json::object()));
    if (j.contains("halfExtents")) o.halfExtents = vec3(j["halfExtents"], o.halfExtents);
    else if (j.contains("bounds")) o.halfExtents = vec3(j["bounds"], o.halfExtents);
    else if (j.contains("size")) {
        const Vec3 size = vec3(j["size"], {0.5f, 0.5f, 0.5f});
        o.halfExtents = {size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
    }
    if (j.contains("visible") && j["visible"].is_boolean()) o.visible = j["visible"].get<bool>();
    return o;
}

} // namespace

JsonSnapshotSource::JsonSnapshotSource(std::string path) : path_(std::move(path)) {}

bool JsonSnapshotSource::read(WorldSnapshot& out, std::string& error) {
    std::ifstream file(path_, std::ios::binary);
    if (!file) { error = "Cannot open snapshot file: " + path_; return false; }
    json root;
    try { file >> root; }
    catch (const std::exception& ex) { error = std::string("Invalid snapshot JSON: ") + ex.what(); return false; }
    if (!root.is_object()) { error = "Snapshot root must be an object"; return false; }

    WorldSnapshot next;
    next.schemaVersion = root.value("schemaVersion", 0);
    next.sequence = root.value("sequence", std::uint64_t{0});
    next.source = stringOr(root, "source", "json-file");
    next.backend = stringOr(root, "backend", next.source);
    next.localPlayerId = stringOr(root, "localPlayerId");
    next.demoData = root.contains("demo") && root["demo"].is_boolean() && root["demo"].get<bool>();
    if (root.contains("localTeam") && root["localTeam"].is_number_integer())
        next.localTeam = root["localTeam"].get<int>();
    if (root.contains("attackPlayerRule") && root["attackPlayerRule"].is_number_integer())
        next.attackPlayerRule = root["attackPlayerRule"].get<int>();

    const json camera = root.value("camera", json::object());
    if (!camera.is_object() || !camera.contains("position") || !isVec3(camera["position"]) ||
        !camera.contains("forward") || !isVec3(camera["forward"])) {
        error = "Snapshot requires the current player eye position and camera look direction";
        return false;
    }
    next.cameraPosition = vec3(camera.value("position", json::object()));
    next.cameraForward = normalized(vec3(camera["forward"]));
    if (length3(next.cameraForward) <= 1.0e-6f) {
        error = "Camera look direction must be a non-zero vector";
        return false;
    }
    if (camera.contains("right") && isVec3(camera["right"]))
        next.cameraRight = normalized(vec3(camera["right"]));
    if (length3(next.cameraRight) <= 1.0e-6f) {
        next.cameraRight = normalized(cross({0.0f, 1.0f, 0.0f}, next.cameraForward));
        if (length3(next.cameraRight) <= 1.0e-6f)
            next.cameraRight = normalized(cross({0.0f, 0.0f, 1.0f}, next.cameraForward));
    }
    readMatrix(root.value("viewMatrix", json::array()), next.viewMatrix);
    readMatrix(root.value("projectionMatrix", json::array()), next.projectionMatrix);
    if (!matrixPresent(next.viewMatrix) || !matrixPresent(next.projectionMatrix)) {
        error = "Snapshot requires 16-value viewMatrix and projectionMatrix";
        return false;
    }

    auto appendEntities = [&](const char* key, const char* forcedKind) {
        const auto it = root.find(key);
        if (it == root.end() || !it->is_array()) return;
        for (const auto& value : *it) if (value.is_object()) next.entities.push_back(readEntity(value, forcedKind));
    };
    appendEntities("entities", nullptr);
    // Also accept separate category lists when the producer uses them.
    if (next.entities.empty()) {
        appendEntities("players", "player");
        appendEntities("creatures", "creature");
    }
    if (root.contains("objects") && root["objects"].is_array())
        for (const auto& value : root["objects"]) if (value.is_object()) next.objects.push_back(readObject(value));

    next.valid = true;
    out = std::move(next);
    error.clear();
    return true;
}

SnapshotPoller::SnapshotPoller(std::unique_ptr<IDataSource> source,
                               std::chrono::milliseconds interval)
    : source_(std::move(source)), interval_(interval), worker_(&SnapshotPoller::run, this) {}

SnapshotPoller::~SnapshotPoller() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
}

void SnapshotPoller::run() {
    bool previouslyConnected = false;
    std::string previousError;
    auto lastSuccessfulSample = std::chrono::steady_clock::time_point{};
    while (!stop_.load()) {
        const auto begin = std::chrono::steady_clock::now();
        WorldSnapshot fresh;
        std::string error;
        bool readOk = false;
        try {
            if (!source_) {
                error = "No snapshot source is configured";
            } else {
                readOk = source_->read(fresh, error);
                if (!readOk && error.empty())
                    error = "Snapshot source read() returned false without an error";
                else if (readOk && !fresh.valid)
                    error = "Snapshot source read() succeeded but returned an invalid snapshot";
            }
        } catch (const std::exception& ex) {
            error = std::string("Snapshot adapter exception: ") + ex.what();
        } catch (...) {
            error = "Snapshot adapter threw a non-standard exception";
        }
        const auto sampledAt = std::chrono::steady_clock::now();
        const bool ok = readOk && fresh.valid;
        const std::string failure = error.empty() ? "Snapshot failed for an unspecified reason" : error;
        if (ok) {
            fresh.sourceReadMs = std::chrono::duration<double, std::milli>(sampledAt - begin).count();
            fresh.updateHz = lastSuccessfulSample == std::chrono::steady_clock::time_point{}
                ? 0.0
                : 1000.0 / std::max(0.001, std::chrono::duration<double, std::milli>(sampledAt - lastSuccessfulSample).count());
            fresh.sampledAt = sampledAt;
            fresh.snapshotAgeMs = 0.0;
            lastSuccessfulSample = sampledAt;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ok) {
                fresh.sequence = ++sequence_;
                latest_ = std::move(fresh);
                connected_ = true;
                lastError_.clear();
            } else {
                // Never keep rendering a stale world/player list after leaving
                // a room or losing the live process/read chain.
                latest_ = WorldSnapshot{};
                latest_.sequence = sequence_;
                connected_ = false;
                lastError_ = failure;
            }
        }
        if (ok) {
            if (!previouslyConnected)
                Diagnostics::instance().write("INFO", "snapshot", "source connected; snapshot polling recovered");
            previouslyConnected = true;
            previousError.clear();
        } else {
            if (previouslyConnected || failure != previousError)
                Diagnostics::instance().write("ERROR", "snapshot", failure);
            previouslyConnected = false;
            previousError = failure;
        }
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        if (elapsed < interval_) std::this_thread::sleep_for(interval_ - elapsed);
    }
}

WorldSnapshot SnapshotPoller::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    WorldSnapshot snapshot = latest_;
    if (snapshot.sampledAt != std::chrono::steady_clock::time_point{})
        snapshot.snapshotAgeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - snapshot.sampledAt).count();
    if (snapshot.cameraSampledAt != std::chrono::steady_clock::time_point{})
        snapshot.cameraAgeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - snapshot.cameraSampledAt).count();
    return snapshot;
}

bool SnapshotPoller::refreshCamera(WorldSnapshot& snapshot) {
    return source_ && source_->refreshCamera(snapshot);
}

std::string SnapshotPoller::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

bool SnapshotPoller::connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

} // namespace esp
