#include "CSDManager.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

// Hyprland currently has no public decoration-mode query. Plugins are tied to
// Hyprland's exact ABI anyway, so use its protocol objects rather than guessing
// from application names.
#define private public
#include <hyprland/src/protocols/XDGDecoration.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#undef private

#include <linux/input-event-codes.h>

namespace {
    PHLMONITOR monitorForTouch(const SP<ITouch>& device) {
        if (device && !device->m_boundOutput.empty()) {
            for (const auto& monitor : State::monitorState()->monitors()) {
                if (monitor->m_name == device->m_boundOutput)
                    return monitor;
            }
        }
        return Desktop::focusState()->monitor();
    }

    bool ruleEnabled(const PHLWINDOW& window, uint32_t index) {
        if (!window || !window->m_ruleApplicator->m_otherProps.props.contains(index))
            return false;
        return truthy(window->m_ruleApplicator->m_otherProps.props.at(index)->effect);
    }

    void dispatchForWindow(const PHLWINDOW& window, const std::string& action) {
        if (!window || action.empty())
            return;

        if (Desktop::focusState()->window() != window)
            Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_CLICK);
        if (window->m_isFloating)
            Desktop::windowState()->raise(window);

        HyprlandAPI::invokeHyprctlCommand("dispatch", action);
    }
} // namespace

bool windowHasCSD(const PHLWINDOW& window) {
    if (!window)
        return false;

    if (ruleEnabled(window, g_pGlobalState->forceSSDRuleIdx))
        return false;
    if (ruleEnabled(window, g_pGlobalState->forceCSDRuleIdx))
        return true;

    const auto mode = g_pGlobalState->config.csdDetection->value();
    if (mode == "off" || mode == "none")
        return false;
    if (mode == "all")
        return true;

    if (window->m_isX11)
        return window->m_X11DoesntWantBorders;

    const auto xdg = window->m_xdgSurface.lock();
    const auto top = xdg ? xdg->m_toplevel.lock() : nullptr;
    if (!top || !top->m_resource)
        return false;

    // Under xdg-decoration, not creating a decoration object means the client
    // owns its decorations. An explicit CLIENT_SIDE request is also respected
    // for detection even though Hyprland currently replies SERVER_SIDE.
    if (!PROTO::xdgDecoration)
        return true;

    const auto resource = top->m_resource->resource();
    const auto deco     = PROTO::xdgDecoration->m_decorations.find(resource);
    if (deco == PROTO::xdgDecoration->m_decorations.end())
        return true;

    return deco->second->mostRecentlyRequested == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
}

void runTouchbarAction(const PHLWINDOW& window, eTouchbarButtonAction action, const std::string& custom) {
    if (!window)
        return;

    switch (action) {
        case eTouchbarButtonAction::CUSTOM:
            if (!custom.empty())
                Config::Supplementary::executor()->spawn(custom);
            return;
        case eTouchbarButtonAction::CLOSE: {
            const auto configured = g_pGlobalState->config.closeAction->value();
            if (!configured.empty())
                dispatchForWindow(window, configured);
            else
                window->sendClose();
            return;
        }
        case eTouchbarButtonAction::MINIMIZE: dispatchForWindow(window, g_pGlobalState->config.minimizeAction->value()); return;
        case eTouchbarButtonAction::MAXIMIZE: {
            const auto configured = g_pGlobalState->config.maximizeAction->value();
            if (!configured.empty()) {
                dispatchForWindow(window, configured);
                return;
            }

            const auto current = Fullscreen::controller()->getFullscreenModes(window).client;
            Fullscreen::controller()->setFullscreenMode(window, std::nullopt, current == Fullscreen::FSMODE_MAXIMIZED ? Fullscreen::FSMODE_NONE : Fullscreen::FSMODE_MAXIMIZED);
            return;
        }
    }
}

CCSDManager::CCSDManager() {
    m_windowOpen    = Event::bus()->m_events.window.open.listen([this](PHLWINDOW window) { registerWindow(window); });
    m_windowDestroy = Event::bus()->m_events.window.destroy.listen([this](PHLWINDOWREF window) {
        if (const auto locked = window.lock())
            unregisterWindow(locked.get());
    });

    m_mouseButton = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent event, Event::SCallbackInfo& info) { onMouseButton(event, info); });
    m_mouseMove   = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D position, Event::SCallbackInfo& info) { onMouseMove(position, info); });
    m_touchDown   = Event::bus()->m_events.input.touch.down.listen([this](ITouch::SDownEvent event, Event::SCallbackInfo& info) { onTouchDown(event, info); });
    m_touchMove   = Event::bus()->m_events.input.touch.motion.listen([this](ITouch::SMotionEvent event, Event::SCallbackInfo& info) { onTouchMove(event, info); });
    m_touchUp     = Event::bus()->m_events.input.touch.up.listen([this](ITouch::SUpEvent event, Event::SCallbackInfo& info) { onTouchUp(event, info); });

    for (const auto& window : Desktop::windowState()->windows())
        registerWindow(window);
}

CCSDManager::~CCSDManager() {
    if (m_dragging && g_layoutManager->dragController()->target())
        g_layoutManager->endDragTarget();

    for (auto& [window, hooks] : m_windows) {
        if (hooks.ownsMaxSuppression)
            window->m_suppressedEvents &= ~Desktop::View::SUPPRESS_MAXIMIZE;
    }
}

void CCSDManager::registerWindow(const PHLWINDOW& window) {
    if (!window)
        return;

    auto& hooks = m_windows[window.get()];
    applySuppression(window);
    if (hooks.listening)
        return;

    const PHLWINDOWREF weak = window;
    if (const auto xdg = window->m_xdgSurface.lock(); xdg && xdg->m_toplevel.lock()) {
        hooks.stateChanged = xdg->m_toplevel.lock()->m_events.stateChanged.listen([this, weak] { handleClientState(weak); });
        hooks.listening    = true;
    } else if (const auto xwayland = window->m_xwaylandSurface.lock()) {
        hooks.stateChanged = xwayland->m_events.stateChanged.listen([this, weak] { handleClientState(weak); });
        hooks.listening    = true;
    }
}

void CCSDManager::unregisterWindow(Desktop::View::CWindow* window) {
    m_windows.erase(window);
    if (const auto dragging = m_dragWindow.lock(); dragging && dragging.get() == window) {
        m_dragWindow.reset();
        m_dragPending = false;
        m_dragging    = false;
    }
}

void CCSDManager::refresh() {
    for (const auto& window : Desktop::windowState()->windows()) {
        registerWindow(window);
        applySuppression(window);
    }
}

void CCSDManager::applySuppression(const PHLWINDOW& window) {
    if (!window)
        return;

    auto&      hooks = m_windows[window.get()];
    const bool want  = windowHasCSD(window) && !g_pGlobalState->config.maximizeAction->value().empty();
    if (want == hooks.maxSuppressionWanted)
        return;

    if (want) {
        if (!(window->m_suppressedEvents & Desktop::View::SUPPRESS_MAXIMIZE)) {
            window->m_suppressedEvents |= Desktop::View::SUPPRESS_MAXIMIZE;
            hooks.ownsMaxSuppression = true;
        }
    } else if (hooks.ownsMaxSuppression) {
        window->m_suppressedEvents &= ~Desktop::View::SUPPRESS_MAXIMIZE;
        hooks.ownsMaxSuppression = false;
    }
    hooks.maxSuppressionWanted = want;
}

void CCSDManager::handleClientState(const PHLWINDOWREF& weak) {
    const auto window = weak.lock();
    if (!window || !windowHasCSD(window))
        return;

    bool minimize = false;
    bool maximize = false;

    if (const auto xdg = window->m_xdgSurface.lock(); xdg && xdg->m_toplevel.lock()) {
        const auto top = xdg->m_toplevel.lock();
        minimize       = top->m_state.requestsMinimize.value_or(false);
        maximize       = top->m_state.requestsMaximize.has_value();
    } else if (const auto xwayland = window->m_xwaylandSurface.lock()) {
        minimize = xwayland->m_state.requestsMinimize.value_or(false);
        maximize = xwayland->m_state.requestsMaximize.has_value();
        if (minimize) {
            xwayland->m_state.requestsMinimize.reset();
            xwayland->setMinimized(false);
        }
    }

    if (minimize)
        runTouchbarAction(window, eTouchbarButtonAction::MINIMIZE);
    if (maximize && !g_pGlobalState->config.maximizeAction->value().empty())
        runTouchbarAction(window, eTouchbarButtonAction::MAXIMIZE);
}

PHLWINDOW CCSDManager::csdWindowAt(const Vector2D& position) const {
    Desktop::CViewHitTester hitTester{*Desktop::viewState()};
    const auto              window = hitTester.windowAt(position, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
    return window && validMapped(window) && windowHasCSD(window) ? window : nullptr;
}

bool CCSDManager::inDragRegion(const PHLWINDOW& window, const Vector2D& position) const {
    if (!window || !g_pGlobalState->config.csdDragEnabled->value())
        return false;

    const CBox   box    = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto   local  = position - box.pos();
    const double height = std::min<double>(g_pGlobalState->config.csdTitlebarHeight->value(), box.h);
    const double left   = g_pGlobalState->config.csdControlsLeft->value();
    const double right  = g_pGlobalState->config.csdControlsRight->value();

    if (!VECINRECT(local, left, 0, std::max(left, box.w - right), height))
        return false;
    if (window->hasPopupAt(position))
        return false;
    if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(window->wlSurface()->resource()))
        return false;
    return true;
}

Vector2D CCSDManager::touchPosition(const ITouch::SDownEvent& event) const {
    const auto monitor = monitorForTouch(event.device);
    if (!monitor)
        return {};
    return monitor->m_position + event.pos * monitor->m_size;
}

Vector2D CCSDManager::touchPosition(const ITouch::SMotionEvent& event) const {
    const auto monitor = m_touchMonitor.lock() ? m_touchMonitor.lock() : Desktop::focusState()->monitor();
    if (!monitor)
        return {};
    return monitor->m_position + event.pos * monitor->m_size;
}

void CCSDManager::armDrag(const Vector2D& position, bool touch, int touchID) {
    const auto window = csdWindowAt(position);
    if (!window || !inDragRegion(window, position))
        return;

    m_dragWindow  = window;
    m_dragOrigin  = position;
    m_dragPending = true;
    m_dragging    = false;
    m_touchDrag   = touch;
    m_touchID     = touchID;
}

void CCSDManager::updateDrag(Event::SCallbackInfo& info, const Vector2D& position, bool touch, int touchID) {
    if (!m_dragPending || touch != m_touchDrag || touchID != m_touchID)
        return;

    const auto window = m_dragWindow.lock();
    if (!window || !validMapped(window)) {
        m_dragPending = false;
        return;
    }

    if ((position - m_dragOrigin).size() < g_pGlobalState->config.csdDragThreshold->value())
        return;

    if (m_dragging)
        return;

    if (g_layoutManager->dragController()->target()) {
        // The toolkit's native xdg_toplevel.move won the race; no fallback is needed.
        m_dragPending = false;
        return;
    }

    if (Desktop::focusState()->window() != window)
        Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_CLICK);
    if (window->m_isFloating)
        Desktop::windowState()->raise(window);

    g_layoutManager->beginDragTarget(window->layoutTarget(), MBIND_MOVE, std::nullopt, true);
    m_dragging = true;
}

void CCSDManager::finishDrag(Event::SCallbackInfo& info, bool touch, int touchID) {
    if (!m_dragPending || touch != m_touchDrag || touchID != m_touchID)
        return;

    if (m_dragging) {
        if (g_layoutManager->dragController()->target())
            g_layoutManager->endDragTarget();
        info.cancelled = true;
    }

    m_dragWindow.reset();
    m_dragPending = false;
    m_dragging    = false;
    m_touchID     = -1;
    m_touchMonitor.reset();
}

void CCSDManager::onMouseButton(IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
    if (event.button != BTN_LEFT)
        return;
    if (event.state == WL_POINTER_BUTTON_STATE_PRESSED)
        armDrag(g_pInputManager->getMouseCoordsInternal(), false, 0);
    else
        finishDrag(info, false, 0);
}

void CCSDManager::onMouseMove(const Vector2D&, Event::SCallbackInfo& info) {
    updateDrag(info, g_pInputManager->getMouseCoordsInternal(), false, 0);
}

void CCSDManager::onTouchDown(ITouch::SDownEvent event, Event::SCallbackInfo&) {
    if (event.touchID != 0)
        return;
    m_touchMonitor = monitorForTouch(event.device);
    armDrag(touchPosition(event), true, event.touchID);
}

void CCSDManager::onTouchMove(ITouch::SMotionEvent event, Event::SCallbackInfo& info) {
    updateDrag(info, touchPosition(event), true, event.touchID);
}

void CCSDManager::onTouchUp(ITouch::SUpEvent event, Event::SCallbackInfo& info) {
    finishDrag(info, true, event.touchID);
}
