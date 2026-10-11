#include "data_source.hpp"
#include "diagnostics.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <TlHelp32.h>

#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace esp {
namespace {
constexpr std::uintptr_t kPlayerControllerGlobalRva = 0x027772F8;
constexpr std::uintptr_t kPositionComponentOffset = 0x270;
constexpr std::uintptr_t kPositionXOffset = 0xDC;
// ClientPlayer::getNickName disassembly reads its std::string at +0x578
// (length +0x588, capacity +0x58c). Actor objects also carry their
// ClientPlayerN type label at +0xd0 in the current build.
constexpr std::uintptr_t kActorTypeLabelOffset = 0xD0;
constexpr std::uintptr_t kActorDisplayNameOffset = 0x210;
// CE-verified symbols: ClientActor::getAttrib returns [this+0x280];
// ActorAttrib::getHP reads [this+0x94], getMaxHP reads [this+0x9c].
constexpr std::uintptr_t kActorAttribOffset = 0x280;
constexpr std::uintptr_t kActorHpOffset = 0x94;
constexpr std::uintptr_t kActorMaxHpOffset = 0x9C;
constexpr std::uintptr_t kPlayerNicknameOffset = 0x578;
constexpr std::uintptr_t kPlayerNicknameLengthOffset = 0x588;
constexpr std::uintptr_t kPlayerNicknameCapacityOffset = 0x58C;
constexpr std::uintptr_t kWorldOffset = 0x238;
// ClientActor::getActorMgr adjusts `this` to World+0x20 before dispatching
// World::GetActorMgr, whose body reads [ecx+0x144]. The actual World-relative
// field is therefore +0x164.
constexpr std::uintptr_t kActorManagerOffset = 0x164;
constexpr std::uintptr_t kActorListBeginOffset = 0x14;
constexpr std::uintptr_t kActorListEndOffset = 0x18;
// ClientActorMgr::tick reads this process-wide begin/end pair and iterates
// each 32-bit actor pointer with `mov ecx,[edx+edi*4]`. CE verified the
// globals at libSandboxEngine.dll+0x25CB430/+0x25CB434 for this build.
constexpr std::uintptr_t kClientActorListBeginGlobalRva = 0x025CB430;
constexpr std::size_t kMaxLiveActors = 2048;
constexpr std::uintptr_t kWorldRendererOffset = 0x180;
constexpr std::uintptr_t kRenderCameraOffset = 0x98;
constexpr std::uintptr_t kPlayerGameCameraOffset = 0x950;
constexpr std::uintptr_t kGameCameraRenderCameraOffset = 0x60;
constexpr std::uintptr_t kCameraViewMatrixOffset = 0x44;
constexpr std::uintptr_t kCameraProjectionMatrixOffset = 0x84;
constexpr std::uintptr_t kLookAngleAOffset = 0xA8;
constexpr std::uintptr_t kLookAngleBOffset = 0xAC;

bool plausiblePointer(std::uint32_t value) {
    // MiniWorldApp is a 32-bit large-address-aware process. Its live heap
    // routinely allocates actors, vectors, and render cameras above 0x7FFF0000
    // (the current CE snapshot has actor storage at 0x8A47AFC8 and camera data
    // at 0x8A64C8D0). Reject only null/low pages and the top reserved range.
    return value >= 0x10000 && value < 0xFFF00000;
}

bool plausiblePosition(const std::array<std::int32_t, 3>& position) {
    constexpr std::int32_t kWorldCoordinateLimit = 10000000;
    for (const auto value : position)
        if (value < -kWorldCoordinateLimit || value > kWorldCoordinateLimit) return false;
    return true;
}

template <typename T>
bool readValue(HANDLE process, std::uintptr_t address, T& out);

bool readCameraMatrix(HANDLE process, std::uintptr_t address, Mat4& out) {
    for (std::size_t i = 0; i < out.v.size(); ++i) {
        if (!readValue(process, address + i * sizeof(float), out.v[i]) ||
            !std::isfinite(out.v[i]) || std::fabs(out.v[i]) > 1.0e7f) return false;
    }
    return true;
}

bool validViewMatrix(const Mat4& m) {
    const auto rowLength = [&](int row) {
        const float a = m.v[row], b = m.v[4 + row], c = m.v[8 + row];
        return std::sqrt(a*a + b*b + c*c);
    };
    return std::fabs(m.v[15] - 1.0f) < 0.02f &&
           rowLength(0) > 0.8f && rowLength(0) < 1.2f &&
           rowLength(1) > 0.8f && rowLength(1) < 1.2f &&
           rowLength(2) > 0.8f && rowLength(2) < 1.2f;
}

bool validProjectionMatrix(const Mat4& m) {
    return m.v[0] > 0.05f && m.v[0] < 10.0f &&
           m.v[5] > 0.05f && m.v[5] < 10.0f &&
           std::fabs(std::fabs(m.v[11]) - 1.0f) < 0.05f &&
           std::fabs(m.v[14]) > 0.01f;
}

Vec3 cameraEyeFromView(const Mat4& m) {
    return {-(m.v[0]*m.v[12] + m.v[1]*m.v[13] + m.v[2]*m.v[14]),
            -(m.v[4]*m.v[12] + m.v[5]*m.v[13] + m.v[6]*m.v[14]),
            -(m.v[8]*m.v[12] + m.v[9]*m.v[13] + m.v[10]*m.v[14])};
}

template <typename T>
bool readValue(HANDLE process, std::uintptr_t address, T& out) {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address),
                             &out, sizeof(T), &bytesRead) != FALSE && bytesRead == sizeof(T);
}

bool readBytes(HANDLE process, std::uintptr_t address, void* out, std::size_t size) {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address),
                             out, size, &bytesRead) != FALSE && bytesRead == size;
}

std::string readActorTypeLabel(HANDLE process, std::uintptr_t actor) {
    std::array<char, 32> bytes{};
    if (!readBytes(process, actor + kActorTypeLabelOffset, bytes.data(), bytes.size())) return {};
    std::size_t length = 0;
    while (length < bytes.size() && bytes[length] != '\0') {
        const unsigned char c = static_cast<unsigned char>(bytes[length]);
        if (c < 0x20 || c > 0x7e) return {};
        ++length;
    }
    if (length == 0 || length == bytes.size()) return {};
    return std::string(bytes.data(), length);
}

std::string readPlayerNickname(HANDLE process, std::uintptr_t actor) {
    std::uint32_t length = 0, capacity = 0;
    if (!readValue(process, actor + kPlayerNicknameLengthOffset, length) ||
        !readValue(process, actor + kPlayerNicknameCapacityOffset, capacity) ||
        length == 0 || length > 128 || capacity < length || capacity > 4096) return {};

    std::uintptr_t data = actor + kPlayerNicknameOffset;
    if (capacity >= 16) {
        std::uint32_t pointer = 0;
        if (!readValue(process, data, pointer) || !plausiblePointer(pointer)) return {};
        data = pointer;
    }
    std::string result(length, '\0');
    if (!readBytes(process, data, result.data(), length)) return {};
    for (unsigned char c : result) {
        if (c == 0 || c < 0x20) return {};
    }
    return result;
}

std::string readActorDisplayName(HANDLE process, std::uintptr_t actor) {
    // ClientActor::getActorName obtains [actor+0x210], then adds 0x10
    // and dereferences that field to get the UTF-8 C string.
    std::uint32_t nameObject = 0, stringData = 0;
    if (!readValue(process, actor + kActorDisplayNameOffset, nameObject) ||
        !plausiblePointer(nameObject) ||
        !readValue(process, static_cast<std::uintptr_t>(nameObject) + 0x10, stringData) ||
        !plausiblePointer(stringData)) return {};
    std::array<char, 96> bytes{};
    if (!readBytes(process, stringData, bytes.data(), bytes.size())) return {};
    std::size_t length = 0;
    while (length < bytes.size() && bytes[length] != '\0') {
        const unsigned char c = static_cast<unsigned char>(bytes[length]);
        if (c < 0x20) return {};
        ++length;
    }
    if (length == 0 || length == bytes.size()) return {};
    return std::string(bytes.data(), length);
}

bool readActorHealth(HANDLE process, std::uintptr_t actor, float& hp, float& maxHp) {
    std::uint32_t attrib = 0;
    if (!readValue(process, actor + kActorAttribOffset, attrib) || !plausiblePointer(attrib))
        return false;
    if (!readValue(process, static_cast<std::uintptr_t>(attrib) + kActorHpOffset, hp) ||
        !readValue(process, static_cast<std::uintptr_t>(attrib) + kActorMaxHpOffset, maxHp) ||
        !std::isfinite(hp) || !std::isfinite(maxHp) || hp < 0.0f || maxHp <= 0.0f ||
        hp > 1000000.0f || maxHp > 1000000.0f)
        return false;
    return true;
}

bool hasPrefix(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool isPlayerTypeLabel(const std::string& label) {
    if (!hasPrefix(label, "ClientPlayer")) return false;
    if (label.size() == std::strlen("ClientPlayer")) return false;
    for (std::size_t i = std::strlen("ClientPlayer"); i < label.size(); ++i)
        if (label[i] < '0' || label[i] > '9') return false;
    return true;
}

bool hasNumberedTypePrefix(const std::string& value, const char* prefix) {
    const std::size_t length = std::strlen(prefix);
    if (value.size() <= length || value.compare(0, length, prefix) != 0) return false;
    for (std::size_t i = length; i < value.size(); ++i)
        if (value[i] < '0' || value[i] > '9') return false;
    return true;
}

bool isCreatureTypeLabel(const std::string& label) {
    return hasNumberedTypePrefix(label, "ClientMob") ||
           hasNumberedTypePrefix(label, "ActorVillager") ||
           hasNumberedTypePrefix(label, "ActorHorse") ||
           hasNumberedTypePrefix(label, "ActorAnimal") ||
           hasNumberedTypePrefix(label, "ActorMonster") ||
           hasNumberedTypePrefix(label, "ActorMob");
}

bool isItemTypeLabel(const std::string& label) {
    return hasNumberedTypePrefix(label, "ClientItem");
}

std::uintptr_t findModuleBase(std::uint32_t pid, const wchar_t* moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::uintptr_t base = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return base;
}

std::uint32_t findProcess(const std::wstring& name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::uint32_t pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name.c_str()) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}
}

LiveProcessSource::LiveProcessSource(std::wstring processName)
    : processName_(std::move(processName)) {}

LiveProcessSource::~LiveProcessSource() { close(); }

void LiveProcessSource::close() {
    if (processHandle_) CloseHandle(static_cast<HANDLE>(processHandle_));
    processHandle_ = nullptr;
    processId_ = 0;
    moduleBase_ = 0;
}

bool LiveProcessSource::attach(std::string& error) {
    const std::uint32_t currentPid = findProcess(processName_);
    if (!currentPid) {
        error = "Waiting for MiniGameApp.exe";
        close();
        return false;
    }
    if (processHandle_ && currentPid == processId_) {
        // A process handle remains non-null after its process exits. If Windows
        // reuses the PID, never keep reading through the stale handle.
        const HANDLE existing = static_cast<HANDLE>(processHandle_);
        DWORD exitCode = 0;
        if (WaitForSingleObject(existing, 0) != WAIT_TIMEOUT ||
            !GetExitCodeProcess(existing, &exitCode) || exitCode != STILL_ACTIVE) {
            close();
        }
    }
    if (processHandle_ && currentPid == processId_) {
        // Resolve the DLL base from the current process module list instead of
        // keeping a one-time address. This handles unload/reload and remapping.
        const std::uintptr_t currentModule = findModuleBase(currentPid, L"libSandboxEngine.dll");
        if (!currentModule) {
            error = "libSandboxEngine.dll is not loaded in MiniGameApp.exe";
            moduleBase_ = 0;
            return false;
        }
        moduleBase_ = currentModule;
        error.clear();
        return true;
    }
    close();

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                 FALSE, currentPid);
    if (!process) {
        error = "Cannot open MiniGameApp.exe for read-only memory access; Win32 error=" +
                std::to_string(GetLastError());
        return false;
    }
    const std::uintptr_t module = findModuleBase(currentPid, L"libSandboxEngine.dll");
    if (!module) {
        CloseHandle(process);
        error = "libSandboxEngine.dll is not loaded in MiniGameApp.exe";
        return false;
    }
    processHandle_ = process;
    processId_ = currentPid;
    moduleBase_ = module;
    error.clear();
    return true;
}

bool LiveProcessSource::read(WorldSnapshot& out, std::string& error) {
    if (!attach(error)) return false;
    const HANDLE process = static_cast<HANDLE>(processHandle_);
    std::uint32_t player = 0;
    if (!readValue(process, moduleBase_ + kPlayerControllerGlobalRva, player)) {
        error = "ReadProcessMemory failed for player-controller global at 0x";
        std::ostringstream address;
        address << std::hex << (moduleBase_ + kPlayerControllerGlobalRva)
                << " (Win32 error=" << GetLastError() << ')';
        error += address.str();
        return false;
    }
    if (!plausiblePointer(player)) {
        std::ostringstream message;
        message << "Player-controller global contains invalid pointer 0x" << std::hex << player;
        error = message.str();
        return false;
    }

    std::uint32_t motion = 0;
    std::uint32_t world = 0;
    if (!readValue(process, player + kPositionComponentOffset, motion)) {
        error = "ReadProcessMemory failed for player position component at player+0x270 (Win32 error=" +
                std::to_string(GetLastError()) + ')';
        return false;
    }
    if (!plausiblePointer(motion)) {
        std::ostringstream message;
        message << "Player position component pointer is invalid: 0x" << std::hex << motion;
        error = message.str();
        return false;
    }
    if (!readValue(process, player + kWorldOffset, world)) {
        error = "ReadProcessMemory failed for World at player+0x238 (Win32 error=" +
                std::to_string(GetLastError()) + ')';
        return false;
    }
    if (!plausiblePointer(world)) {
        std::ostringstream message;
        message << "Player World pointer is invalid: 0x" << std::hex << world;
        error = message.str();
        return false;
    }

    std::array<std::int32_t, 3> rawPosition{};
    if (!readValue(process, motion + kPositionXOffset, rawPosition[0]) ||
        !readValue(process, motion + kPositionXOffset + 4, rawPosition[1]) ||
        !readValue(process, motion + kPositionXOffset + 8, rawPosition[2])) {
        error = "Cannot read the live player position";
        return false;
    }
    // World::GetViewerPosition converts these engine position integers directly
    // to floats (cvtdq2ps), so preserve those units without guessed scaling.
    const Vec3 playerPosition{static_cast<float>(rawPosition[0]),
                              static_cast<float>(rawPosition[1]),
                              static_cast<float>(rawPosition[2])};
    if (!std::isfinite(playerPosition.x) || !std::isfinite(playerPosition.y) ||
        !std::isfinite(playerPosition.z)) {
        error = "The live player position is invalid";
        return false;
    }

    WorldSnapshot next;
    next.schemaVersion = 1;
    next.source = "live-process";
    next.backend = "Win32 ReadProcessMemory (read only)";
    next.cameraPosition = playerPosition;
    next.unitsPerMeter = 100.0f;
    next.live.available = true;
    next.live.processId = processId_;
    next.live.playerController = player;
    next.live.positionComponent = motion;
    next.live.world = world;
    next.live.playerPositionRaw = rawPosition;
    const bool lookARead = readValue(process, motion + kLookAngleAOffset, next.live.playerLookAnglesRaw[0]);
    const bool lookBRead = readValue(process, motion + kLookAngleBOffset, next.live.playerLookAnglesRaw[1]);
    next.live.lookAnglesAvailable = lookARead && lookBRead &&
        std::isfinite(next.live.playerLookAnglesRaw[0]) && std::isfinite(next.live.playerLookAnglesRaw[1]);
    // The raw angles are useful diagnostics, but CE disassembly shows the
    // conversion's sin/cos terms do not match the assumed yaw/pitch basis.
    // Keep heading unavailable until the exact output mapping is validated.

    // Read both camera paths independently. getPointToScreen uses the player
    // control camera; the world renderer may temporarily hold a different
    // camera during transitions. A pointer mismatch alone must not suppress
    // every entity when either candidate still has a valid matrix pair.
    next.live.actorManagerRead = readValue(process, world + kActorManagerOffset, next.live.actorManager);
    next.live.rendererRead = readValue(process, world + kWorldRendererOffset, next.live.worldRenderer);
    if (plausiblePointer(next.live.worldRenderer))
        next.live.cameraRead = readValue(process, next.live.worldRenderer + kRenderCameraOffset, next.live.renderCamera);
    std::uint32_t gameCamera = 0, gameRenderCamera = 0;
    next.live.gameCameraRead = readValue(process, player + kPlayerGameCameraOffset, gameCamera) &&
                               plausiblePointer(gameCamera);
    if (next.live.gameCameraRead)
        readValue(process, gameCamera + kGameCameraRenderCameraOffset, gameRenderCamera);
    const bool cameraPathsAgree = next.live.cameraRead && plausiblePointer(next.live.renderCamera) &&
                                  gameRenderCamera == next.live.renderCamera;
    struct CameraCandidate { std::uint32_t address = 0; Mat4 view{}; Mat4 projection{}; bool valid = false; };
    auto readCameraCandidate = [&](std::uint32_t address) {
        CameraCandidate candidate;
        candidate.address = address;
        if (!plausiblePointer(address)) return candidate;
        const bool viewOk = readCameraMatrix(process,
            static_cast<std::uintptr_t>(address) + kCameraViewMatrixOffset, candidate.view) &&
            validViewMatrix(candidate.view);
        const bool projectionOk = readCameraMatrix(process,
            static_cast<std::uintptr_t>(address) + kCameraProjectionMatrixOffset, candidate.projection) &&
            validProjectionMatrix(candidate.projection);
        candidate.valid = viewOk && projectionOk;
        return candidate;
    };
    // Prefer the same camera used by PlayerControl::getPointToScreen. If that
    // candidate is transiently unavailable, use the validated renderer camera.
    CameraCandidate selectedCamera = readCameraCandidate(gameRenderCamera);
    const char* cameraSource = "player-control";
    if (!selectedCamera.valid) {
        CameraCandidate rendererCamera = readCameraCandidate(next.live.renderCamera);
        if (rendererCamera.valid) {
            selectedCamera = rendererCamera;
            cameraSource = "world-renderer-fallback";
        }
    }
    next.live.viewMatrixRead = selectedCamera.valid;
    next.live.projectionMatrixRead = selectedCamera.valid;
    next.live.matrixAvailable = selectedCamera.valid;
    if (selectedCamera.valid) {
        next.live.renderCamera = selectedCamera.address;
        next.viewMatrix = selectedCamera.view;
        next.projectionMatrix = selectedCamera.projection;
        next.cameraPosition = cameraEyeFromView(next.viewMatrix);
        next.cameraRight = {next.viewMatrix.v[0], next.viewMatrix.v[4], next.viewMatrix.v[8]};
        next.cameraForward = {-next.viewMatrix.v[2], -next.viewMatrix.v[6], -next.viewMatrix.v[10]};
    }

    // The World ActorManager list provides player actors. ClientActorMgr::tick
    // separately walks a process-wide list containing mobs and other client
    // actors, so collect both sources and deduplicate by the real actor pointer.
    std::unordered_set<std::uint32_t> seenActors;
    auto appendActor = [&](std::uint32_t actor) {
        if (!plausiblePointer(actor)) {
            ++next.live.invalidActorPointers;
            return false;
        }
        if (actor == player || seenActors.find(actor) != seenActors.end()) return false;

        std::uint32_t component = 0;
        if (!readValue(process, actor + kPositionComponentOffset, component) ||
            !plausiblePointer(component)) {
            ++next.live.missingPositionComponents;
            return false;
        }
        std::array<std::int32_t, 3> position{};
        if (!readValue(process, component + kPositionXOffset, position[0]) ||
            !readValue(process, component + kPositionXOffset + 4, position[1]) ||
            !readValue(process, component + kPositionXOffset + 8, position[2])) {
            ++next.live.positionReadFailures;
            return false;
        }
        if (!plausiblePosition(position)) {
            ++next.live.invalidPositions;
            return false;
        }

        const std::string typeLabel = readActorTypeLabel(process, actor);
        const bool isPlayer = isPlayerTypeLabel(typeLabel);
        const bool isCreature = isCreatureTypeLabel(typeLabel);
        const bool isItem = isItemTypeLabel(typeLabel);
        Entity entity;
        std::ostringstream id;
        id << "actor-0x" << std::hex << actor;
        entity.id = id.str();
        entity.kind = isPlayer ? "player" : (isCreature ? "creature" : (isItem ? "object" : "unknown"));
        if (isPlayer) {
            // Read the same nickname string returned by the verified
            // ClientPlayer::getNickName accessor on every snapshot.
            entity.name = readPlayerNickname(process, actor);
            if (entity.name.empty()) {
                entity.name = "Player " + typeLabel.substr(std::strlen("ClientPlayer"));
                ++next.live.playerNameReadFailures;
            } else {
                ++next.live.playerNamesRead;
            }
        } else {
            entity.name = readActorDisplayName(process, actor);
            if (entity.name.empty()) entity.name = typeLabel.empty() ? "Unknown actor" : typeLabel;
        }
        float hp = 0.0f, maxHp = 0.0f;
        if (!isItem && readActorHealth(process, actor, hp, maxHp)) {
            entity.health = hp;
            entity.maxHealth = maxHp;
            ++next.live.healthReadCount;
        }
        std::uint32_t body = 0;
        if (readValue(process, actor + 0x194, body) && plausiblePointer(body))
            ++next.live.bodyPointerCount;

        // Lua's BlockESP uses 0.35m x 0.35m x 0.35m cubes for dropped items.
        // Game positions are in centimeters; anchor the item cube at the
        // reported ground position, matching its Y..Y+height placement.
        entity.boundsAvailable = true;
        // Match mianbao.lua's entity bounds: players/creatures use a 0.82 m
        // footprint and 1.75 m height, with actor Y at the feet; items stay 0.35 m cubes.
        entity.halfExtents = isItem ? Vec3{17.5f, 17.5f, 17.5f}
                                    : Vec3{41.0f, 87.5f, 41.0f};
        entity.position = {static_cast<float>(position[0]),
                           static_cast<float>(position[1]) + (isItem ? 17.5f : 87.5f),
                           static_cast<float>(position[2])};
        seenActors.insert(actor);
        next.entities.push_back(std::move(entity));
        return true;
    };

    if (plausiblePointer(next.live.actorManager)) {
        std::uint32_t begin = 0, end = 0;
        const bool boundsRead =
            readValue(process, next.live.actorManager + kActorListBeginOffset, begin) &&
            readValue(process, next.live.actorManager + kActorListEndOffset, end);
        next.live.actorListPointersRead = boundsRead;
        if (boundsRead) {
            next.live.actorListBegin = begin;
            next.live.actorListEnd = end;
        }
        if (boundsRead && plausiblePointer(begin) && plausiblePointer(end) && end >= begin &&
            (end - begin) % sizeof(std::uint32_t) == 0 &&
            (end - begin) / sizeof(std::uint32_t) <= kMaxLiveActors) {
            next.live.actorListValid = true;
            const std::size_t count = (end - begin) / sizeof(std::uint32_t);
            next.live.actorSlots = static_cast<std::uint32_t>(count);
            std::vector<std::uint32_t> actors(count);
            if (count == 0 || readBytes(process, begin, actors.data(), count * sizeof(std::uint32_t))) {
                std::uint32_t verifyBegin = 0, verifyEnd = 0;
                if (readValue(process, next.live.actorManager + kActorListBeginOffset, verifyBegin) &&
                    readValue(process, next.live.actorManager + kActorListEndOffset, verifyEnd) &&
                    verifyBegin == begin && verifyEnd == end) {
                    for (const std::uint32_t actor : actors) appendActor(actor);
                } else {
                    next.live.actorListValid = false;
                }
            } else {
                next.live.actorListValid = false;
            }
        }
    }

    // This is the exact pointer array consumed by ClientActorMgr::tick at
    // libSandboxEngine.dll+0x5B4C60. It is a contiguous 4-byte actor-pointer
    // vector; re-read its bounds after copying to reject concurrent reallocation.
    const std::uintptr_t clientListGlobal = moduleBase_ + kClientActorListBeginGlobalRva;
    std::uint32_t clientBegin = 0, clientEnd = 0;
    const bool clientBoundsRead = readValue(process, clientListGlobal, clientBegin) &&
                                  readValue(process, clientListGlobal + sizeof(std::uint32_t), clientEnd);
    next.live.clientActorListPointersRead = clientBoundsRead;
    if (clientBoundsRead) {
        next.live.clientActorListBegin = clientBegin;
        next.live.clientActorListEnd = clientEnd;
    }
    if (clientBoundsRead && plausiblePointer(clientBegin) && plausiblePointer(clientEnd) &&
        clientEnd >= clientBegin && (clientEnd - clientBegin) % sizeof(std::uint32_t) == 0 &&
        (clientEnd - clientBegin) / sizeof(std::uint32_t) <= kMaxLiveActors) {
        const std::size_t count = (clientEnd - clientBegin) / sizeof(std::uint32_t);
        next.live.clientActorSlots = static_cast<std::uint32_t>(count);
        std::vector<std::uint32_t> actors(count);
        if ((count == 0 || readBytes(process, clientBegin, actors.data(), count * sizeof(std::uint32_t)))) {
            std::uint32_t verifyBegin = 0, verifyEnd = 0;
            if (readValue(process, clientListGlobal, verifyBegin) &&
                readValue(process, clientListGlobal + sizeof(std::uint32_t), verifyEnd) &&
                verifyBegin == clientBegin && verifyEnd == clientEnd) {
                next.live.clientActorListValid = true;
                for (const std::uint32_t actor : actors)
                    if (appendActor(actor)) ++next.live.clientActorCount;
            }
        }
    }

    next.live.liveActorCount = static_cast<std::uint32_t>(next.entities.size());
    for (const Entity& entity : next.entities) {
        if (entity.kind == "player") ++next.live.playerCount;
        else if (entity.kind == "creature" || entity.kind == "mob") ++next.live.creatureCount;
        else ++next.live.unknownCount;
    }

    // Rate-limited diagnostics preserve a useful history without flooding the
    // log at the 30 Hz memory-poll cadence.
    static auto lastSummary = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastSummary >= std::chrono::seconds(1)) {
        lastSummary = now;
        std::ostringstream line;
        line << "pid=" << processId_ << " module=0x" << std::hex << moduleBase_
             << " player=0x" << player << " motion=0x" << motion << " world=0x" << world
             << " actorManager=0x" << next.live.actorManager << " renderer=0x" << next.live.worldRenderer
             << " camera=0x" << next.live.renderCamera << std::dec
             << " playerPosRaw=" << rawPosition[0] << ',' << rawPosition[1] << ',' << rawPosition[2]
             << " lookAngles=";
        if (next.live.lookAnglesAvailable)
            line << next.live.playerLookAnglesRaw[0] << ',' << next.live.playerLookAnglesRaw[1];
        else line << "unavailable";
        line << " actorListPointersRead=" << next.live.actorListPointersRead << " actorList=";
        if (next.live.actorListValid)
            line << "0x" << std::hex << next.live.actorListBegin << "..0x" << next.live.actorListEnd << std::dec;
        else line << "invalid";
        line << " slots=" << next.live.actorSlots << " validPositions=" << next.live.liveActorCount
             << " players=" << next.live.playerCount << " playerNamesRead=" << next.live.playerNamesRead
             << " playerNameFailures=" << next.live.playerNameReadFailures
             << " creatures=" << next.live.creatureCount << " unknown=" << next.live.unknownCount
             << " healthRead=" << next.live.healthReadCount
             << " bodyPointers=" << next.live.bodyPointerCount
             << " clientActorListRead=" << next.live.clientActorListPointersRead
             << " clientActorListValid=" << next.live.clientActorListValid
             << " clientActorList=0x" << std::hex << next.live.clientActorListBegin
             << "..0x" << next.live.clientActorListEnd << std::dec
             << " clientActorSlots=" << next.live.clientActorSlots
             << " clientActorsAdded=" << next.live.clientActorCount
             << " invalidPointers=" << next.live.invalidActorPointers
             << " missingComponents=" << next.live.missingPositionComponents
             << " positionReadFailures=" << next.live.positionReadFailures
             << " invalidPositions=" << next.live.invalidPositions
             << " gameCamera=0x" << std::hex << gameCamera << " gameRenderCamera=0x" << gameRenderCamera
             << std::dec << " cameraPathsAgree=" << cameraPathsAgree
             << " cameraSource=" << (next.live.matrixAvailable ? cameraSource : "unavailable")
             << " cameraForward=" << next.cameraForward.x << ',' << next.cameraForward.y << ',' << next.cameraForward.z
             << " actorManagerRead=" << next.live.actorManagerRead
             << " rendererRead=" << next.live.rendererRead << " cameraRead=" << next.live.cameraRead
             << " viewMatrixRead=" << next.live.viewMatrixRead
             << " projectionMatrixRead=" << next.live.projectionMatrixRead
             << " matrixAvailable=" << next.live.matrixAvailable
             << " bounds=Lua-default-actor-fallback classification=runtime-type-label"
             << " teamSide=unavailable creatureSource=ClientActorMgr::tick";
        if (next.live.matrixAvailable) {
            line << " cameraEye=" << next.cameraPosition.x << ',' << next.cameraPosition.y << ','
                 << next.cameraPosition.z << " viewT=" << next.viewMatrix.v[12] << ','
                 << next.viewMatrix.v[13] << ',' << next.viewMatrix.v[14]
                 << " projScale=" << next.projectionMatrix.v[0] << ',' << next.projectionMatrix.v[5]
                 << " projDepth=" << next.projectionMatrix.v[10] << ',' << next.projectionMatrix.v[14];
        }
        for (std::size_t i = 0; i < next.entities.size() && i < 4; ++i) {
            std::uint32_t vtable = 0;
            std::uint32_t actorAddress = 0;
            if (std::sscanf(next.entities[i].id.c_str(), "actor-0x%x", &actorAddress) == 1)
                readValue(process, actorAddress, vtable);
            line << " sample[" << i << "]=" << next.entities[i].id << ":"
                 << next.entities[i].kind << ":" << next.entities[i].name << ":bounds="
                 << next.entities[i].boundsAvailable << ":"
                 << next.entities[i].position.x << ',' << next.entities[i].position.y << ','
                 << next.entities[i].position.z << ":vtable=0x" << std::hex << vtable << std::dec;
        }
        Diagnostics::instance().write("INFO", "live-source", line.str());
    }

    next.valid = true;
    out = std::move(next);
    error.clear();
    return true;
}

} // namespace esp
