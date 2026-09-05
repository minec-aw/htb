#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <optional>

// Shared completion path for native pointer drags (including emulated input)
// and the plugin's directly tracked touch/tablet drags.
class CTopEdgeSnap {
  public:
    void pointerButton(const IPointer::SButtonEvent& event);
    void cancelPointer();
    void finish(const PHLWINDOW& window, const Vector2D& release);

  private:
    std::optional<Vector2D>   m_pointerStart;
    UP<SEventLoopDoLaterLock> m_pending;
};
