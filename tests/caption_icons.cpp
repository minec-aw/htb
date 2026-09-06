#include "ChromiumCaptionIcons.hpp"
#include <cairo/cairo.h>
#include <librsvg/rsvg.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

static cairo_surface_t* raster(std::string_view name, int size) {
    const auto svg = ChromiumCaptionIcons::svg(name, size);
    assert(!svg.empty());
    GError* error  = nullptr;
    auto*   handle = rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(svg.data()), svg.size(), &error);
    assert(handle && !error);
    auto*               surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    auto*               cr      = cairo_create(surface);
    const RsvgRectangle viewport{0, 0, static_cast<double>(size), static_cast<double>(size)};
    assert(rsvg_handle_render_document(handle, cr, &viewport, &error) && !error);
    cairo_destroy(cr);
    g_object_unref(handle);
    cairo_surface_flush(surface);
    return surface;
}

static int opaquePixels(std::string_view name, int size) {
    auto*       surface = raster(name, size);
    const auto* data    = cairo_image_surface_get_data(surface);
    const int   stride  = cairo_image_surface_get_stride(surface);
    int         opaque  = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t pixel;
            std::memcpy(&pixel, data + y * stride + x * 4, sizeof(pixel));
            if ((pixel >> 24) == 255)
                ++opaque;
        }
    }
    cairo_surface_destroy(surface);
    return opaque;
}

int main(int argc, char** argv) {
    using namespace ChromiumCaptionIcons;
    assert(svg("unknown", 24).empty());
    assert(svg("window-close-symbolic", 0).empty());
    assert(representationSize(12) == 12);
    assert(representationSize(15) == 24);
    assert(representationSize(23) == 24); // ceil(12 * 1.875)
    assert(representationSize(24) == 24);
    assert(representationSize(36) == 12); // largest exact divisor
    assert(representationSize(48) == 24);
    assert(opaquePixels("window-minimize-symbolic", 24) == 66);  // 22 x 3, not Adwaita's centered 8 x 2
    assert(opaquePixels("window-maximize-symbolic", 24) == 228); // outer 22^2 minus inner 16^2
    assert(opaquePixels("window-restore-symbolic", 24) == 225);  // two distinct overlapping-window outlines
    assert(opaquePixels("window-minimize-symbolic", 12) == 20);
    assert(opaquePixels("window-maximize-symbolic", 12) == 64);
    assert(opaquePixels("window-restore-symbolic", 12) == 68);
    for (const auto name : {"window-minimize-symbolic", "window-maximize-symbolic", "window-restore-symbolic", "window-close-symbolic"}) {
        for (const int size : {12, 15, 23, 24, 36, 48}) {
            const auto count = opaquePixels(name, size);
            assert(count > 0 && count < size * size);
        }
    }
    if (argc > 1) {
        auto* preview = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 160, 80);
        auto* cr      = cairo_create(preview);
        cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
        cairo_paint(cr);
        int x = 12;
        for (const auto name : {"window-minimize-symbolic", "window-maximize-symbolic", "window-restore-symbolic", "window-close-symbolic"}) {
            auto* icon = raster(name, 23);
            cairo_set_source_rgba(cr, .91, .92, .93, 1);
            cairo_mask_surface(cr, icon, x, 7);
            cairo_set_source_rgba(cr, .91, .92, .93, .38);
            cairo_mask_surface(cr, icon, x, 47);
            cairo_surface_destroy(icon);
            x += 36;
        }
        cairo_destroy(cr);
        assert(cairo_surface_write_to_png(preview, argv[1]) == CAIRO_STATUS_SUCCESS);
        cairo_surface_destroy(preview);
    }
    std::cout << "Chromium caption SVG raster/representation tests passed\n";
}
