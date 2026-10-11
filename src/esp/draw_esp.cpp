#include "draw_esp.hpp"
#include "projection.hpp"
#include "rigs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

namespace esp {
namespace {
constexpr float kPi = 3.14159265358979323846f;

ImU32 color(const std::array<float, 4>& rgba, float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(rgba[0], rgba[1], rgba[2],
                                                   std::clamp(rgba[3] * alpha, 0.0f, 1.0f)));
}

void drawOutlinedText(ImDrawList* draw, ImFont* font, float fontSize, ImVec2 pos,
                      ImU32 foreground, const char* text, float outlineRadius = 1.2f) {
    if (!text || !*text) return;
    const ImU32 foregroundAlpha = (foreground >> 24) & 0xff;
    const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(foregroundAlpha * 0.94f));
    const ImVec2 outlineOffsets[] = {
        {-outlineRadius, 0}, {outlineRadius, 0}, {0, -outlineRadius}, {0, outlineRadius},
        {-outlineRadius, -outlineRadius}, {outlineRadius, -outlineRadius},
        {-outlineRadius, outlineRadius}, {outlineRadius, outlineRadius}
    };
    for (const ImVec2& offset : outlineOffsets)
        draw->AddText(font, fontSize, ImVec2(pos.x + offset.x, pos.y + offset.y), outline, text);
    // Draw the glyph once to avoid subpixel overdraw making distant text look blurred.
    draw->AddText(font, fontSize, pos, foreground, text);
}

Vec4 transform(const Mat4& m, Vec4 p) {
    return {m.v[0]*p.x + m.v[4]*p.y + m.v[8]*p.z + m.v[12]*p.w,
            m.v[1]*p.x + m.v[5]*p.y + m.v[9]*p.z + m.v[13]*p.w,
            m.v[2]*p.x + m.v[6]*p.y + m.v[10]*p.z + m.v[14]*p.w,
            m.v[3]*p.x + m.v[7]*p.y + m.v[11]*p.z + m.v[15]*p.w};
}

Vec4 toClip(const WorldSnapshot& w, Vec3 p) {
    return transform(w.projectionMatrix,
                     transform(w.viewMatrix, {p.x, p.y, p.z, 1.0f}));
}

float plane(const Vec4& p, int index) {
    switch (index) {
        case 0: return p.x + p.w; // left
        case 1: return p.w - p.x; // right
        case 2: return p.y + p.w; // bottom
        case 3: return p.w - p.y; // top
        case 4: return p.z;       // near, D3D depth range [0,w]
        default: return p.w - p.z; // far
    }
}

Vec4 lerp(const Vec4& a, const Vec4& b, float t) {
    return {a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t,
            a.z + (b.z-a.z)*t, a.w + (b.w-a.w)*t};
}

bool clipLine(Vec4& a, Vec4& b) {
    for (int i = 0; i < 6; ++i) {
        const float da = plane(a, i), db = plane(b, i);
        if (da < 0.0f && db < 0.0f) return false;
        if ((da < 0.0f) != (db < 0.0f)) {
            const float den = da - db;
            if (std::fabs(den) < 1.0e-8f) return false;
            const Vec4 hit = lerp(a, b, da / den);
            if (da < 0.0f) a = hit; else b = hit;
        }
    }
    return a.w > 1.0e-5f && b.w > 1.0e-5f;
}

std::vector<Vec4> clipPolygon(std::vector<Vec4> polygon) {
    for (int i = 0; i < 6 && !polygon.empty(); ++i) {
        std::vector<Vec4> next;
        Vec4 prev = polygon.back();
        float dp = plane(prev, i);
        for (const Vec4& cur : polygon) {
            const float dc = plane(cur, i);
            const bool inPrev = dp >= 0.0f, inCur = dc >= 0.0f;
            if (inPrev != inCur) {
                const float den = dp - dc;
                if (std::fabs(den) > 1.0e-8f) next.push_back(lerp(prev, cur, dp / den));
            }
            if (inCur) next.push_back(cur);
            prev = cur;
            dp = dc;
        }
        polygon = std::move(next);
    }
    return polygon;
}

ImVec2 toScreen(const Vec4& p, int width, int height) {
    const float iw = 1.0f / p.w;
    return ImVec2((p.x * iw * 0.5f + 0.5f) * width,
                  (0.5f - p.y * iw * 0.5f) * height);
}

struct BoxGeometry {
    std::array<Vec4, 8> clip{};
    std::vector<std::pair<ImVec2, ImVec2>> edges;
    std::vector<std::vector<ImVec2>> faces;
    ImVec2 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    ImVec2 max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    bool any = false;
};

BoxGeometry projectBox(const WorldSnapshot& w, Vec3 center, Vec3 ext, int width, int height) {
    BoxGeometry g;
    for (int i = 0; i < 8; ++i) {
        const Vec3 p{center.x + ((i & 1) ? ext.x : -ext.x),
                     center.y + ((i & 2) ? ext.y : -ext.y),
                     center.z + ((i & 4) ? ext.z : -ext.z)};
        g.clip[i] = toClip(w, p);
    }
    // Clip all twelve edges independently. A corner behind the near plane no
    // longer makes the entire box disappear.
    for (int i = 0; i < 8; ++i) {
        for (int bit : {1, 2, 4}) {
            if (i & bit) continue;
            Vec4 a = g.clip[i], b = g.clip[i | bit];
            if (!clipLine(a, b)) continue;
            ImVec2 pa = toScreen(a, width, height), pb = toScreen(b, width, height);
            g.edges.emplace_back(pa, pb);
            for (const ImVec2& p : {pa, pb}) {
                g.min.x = std::min(g.min.x, p.x); g.min.y = std::min(g.min.y, p.y);
                g.max.x = std::max(g.max.x, p.x); g.max.y = std::max(g.max.y, p.y);
            }
            g.any = true;
        }
    }
    // Face polygons are clipped in homogeneous clip space before projection.
    static constexpr int faceIds[6][4] = {
        {0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
        {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}
    };
    for (const auto& face : faceIds) {
        std::vector<Vec4> poly{g.clip[face[0]], g.clip[face[1]], g.clip[face[2]], g.clip[face[3]]};
        poly = clipPolygon(std::move(poly));
        if (poly.size() < 3) continue;
        std::vector<ImVec2> screen;
        screen.reserve(poly.size());
        for (const auto& p : poly) screen.push_back(toScreen(p, width, height));
        g.faces.push_back(std::move(screen));
    }
    return g;
}

float distance(Vec3 a, Vec3 b) {
    const float x = a.x-b.x, y = a.y-b.y, z = a.z-b.z;
    return std::sqrt(x*x + y*y + z*z);
}

std::optional<bool> playerIsEnemy(const WorldSnapshot& w, const Entity& e);

const std::array<float, 4>& entityColor(const WorldSnapshot& w, const Entity& e, const Settings& s) {
    if (e.kind == "object") return s.objectColor;
    if (e.kind == "creature" || e.kind == "mob") return s.creatureColor;
    if (const auto enemy = playerIsEnemy(w, e); enemy.has_value())
        return *enemy ? s.enemyColor : s.friendColor;
    if (e.relation == "friend") return s.friendColor;
    if (e.relation == "enemy") return s.enemyColor;
    if (e.team && w.localTeam) return *e.team == *w.localTeam ? s.friendColor : s.enemyColor;
    return s.playerColor;
}

std::optional<bool> playerIsEnemy(const WorldSnapshot& w, const Entity& e) {
    if (e.kind != "player") return std::nullopt;
    if (w.attackPlayerRule && e.team && w.localTeam) {
        // Exact mianbao.lua rules:
        // rule 2: same non-zero team is friendly; zero-team players are hostile.
        // rule 1: player attacks disabled, so all players are friendly.
        // all other rule values: free-for-all, so all other players are hostile.
        if (*w.attackPlayerRule == 2)
            return !(*e.team == *w.localTeam && *w.localTeam != 0);
        if (*w.attackPlayerRule == 1) return false;
        return true;
    }
    if (e.relation == "enemy") return true;
    if (e.relation == "friend") return false;
    if (e.team && w.localTeam) return *e.team != *w.localTeam;
    return std::nullopt;
}

void drawAntenna(ImDrawList* draw, const WorldSnapshot& world, const Entity& entity,
                 const Settings& settings, const std::array<float, 4>& rgba,
                 int width, int height) {
    // Match mianbao.lua: anchor at the viewport's horizontal midpoint and end
    // at the target's projected world-space center.
    const auto target = worldToScreen(world, entity.position, width, height);
    if (!target) return; // Do not draw for behind/off-screen targets.
    float anchorY = static_cast<float>(height) * 0.5f;
    if (settings.antennaAnchor == 0) anchorY = 0.0f;
    else if (settings.antennaAnchor == 1) anchorY = static_cast<float>(height);
    draw->AddLine(ImVec2(static_cast<float>(width) * 0.5f, anchorY),
                  ImVec2(target->x, target->y), color(rgba, settings.antennaOpacity),
                  settings.antennaThickness);
}

struct HealthBarPlacement {
    ImVec2 topCenter{};
    ImVec2 bottomCenter{};
    bool valid = false;
};

void drawHealthQuad(ImDrawList* draw, ImVec2 bottom, ImVec2 top, float width,
                    ImU32 fill, float expand = 0.0f) {
    float dx = top.x - bottom.x, dy = top.y - bottom.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    dx /= len; dy /= len;
    const float nx = -dy, ny = dx;
    const float half = width * 0.5f + expand;
    const ImVec2 b0(bottom.x + nx * half, bottom.y + ny * half);
    const ImVec2 b1(bottom.x - nx * half, bottom.y - ny * half);
    const ImVec2 t1(top.x - nx * half, top.y - ny * half);
    const ImVec2 t0(top.x + nx * half, top.y + ny * half);
    const ImVec2 points[] = {b0, b1, t1, t0};
    draw->AddConvexPolyFilled(points, 4, fill);
}

HealthBarPlacement drawHealthBar(ImDrawList* draw, const BoxGeometry& box, float ratio,
                                 bool use3d, float opacity, int width, int height) {
    HealthBarPlacement placement;
    const float projectedHeight = std::max(1.0f, box.max.y - box.min.y);
    const float scale = projectedHeight / 120.0f;
    const float barWidth = std::clamp(4.0f * scale, 1.5f, 8.0f);
    const float barHeight = std::clamp(projectedHeight, 22.0f, 240.0f);
    ratio = std::clamp(ratio, 0.0f, 1.0f);

    ImVec2 bottom, top;
    if (use3d) {
        float rightEdgeX = -std::numeric_limits<float>::infinity();
        bool foundEdge = false;
        for (int i = 0; i < 8; ++i) {
            if (i & 2) continue;
            Vec4 a = box.clip[i], b = box.clip[i | 2];
            if (!clipLine(a, b)) continue;
            const ImVec2 pa = toScreen(a, width, height);
            const ImVec2 pb = toScreen(b, width, height);
            const float edgeX = (pa.x + pb.x) * 0.5f;
            if (edgeX > rightEdgeX) {
                rightEdgeX = edgeX;
                if (pa.y > pb.y) { bottom = pa; top = pb; }
                else { bottom = pb; top = pa; }
                foundEdge = true;
            }
        }
        if (!foundEdge) return placement;
        const ImVec2 edgeMid((bottom.x + top.x) * 0.5f, (bottom.y + top.y) * 0.5f);
        const ImVec2 boxMid((box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f);
        float outX = edgeMid.x - boxMid.x, outY = edgeMid.y - boxMid.y;
        const float outLen = std::sqrt(outX * outX + outY * outY);
        if (outLen > 1.0e-3f) { outX /= outLen; outY /= outLen; }
        else { outX = 1.0f; outY = 0.0f; }
        const float gap = std::clamp(5.0f * scale, 2.0f, 18.0f) + barWidth * 0.5f;
        bottom.x += outX * gap; bottom.y += outY * gap;
        top.x += outX * gap; top.y += outY * gap;
    } else {
        const float x = box.max.x + 5.0f + barWidth * 0.5f;
        bottom = ImVec2(x, box.max.y);
        top = ImVec2(x, box.max.y - barHeight);
    }

    // Match mianbao.lua: thin black outline, dark full-height backing, and
    // health fill rising from the bottom. In 3D the bar follows a projected box edge.
    drawHealthQuad(draw, bottom, top, barWidth, IM_COL32(0, 0, 0,
        static_cast<int>(255.0f * opacity)), 1.0f);
    drawHealthQuad(draw, bottom, top, barWidth, IM_COL32(28, 30, 34,
        static_cast<int>(230.0f * opacity)));
    const ImVec2 filledTop(bottom.x + (top.x - bottom.x) * ratio,
                           bottom.y + (top.y - bottom.y) * ratio);
    const ImVec4 low(0.98f, 0.17f, 0.17f, opacity);
    const ImVec4 mid(1.00f, 0.86f, 0.10f, opacity);
    const ImVec4 high(0.10f, 0.95f, 0.20f, opacity);
    ImVec4 hpRgba = low;
    if (ratio >= 0.80f) hpRgba = high;
    else if (ratio >= 0.55f) {
        const float t = (ratio - 0.55f) / 0.25f;
        hpRgba = ImVec4(mid.x + (high.x - mid.x) * t, mid.y + (high.y - mid.y) * t,
                        mid.z + (high.z - mid.z) * t, opacity);
    } else if (ratio >= 0.45f) hpRgba = mid;
    else if (ratio > 0.10f) {
        const float t = (ratio - 0.10f) / 0.35f;
        hpRgba = ImVec4(low.x + (mid.x - low.x) * t, low.y + (mid.y - low.y) * t,
                        low.z + (mid.z - low.z) * t, opacity);
    }
    const ImU32 hpColor = ImGui::ColorConvertFloat4ToU32(hpRgba);
    if (ratio > 0.0f) drawHealthQuad(draw, bottom, filledTop, barWidth, hpColor);
    placement.topCenter = top;
    placement.bottomCenter = bottom;
    placement.valid = true;
    return placement;
}

enum class EntityFilter { None, Kind, Relation, Visibility, Distance };

EntityFilter entityFilter(const WorldSnapshot& w, const Entity& e, const Settings& s) {
    if (e.kind == "player" && !s.showPlayers) return EntityFilter::Kind;
    if ((e.kind == "creature" || e.kind == "mob") && !s.showCreatures) return EntityFilter::Kind;
    if (e.kind == "object" && !s.showObjects) return EntityFilter::Kind;
    if (e.kind != "player" && e.kind != "creature" && e.kind != "mob" &&
        e.kind != "object" && !s.showUnknown)
        return EntityFilter::Kind;
    const std::optional<bool> playerEnemy = playerIsEnemy(w, e);
    if (playerEnemy.has_value())
        return *playerEnemy ? (s.showEnemies ? EntityFilter::None : EntityFilter::Relation)
                            : (s.showFriends ? EntityFilter::None : EntityFilter::Relation);
    const bool hasKnownRelation = e.relation == "friend" || e.relation == "enemy" ||
        (e.team.has_value() && w.localTeam.has_value());
    if (e.relation == "friend" || (e.team && w.localTeam && *e.team == *w.localTeam))
        return s.showFriends ? EntityFilter::None : EntityFilter::Relation;
    if (e.relation == "enemy" || (e.team && w.localTeam)) return s.showEnemies ? EntityFilter::None : EntityFilter::Relation;
    // A player with no team/relation metadata is not known to be neutral.
    // Keep rendering it so missing team metadata cannot hide enemy players.
    if (!hasKnownRelation && e.kind == "player") {
        // Continue through visibility and distance filters below.
    } else if (e.kind != "object" && !s.showNeutral) return EntityFilter::Relation;
    if (s.visibleOnly && e.visible && !*e.visible) return EntityFilter::Visibility;
    const float dMeters = distance(e.position, w.cameraPosition) / std::max(1.0f, w.unitsPerMeter);
    return s.maxDistance <= 0.0f || dMeters <= s.maxDistance ? EntityFilter::None : EntityFilter::Distance;
}

bool matrixAvailable(const WorldSnapshot& world) {
    bool view = false, projection = false;
    for (float x : world.viewMatrix.v) view = view || x != 0.0f;
    for (float x : world.projectionMatrix.v) projection = projection || x != 0.0f;
    return view && projection;
}

std::vector<std::pair<std::string, std::string>> defaultLinks() {
    return {{"head","neck"}, {"neck","chest"}, {"chest","spine"}, {"spine","pelvis"},
            {"chest","shoulderL"}, {"shoulderL","elbowL"}, {"elbowL","handL"},
            {"chest","shoulderR"}, {"shoulderR","elbowR"}, {"elbowR","handR"},
            {"pelvis","hipL"}, {"hipL","kneeL"}, {"kneeL","footL"},
            {"pelvis","hipR"}, {"hipR","kneeR"}, {"kneeR","footR"}};
}

// Draws a rig given an explicit bone map and link list (the generic path used
// by both the data-driven skeleton and the synthesized default rig).
void drawSkeletonRig(ImDrawList* draw, const WorldSnapshot& w,
                     const std::unordered_map<std::string, Vec3>& bones,
                     const std::vector<std::pair<std::string, std::string>>& links,
                     const Settings& s, int width, int height) {
    const ImU32 c = color(s.skeletonColor, s.skeletonOpacity);
    for (const auto& link : links) {
        const auto ia = bones.find(link.first), ib = bones.find(link.second);
        if (ia == bones.end() || ib == bones.end()) continue;
        Vec4 a = toClip(w, ia->second), b = toClip(w, ib->second);
        if (!clipLine(a, b)) continue;
        draw->AddLine(toScreen(a, width, height), toScreen(b, width, height), c, s.skeletonThickness);
    }
}

// Data-driven skeleton: uses the entity's explicit bone coordinates when
// present, falling back to the entity's own bone_links or the default humanoid
// chain. Kept separate from entity parsing so arbitrary creature rigs can
// provide their own bone_links.
void drawSkeletonProjected(ImDrawList* draw, const WorldSnapshot& w, const Entity& e,
                           const Settings& s, int width, int height) {
    const auto& links = e.boneLinks.empty() ? defaultLinks() : e.boneLinks;
    drawSkeletonRig(draw, w, e.bones, links, s, width, height);
}

std::string localizedEntityName(const Entity& entity, bool chinese) {
    const std::string& name = entity.name;
    const auto numberedSuffix = [&](const char* prefix) -> std::optional<std::string> {
        const std::size_t length = std::strlen(prefix);
        if (name.size() <= length || name.compare(0, length, prefix) != 0) return std::nullopt;
        for (std::size_t i = length; i < name.size(); ++i)
            if (name[i] < '0' || name[i] > '9') return std::nullopt;
        return name.substr(length);
    };
    if (name == "Unknown actor" || name == "Unknown Actor")
        return chinese ? "實體" : "Actor";
    if (name == "(unnamed)") return chinese ? "（未命名）" : name;
    if (name == "Object") return chinese ? "物件" : name;
    if (name == "Dropped item") return chinese ? "掉落物品" : name;
    if (name == "Local Player") return chinese ? "本地玩家" : name;
    if (name == "Forest Creature") return chinese ? "森林生物" : name;
    if (name == "Crate") return chinese ? "箱子" : name;
    if (name.size() > 7 && name.compare(0, 7, "Player ") == 0) {
        const bool digits = std::all_of(name.begin() + 7, name.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (digits) return (chinese ? "玩家 " : "Player ") + name.substr(7);
    }
    if (chinese) {
        if (const auto suffix = numberedSuffix("ClientItem")) return "客戶端物品" + *suffix;
        if (const auto suffix = numberedSuffix("ClientMob")) return "客戶端生物" + *suffix;
        if (const auto suffix = numberedSuffix("ActorVillager")) return "村民" + *suffix;
        if (const auto suffix = numberedSuffix("ActorHorse")) return "馬" + *suffix;
        if (const auto suffix = numberedSuffix("ActorAnimal")) return "動物" + *suffix;
        if (const auto suffix = numberedSuffix("ActorMonster")) return "怪物" + *suffix;
        if (const auto suffix = numberedSuffix("ActorMob")) return "生物" + *suffix;
    } else {
        if (const auto suffix = numberedSuffix("ClientItem")) return "Client Item " + *suffix;
        if (const auto suffix = numberedSuffix("ClientMob")) return "Client Creature " + *suffix;
        if (const auto suffix = numberedSuffix("ActorVillager")) return "Villager " + *suffix;
        if (const auto suffix = numberedSuffix("ActorHorse")) return "Horse " + *suffix;
        if (const auto suffix = numberedSuffix("ActorAnimal")) return "Animal " + *suffix;
        if (const auto suffix = numberedSuffix("ActorMonster")) return "Monster " + *suffix;
        if (const auto suffix = numberedSuffix("ActorMob")) return "Creature " + *suffix;
    }
    return name;
}

void drawEntity(ImDrawList* draw, const WorldSnapshot& w, const Entity& e,
                const Settings& s, int width, int height, DrawStats& stats) {
    switch (entityFilter(w, e, s)) {
        case EntityFilter::Kind: ++stats.filteredKind; return;
        case EntityFilter::Relation: ++stats.filteredRelation; return;
        case EntityFilter::Visibility: ++stats.filteredVisibility; return;
        case EntityFilter::Distance: ++stats.filteredDistance; return;
        case EntityFilter::None: break;
    }
    const bool player = e.kind == "player";
    const bool creature = e.kind == "creature" || e.kind == "mob";
    const bool object = e.kind == "object";
    const EntityVisualOptions& visual = player ? s.playerVisual
        : (creature ? s.creatureVisual : s.unknownVisual);
    const bool showBox = object ? s.showObjectBox : (s.showBox && visual.box);
    const bool showFill = object ? s.showObjectFill : (s.showBoxFill && visual.fill);
    const bool showAntenna = !object && s.showAntenna && visual.antenna;
    const bool showSkeleton = !object && s.showSkeleton && visual.skeleton;
    const bool showName = object ? s.showObjectName : (s.showName && visual.name);
    const bool showDistance = object ? s.showObjectDistance : (s.showDistance && visual.distance);
    const bool showHealth = !object && s.showHealth && visual.health && e.health.has_value();
    const float boxOpacity = object ? s.objectOpacity : s.boxOpacity;
    const float fillOpacity = object ? s.objectFillOpacity : s.boxFillOpacity;
    const float boxThickness = object ? s.objectThickness : s.boxThickness;
    const bool boundsReady = e.boundsAvailable;
    const bool matrixReady = matrixAvailable(w);
    if (!boundsReady) ++stats.missingBounds;
    if (!matrixReady) ++stats.missingMatrix;
    if (!matrixReady) return;
    if (!boundsReady) {
        // Verified live positions can still be projected. Keep them as point
        // markers until a class-specific AABB is read; never invent box sizes.
        const auto point = worldToScreen(w, e.position, width, height);
        if (!point) { ++stats.clipped; return; }
        const auto& rgba = entityColor(w, e, s);
        const ImU32 c = color(rgba, boxOpacity);
        if (showBox) draw->AddCircleFilled(ImVec2(point->x, point->y), 4.0f, c, 12);
        if (showAntenna) drawAntenna(draw, w, e, s, rgba, width, height);
        const float dMeters = distance(e.position, w.cameraPosition) / std::max(1.0f, w.unitsPerMeter);
        const float distanceScale = 1.0f + std::clamp(dMeters / 100.0f, 0.0f, 0.75f);
        const float pointFontSize = std::round(std::clamp(14.0f * distanceScale, 14.0f, 28.0f));
        ImFont* textFont = ImGui::GetFont();
        const ImU32 textColor = color(rgba, s.textOpacity);
        const std::string displayName = localizedEntityName(e, s.guiLanguage == 0);
        if (showName) drawOutlinedText(draw, textFont, pointFontSize,
                                         ImVec2(point->x + 6.0f, point->y - 8.0f),
                                         textColor, displayName.c_str());
        if (showDistance) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " %.1fm", dMeters);
            drawOutlinedText(draw, textFont, pointFontSize, ImVec2(point->x + 6.0f, point->y + 8.0f),
                             textColor, buf);
        }
        ++stats.drawn;
        return;
    }
    Vec3 boxExtents = e.halfExtents;
    const bool cubeEntity = e.kind == "object" || e.kind == "unknown";
    if (cubeEntity && s.use3dBoxes) {
        const float side = std::max({boxExtents.x, boxExtents.y, boxExtents.z});
        boxExtents = {side, side, side};
    }
    const BoxGeometry box = projectBox(w, e.position, boxExtents, width, height);
    if (!box.any) {
        // A target may have a valid projected center while its full bounds
        // fall outside the viewport. Keep the antenna useful in that case.
        const auto point = worldToScreen(w, e.position, width, height);
        if (showAntenna && point) {
            const auto& rgba = entityColor(w, e, s);
            drawAntenna(draw, w, e, s, rgba, width, height);
        }
        ++stats.clipped;
        return;
    }
    const ImVec2 center((box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f);
    const auto& rgba = entityColor(w, e, s);
    const ImU32 edge = color(rgba, boxOpacity);

    const bool use3dBox = s.use3dBoxes;
    ImVec2 boxMin = box.min;
    ImVec2 boxMax = box.max;
    if (cubeEntity && !use3dBox) {
        const float side = std::max(box.max.x - box.min.x, box.max.y - box.min.y);
        boxMin = ImVec2(center.x - side * 0.5f, center.y - side * 0.5f);
        boxMax = ImVec2(center.x + side * 0.5f, center.y + side * 0.5f);
    }
    if (showFill) {
        if (use3dBox) {
            const ImU32 fill = color(rgba, fillOpacity);
            for (const auto& face : box.faces) if (face.size() >= 3)
                draw->AddConvexPolyFilled(face.data(), static_cast<int>(face.size()), fill);
        } else {
            draw->AddRectFilled(boxMin, boxMax, color(rgba, fillOpacity));
        }
    }
    if (showBox) {
        if (use3dBox) {
            for (const auto& line : box.edges) draw->AddLine(line.first, line.second, edge, boxThickness);
        } else {
            draw->AddRect(boxMin, boxMax, edge, 0.0f, 0, boxThickness);
        }
    }

    if (showAntenna) {
        drawAntenna(draw, w, e, s, rgba, width, height);
    }
    if (showSkeleton) {
        if (!e.bones.empty()) {
            drawSkeletonProjected(draw, w, e, s, width, height);
        } else if (s.applyDefaultRigWhenMissing && w.demoData) {
            // Offline/demo entities may carry only a position. Synthesize a
            // generic biped rig so the skeleton renderer is still exercised.
            // This never runs for live data (guarded by w.demoData).
            const Rig rig = buildRig((e.kind == "creature" || e.kind == "mob")
                                         ? RigKind::Creature : RigKind::Humanoid,
                                     e.position, e.halfExtents.y);
            drawSkeletonRig(draw, w, rig.bones, rig.links, s, width, height);
        }
    }

    const bool drawHealth = showHealth;
    const bool drawHealthNumber = drawHealth && s.healthAsNumber;
    std::string label;
    if (showName) label = localizedEntityName(e, s.guiLanguage == 0);
    const float d = distance(e.position, w.cameraPosition) / std::max(1.0f, w.unitsPerMeter);
    if (showDistance) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%s%.1fm", label.empty() ? "" : "  ", d);
        label += buf;
    }
    char hp[32]{};
    if (drawHealthNumber) {
        const float maxHp = std::max(1.0f, e.maxHealth.value_or(100.0f));
        std::snprintf(hp, sizeof(hp), "  %.0f/%.0f", *e.health, maxHp);
        label += hp;
    }
    ImFont* labelFont = ImGui::GetFont();
    const float projectedHeight = box.max.y - box.min.y;
    const float overlayScale = std::clamp(projectedHeight / 80.0f, 0.85f, 2.5f);
    // Keep distant labels readable: the projected box naturally shrinks with
    // distance, so compensate gradually instead of letting the text collapse.
    const float distanceScale = 1.0f + std::clamp(d / 100.0f, 0.0f, 0.75f);
    // Round to whole pixels: fractional glyph sizes shimmer and soften at distance.
    const float labelFontSize = std::round(std::clamp(12.0f * overlayScale * distanceScale, 12.0f, 32.0f));
    const ImVec2 labelSize = label.empty() ? ImVec2(0, 0)
        : labelFont->CalcTextSizeA(labelFontSize, std::numeric_limits<float>::max(), 0.0f, label.c_str());
    const float labelY = std::max(2.0f, box.min.y - labelSize.y - 3.0f);

    if (!label.empty()) {
        // Scale nickname text with the projected target height, matching the
        // Lua ESP overlay's near-target enlargement while keeping a readable clamp.
        const float outlineRadius = std::clamp(labelFontSize * (d > 30.0f ? 0.075f : 0.095f), 0.85f, 1.35f);
        drawOutlinedText(draw, labelFont, labelFontSize,
                         ImVec2(center.x - labelSize.x * 0.5f, labelY),
                         color({1,1,1,1}, s.textOpacity), label.c_str(), outlineRadius);
    }

    if (drawHealth) {
        const float maxHp = std::max(1.0f, e.maxHealth.value_or(100.0f));
        const float ratio = std::clamp(*e.health / maxHp, 0.0f, 1.0f);
        drawHealthBar(draw, box, ratio, use3dBox, s.healthOpacity, width, height);
    }
    ++stats.drawn;
}

void drawObject(ImDrawList* draw, const WorldSnapshot& w, const WorldObject& o,
                const Settings& s, int width, int height, DrawStats& stats) {
    ++stats.objectsSeen;
    if (!s.showObjects || (s.visibleOnly && o.visible && !*o.visible)) return;
    if (s.maxDistance > 0.0f &&
        distance(o.position, w.cameraPosition) / std::max(1.0f, w.unitsPerMeter) > s.maxDistance) {
        ++stats.objectsFilteredDistance;
        return;
    }
    if (!matrixAvailable(w)) { ++stats.objectsMissingMatrix; return; }
    Vec3 boxExtents = o.halfExtents;
    if (s.use3dBoxes) {
        const float side = std::max({boxExtents.x, boxExtents.y, boxExtents.z});
        boxExtents = {side, side, side};
    }
    const BoxGeometry box = projectBox(w, o.position, boxExtents, width, height);
    if (!box.any) { ++stats.objectsClipped; return; }
    const ImU32 c = color(s.objectColor, s.objectOpacity);
    const ImVec2 center((box.min.x+box.max.x)*0.5f, (box.min.y+box.max.y)*0.5f);
    ImVec2 boxMin = box.min;
    ImVec2 boxMax = box.max;
    if (!s.use3dBoxes) {
        const float side = std::max(box.max.x - box.min.x, box.max.y - box.min.y);
        boxMin = ImVec2(center.x - side * 0.5f, center.y - side * 0.5f);
        boxMax = ImVec2(center.x + side * 0.5f, center.y + side * 0.5f);
    }
    if (s.showObjectFill) {
        const ImU32 fill = color(s.objectColor, s.objectFillOpacity);
        if (s.use3dBoxes) {
            for (const auto& face : box.faces) if (face.size() >= 3)
                draw->AddConvexPolyFilled(face.data(), static_cast<int>(face.size()), fill);
        } else {
            draw->AddRectFilled(boxMin, boxMax, fill);
        }
    }
    if (s.showObjectBox && s.use3dBoxes) {
        for (const auto& edge : box.edges) draw->AddLine(edge.first, edge.second, c, s.objectThickness);
    } else if (s.showObjectBox) {
        draw->AddRect(boxMin, boxMax, c, 0.0f, 0, s.objectThickness);
    }
    Entity objectLabel;
    objectLabel.kind = "object";
    objectLabel.name = o.name;
    std::string label = s.showObjectName
        ? localizedEntityName(objectLabel, s.guiLanguage == 0) : std::string{};
    if (s.showObjectDistance) {
        char buf[24]; std::snprintf(buf, sizeof(buf), " %.1fm",
                                    distance(o.position, w.cameraPosition) / std::max(1.0f, w.unitsPerMeter));
        label += buf;
    }
    if (!label.empty()) {
        ImFont* font = ImGui::GetFont();
        const float objectDistance = distance(o.position, w.cameraPosition) /
                                     std::max(1.0f, w.unitsPerMeter);
        const float fontSize = std::round(std::clamp(14.0f *
            (1.0f + std::clamp(objectDistance / 100.0f, 0.0f, 0.75f)), 14.0f, 30.0f));
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(),
                                                     0.0f, label.c_str());
        drawOutlinedText(draw, font, fontSize,
                         ImVec2(center.x - textSize.x * 0.5f,
                                std::max(2.0f, box.min.y - textSize.y - 3.0f)),
                         c, label.c_str());
    }
    ++stats.objectsDrawn;
}

} // namespace

DrawStats drawWorld(ImDrawList* draw, const WorldSnapshot& world, const Settings& settings,
                    int width, int height, float) {
    DrawStats stats;
    if (!world.valid) return stats;
    int playerCount = 0, creatureCount = 0, unknownCount = 0, liveObjectCount = 0;
    for (const Entity& e : world.entities) {
        if (e.kind == "player") ++playerCount;
        else if (e.kind == "creature" || e.kind == "mob") ++creatureCount;
        else if (e.kind == "object") ++liveObjectCount;
        else ++unknownCount;
        drawEntity(draw, world, e, settings, width, height, stats);
    }
    for (const WorldObject& o : world.objects) drawObject(draw, world, o, settings, width, height, stats);
    if (world.demoData) {
        constexpr const char* warning = "DEMO DATA - NOT LIVE GAME DATA";
        const ImVec2 size = ImGui::CalcTextSize(warning);
        const ImVec2 pos((static_cast<float>(width) - size.x) * 0.5f, 12.0f);
        draw->AddRectFilled(ImVec2(pos.x - 8.0f, pos.y - 4.0f),
                            ImVec2(pos.x + size.x + 8.0f, pos.y + size.y + 4.0f),
                            IM_COL32(130, 20, 35, 225), 4.0f);
        draw->AddText(pos, IM_COL32(255, 245, 245, 255), warning);
    }
    if (settings.showEntityCount) {
        char text[160];
        if (settings.guiLanguage == 0)
            std::snprintf(text, sizeof(text), "玩家 %d   生物 %d   實體 %d   物件 %zu",
                          playerCount, creatureCount, unknownCount,
                          world.objects.size() + static_cast<std::size_t>(liveObjectCount));
        else
            std::snprintf(text, sizeof(text), "Players %d   Creatures %d   Actors %d   Objects %zu",
                          playerCount, creatureCount, unknownCount,
                          world.objects.size() + static_cast<std::size_t>(liveObjectCount));
        const ImVec2 position(
            std::clamp(settings.entityCountX, 0.0f, std::max(0.0f, static_cast<float>(width) - 1.0f)),
            std::clamp(settings.entityCountY, 0.0f, std::max(0.0f, static_cast<float>(height) - 1.0f)));
        drawOutlinedText(draw, ImGui::GetFont(), std::clamp(settings.entityCountFontSize, 10.0f, 64.0f),
                         position, color(settings.entityCountColor, settings.textOpacity), text, 1.5f);
    }
    stats.playerEntities = static_cast<std::size_t>(playerCount);
    stats.creatureEntities = static_cast<std::size_t>(creatureCount);
    stats.unknownEntities = static_cast<std::size_t>(unknownCount);
    return stats;
}

void drawCrosshair(ImDrawList* draw, const Settings& s, int width, int height, float seconds) {
    const ImVec2 c(width * 0.5f, height * 0.5f);
    const ImU32 ink = color(s.crosshairColor, s.crosshairOpacity);
    const float size = s.crosshairSize, gap = s.crosshairGap, thick = s.crosshairThickness;
    if (s.showCrosshair) switch (s.crosshairStyle) {
        case 1:
            draw->AddCircle(c, size, ink, 48, thick);
            draw->AddCircleFilled(c, std::max(1.0f, thick * 0.65f), ink, 16);
            break;
        case 2: {
            const float arm = std::max(3.0f, size * 0.42f);
            for (int sx : {-1, 1}) for (int sy : {-1, 1}) {
                const ImVec2 p(c.x + sx * size, c.y + sy * size);
                draw->AddLine(p, ImVec2(p.x - sx * arm, p.y), ink, thick);
                draw->AddLine(p, ImVec2(p.x, p.y - sy * arm), ink, thick);
            }
            break;
        }
        case 3: {
            const float spread = gap + (std::sin(seconds * 3.0f) * 0.5f + 0.5f) * size * 0.35f;
            draw->AddLine(ImVec2(c.x - spread - size, c.y), ImVec2(c.x - spread, c.y), ink, thick);
            draw->AddLine(ImVec2(c.x + spread, c.y), ImVec2(c.x + spread + size, c.y), ink, thick);
            draw->AddLine(ImVec2(c.x, c.y - spread - size), ImVec2(c.x, c.y - spread), ink, thick);
            draw->AddLine(ImVec2(c.x, c.y + spread), ImVec2(c.x, c.y + spread + size), ink, thick);
            break;
        }
        default:
            draw->AddLine(ImVec2(c.x - gap - size, c.y), ImVec2(c.x - gap, c.y), ink, thick);
            draw->AddLine(ImVec2(c.x + gap, c.y), ImVec2(c.x + gap + size, c.y), ink, thick);
            draw->AddLine(ImVec2(c.x, c.y - gap - size), ImVec2(c.x, c.y - gap), ink, thick);
            draw->AddLine(ImVec2(c.x, c.y + gap), ImVec2(c.x, c.y + gap + size), ink, thick);
            break;
    }
    if (s.showCrosshairRing)
        draw->AddCircle(c, s.ringRadius, color(s.ringColor, s.ringOpacity), 96, s.ringThickness);
}

RadarStats drawRadar(const WorldSnapshot& world, Settings& settings) {
    RadarStats stats;
    if (!settings.showRadar) return stats;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float maxDiameter = std::max(1.0f, std::min(display.x, display.y));
    const float diameter = std::min(settings.radarRadius * 2.0f,
                                    std::max(1.0f, maxDiameter - 42.0f));
    const float windowWidth = std::min(display.x, diameter + 20.0f);
    const float windowHeight = std::min(display.y, diameter + 42.0f);
    if (settings.radarX < 0.0f) settings.radarX = std::max(0.0f, display.x - windowWidth - 8.0f);
    settings.radarX = std::clamp(settings.radarX, 0.0f, std::max(0.0f, display.x - windowWidth));
    settings.radarY = std::clamp(settings.radarY, 0.0f, std::max(0.0f, display.y - windowHeight));
    ImGui::SetNextWindowPos(ImVec2(settings.radarX, settings.radarY),
                            settings.radarPositionOverride ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    settings.radarPositionOverride = false;
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(1.0f, 1.0f),
                                        ImVec2(std::max(1.0f, display.x),
                                               std::max(1.0f, display.y)));
    const bool chinese = settings.guiLanguage == 0;
    ImGui::Begin(chinese ? "雷達###Radar" : "Radar###Radar", &settings.showRadar);
    settings.radarX = ImGui::GetWindowPos().x;
    settings.radarY = ImGui::GetWindowPos().y;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float radius = std::max(1.0f, std::min({settings.radarRadius,
        (avail.x - 6.0f) * 0.5f, (avail.y - 6.0f) * 0.5f}));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 center(origin.x + avail.x * 0.5f, origin.y + radius + 3.0f);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddCircleFilled(center, radius, IM_COL32(10, 15, 23, 190), 64);
    d->AddCircle(center, radius, IM_COL32(150, 175, 195, 210), 64, 1.3f);
    d->AddCircle(center, radius * 0.5f, IM_COL32(100, 130, 150, 130), 48, 1.0f);
    d->AddLine(ImVec2(center.x-radius, center.y), ImVec2(center.x+radius, center.y), IM_COL32(100,130,150,100));
    d->AddLine(ImVec2(center.x, center.y-radius), ImVec2(center.x, center.y+radius), IM_COL32(100,130,150,100));

    const float fl = std::sqrt(world.cameraForward.x*world.cameraForward.x + world.cameraForward.z*world.cameraForward.z);
    const float rl = std::sqrt(world.cameraRight.x*world.cameraRight.x + world.cameraRight.z*world.cameraRight.z);
    stats.headingAvailable = fl > 1e-5f && rl > 1e-5f;
    stats.headingFallbackUsed = !stats.headingAvailable;
    const Vec3 fwd = fl > 1e-5f ? Vec3{world.cameraForward.x/fl, 0, world.cameraForward.z/fl} : Vec3{0,0,1};
    const Vec3 right = rl > 1e-5f ? Vec3{world.cameraRight.x/rl, 0, world.cameraRight.z/rl} : Vec3{1,0,0};
    const float rangeSourceUnits = std::max(1.0f, settings.radarRange * std::max(1.0f, world.unitsPerMeter));
    const float scale = radius / rangeSourceUnits;
    constexpr ImU32 markerOutline = IM_COL32(5, 7, 10, 255);
    auto plot = [&](Vec3 pos, ImU32 c, bool square) {
        const Vec3 delta{pos.x-world.cameraPosition.x, 0, pos.z-world.cameraPosition.z};
        float x = (delta.x*right.x + delta.z*right.z) * scale;
        float y = (delta.x*fwd.x + delta.z*fwd.z) * scale;
        const float len = std::sqrt(x*x+y*y);
        if (len > radius && len > 0) { x *= radius/len; y *= radius/len; }
        const ImVec2 p(center.x+x, center.y-y);
        if (square) {
            d->AddRectFilled(ImVec2(p.x-4.5f,p.y-4.5f), ImVec2(p.x+4.5f,p.y+4.5f), markerOutline);
            d->AddRectFilled(ImVec2(p.x-2.8f,p.y-2.8f), ImVec2(p.x+2.8f,p.y+2.8f), c);
        } else {
            d->AddCircleFilled(p, 5.5f, markerOutline, 16);
            d->AddCircleFilled(p, 3.6f, c, 16);
        }
    };
    for (const auto& o : world.objects) {
        if (!settings.showRadarObjects) continue;
        if (settings.visibleOnly && o.visible && !*o.visible) { ++stats.objectsFiltered; continue; }
        if (settings.maxDistance > 0.0f &&
            distance(o.position, world.cameraPosition) / std::max(1.0f, world.unitsPerMeter) > settings.maxDistance) {
            ++stats.objectsFiltered;
            continue;
        }
        plot(o.position, color(settings.objectColor), true);
        ++stats.objectsPlotted;
    }
    for (const auto& e : world.entities) {
        if (e.kind == "player" && !settings.showRadarPlayers) continue;
        if ((e.kind == "creature" || e.kind == "mob") && !settings.showRadarCreatures) continue;
        if (e.kind == "object" && !settings.showRadarObjects) continue;
        if (e.kind != "player" && e.kind != "creature" && e.kind != "mob" &&
            e.kind != "object" && !settings.showRadarUnknown) continue;
        if (entityFilter(world, e, settings) != EntityFilter::None) { ++stats.entitiesFiltered; continue; }
        plot(e.position, color(entityColor(world, e, settings)), false);
        ++stats.entitiesPlotted;
    }
    const ImVec2 headingA(center.x, center.y-7), headingB(center.x-5, center.y+5), headingC(center.x+5, center.y+5);
    d->AddTriangle(headingA, headingB, headingC, markerOutline, 3.0f);
    d->AddTriangleFilled(headingA, headingB, headingC, IM_COL32(255,220,80,255));
    if (!world.valid) {
        const char* waiting = chinese ? "等待資料" : "Waiting for data";
        const ImVec2 textSize = ImGui::CalcTextSize(waiting);
        d->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y + radius + 6.0f),
                   IM_COL32(210, 220, 235, 220), waiting);
    }
    ImGui::Dummy(ImVec2(avail.x, radius*2.0f+6.0f));
    ImGui::End();
    return stats;
}

} // namespace esp
