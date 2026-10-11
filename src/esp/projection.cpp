#include "projection.hpp"
#include <cmath>

namespace esp {
namespace {
Vec4 transform(const Mat4& m, Vec4 p) {
    return {
        m.v[0] * p.x + m.v[4] * p.y + m.v[8] * p.z + m.v[12] * p.w,
        m.v[1] * p.x + m.v[5] * p.y + m.v[9] * p.z + m.v[13] * p.w,
        m.v[2] * p.x + m.v[6] * p.y + m.v[10] * p.z + m.v[14] * p.w,
        m.v[3] * p.x + m.v[7] * p.y + m.v[11] * p.z + m.v[15] * p.w,
    };
}
}

std::optional<Vec2> worldToScreen(const WorldSnapshot& world, Vec3 point, int width, int height) {
    if (!world.valid || width <= 0 || height <= 0) return std::nullopt;
    const Vec4 view = transform(world.viewMatrix, {point.x, point.y, point.z, 1.0f});
    const Vec4 clip = transform(world.projectionMatrix, view);
    if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z) ||
        !std::isfinite(clip.w) || clip.w <= 1.0e-4f) return std::nullopt;
    if (clip.x < -clip.w || clip.x > clip.w || clip.y < -clip.w || clip.y > clip.w ||
        clip.z < 0.0f || clip.z > clip.w) return std::nullopt;
    const float nx = clip.x / clip.w;
    const float ny = clip.y / clip.w;
    return Vec2{(nx * 0.5f + 0.5f) * static_cast<float>(width),
                (0.5f - ny * 0.5f) * static_cast<float>(height)};
}

} // namespace esp
