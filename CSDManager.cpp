#include "CSDManager.hpp"
#include "barDeco.hpp"
#include "TopEdgeSnap.hpp"
#include "TopEdgePolicy.hpp"
#include "MaximizeManager.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

// Hyprland currently has no public decoration-mode query. Plugins are tied to
// Hyprland's exact ABI anyway, so use its protocol objects rather than guessing
// from application names.
#define private public
#include <hyprland/src/protocols/XDGDecoration.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#undef private

#include <linux/input-event-codes.h>

#include <cmath>
#include <format>

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

        const auto split = action.find_first_of(" \t");
        const auto name  = action.substr(0, split);
        const auto args  = split == std::string::npos ? std::string{} : action.substr(split + 1);
        if (const auto dispatcher = g_pKeybindManager->m_dispatchers.find(name); dispatcher != g_pKeybindManager->m_dispatchers.end())
            dispatcher->second(args);
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

            if (g_pGlobalState->maximizeManager)
                g_pGlobalState->maximizeManager->toggle(window);
            return;
        }
    }
}

CCSDManager::CCSDManager() {
    m_windowOpen    = Event::bus()->m_events.window.open.listen([this](PHLWINDOW window) { registerWindow(window); });
    m_windowClose   = Event::bus()->m_events.window.close.listen([this](PHLWINDOW window) { unregisterWindow(window.get()); });
    m_windowDestroy = Event::bus()->m_events.window.destroy.listen([this](PHLWINDOWREF window) {
        if (const auto locked = window.lock())
            unregisterWindow(locked.get());
    });

    m_mouseButton = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent event, Event::SCallbackInfo& info) { onMouseButton(event, info); });
    m_mouseMove   = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D position, Event::SCallbackInfo& info) { onMouseMove(position, info); });
    m_touchDown   = Event::bus()->m_events.input.touch.down.listen([this](ITouch::SDownEvent event, Event::SCallbackInfo& info) { onTouchDown(event, info); });
    m_touchMove   = Event::bus()->m_events.input.touch.motion.listen([this](ITouch::SMotionEvent event, Event::SCallbackInfo& info) { onTouchMove(event, info); });
    m_touchUp     = Event::bus()->m_events.input.touch.up.listen([this](ITouch::SUpEvent event, Event::SCallbackInfo& info) { onTouchUp(event, info); });
    m_touchCancel = Event::bus()->m_events.input.touch.cancel.listen([this](ITouch::SCancelEvent event, Event::SCallbackInfo& info) { onTouchCancel(event, info); });
    m_tabletAxis  = Event::bus()->m_events.input.tablet.axis.listen([this](CTablet::SAxisEvent event, Event::SCallbackInfo& info) { onTabletAxis(event, info); });
    m_tabletTip   = Event::bus()->m_events.input.tablet.tip.listen([this](CTablet::STipEvent event, Event::SCallbackInfo& info) { onTabletTip(event, info); });
    m_tabletProximity =
        Event::bus()->m_events.input.tablet.proximity.listen([this](CTablet::SProximityEvent event, Event::SCallbackInfo& info) { onTabletProximity(event, info); });

    for (const auto& window : Desktop::windowState()->windows())
        registerWindow(window);
}

CCSDManager::~CCSDManager() {
    // Put Hyprland's original xdg_toplevel.move handlers back before this
    // plugin's code is unloaded.
    for (auto& [window, hooks] : m_windows) {
        if (hooks.moveOverridden && hooks.xdgResource) {
            hooks.xdgResource->setMove(std::move(hooks.originalMove));
            hooks.xdgResource->setSetMaximized(std::move(hooks.originalMaximize));
            hooks.xdgResource->setUnsetMaximized(std::move(hooks.originalUnmaximize));
        }
    }

    if (m_emulatingStylus)
        releaseEmulatedStylus(0);
    if (m_stylusPinnedWindow) {
        if (const auto window = m_stylusCSDWindow.lock())
            (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE, window);
    }
    if (m_emulatingPointer)
        g_pInputManager->onMouseButton(IPointer::SButtonEvent{.timeMs = 0, .button = BTN_LEFT, .state = WL_POINTER_BUTTON_STATE_RELEASED, .mouse = false}, nullptr);

    if (m_dragging && !m_touchDrag && g_layoutManager->dragController()->target())
        g_layoutManager->endDragTarget();
    if (m_pinnedForTouchDrag) {
        if (const auto window = m_dragWindow.lock())
            (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE, window);
    }

    for (auto& [key, hooks] : m_windows) {
        if (const auto window = hooks.window.lock(); window && hooks.ownsMaxSuppression)
            window->m_suppressedEvents &= ~Desktop::View::SUPPRESS_MAXIMIZE;
    }
}

void CCSDManager::registerWindow(const PHLWINDOW& window) {
    if (!validMapped(window))
        return;

    auto& hooks  = m_windows[window.get()];
    hooks.window = window;
    applySuppression(window);
    if (hooks.listening)
        return;

    const PHLWINDOWREF weak = window;
    if (const auto xdg = window->m_xdgSurface.lock(); xdg && xdg->m_toplevel.lock()) {
        const auto top     = xdg->m_toplevel.lock();
        hooks.stateChanged = top->m_events.stateChanged.listen([this, weak] { handleClientState(weak); });

        // Hyprland 0.56 validates xdg_toplevel.move only against pointer-button
        // serials. Touch-down and tablet-tool-down serials are valid implicit
        // grabs too, so retain native pointer handling and add validated paths.
        if (top->m_resource) {
            hooks.xdgResource        = top->m_resource;
            hooks.originalMove       = top->m_resource->requests.move;
            hooks.originalMaximize   = top->m_resource->requests.setMaximized;
            hooks.originalUnmaximize = top->m_resource->requests.unsetMaximized;
            auto maximizeRequest     = [weak](bool enabled) {
                const auto window = weak.lock();
                if (!validMapped(window) || (window->m_suppressedEvents & Desktop::View::SUPPRESS_MAXIMIZE))
                    return;
                if (!g_pGlobalState->config.maximizeAction->value().empty())
                    runTouchbarAction(window, eTouchbarButtonAction::MAXIMIZE);
                else if (g_pGlobalState->maximizeManager) {
                    // Stock Hyprland advertises maximized even on normal
                    // windows to suppress CSD margins. Clients consequently
                    // send unset_maximized when that caption button is clicked.
                    // Match native compatibility behavior without forcing the
                    // window through workspace-fullscreen stacking first.
                    if (window->m_suppressNextMaximize) {
                        window->m_suppressNextMaximize = false;
                        return;
                    }
                    const auto modes             = Fullscreen::controller()->getFullscreenModes(window);
                    const auto surface           = window->m_xdgSurface.lock();
                    const auto top               = surface ? surface->m_toplevel.lock() : nullptr;
                    const bool compatibilityHint = top && std::ranges::find(top->m_pendingApply.states, XDG_TOPLEVEL_STATE_MAXIMIZED) != top->m_pendingApply.states.end();
                    const bool normal            = modes.internal == Fullscreen::FSMODE_NONE && modes.client == Fullscreen::FSMODE_NONE;
                    g_pGlobalState->maximizeManager->set(window, enabled || (normal && compatibilityHint));
                }
            };
            top->m_resource->setSetMaximized([maximizeRequest](CXdgToplevel*) { maximizeRequest(true); });
            top->m_resource->setUnsetMaximized([maximizeRequest](CXdgToplevel*) { maximizeRequest(false); });
            top->m_resource->setMove([this, weak](CXdgToplevel* resource, wl_resource* seatResource, uint32_t serial) {
                const auto seat = CWLSeatResource::fromResource(seatResource);
                if (!seat || seat->client() != resource->client())
                    return;

                const auto targetWindow = weak.lock();
                const auto owner        = targetWindow ? targetWindow->m_xdgSurface.lock() : nullptr;
                const auto surface      = owner ? owner->m_surface.lock() : nullptr;
                const auto topResource  = owner ? owner->m_toplevel.lock() : nullptr;
                if (!targetWindow || !surface || !topResource)
                    return;

                if (g_pSeatManager->pointerButtonSerialValid(seat, serial, surface)) {
                    if (g_pGlobalState->maximizeManager)
                        g_pGlobalState->maximizeManager->prepareDrag(targetWindow, g_pInputManager->getMouseCoordsInternal());
                    topResource->m_events.requestMove.emit(SXDGToplevelMoveRequest{.seat = seat, .serial = serial});
                    return;
                }

                if (!g_pSeatManager->serialValid(seat, serial))
                    return;

                if (beginProtocolTouchMove(targetWindow)) {
                    // The app has explicitly handed its touch grab to the
                    // compositor. Stop delivering that sequence to widgets/tabs.
                    g_pSeatManager->sendTouchCancel();
                    m_clientTouchCancelled = true;
                    return;
                }

                // Tablet-tool down serials are also valid implicit grabs. Keep
                // tablet input native until Chromium identifies the pixel as
                // draggable, then track that exact stylus gesture ourselves.
                (void)beginProtocolStylusMove(targetWindow);
            });
            hooks.moveOverridden = true;
        }
        hooks.listening = true;
    } else if (const auto xwayland = window->m_xwaylandSurface.lock()) {
        hooks.stateChanged = xwayland->m_events.stateChanged.listen([this, weak] { handleClientState(weak); });
        hooks.listening    = true;
    }
}

void CCSDManager::unregisterWindow(Desktop::View::CWindow* window) {
    if (const auto it = m_windows.find(window); it != m_windows.end()) {
        auto& hooks = it->second;
        if (hooks.moveOverridden && hooks.xdgResource) {
            hooks.xdgResource->setMove(std::move(hooks.originalMove));
            hooks.xdgResource->setSetMaximized(std::move(hooks.originalMaximize));
            hooks.xdgResource->setUnsetMaximized(std::move(hooks.originalUnmaximize));
        }
        if (const auto owner = hooks.window.lock(); owner && hooks.ownsMaxSuppression)
            owner->m_suppressedEvents &= ~Desktop::View::SUPPRESS_MAXIMIZE;
        m_windows.erase(it);
    }
    if (const auto stylusWindow = m_stylusCSDWindow.lock(); stylusWindow && stylusWindow.get() == window) {
        m_stylusCSDWindow.reset();
        m_stylusCSDActive    = false;
        m_stylusCSDDragging  = false;
        m_stylusWindowMoved  = false;
        m_stylusPinnedWindow = false;
        m_stylusTool.reset();
        m_stylusTablet.reset();
    }
    if (const auto dragging = m_dragWindow.lock(); dragging && dragging.get() == window) {
        if (m_emulatingPointer)
            g_pInputManager->onMouseButton(IPointer::SButtonEvent{.timeMs = 0, .button = BTN_LEFT, .state = WL_POINTER_BUTTON_STATE_RELEASED, .mouse = false}, nullptr);
        m_dragWindow.reset();
        m_dragPending        = false;
        m_dragging           = false;
        m_pinnedForTouchDrag = false;
        m_emulatingPointer   = false;
        m_touchActive        = false;
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

    auto& hooks = m_windows[window.get()];
    // Wayland requests are replaced above, so do not modify their suppression
    // flags. XWayland stateChanged needs native maximize suppressed first.
    const bool want = window->m_isX11;
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
    if (!window)
        return;

    bool                minimize = false;
    std::optional<bool> maximize;

    if (const auto xdg = window->m_xdgSurface.lock(); xdg && xdg->m_toplevel.lock()) {
        const auto top = xdg->m_toplevel.lock();
        minimize       = top->m_state.requestsMinimize.value_or(false);
        // Wayland maximize is handled by the resource callbacks above.
    } else if (const auto xwayland = window->m_xwaylandSurface.lock()) {
        minimize = xwayland->m_state.requestsMinimize.value_or(false);
        maximize = xwayland->m_state.requestsMaximize;
        xwayland->m_state.requestsMaximize.reset();
        if (minimize) {
            xwayland->m_state.requestsMinimize.reset();
            xwayland->setMinimized(false);
        }
    }

    if (minimize)
        runTouchbarAction(window, eTouchbarButtonAction::MINIMIZE);
    if (maximize.has_value() && m_windows[window.get()].ownsMaxSuppression) {
        if (!g_pGlobalState->config.maximizeAction->value().empty())
            runTouchbarAction(window, eTouchbarButtonAction::MAXIMIZE);
        else if (g_pGlobalState->maximizeManager)
            g_pGlobalState->maximizeManager->set(window, *maximize);
    }
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

bool CCSDManager::beginProtocolTouchMove(const PHLWINDOW& window) {
    if (!g_pGlobalState->config.csdDragEnabled->value() || !m_touchActive || !window)
        return false;

    const auto touchedWindow = m_dragWindow.lock();
    if (!touchedWindow || touchedWindow != window)
        return false;

    m_dragPending = true;
    m_dragging    = false;
    m_touchDrag   = true;
    return true;
}

bool CCSDManager::beginProtocolStylusMove(const PHLWINDOW& window) {
    if (!g_pGlobalState->config.stylusDragEnabled->value() || !m_stylusCSDActive || !window || m_stylusCSDWindow.lock() != window)
        return false;

    m_stylusCSDDragging = true;
    return true;
}

bool CCSDManager::armDrag(const Vector2D& position, bool touch, int touchID) {
    const auto window = csdWindowAt(position);
    if (!window || !inDragRegion(window, position))
        return false;

    m_dragWindow         = window;
    m_dragOrigin         = position;
    m_dragGrabOffset     = position - window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    m_dragPending        = true;
    m_dragging           = false;
    m_touchDrag          = touch;
    m_touchID            = touchID;
    m_pinnedForTouchDrag = false;
    return true;
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

    if (touch) {
        if (!m_clientTouchCancelled) {
            g_pSeatManager->sendTouchCancel();
            m_clientTouchCancelled = true;
        }

        if (!m_dragging) {
            if (g_pGlobalState->maximizeManager && g_pGlobalState->maximizeManager->prepareDrag(window, position))
                m_dragGrabOffset = position - window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
            if (Desktop::focusState()->window() != window)
                Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_CLICK);
            if (window->m_isFloating)
                Desktop::windowState()->raise(window);
            else
                (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_ENABLE, window);

            if (!window->m_pinned) {
                (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_ENABLE, window);
                m_pinnedForTouchDrag = true;
            }
            m_dragging = true;
        }

        const auto target = position - m_dragGrabOffset;
        g_pKeybindManager->m_dispatchers["movewindowpixel"](std::format("exact {} {},activewindow", static_cast<int>(target.x), static_cast<int>(target.y)));
        info.cancelled = true;
        return;
    }

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

    if (g_pGlobalState->maximizeManager)
        g_pGlobalState->maximizeManager->prepareDrag(window, position);
    g_layoutManager->beginDragTarget(window->layoutTarget(), MBIND_MOVE, std::nullopt, true);
    m_dragging = true;
}

void CCSDManager::finishDrag(Event::SCallbackInfo& info, bool touch, int touchID, bool snap) {
    if (!m_dragPending || touch != m_touchDrag || touchID != m_touchID)
        return;

    if (m_dragging) {
        if (touch) {
            if (m_pinnedForTouchDrag) {
                if (const auto window = m_dragWindow.lock())
                    (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE, window);
            }
        } else if (g_layoutManager->dragController()->target())
            g_layoutManager->endDragTarget();
    }

    if (touch || m_dragging)
        info.cancelled = true;

    if (snap && touch && m_dragging && g_pGlobalState->topEdgeSnap)
        g_pGlobalState->topEdgeSnap->finish(m_dragWindow.lock(), m_lastTouchPosition);

    m_dragWindow.reset();
    m_dragPending          = false;
    m_dragging             = false;
    m_pinnedForTouchDrag   = false;
    m_clientTouchCancelled = false;
    if (touch)
        m_touchActive = false;
    m_touchID = -1;
    m_touchMonitor.reset();
}

void CCSDManager::onMouseButton(IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
    // Inspect the layout's move target before any release handler clears it.
    // Emulated touch/stylus pointer sequences intentionally use this path too.
    if (g_pGlobalState->topEdgeSnap)
        g_pGlobalState->topEdgeSnap->pointerButton(event);
    if (m_emulatingPointer || m_emulatingStylus || event.button != BTN_LEFT)
        return;
    if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (g_pGlobalState->config.csdDragFallback->value())
            armDrag(g_pInputManager->getMouseCoordsInternal(), false, 0);
    } else
        finishDrag(info, false, 0);
}

void CCSDManager::onMouseMove(const Vector2D&, Event::SCallbackInfo& info) {
    if (!m_emulatingPointer && !m_emulatingStylus)
        updateDrag(info, g_pInputManager->getMouseCoordsInternal(), false, 0);
}

void CCSDManager::onTouchDown(ITouch::SDownEvent event, Event::SCallbackInfo& info) {
    // Always remember the implicit touch grab. The application receives the
    // event and decides whether this exact pixel is draggable by issuing
    // xdg_toplevel.move; tabs, back buttons, and other widgets remain native.
    if (m_touchActive)
        return;

    m_touchMonitor                   = monitorForTouch(event.device);
    const auto              position = touchPosition(event);
    Desktop::CViewHitTester hitTester{*Desktop::viewState()};
    const auto              window = hitTester.windowAt(position, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
    if (!window || !validMapped(window)) {
        m_touchMonitor.reset();
        return;
    }

    m_touchActive          = true;
    m_touchID              = event.touchID;
    m_touchDrag            = true;
    m_dragWindow           = window;
    m_dragOrigin           = position;
    m_lastTouchPosition    = position;
    m_dragGrabOffset       = position - window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    m_dragPending          = g_pGlobalState->config.csdDragFallback->value() && windowHasCSD(window) && inDragRegion(window, position);
    m_dragging             = false;
    m_pinnedForTouchDrag   = false;
    m_clientTouchCancelled = false;
    m_emulatingPointer     = false;

    if (g_pGlobalState->config.csdDragEnabled->value() && g_pGlobalState->config.csdTouchEmulation->value() && windowHasCSD(window)) {
        const auto box   = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto local = position - box.pos();
        if (VECINRECT(local, 0, 0, box.w, std::min<double>(box.h, g_pGlobalState->config.csdTitlebarHeight->value()))) {
            // Forward this top-area touch as a complete pointer sequence. The
            // client performs its own precise hit testing, so Chromium tabs,
            // navigation buttons, and genuine drag regions all keep working.
            m_emulatingPointer = true;
            m_dragPending      = false;
            Pointer::mgr()->warpTo(position);
            g_pInputManager->refocus();
            g_pInputManager->onMouseButton(IPointer::SButtonEvent{.timeMs = event.timeMs, .button = BTN_LEFT, .state = WL_POINTER_BUTTON_STATE_PRESSED, .mouse = false}, nullptr);
            info.cancelled = true;
        }
    }
}

void CCSDManager::onTouchMove(ITouch::SMotionEvent event, Event::SCallbackInfo& info) {
    if (m_touchActive && event.touchID == m_touchID)
        m_lastTouchPosition = touchPosition(event);
    if (m_emulatingPointer && m_touchActive && event.touchID == m_touchID) {
        Pointer::mgr()->warpTo(touchPosition(event));
        g_pInputManager->simulateMouseMovement();
        info.cancelled = true;
        return;
    }
    updateDrag(info, touchPosition(event), true, event.touchID);
}

void CCSDManager::onTouchUp(ITouch::SUpEvent event, Event::SCallbackInfo& info) {
    if (m_emulatingPointer && m_touchActive && event.touchID == m_touchID) {
        g_pInputManager->onMouseButton(IPointer::SButtonEvent{.timeMs = event.timeMs, .button = BTN_LEFT, .state = WL_POINTER_BUTTON_STATE_RELEASED, .mouse = false}, nullptr);
        info.cancelled         = true;
        m_emulatingPointer     = false;
        m_touchActive          = false;
        m_clientTouchCancelled = false;
        m_touchID              = -1;
        m_dragPending          = false;
        m_dragging             = false;
        m_pinnedForTouchDrag   = false;
        m_dragWindow.reset();
        m_touchMonitor.reset();
        return;
    }

    if (m_dragPending) {
        finishDrag(info, true, event.touchID);
        return;
    }

    if (m_touchActive && event.touchID == m_touchID) {
        m_touchActive          = false;
        m_clientTouchCancelled = false;
        m_touchID              = -1;
        m_dragWindow.reset();
        m_touchMonitor.reset();
    }
}

void CCSDManager::onTouchCancel(ITouch::SCancelEvent event, Event::SCallbackInfo& info) {
    if (!m_touchActive || event.touchID != m_touchID)
        return;
    if (m_emulatingPointer) {
        if (g_pGlobalState->topEdgeSnap)
            g_pGlobalState->topEdgeSnap->cancelPointer();
        onTouchUp(ITouch::SUpEvent{.timeMs = event.timeMs, .touchID = event.touchID}, info);
    } else if (m_dragPending)
        finishDrag(info, true, event.touchID, false);
    else {
        m_touchActive = false;
        m_dragWindow.reset();
        m_touchMonitor.reset();
    }
}

Vector2D CCSDManager::tabletPosition(const SP<CTablet>& tablet, const Vector2D& normalized) {
    if (!tablet)
        return g_pInputManager->getMouseCoordsInternal();

    auto mapped = normalized;
    if (!tablet->m_activeArea.empty()) {
        if (!std::isnan(mapped.x))
            mapped.x = (mapped.x - tablet->m_activeArea.x) / (tablet->m_activeArea.w - tablet->m_activeArea.x);
        if (!std::isnan(mapped.y))
            mapped.y = (mapped.y - tablet->m_activeArea.y) / (tablet->m_activeArea.h - tablet->m_activeArea.y);
    }
    Pointer::mgr()->warpAbsolute(mapped, tablet);
    return g_pInputManager->getMouseCoordsInternal();
}

bool CCSDManager::overPluginTitlebar(const Vector2D& position) const {
    for (const auto& bar : g_pGlobalState->bars) {
        if (bar && bar->containsPoint(position))
            return true;
    }
    return false;
}

PHLWINDOW CCSDManager::csdTitlebarAt(const Vector2D& position) const {
    Desktop::CViewHitTester hitTester{*Desktop::viewState()};
    const auto              window = hitTester.windowAt(position, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
    if (!window || !validMapped(window) || !windowHasCSD(window))
        return nullptr;

    const auto box   = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto local = position - box.pos();
    return VECINRECT(local, 0, 0, box.w, std::min<double>(box.h, g_pGlobalState->config.csdTitlebarHeight->value())) ? window : nullptr;
}

void CCSDManager::releaseEmulatedStylus(uint32_t timeMs, bool snap) {
    if (!m_emulatingStylus)
        return;
    if (!snap && g_pGlobalState->topEdgeSnap)
        g_pGlobalState->topEdgeSnap->cancelPointer();
    g_pInputManager->onMouseButton(IPointer::SButtonEvent{.timeMs = timeMs, .button = BTN_LEFT, .state = WL_POINTER_BUTTON_STATE_RELEASED, .mouse = false}, nullptr);
    m_emulatingStylus = false;
    m_stylusTool.reset();
    m_stylusTablet.reset();
}

void CCSDManager::onTabletTip(CTablet::STipEvent event, Event::SCallbackInfo& info) {
    if (!g_pGlobalState->config.stylusDragEnabled->value())
        return;

    if (event.in) {
        if (m_emulatingStylus || m_emulatingPointer || m_stylusCSDActive)
            return;

        m_stylusTablet      = event.tablet;
        m_stylusNormalized  = event.tip;
        const auto position = event.tablet->m_relativeInput ? g_pInputManager->getMouseCoordsInternal() : tabletPosition(event.tablet, event.tip);

        if (overPluginTitlebar(position)) {
            m_emulatingStylus = true;
            m_stylusTool      = event.tool;
            g_pInputManager->refocus();
            g_pInputManager->onMouseButton(IPointer::SButtonEvent{.timeMs = event.timeMs, .button = BTN_LEFT, .state = WL_POINTER_BUTTON_STATE_PRESSED, .mouse = false}, nullptr);
            info.cancelled = true;
            return;
        }

        const auto csdWindow = csdTitlebarAt(position);
        if (!csdWindow) {
            m_stylusTablet.reset();
            return;
        }

        // Keep this as native tablet input. Chromium can now use its own exact
        // hit testing and hand us a tablet serial only for a real drag region.
        m_stylusTool         = event.tool;
        m_stylusCSDWindow    = csdWindow;
        m_stylusGrabOffset   = position - csdWindow->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        m_stylusDownPosition = position;
        m_stylusCSDActive    = true;
        m_stylusCSDDragging  = false;
        m_stylusWindowMoved  = false;
        m_stylusPinnedWindow = false;
        return;
    }

    if (m_emulatingStylus && event.tool == m_stylusTool) {
        if (event.tablet && !event.tablet->m_relativeInput) {
            m_stylusNormalized = event.tip;
            tabletPosition(event.tablet, event.tip);
            g_pInputManager->simulateMouseMovement();
        }
        releaseEmulatedStylus(event.timeMs, true);
        info.cancelled = true;
        return;
    }

    if (!m_stylusCSDActive || event.tool != m_stylusTool)
        return;

    if (m_stylusPinnedWindow) {
        if (const auto window = m_stylusCSDWindow.lock())
            (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE, window);
    }
    const auto release = event.tablet && !event.tablet->m_relativeInput ? tabletPosition(event.tablet, event.tip) : g_pInputManager->getMouseCoordsInternal();
    if (m_stylusWindowMoved && TopEdgePolicy::moved(m_stylusDownPosition.x, m_stylusDownPosition.y, release.x, release.y) && g_pGlobalState->topEdgeSnap)
        g_pGlobalState->topEdgeSnap->finish(m_stylusCSDWindow.lock(), release);

    m_stylusCSDWindow.reset();
    m_stylusCSDActive    = false;
    m_stylusCSDDragging  = false;
    m_stylusWindowMoved  = false;
    m_stylusPinnedWindow = false;
    m_stylusTool.reset();
    m_stylusTablet.reset();
    // Do not cancel: Hyprland and the client still need the tablet-tool up.
}

void CCSDManager::onTabletAxis(CTablet::SAxisEvent event, Event::SCallbackInfo& info) {
    const bool emulated = m_emulatingStylus && event.tool == m_stylusTool;
    const bool csdDrag  = m_stylusCSDActive && m_stylusCSDDragging && event.tool == m_stylusTool;
    if ((!emulated && !csdDrag) || !event.tablet)
        return;

    if (event.tablet->m_relativeInput)
        Pointer::mgr()->move({std::isnan(event.axisDelta.x) ? 0.0 : event.axisDelta.x, std::isnan(event.axisDelta.y) ? 0.0 : event.axisDelta.y});
    else {
        if ((event.updatedAxes & CTablet::HID_TABLET_TOOL_AXIS_X) && !std::isnan(event.axis.x))
            m_stylusNormalized.x = event.axis.x;
        if ((event.updatedAxes & CTablet::HID_TABLET_TOOL_AXIS_Y) && !std::isnan(event.axis.y))
            m_stylusNormalized.y = event.axis.y;
        tabletPosition(event.tablet, m_stylusNormalized);
    }

    if (emulated) {
        g_pInputManager->simulateMouseMovement();
        info.cancelled = true;
        return;
    }

    const auto window = m_stylusCSDWindow.lock();
    if (!window || !validMapped(window))
        return;

    if (!m_stylusWindowMoved) {
        const auto point = g_pInputManager->getMouseCoordsInternal();
        if (g_pGlobalState->maximizeManager && g_pGlobalState->maximizeManager->prepareDrag(window, point))
            m_stylusGrabOffset = point - window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
        if (Desktop::focusState()->window() != window)
            Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_CLICK);
        if (window->m_isFloating)
            Desktop::windowState()->raise(window);
        else
            (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_ENABLE, window);
        if (!window->m_pinned) {
            (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_ENABLE, window);
            m_stylusPinnedWindow = true;
        }
        m_stylusWindowMoved = true;
    }

    const auto target = g_pInputManager->getMouseCoordsInternal() - m_stylusGrabOffset;
    g_pKeybindManager->m_dispatchers["movewindowpixel"](std::format("exact {} {},activewindow", static_cast<int>(target.x), static_cast<int>(target.y)));
    info.cancelled = true;
}

void CCSDManager::onTabletProximity(CTablet::SProximityEvent event, Event::SCallbackInfo&) {
    if (event.in || event.tool != m_stylusTool)
        return;
    if (m_emulatingStylus) {
        releaseEmulatedStylus(event.timeMs);
        return;
    }
    if (m_stylusCSDActive) {
        if (m_stylusPinnedWindow) {
            if (const auto window = m_stylusCSDWindow.lock())
                (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE, window);
        }
        m_stylusCSDWindow.reset();
        m_stylusCSDActive    = false;
        m_stylusCSDDragging  = false;
        m_stylusWindowMoved  = false;
        m_stylusPinnedWindow = false;
        m_stylusTool.reset();
        m_stylusTablet.reset();
    }
}
