#pragma once

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/render/Renderer.hpp>

namespace DragMotion {
    inline void moveImmediately(const PHLWINDOW& window, const Vector2D& position) {
        if (!validMapped(window))
            return;

        // Preserve the normal move action's layout/workspace bookkeeping, but
        // follow direct input immediately, just like Hyprland's mouse drag
        // controller. No global animation settings or window rules are changed.
        // Target the grabbed window, not whichever app is currently focused.
        g_pHyprRenderer->damageWindow(window);
        if (!Config::Actions::move(position, false, window))
            return;
        if (const auto target = window->layoutTarget())
            target->warpPositionSize();
        g_pHyprRenderer->damageWindow(window);
    }
}
