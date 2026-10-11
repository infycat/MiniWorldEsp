#pragma once
#include "types.hpp"
#include <optional>

namespace esp {
std::optional<Vec2> worldToScreen(const WorldSnapshot& world, Vec3 point, int width, int height);
} // namespace esp
