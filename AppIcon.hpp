#pragma once

#include <hyprland/src/render/Texture.hpp>

#include <string>

namespace AppIcon {
    // Resolve a window app-id / WM_CLASS through desktop files and the selected
    // freedesktop icon theme, then upload it as a Hyprland texture.
    SP<Render::ITexture> load(const std::string& appClass, const std::string& initialClass, const std::string& theme, int pixelSize);
}
