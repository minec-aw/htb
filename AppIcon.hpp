#pragma once

#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/helpers/Color.hpp>

#include <string>

namespace AppIcon {
    // Resolve a window app-id / WM_CLASS through desktop files and the selected
    // freedesktop icon theme, then upload it as a Hyprland texture.
    SP<Render::ITexture> load(const std::string& appClass, const std::string& initialClass, const std::string& theme, int pixelSize);

    // Load one of the freedesktop symbolic icons used by Chromium's Linux
    // caption buttons and tint it like GTK does.
    SP<Render::ITexture> loadNamed(const std::string& iconName, const std::string& theme, int pixelSize, const CHyprColor& tint);
}
