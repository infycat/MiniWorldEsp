#pragma once

#include "types.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace esp {

// Visualization rigs are generic, game-agnostic skeletons used to draw a
// plausible stick figure when a data source does not supply explicit bone
// coordinates. They are an OFFLINE / DEMO aid only and never invent live game
// positions. Live bones must be supplied as real world-space coordinates
// (for example, exported by an in-process accessor and fed through
// JsonSnapshotSource).
enum class RigKind { Humanoid, Creature };

struct Rig {
    std::unordered_map<std::string, Vec3> bones;
    std::vector<std::pair<std::string, std::string>> links;
};

// Build a generic biped rig in world space. `root` is the entity's world-space
// box center; `scale` is its half-height. The same topology is used for both
// kinds because the demo creature sample shares the humanoid link chain.
Rig buildRig(RigKind kind, Vec3 root, float scale);

} // namespace esp
