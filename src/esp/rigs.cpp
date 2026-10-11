#include "rigs.hpp"

namespace esp {
namespace {

// Local-space bone offsets, expressed as fractions of the entity half-height
// (scale). Y grows upward from the box center. The X spread places shoulders,
// elbows, hands, hips, knees and feet at human proportions. These numbers are
// for visualization only and carry no game-specific meaning.
struct LocalBone { const char* name; float dx; float dy; };

constexpr LocalBone kHumanoid[] = {
    {"head",       0.00f,  0.98f},
    {"neck",       0.00f,  0.72f},
    {"chest",      0.00f,  0.49f},
    {"spine",      0.00f,  0.27f},
    {"pelvis",     0.00f,  0.07f},
    {"shoulderL", -0.27f,  0.51f},
    {"elbowL",    -0.42f,  0.24f},
    {"handL",     -0.51f,  0.01f},
    {"shoulderR",  0.27f,  0.51f},
    {"elbowR",     0.42f,  0.24f},
    {"handR",      0.51f,  0.01f},
    {"hipL",      -0.18f, -0.07f},
    {"kneeL",     -0.20f, -0.49f},
    {"footL",     -0.21f, -0.96f},
    {"hipR",       0.18f, -0.07f},
    {"kneeR",      0.20f, -0.49f},
    {"footR",      0.21f, -0.96f},
};
constexpr std::size_t kHumanoidCount = sizeof(kHumanoid) / sizeof(kHumanoid[0]);

const std::vector<std::pair<std::string, std::string>>& humanoidLinks() {
    static const std::vector<std::pair<std::string, std::string>> links = {
        {"head", "neck"}, {"neck", "chest"}, {"chest", "spine"}, {"spine", "pelvis"},
        {"chest", "shoulderL"}, {"shoulderL", "elbowL"}, {"elbowL", "handL"},
        {"chest", "shoulderR"}, {"shoulderR", "elbowR"}, {"elbowR", "handR"},
        {"pelvis", "hipL"}, {"hipL", "kneeL"}, {"kneeL", "footL"},
        {"pelvis", "hipR"}, {"hipR", "kneeR"}, {"kneeR", "footR"}
    };
    return links;
}

} // namespace

Rig buildRig(RigKind, Vec3 root, float scale) {
    Rig rig;
    for (std::size_t i = 0; i < kHumanoidCount; ++i) {
        const LocalBone& b = kHumanoid[i];
        rig.bones[b.name] = {root.x + b.dx * scale, root.y + b.dy * scale, root.z};
    }
    rig.links = humanoidLinks();
    return rig;
}

} // namespace esp
