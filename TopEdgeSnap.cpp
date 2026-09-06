#include "TopEdgeSnap.hpp"
#include "TopEdgePolicy.hpp"
#include "globals.hpp"
#include "MaximizeManager.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <linux/input-event-codes.h>
#include <utility>

namespace {
    PHLMONITOR topEdgeMonitor(const Vector2D& point) {
        const auto distance = std::clamp<Config::INTEGER>(g_pGlobalState->config.topEdgeDistance->value(), 0, 128);
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (!monitor->m_enabled || monitor->isMirror())
                continue;
            const auto box = monitor->logicalBox();
            if (TopEdgePolicy::contains(box.x, box.y, box.w, box.h, point.x, point.y, distance))
                return monitor;
        }
        return nullptr;
    }
}

void CTopEdgeSnap::pointerButton(const IPointer::SButtonEvent& event) {
    if (event.button != BTN_LEFT)
        return;

    const auto point = g_pInputManager->getMouseCoordsInternal();
    if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        m_pointerStart = point;
        return;
    }

    const auto start = std::exchange(m_pointerStart, std::nullopt);
    if (!start || !TopEdgePolicy::moved(start->x, start->y, point.x, point.y))
        return;

    // Called before release is delivered to the layout. No target means a
    // client widget/tab drag, not a compositor window move. Never snap resize.
    const auto& drag   = g_layoutManager->dragController();
    const auto  target = drag->target();
    if (target && drag->mode() == MBIND_MOVE && drag->dragThresholdReached())
        finish(target->window(), point);
}

void CTopEdgeSnap::cancelPointer() {
    m_pointerStart.reset();
}

void CTopEdgeSnap::finish(const PHLWINDOW& window, const Vector2D& release) {
    if (!g_pGlobalState->config.topEdgeMaximize->value() || !validMapped(window) || window->isHidden() || window->m_pinned || g_pSessionLockManager->isSessionLocked() ||
        !topEdgeMonitor(release))
        return;

    const PHLWINDOWREF weakWindow = window;
    // dragEnd() can retile and recalculate geometry. Maximize only once that
    // cleanup (and temporary touch pin restoration) has completed. Owning the
    // idle lock also cancels pending work when the plugin is unloaded.
    m_pending = g_pEventLoopManager->doLaterLock([this, weakWindow, release] {
        m_pending.reset();
        const auto window = weakWindow.lock();
        if (!g_pGlobalState->config.topEdgeMaximize->value() || !validMapped(window) || window->isHidden() || window->m_pinned || g_pSessionLockManager->isSessionLocked() ||
            g_layoutManager->dragController()->target())
            return;

        const auto monitor = topEdgeMonitor(release);
        if (!monitor)
            return;
        const auto modes = Fullscreen::controller()->getFullscreenModes(window);
        if (modes.internal != Fullscreen::FSMODE_NONE || modes.client != Fullscreen::FSMODE_NONE)
            return;

        // Directly moved touch windows may not yet have migrated to the output
        // containing the finger. Maximize on that output, not the old one.
        if (window->m_monitor.lock() != monitor) {
            const auto workspace = monitor->m_activeSpecialWorkspace ? monitor->m_activeSpecialWorkspace : monitor->m_activeWorkspace;
            if (!workspace)
                return;
            Desktop::globalWindowController()->moveWindowToWorkspace(window, workspace);
        }

        // The same stateful maximize path as caption buttons. Tiled windows
        // stay layout-managed; floating windows keep normal z-order.
        if (g_pGlobalState->maximizeManager)
            g_pGlobalState->maximizeManager->set(window, true);
    });
}
