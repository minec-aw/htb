#include "AppIcon.hpp"
#include "ChromiumCaptionIcons.hpp"

#include <hyprland/src/render/Renderer.hpp>

#include <cairo/cairo.h>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {
    std::string lower(std::string value) {
        std::ranges::transform(value, value.begin(), [](unsigned char c) { return std::tolower(c); });
        return value;
    }

    std::string trim(std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::vector<fs::path> dataRoots() {
        std::vector<fs::path> roots;
        if (const char* home = std::getenv("HOME")) {
            const char* xdgHome = std::getenv("XDG_DATA_HOME");
            roots.emplace_back(xdgHome && *xdgHome ? xdgHome : std::string(home) + "/.local/share");
            roots.emplace_back(std::string(home) + "/.icons");
        }

        const std::string dirs = std::getenv("XDG_DATA_DIRS") ? std::getenv("XDG_DATA_DIRS") : "/usr/local/share:/usr/share";
        std::stringstream stream(dirs);
        for (std::string dir; std::getline(stream, dir, ':');) {
            if (!dir.empty())
                roots.emplace_back(dir);
        }
        return roots;
    }

    std::optional<std::string> iconFromDesktopFile(const fs::path& path, const std::vector<std::string>& classes, bool requireClassMatch) {
        std::ifstream file(path);
        if (!file)
            return std::nullopt;

        bool        inDesktopEntry = false;
        std::string icon;
        std::string startupClass;
        for (std::string line; std::getline(file, line);) {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            if (line.front() == '[') {
                if (inDesktopEntry)
                    break;
                inDesktopEntry = line == "[Desktop Entry]";
                continue;
            }
            if (!inDesktopEntry)
                continue;

            const auto split = line.find('=');
            if (split == std::string::npos)
                continue;
            const auto key   = trim(line.substr(0, split));
            const auto value = trim(line.substr(split + 1));
            if (key == "Icon")
                icon = value;
            else if (key == "StartupWMClass" || key == "X-GNOME-WMClass")
                startupClass = lower(value);
        }

        if (icon.empty())
            return std::nullopt;
        if (!requireClassMatch)
            return icon;

        return std::ranges::find(classes, startupClass) != classes.end() ? std::optional<std::string>{icon} : std::nullopt;
    }

    std::optional<std::string> desktopIcon(const std::string& appClass, const std::string& initialClass) {
        std::vector<std::string> classes;
        for (const auto& value : {appClass, initialClass}) {
            const auto normalized = lower(trim(value));
            if (!normalized.empty() && std::ranges::find(classes, normalized) == classes.end())
                classes.push_back(normalized);
        }
        if (classes.empty())
            return std::nullopt;

        const auto roots = dataRoots();
        for (const auto& root : roots) {
            const auto applications = root / "applications";
            for (const auto& appClassCandidate : classes) {
                for (const auto& filename : {appClassCandidate + ".desktop", appClassCandidate}) {
                    const auto path = applications / filename;
                    if (fs::exists(path)) {
                        if (const auto icon = iconFromDesktopFile(path, classes, false))
                            return icon;
                    }
                }
            }
        }

        // Some desktop filenames and Wayland app IDs differ. StartupWMClass is
        // the standardized bridge, so scan it only when direct lookup failed.
        for (const auto& root : roots) {
            const auto      applications = root / "applications";
            std::error_code error;
            if (!fs::is_directory(applications, error))
                continue;
            for (fs::recursive_directory_iterator it(applications, fs::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) {
                if (error) {
                    error.clear();
                    continue;
                }
                if (!it->is_regular_file(error) || it->path().extension() != ".desktop")
                    continue;
                if (const auto icon = iconFromDesktopFile(it->path(), classes, true))
                    return icon;
            }
        }
        return std::nullopt;
    }

    int iconScore(const fs::path& path, int pixelSize) {
        const auto text  = lower(path.string());
        int        score = text.find("/apps/") != std::string::npos ? 50 : 0;
        if (text.find("symbolic") != std::string::npos)
            score -= 20;
        if (text.find(std::format("/{0}x{0}/", pixelSize)) != std::string::npos || text.find(std::format("/{}/", pixelSize)) != std::string::npos)
            score += 100;
        if (path.extension() == ".svg")
            score += 30;
        else if (path.extension() == ".png")
            score += 20;
        return score;
    }

    std::optional<fs::path> findInDirectory(const fs::path& directory, const std::string& iconName, int pixelSize) {
        std::error_code error;
        if (!fs::is_directory(directory, error))
            return std::nullopt;

        const fs::path          requested(iconName);
        const bool              hasExtension  = requested.has_extension();
        const auto              requestedName = lower(hasExtension ? requested.filename().string() : requested.stem().string());

        std::optional<fs::path> best;
        int                     bestScore = std::numeric_limits<int>::min();
        for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (!it->is_regular_file(error))
                continue;
            const auto extension = lower(it->path().extension().string());
            if (extension != ".png" && extension != ".svg")
                continue;

            const bool matches = hasExtension ? lower(it->path().filename().string()) == requestedName : lower(it->path().stem().string()) == requestedName;
            if (!matches)
                continue;

            const int score = iconScore(it->path(), pixelSize);
            if (!best || score > bestScore) {
                best      = it->path();
                bestScore = score;
            }
        }
        return best;
    }

    std::optional<fs::path> iconPath(const std::string& icon, const std::string& theme, int pixelSize) {
        if (icon.empty())
            return std::nullopt;
        const fs::path direct(icon);
        if (direct.is_absolute() && fs::is_regular_file(direct))
            return direct;

        std::vector<std::string> themes;
        for (const auto& candidate : {theme, std::string{"hicolor"}, std::string{"Adwaita"}, std::string{"breeze"}}) {
            if (!candidate.empty() && std::ranges::find(themes, candidate) == themes.end())
                themes.push_back(candidate);
        }

        for (const auto& root : dataRoots()) {
            for (const auto& candidate : themes) {
                if (const auto result = findInDirectory(root / "icons" / candidate, icon, pixelSize))
                    return result;
                // ~/.icons is itself an icon root rather than an XDG data root.
                if (root.filename() == ".icons") {
                    if (const auto result = findInDirectory(root / candidate, icon, pixelSize))
                        return result;
                }
            }
            if (const auto result = findInDirectory(root / "pixmaps", icon, pixelSize))
                return result;
        }
        return std::nullopt;
    }

    cairo_surface_t* renderPNG(const fs::path& path, int size) {
        cairo_surface_t* source = cairo_image_surface_create_from_png(path.c_str());
        if (cairo_surface_status(source) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(source);
            return nullptr;
        }

        const int width  = cairo_image_surface_get_width(source);
        const int height = cairo_image_surface_get_height(source);
        if (width < 1 || height < 1) {
            cairo_surface_destroy(source);
            return nullptr;
        }

        cairo_surface_t* target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t*         cairo  = cairo_create(target);
        cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cairo);
        cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
        const double scale = std::min<double>(size / static_cast<double>(width), size / static_cast<double>(height));
        cairo_translate(cairo, (size - width * scale) / 2.0, (size - height * scale) / 2.0);
        cairo_scale(cairo, scale, scale);
        cairo_set_source_surface(cairo, source, 0, 0);
        cairo_paint(cairo);
        cairo_destroy(cairo);
        cairo_surface_destroy(source);
        return target;
    }

    // Consumes the handle for both on-disk theme icons and embedded vectors.
    cairo_surface_t* renderSVGHandle(RsvgHandle* handle, int size) {
        GError* error = nullptr;
        if (!handle)
            return nullptr;

        cairo_surface_t* target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t*         cairo  = cairo_create(target);
        cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cairo);
        cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
        const RsvgRectangle viewport{0, 0, static_cast<double>(size), static_cast<double>(size)};
        if (!rsvg_handle_render_document(handle, cairo, &viewport, &error)) {
            cairo_destroy(cairo);
            cairo_surface_destroy(target);
            g_object_unref(handle);
            if (error)
                g_error_free(error);
            return nullptr;
        }
        cairo_destroy(cairo);
        g_object_unref(handle);
        return target;
    }

    cairo_surface_t* renderSVG(const fs::path& path, int size) {
        return renderSVGHandle(rsvg_handle_new_from_file(path.c_str(), nullptr), size);
    }
} // namespace

SP<Render::ITexture> AppIcon::load(const std::string& appClass, const std::string& initialClass, const std::string& theme, int pixelSize) {
    if (pixelSize < 1)
        return nullptr;

    const auto icon = desktopIcon(appClass, initialClass);
    const auto path = icon ? iconPath(*icon, theme, pixelSize) : std::nullopt;
    if (!path)
        return nullptr;

    cairo_surface_t* surface = lower(path->extension().string()) == ".svg" ? renderSVG(*path, pixelSize) : renderPNG(*path, pixelSize);
    if (!surface)
        return nullptr;

    auto texture = g_pHyprRenderer->createTexture(surface);
    cairo_surface_destroy(surface);
    return texture;
}

SP<Render::ITexture> AppIcon::loadNamed(const std::string& iconName, const std::string& theme, int pixelSize, const CHyprColor& tint) {
    if (iconName.empty() || pixelSize < 1)
        return nullptr;

    cairo_surface_t* surface = nullptr;
    if (theme == "chromium") {
        const auto svg = ChromiumCaptionIcons::svg(iconName, pixelSize);
        if (svg.empty())
            return nullptr;
        surface = renderSVGHandle(rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(svg.data()), svg.size(), nullptr), pixelSize);
    } else {
        const auto path = iconPath(iconName, theme, pixelSize);
        if (!path)
            return nullptr;
        surface = lower(path->extension().string()) == ".svg" ? renderSVG(*path, pixelSize) : renderPNG(*path, pixelSize);
    }
    if (!surface)
        return nullptr;

    // Symbolic icons carry only an alpha mask. Chromium asks GTK to recolor
    // that mask for the current title-button state; do the same here.
    cairo_t* cairo = cairo_create(surface);
    cairo_set_operator(cairo, CAIRO_OPERATOR_IN);
    cairo_set_source_rgba(cairo, tint.r, tint.g, tint.b, tint.a);
    cairo_paint(cairo);
    cairo_destroy(cairo);

    auto texture = g_pHyprRenderer->createTexture(surface);
    cairo_surface_destroy(surface);
    return texture;
}
