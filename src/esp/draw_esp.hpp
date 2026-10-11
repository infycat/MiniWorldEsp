#pragma once
#include "types.hpp"
#include "imgui.h"

namespace esp {
struct DrawStats {
    std::size_t playerEntities = 0;
    std::size_t creatureEntities = 0;
    std::size_t unknownEntities = 0;
    std::size_t filteredKind = 0;
    std::size_t filteredRelation = 0;
    std::size_t filteredVisibility = 0;
    std::size_t filteredDistance = 0;
    std::size_t missingBounds = 0;
    std::size_t missingMatrix = 0;
    std::size_t clipped = 0;
    std::size_t drawn = 0;
    std::size_t objectsSeen = 0;
    std::size_t objectsFilteredDistance = 0;
    std::size_t objectsMissingMatrix = 0;
    std::size_t objectsClipped = 0;
    std::size_t objectsDrawn = 0;
};

struct RadarStats {
    std::size_t entitiesPlotted = 0;
    std::size_t entitiesFiltered = 0;
    std::size_t objectsPlotted = 0;
    std::size_t objectsFiltered = 0;
    bool headingAvailable = false;
    bool headingFallbackUsed = false;
};

DrawStats drawWorld(ImDrawList* draw, const WorldSnapshot& world, const Settings& settings,
                    int width, int height, float seconds);
void drawCrosshair(ImDrawList* draw, const Settings& settings, int width, int height,
                   float seconds);
RadarStats drawRadar(const WorldSnapshot& world, Settings& settings);
} // namespace esp
