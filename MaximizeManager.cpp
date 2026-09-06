#include "MaximizeManager.hpp"
#include "globals.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <algorithm>
#include <utility>

using namespace Fullscreen;
using namespace Desktop::Types;

namespace {
    struct SBusy {
        bool& flag;
        bool  previous;
        explicit SBusy(bool& value) : flag(value), previous(std::exchange(value, true)) {}
        ~SBusy() {
            flag = previous;
        }
    };
    template <typename T>
    std::optional<T> savedOverride(const COverridableVar<T>& value) {
        return value.hasValue() && value.getPriority() == PRIORITY_SET_PROP ? std::optional<T>{value.value()} : std::nullopt;
    }
    template <typename T>
    void restoreOverride(COverridableVar<T>& value, const std::optional<T>& saved, T ours) {
        // Restore just the slot we borrowed, not a stale copy of all rules.
        // A different explicit setprop made in the meantime takes precedence.
        if (value.hasValue() && value.getPriority() == PRIORITY_SET_PROP && value.value() == ours)
            value.matchOptional(saved, PRIORITY_SET_PROP);
    }
    void updateFrame(const PHLWINDOW& window) {
        window->m_borderSizeCacheDirty = true;
        window->updateWindowDecos();
        window->updateDecorationValues();
        g_pHyprRenderer->damageWindow(window);
    }
}

CMaximizeManager::CMaximizeManager() {
    auto changed = [this](PHLWINDOW) { scheduleRefresh(); };
    m_open       = Event::bus()->m_events.window.open.listen(changed);
    m_fullscreen = Event::bus()->m_events.window.fullscreen.listen(changed);
    m_floating   = Event::bus()->m_events.window.floating.listen(changed);
    m_rules      = Event::bus()->m_events.window.updateRules.listen(changed);
    m_close      = Event::bus()->m_events.window.close.listen([this](PHLWINDOW window) {
        const auto it = m_states.find(window.get());
        if (it != m_states.end()) {
            restoreFrame(window, it->second);
            m_states.erase(it);
        }
    });
    m_config     = Event::bus()->m_events.config.reloaded.listen([this] { scheduleRefresh(); });
    m_monitor    = Event::bus()->m_events.monitor.layoutChanged.listen([this] { scheduleRefresh(); });
    m_workspace  = Event::bus()->m_events.window.moveToWorkspace.listen([this](PHLWINDOW, PHLWORKSPACE) { scheduleRefresh(); });
    m_active     = Event::bus()->m_events.window.active.listen([this](PHLWINDOW window, Desktop::eFocusReason reason) {
        if (m_busy || m_stopping || !validMapped(window) || !window->m_isFloating || reason == Desktop::FOCUS_REASON_FFM)
            return;
        // Explicit activation (Alt-Tab, clicks, task switchers) should also
        // raise normal floating windows in a desktop-maximized workspace.
        for (const auto& [key, state] : m_states) {
            const auto maximized = state.window.lock();
            if (state.desktop && !state.suspended && maximized && maximized->m_workspace == window->m_workspace) {
                Desktop::windowState()->raise(window);
                break;
            }
        }
    });
    m_render     = Event::bus()->m_events.render.preChecks.listen([this](PHLMONITOR) {
        // Layer-shell panels can change their reserved area without changing
        // the monitor layout. Only queue work when the usable area changed.
        for (const auto& [key, state] : m_states) {
            const auto window  = state.window.lock();
            const auto monitor = window ? window->m_monitor.lock() : nullptr;
            if (!state.desktop || state.suspended || !monitor)
                continue;
            const auto area = monitor->logicalBoxMinusReserved();
            if (area.pos() != state.workArea.pos() || area.size() != state.workArea.size()) {
                scheduleRefresh();
                break;
            }
        }
    });
    m_mouseMove  = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D, Event::SCallbackInfo&) { onNativeDrag(); });
    scheduleRefresh();
}

CMaximizeManager::~CMaximizeManager() {
    m_stopping = true;
    m_lifetime.reset();
    m_pending.reset();
    SBusy guard(m_busy);
    // Restore while the plugin and its config values are still loaded.
    while (!m_states.empty()) {
        const auto window = m_states.begin()->second.window.lock();
        if (window && validMapped(window)) {
            if (m_states.begin()->second.desktop && !m_states.begin()->second.suspended)
                restore(window);
            else {
                restoreFrame(window, m_states.begin()->second);
                m_states.erase(window.get());
            }
        } else
            m_states.erase(m_states.begin());
    }
}

bool CMaximizeManager::useDesktop(const PHLWINDOW& window) const {
    return window->m_isFloating && !window->m_group && g_pGlobalState->config.maximizeMode->value() != "native";
}

bool CMaximizeManager::isDesktopMaximized(const PHLWINDOW& window) const {
    if (!window)
        return false;
    const auto it = m_states.find(window.get());
    return it != m_states.end() && it->second.desktop && !it->second.suspended && controller()->getFullscreenModes(window).internal != FSMODE_FULLSCREEN;
}

void CMaximizeManager::notifyClient(const PHLWINDOW& window, bool maximized) {
    // Hyprland 0.56 deliberately keeps the XDG maximized hint set to suppress
    // client shadow margins: its renderer does NOT account for xdg geometry
    // offsets. Clearing it (as 1.6.0 did globally) shifts/clips Chromium's
    // content inside the compositor border. Preserve the compositor's native
    // compatibility hint, including when repairing a window affected by 1.6.0.
    // Real maximize/restore state remains in our controller, not this hint.
    if (const auto xdg = window->m_xdgSurface.lock(); xdg && xdg->m_toplevel)
        xdg->m_toplevel->setMaximized(true);
    else if (const auto x11 = window->m_xwaylandSurface.lock(); x11 && x11->m_maximized != maximized) {
        x11->m_maximized = maximized;
        x11->setFullscreen(x11->m_fullscreen); // publish the updated EWMH state
    }
}

void CMaximizeManager::setModes(const PHLWINDOW& window, int internal, int client) {
    auto&      sync     = window->m_ruleApplicator->syncFullscreen();
    const auto previous = savedOverride(sync);
    // Only decouple while making THIS transition. F11 and other native
    // fullscreen requests must continue to honor the user's sync setting.
    sync.set(false, PRIORITY_SET_PROP);
    const bool wasPinned          = window->m_pinned;
    const bool wasPinFullscreened = window->m_pinFullscreened;
    controller()->setFullscreenMode(window, static_cast<eFullscreenMode>(internal), static_cast<eFullscreenMode>(client),
                                    window->m_isFloating && internal == FSMODE_NONE ? std::optional<bool>{true} : std::nullopt);
    restoreOverride(sync, previous, false);
    notifyClient(window, client == FSMODE_MAXIMIZED);
    if (window->m_isFloating && internal == FSMODE_NONE && wasPinned) {
        // Native fullscreen's pin bookkeeping also runs for client-only state
        // changes. Desktop maximization must not silently unpin a window.
        window->m_pinned          = wasPinned;
        window->m_pinFullscreened = wasPinFullscreened;
    }
}

void CMaximizeManager::stripFrame(const PHLWINDOW& window, SState& state) {
    const auto border   = savedOverride(window->m_ruleApplicator->borderSize());
    const auto rounding = savedOverride(window->m_ruleApplicator->rounding());
    const auto shadow   = savedOverride(window->m_ruleApplicator->noShadow());
    if (border != std::optional<int64_t>{0})
        state.border = border;
    if (rounding != std::optional<int64_t>{0})
        state.rounding = rounding;
    if (shadow != std::optional<bool>{true})
        state.shadow = shadow;
    window->m_ruleApplicator->borderSize().set(0, PRIORITY_SET_PROP);
    window->m_ruleApplicator->rounding().set(0, PRIORITY_SET_PROP);
    window->m_ruleApplicator->noShadow().set(true, PRIORITY_SET_PROP);
    updateFrame(window);
}

void CMaximizeManager::restoreFrame(const PHLWINDOW& window, const SState& state) {
    restoreOverride(window->m_ruleApplicator->borderSize(), state.border, int64_t{0});
    restoreOverride(window->m_ruleApplicator->rounding(), state.rounding, int64_t{0});
    restoreOverride(window->m_ruleApplicator->noShadow(), state.shadow, true);
    if (validMapped(window))
        updateFrame(window);
}

void CMaximizeManager::fit(const PHLWINDOW& window) {
    const auto monitor = window->m_monitor.lock();
    if (!monitor || !window->layoutTarget())
        return;
    // Panels are reserved, tiling gaps are not. Keep the titlebar inside the
    // usable area by subtracting its reserved extents from the client box.
    const auto area = monitor->logicalBoxMinusReserved();
    if (const auto it = m_states.find(window.get()); it != m_states.end())
        it->second.workArea = area;
    const auto reserved = window->getFullWindowReservedArea();
    CBox       box{area.pos() + reserved.topLeft, area.size() - reserved.topLeft - reserved.bottomRight};
    box.round();
    if (box.w < 1 || box.h < 1)
        return;
    const auto current = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    if (current.pos() != box.pos() || current.size() != box.size())
        window->layoutTarget()->setPositionGlobal(box);
}

void CMaximizeManager::restore(const PHLWINDOW& window, bool geometry) {
    const auto it = m_states.find(window.get());
    if (it == m_states.end()) {
        setModes(window, FSMODE_NONE, FSMODE_NONE);
        return;
    }
    const auto state = it->second;
    m_states.erase(it);
    restoreFrame(window, state);
    setModes(window, FSMODE_NONE, FSMODE_NONE);
    if (geometry && state.desktop && window->m_isFloating && !window->m_group) {
        auto box = state.restoreBox;
        if (const auto monitor = window->m_monitor.lock()) {
            box.translate(monitor->m_position - state.restoreMonitorOrigin);
            const auto area     = monitor->logicalBoxMinusReserved();
            const auto reserved = window->getFullWindowReservedArea();
            // Keep a reachable titlebar when restoring on a smaller output.
            box.x = std::clamp(box.x, area.x + reserved.topLeft.x, std::max(area.x + reserved.topLeft.x, area.x + area.w - box.w - reserved.bottomRight.x));
            box.y = std::clamp(box.y, area.y + reserved.topLeft.y, std::max(area.y + reserved.topLeft.y, area.y + area.h - box.h - reserved.bottomRight.y));
        }
        window->layoutTarget()->setPositionGlobal(box);
    }
}

void CMaximizeManager::set(const PHLWINDOW& window, bool maximized) {
    if (m_busy || !validMapped(window) || window->isHidden())
        return;
    SBusy guard(m_busy);
    auto  modes = controller()->getFullscreenModes(window);
    if (modes.internal == FSMODE_FULLSCREEN)
        return; // never turn an application's true fullscreen into maximize
    if (!maximized) {
        restore(window);
        return;
    }

    const bool desktop = useDesktop(window);
    auto       it      = m_states.find(window.get());
    if (it != m_states.end() && it->second.desktop == desktop && !it->second.suspended) {
        stripFrame(window, it->second);
        if (desktop) {
            if (modes.internal != FSMODE_NONE || modes.client != FSMODE_MAXIMIZED)
                setModes(window, FSMODE_NONE, FSMODE_MAXIMIZED);
            fit(window);
        } else if (modes.internal != FSMODE_MAXIMIZED || modes.client != FSMODE_MAXIMIZED)
            setModes(window, FSMODE_MAXIMIZED, FSMODE_MAXIMIZED);
        return;
    }
    if (it != m_states.end())
        restore(window);
    else if (desktop && modes.internal == FSMODE_MAXIMIZED)
        setModes(window, FSMODE_NONE, FSMODE_NONE); // migrate an existing native floating maximum

    SState state;
    state.window     = window;
    state.desktop    = desktop;
    state.restoreBox = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    if (const auto monitor = window->m_monitor.lock())
        state.restoreMonitorOrigin = monitor->m_position;
    state.border   = savedOverride(window->m_ruleApplicator->borderSize());
    state.rounding = savedOverride(window->m_ruleApplicator->rounding());
    state.shadow   = savedOverride(window->m_ruleApplicator->noShadow());
    auto& stored   = m_states.emplace(window.get(), std::move(state)).first->second;
    stripFrame(window, stored);
    setModes(window, desktop ? FSMODE_NONE : FSMODE_MAXIMIZED, FSMODE_MAXIMIZED);
    if (desktop)
        fit(window);
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    Desktop::windowState()->raise(window);
}

void CMaximizeManager::toggle(const PHLWINDOW& window) {
    if (!window)
        return;
    const auto modes = controller()->getFullscreenModes(window);
    set(window, !(isDesktopMaximized(window) || modes.internal == FSMODE_MAXIMIZED || modes.client == FSMODE_MAXIMIZED));
}

bool CMaximizeManager::prepareDrag(const PHLWINDOW& window, const Vector2D& point) {
    if (m_busy || !isDesktopMaximized(window))
        return false;
    SBusy        guard(m_busy);
    const auto   maximized = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    const double fraction  = std::clamp((point.x - maximized.x) / std::max(1.0, maximized.w), 0.0, 1.0);
    const double y         = std::clamp(point.y - maximized.y, -128.0, 64.0);
    restore(window);
    const auto size = window->size(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    window->layoutTarget()->setPositionGlobal(CBox{point - Vector2D{size.x * fraction, y}, size});
    return true;
}

void CMaximizeManager::onNativeDrag() {
    if (m_busy)
        return;
    const auto& drag   = g_layoutManager->dragController();
    const auto  target = drag->target();
    if (!target || !drag->dragThresholdReached() || !isDesktopMaximized(target->window()))
        return;
    const auto window = target->window();
    if (drag->mode() == MBIND_MOVE) {
        const auto point     = g_pInputManager->getMouseCoordsInternal();
        const bool exclusive = drag->exclusiveDeviceGrab();
        // Native modifier-drag has already cached the maximized box. Rebase it
        // onto the restored box before consuming the first drag motion.
        g_layoutManager->endDragTarget();
        prepareDrag(window, point);
        g_layoutManager->beginDragTarget(window->layoutTarget(), MBIND_MOVE, std::nullopt, exclusive);
    } else {
        // Resize keeps the current box so the grabbed edge doesn't jump.
        SBusy guard(m_busy);
        restore(window, false);
    }
}

void CMaximizeManager::scheduleRefresh() {
    if (m_busy || m_stopping || m_pending)
        return;
    m_pending = g_pEventLoopManager->doLaterLock([this, lifetime = std::weak_ptr<int>(m_lifetime)] {
        if (lifetime.expired())
            return;
        m_pending.reset();
        refresh();
    });
}

void CMaximizeManager::refresh() {
    if (m_busy || m_stopping)
        return;
    // Work on windows, not map iterators: transitions may erase/reinsert state.
    const auto windows = Desktop::windowState()->windows();
    for (const auto& window : windows) {
        if (!validMapped(window))
            continue;
        auto modes = controller()->getFullscreenModes(window);
        auto it    = m_states.find(window.get());
        if (it == m_states.end()) {
            if (modes.internal == FSMODE_MAXIMIZED)
                set(window, true); // native binding/initial state; tiled stays native
            else if (modes.internal != FSMODE_FULLSCREEN)
                notifyClient(window, modes.client == FSMODE_MAXIMIZED);
            continue;
        }
        if (it->second.desktop != useDesktop(window)) {
            if (modes.internal != FSMODE_FULLSCREEN)
                set(window, true); // float/tile or maximize_mode changed
            continue;
        }
        SBusy guard(m_busy);
        auto& state = it->second;
        if (modes.internal == FSMODE_FULLSCREEN) {
            state.suspended = true;
            continue;
        }
        if (state.desktop) {
            if (!state.suspended && modes.internal == FSMODE_NONE && modes.client == FSMODE_NONE) {
                restore(window);
                continue;
            }
            state.suspended = false;
            if (modes.internal != FSMODE_NONE || modes.client != FSMODE_MAXIMIZED)
                setModes(window, FSMODE_NONE, FSMODE_MAXIMIZED);
            stripFrame(window, state);
            fit(window);
        } else if (modes.internal != FSMODE_MAXIMIZED) {
            const auto saved = state;
            m_states.erase(it);
            restoreFrame(window, saved);
        } else
            stripFrame(window, state);
    }
}
