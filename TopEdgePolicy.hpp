#pragma once

#include <algorithm>
#include <cmath>

namespace TopEdgePolicy {
    // All coordinates are logical, not output pixels. Half-open monitor bounds
    // disambiguate adjacent outputs. Test the RELEASE point, not window bounds
    // or the closest position reached earlier in a gesture.
    inline bool contains(double x, double y, double width, double height, double releaseX, double releaseY, double distance) {
        if (!std::isfinite(releaseX) || !std::isfinite(releaseY) || !std::isfinite(distance) || width <= 0 || height <= 0 || distance < 0)
            return false;
        return releaseX >= x && releaseX < x + width && releaseY >= y && releaseY < y + height && releaseY - y <= std::min(distance, height);
    }

    inline bool moved(double startX, double startY, double endX, double endY) {
        // A click (or minor pen jitter) near the edge must not maximize.
        return std::hypot(endX - startX, endY - startY) >= 3.0;
    }
}
