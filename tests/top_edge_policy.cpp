#include "TopEdgePolicy.hpp"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
    using TopEdgePolicy::contains;
    // 2880x1920 @ 2x is a 1440x960 logical output. No double scaling.
    assert(contains(0, 0, 1440, 960, 700, 0, 12));
    assert(contains(0, 0, 1440, 960, 700, 12, 12));
    assert(!contains(0, 0, 1440, 960, 700, 12.1, 12));
    assert(!contains(0, 0, 1440, 960, 700, 500, 12));
    assert(!contains(0, 0, 1440, 960, 700, 959, 12));
    assert(!contains(0, 0, 1440, 960, 0, 600, 12));
    assert(!contains(0, 0, 1440, 960, 1439, 600, 12));
    // Absolute offsets, including monitors above and left of the origin.
    assert(contains(-1920, -1080, 1920, 1080, -600, -1075, 12));
    assert(!contains(-1920, -1080, 1920, 1080, -600, -5, 12));
    assert(contains(1440, 200, 1280, 720, 1500, 205, 12));
    assert(!contains(1440, 200, 1280, 720, 1500, 5, 12));
    // Adjacent outputs must not both accept the same release point.
    assert(!contains(0, 0, 1440, 960, 1440, 4, 12));
    assert(contains(1440, 0, 1280, 720, 1440, 4, 12));
    assert(!contains(0, 0, 1440, 960, 100, -1, 12));
    assert(!contains(0, 0, 1440, 960, -1, 0, 12));
    assert(contains(0, 0, 1440, 960, 100, 0, 0));
    assert(!contains(0, 0, 1440, 960, 100, 1, 0));
    assert(!contains(0, 0, 1440, 960, 100, 0, -1));
    assert(!contains(0, 0, 0, 960, 0, 0, 12));
    assert(!contains(0, 0, 1440, 0, 0, 0, 12));
    assert(!contains(0, 0, 1440, 960, std::numeric_limits<double>::quiet_NaN(), 0, 12));
    assert(!contains(0, 0, 1440, 960, 100, std::numeric_limits<double>::infinity(), 12));
    assert(!TopEdgePolicy::moved(100, 2, 100, 2));
    assert(!TopEdgePolicy::moved(100, 2, 101, 3));
    assert(TopEdgePolicy::moved(100, 200, 100, 2));
    // Entering the zone then leaving it does not latch a snap.
    assert(contains(0, 0, 1440, 960, 100, 3, 12));
    assert(!contains(0, 0, 1440, 960, 100, 100, 12));
    std::cout << "Top-edge policy tests passed\n";
}
