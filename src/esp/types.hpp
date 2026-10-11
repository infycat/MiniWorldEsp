#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace esp {

struct Vec2 { float x = 0.0f, y = 0.0f; };
struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
struct Vec4 { float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f; };

// Matrices are 4x4 column-major, matching the snapshot contract.
struct Mat4 { std::array<float, 16> v{}; };

struct Entity {
    std::string id;
    std::string name;
    std::string kind = "unknown";      // player | creature | unknown
    std::string relation = "neutral";  // friend | enemy | neutral
    Vec3 position{};                   // world-space center
    Vec3 halfExtents{0.3f, 0.9f, 0.3f};
    bool boundsAvailable = true;
    Vec3 velocity{};
    std::optional<float> health;
    std::optional<float> maxHealth;
    std::optional<int> team;
    std::optional<bool> visible;
    std::unordered_map<std::string, Vec3> bones; // actual source positions only
    std::vector<std::pair<std::string, std::string>> boneLinks;
};

struct WorldObject {
    std::string id;
    std::string name;
    std::string kind = "object";
    Vec3 position{};
    Vec3 halfExtents{0.25f, 0.25f, 0.25f};
    std::optional<bool> visible;
};

struct WorldSnapshot {
    int schemaVersion = 0;
    std::uint64_t sequence = 0;
    // Runtime-only sampling telemetry. These fields are not part of the JSON schema.
    double sourceReadMs = 0.0;
    double updateHz = 0.0;
    double snapshotAgeMs = 0.0;
    double cameraAgeMs = 0.0;
    std::chrono::steady_clock::time_point sampledAt{};
    std::chrono::steady_clock::time_point cameraSampledAt{};
    std::string source;
    std::string backend;
    Vec3 cameraPosition{};
    float unitsPerMeter = 1.0f; // live game coordinates use 100 units per meter; demo JSON uses meters
    Vec3 cameraForward{0.0f, 0.0f, 1.0f};
    Vec3 cameraRight{1.0f, 0.0f, 0.0f};
    // Mirrors mianbao.lua: WorldMgr:getGameRuleValue(GMRULE_ATTACKPLAYER, 0).
    // 1 = no player attacks; 2 = team attacks; other values = free-for-all.
    std::optional<int> attackPlayerRule;
    Mat4 viewMatrix{};
    Mat4 projectionMatrix{};
    std::optional<int> localTeam;
    std::string localPlayerId;
    std::vector<Entity> entities;
    std::vector<WorldObject> objects;
    struct LiveTelemetry {
        bool available = false;
        std::uint32_t processId = 0;
        std::uint32_t playerController = 0;
        std::uint32_t positionComponent = 0;
        std::uint32_t world = 0;
        std::uint32_t actorManager = 0;
        std::uint32_t worldRenderer = 0;
        std::uint32_t renderCamera = 0;
        std::uint32_t actorListBegin = 0;
        std::uint32_t actorListEnd = 0;
        std::uint32_t clientActorListBegin = 0;
        std::uint32_t clientActorListEnd = 0;
        std::uint32_t clientActorSlots = 0;
        std::uint32_t clientActorCount = 0;
        std::uint32_t liveActorCount = 0;
        std::uint32_t actorSlots = 0;
        std::uint32_t invalidActorPointers = 0;
        std::uint32_t missingPositionComponents = 0;
        std::uint32_t positionReadFailures = 0;
        std::uint32_t invalidPositions = 0;
        std::uint32_t playerCount = 0;
        std::uint32_t playerNamesRead = 0;
        std::uint32_t playerNameReadFailures = 0;
        std::uint32_t creatureCount = 0;
        std::uint32_t healthReadCount = 0;
        std::uint32_t bodyPointerCount = 0;
        bool actorListValid = false;
        bool actorListPointersRead = false;
        bool clientActorListValid = false;
        bool clientActorListPointersRead = false;
        bool actorManagerRead = false;
        bool rendererRead = false;
        bool cameraRead = false;
        bool gameCameraRead = false;
        bool viewMatrixRead = false;
        bool projectionMatrixRead = false;
        bool lookAnglesAvailable = false;
        bool matrixAvailable = false;
        std::array<std::int32_t, 3> playerPositionRaw{};
        std::array<float, 2> playerLookAnglesRaw{};
        std::uint32_t unknownCount = 0;
    } live{};
    bool demoData = false;
    bool valid = false;
};

struct Settings {
    bool guiVisible = true;
    int guiTheme = 0; // 0 dark, 1 white/blue
    int guiLanguage = 0; // 0 Traditional Chinese, 1 English
    bool showPlayers = true;
    bool showCreatures = true;
    bool showUnknown = true;
    bool showNeutral = true;
    bool showFriends = true;
    bool showEnemies = true;
    bool showObjects = true;
    bool showObjectBox = true;
    bool showObjectFill = false;
    bool showObjectName = true;
    bool showObjectDistance = true;
    bool showBox = true;
    bool showBoxFill = false;
    bool showName = true;
    bool showDistance = true;
    bool showHealth = true;
    bool healthAsNumber = false;
    bool showAntenna = false;
    bool showSkeleton = true;
    // When a demo/offline entity has no explicit bone coordinates, synthesize a
    // generic biped rig so the skeleton renderer stays useful. Never applies to
    // live data; the renderer guards this with the snapshot's demoData flag.
    bool applyDefaultRigWhenMissing = true;
    bool showRadar = true;
    bool showCrosshair = true;
    bool showCrosshairRing = true;
    bool use3dBoxes = true;
    bool visibleOnly = false; // unknown visibility remains visible
    bool showEntityCount = true;
    float entityCountFontSize = 20.0f;
    float entityCountX = 14.0f;
    float entityCountY = 12.0f;
    int antennaAnchor = 2;    // 0 top, 1 bottom, 2 screen center (Lua ESP default)
    int crosshairStyle = 0;   // 0 cross, 1 circle, 2 corners, 3 animated

    float maxDistance = 0.0f; // 0 = unlimited
    float boxOpacity = 0.95f;
    float boxFillOpacity = 0.18f;
    float boxThickness = 1.6f;
    float skeletonOpacity = 0.95f;
    float skeletonThickness = 1.4f;
    float antennaOpacity = 1.0f;
    float antennaThickness = 2.0f;
    float textOpacity = 1.0f;
    float healthOpacity = 0.95f;
    float radarRadius = 100.0f;
    float radarX = -1.0f; // negative means right edge default
    float radarY = 24.0f;
    bool radarPositionOverride = false; // runtime-only: GUI position sliders changed this frame
    float radarRange = 50.0f; // meters
    float crosshairSize = 12.0f;
    float crosshairGap = 4.0f;
    float crosshairThickness = 1.7f;
    float crosshairOpacity = 0.95f;
    float ringRadius = 58.0f;
    // Keep the selected ring color at full brightness by default. Users can
    // lower opacity explicitly when they want it blended with the game scene.
    float ringOpacity = 1.0f;
    float ringThickness = 1.2f;
    float objectOpacity = 0.95f;
    float objectFillOpacity = 0.16f;
    float objectThickness = 1.4f;

    // ImGui colors are linear RGBA floats. A separate fill color lets users
    // tune face concentration without changing box edge visibility.
    std::array<float, 4> playerColor{0.20f, 0.80f, 1.00f, 1.00f};
    std::array<float, 4> friendColor{0.25f, 0.95f, 0.55f, 1.00f};
    std::array<float, 4> enemyColor{1.00f, 0.28f, 0.25f, 1.00f};
    std::array<float, 4> creatureColor{1.00f, 0.62f, 0.18f, 1.00f};
    std::array<float, 4> objectColor{0.94f, 0.77f, 0.32f, 1.00f};
    std::array<float, 4> skeletonColor{0.82f, 0.96f, 1.00f, 1.00f};
    std::array<float, 4> crosshairColor{1.00f, 1.00f, 1.00f, 1.00f};
    std::array<float, 4> ringColor{0.25f, 0.85f, 1.00f, 1.00f};
};

} // namespace esp
