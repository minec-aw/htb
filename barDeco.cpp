#include "barDeco.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/parserUtils/ParserUtils.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include "globals.hpp"
#include "AppIcon.hpp"
#include "TopEdgeSnap.hpp"
#include "MaximizeManager.hpp"
#include "DragMotion.hpp"
#include "BarPassElement.hpp"

#include <climits>
#include <numbers>

using namespace Render::GL;

static CHyprColor configColor(Config::INTEGER color) {
    return CHyprColor{static_cast<uint64_t>(color)};
}

CHyprBar::CHyprBar(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
    m_pWindow       = pWindow;
    m_bLastCSDState = windowHasCSD(pWindow);

    const auto PMONITOR         = pWindow->m_monitor.lock();
    PMONITOR->m_scheduledRecalc = true;

    // button events
    m_pMouseButtonCallback = Event::bus()->m_events.input.mouse.button.listen([&](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(info, e); });
    m_pTouchDownCallback   = Event::bus()->m_events.input.touch.down.listen([&](ITouch::SDownEvent e, Event::SCallbackInfo& info) { onTouchDown(info, e); });
    m_pTouchUpCallback     = Event::bus()->m_events.input.touch.up.listen([&](ITouch::SUpEvent e, Event::SCallbackInfo& info) { onTouchUp(info, e); });
    m_pTouchCancelCallback = Event::bus()->m_events.input.touch.cancel.listen([this](ITouch::SCancelEvent e, Event::SCallbackInfo& info) {
        if (m_bTouchEv && e.touchID == m_touchId)
            handleUpEvent(info, false);
    });

    // move events
    m_pTouchMoveCallback = Event::bus()->m_events.input.touch.motion.listen([&](ITouch::SMotionEvent e, Event::SCallbackInfo& info) { onTouchMove(info, e); });
    m_pMouseMoveCallback = Event::bus()->m_events.input.mouse.move.listen([&](Vector2D c, Event::SCallbackInfo& info) { onMouseMove(c); });

    Animation::mgr()->createAnimation(configColor(g_pGlobalState->config.barColor->value()), m_cRealBarColor, Config::animationTree()->getAnimationPropertyConfig("border"),
                                      pWindow, AVARDAMAGE_NONE);
    m_cRealBarColor->setUpdateCallback([&](auto) { damageEntire(); });
}

CHyprBar::~CHyprBar() {
    std::erase(g_pGlobalState->bars, m_self);
}

SDecorationPositioningInfo CHyprBar::getPositioningInfo() {
    const auto                 HEIGHT     = g_pGlobalState->config.barHeight->value();
    const auto                 ENABLED    = g_pGlobalState->config.enabled->value();
    const auto                 PRECEDENCE = g_pGlobalState->config.barPrecedenceOverBorder->value();

    SDecorationPositioningInfo info;
    const bool                 HIDDEN = shouldHide();
    info.policy                       = HIDDEN ? DECORATION_POSITION_ABSOLUTE : DECORATION_POSITION_STICKY;
    info.edges                        = DECORATION_EDGE_TOP;
    info.priority                     = PRECEDENCE ? 10005 : 5000;
    info.reserved                     = true;
    info.desiredExtents               = {{0, HIDDEN || !ENABLED ? 0 : HEIGHT}, {0, 0}};
    return info;
}

void CHyprBar::onPositioningReply(const SDecorationPositioningReply& reply) {
    if (reply.assignedGeometry.size() != m_bAssignedBox.size())
        m_bWindowSizeChanged = true;

    m_bAssignedBox = reply.assignedGeometry;
}

std::string CHyprBar::getDisplayName() {
    return "Hyprtouchbar";
}

bool CHyprBar::shouldHide() const {
    return m_hidden || windowHasCSD(m_pWindow.lock());
}

bool CHyprBar::buttonEnabled(const SHyprButton& button) const {
    switch (button.action) {
        case eTouchbarButtonAction::CLOSE: return g_pGlobalState->config.showClose->value();
        case eTouchbarButtonAction::MAXIMIZE: return g_pGlobalState->config.showMaximize->value();
        case eTouchbarButtonAction::MINIMIZE: return g_pGlobalState->config.showMinimize->value();
        default: return true;
    }
}

bool CHyprBar::inputIsValid() {
    if (!g_pGlobalState->config.enabled->value() || shouldHide())
        return false;

    if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_pWindow->wlSurface()->resource()))
        return false;

    const auto MOUSE    = g_pInputManager->getMouseCoordsInternal();
    auto       PMONITOR = Desktop::focusState()->monitor();

    if (!PMONITOR)
        return false;

    Desktop::CViewHitTester hitTester{*Desktop::viewState()};

    const auto              WINDOWATCURSOR = hitTester.windowAt(MOUSE, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

    auto                    focusState = Desktop::focusState();
    auto                    window     = focusState->window();

    if (WINDOWATCURSOR != m_pWindow && m_pWindow != window)
        return false;

    PHLLS    foundSurface = nullptr;
    Vector2D surfaceCoords;

    // Check Top Layer
    hitTester.layerSurfaceAt(MOUSE, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &foundSurface);
    if (foundSurface)
        return false;

    // Check Overlay Layer
    hitTester.layerSurfaceAt(MOUSE, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &foundSurface);
    if (foundSurface)
        return false;

    return true;
}

void CHyprBar::onMouseButton(Event::SCallbackInfo& info, IPointer::SButtonEvent e) {
    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        // A drag commonly ends outside the titlebar. Always finish a gesture
        // this decoration started instead of leaving mouse/stylus move mode
        // armed because the release point no longer passes hit testing.
        if (m_bDragPending || m_bDraggingThis || m_bCancelledDown)
            handleUpEvent(info);
        return;
    }

    if (inputIsValid())
        handleDownEvent(info, std::nullopt);
}

void CHyprBar::onTouchDown(Event::SCallbackInfo& info, ITouch::SDownEvent e) {
    // Touch IDs are not guaranteed to start at zero (notably under nested
    // compositors). Track whichever finger starts the drag instead. Do not use
    // inputIsValid() here: it intentionally checks mouse coordinates, which
    // made touch dragging depend on where the mouse cursor happened to be.
    if (!g_pGlobalState->config.enabled->value() || shouldHide() || m_bDragPending || m_bDraggingThis)
        return;
    if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_pWindow->wlSurface()->resource()))
        return;

    PHLMONITOR monitor = nullptr;
    for (const auto& candidate : State::monitorState()->monitors()) {
        if (candidate->m_name == (!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "")) {
            monitor = candidate;
            break;
        }
    }
    monitor = monitor ? monitor : Desktop::focusState()->monitor();
    if (!monitor)
        return;

    const auto touch = monitor->m_position + e.pos * monitor->m_size;
    const auto box   = assignedBoxGlobal();
    if (!VECINRECT(touch, box.x, box.y, box.x + box.w, box.y + box.h))
        return;

    handleDownEvent(info, e);
}

void CHyprBar::onTouchUp(Event::SCallbackInfo& info, ITouch::SUpEvent e) {
    if (!m_bDragPending || !m_bTouchEv || e.touchID != m_touchId)
        return;

    handleUpEvent(info);
}

void CHyprBar::onMouseMove(Vector2D coords) {
    // Hardware cursors do not damage the titlebar, so explicitly redraw hover states.
    damageOnButtonHover();

    if (!m_bDragPending || m_bTouchEv || !validMapped(m_pWindow))
        return;

    m_bDragPending = false;
    handleMovement();
}

void CHyprBar::onTouchMove(Event::SCallbackInfo& info, ITouch::SMotionEvent e) {
    if (!m_bDragPending || !m_bTouchEv || !validMapped(m_pWindow) || e.touchID != m_touchId)
        return;

    // The input device stays mapped to the output on which the gesture began,
    // even if the dragged window moves to another monitor.
    const auto PMONITOR = m_touchMonitor.lock();
    if (!PMONITOR)
        return;
    const auto COORDS   = PMONITOR->m_position + e.pos * PMONITOR->m_size;
    m_lastTouchPosition = COORDS;

    if (!m_bDraggingThis && (COORDS - m_touchDownPosition).size() < g_pGlobalState->config.csdDragThreshold->value())
        return;

    const auto PWINDOW = m_pWindow.lock();
    if (!m_bDraggingThis) {
        if (g_pGlobalState->maximizeManager && g_pGlobalState->maximizeManager->prepareDrag(PWINDOW, COORDS))
            m_touchGrabOffset = COORDS - PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
        // `setfloating` is idempotent. Unlike the old implementation, do not
        // resize an already-floating window and never tile it on release.
        if (!PWINDOW->m_isFloating)
            g_pKeybindManager->m_dispatchers["setfloating"]("activewindow");

        // Temporarily pin while dragging across workspaces, then restore the
        // original pin state when the finger is released.
        if (!PWINDOW->m_pinned) {
            (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_ENABLE, PWINDOW);
            m_bPinnedForTouchDrag = true;
        }
    }

    const auto TARGET = COORDS - m_touchGrabOffset;
    DragMotion::moveImmediately(PWINDOW, TARGET);
    m_bDraggingThis = true;
    info.cancelled  = true;
}

void CHyprBar::handleDownEvent(Event::SCallbackInfo& info, std::optional<ITouch::SDownEvent> touchEvent) {
    m_bTouchEv = touchEvent.has_value();
    if (m_bTouchEv)
        m_touchId = touchEvent.value().touchID;

    const auto PWINDOW = m_pWindow.lock();

    auto       COORDS = cursorRelativeToBar();
    if (m_bTouchEv) {
        ITouch::SDownEvent e        = touchEvent.value();
        PHLMONITOR         PMONITOR = nullptr;
        for (auto& m : State::monitorState()->monitors()) {
            if (m->m_name == (!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "")) {
                PMONITOR = m;
                break;
            }
        }
        PMONITOR = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
        if (!PMONITOR)
            return;
        m_touchMonitor        = PMONITOR;
        m_touchDownPosition   = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y);
        m_lastTouchPosition   = m_touchDownPosition;
        m_touchGrabOffset     = m_touchDownPosition - PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        m_bPinnedForTouchDrag = false;
        COORDS                = m_touchDownPosition - assignedBoxGlobal().pos();
    }

    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const auto ON_DOUBLE_CLICK  = g_pGlobalState->config.onDoubleClick->value();

    const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";

    if (!VECINRECT(COORDS, 0, 0, assignedBoxGlobal().w, HEIGHT - 1)) {

        if (m_bDraggingThis) {
            if (m_bPinnedForTouchDrag)
                (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE, PWINDOW);
            m_bPinnedForTouchDrag = false;
            g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
            Log::logger->log(Log::DEBUG, "[hyprtouchbar] Dragging ended on {:x}", (uintptr_t)PWINDOW.get());
        }

        m_bDraggingThis = false;
        m_bDragPending  = false;
        m_bTouchEv      = false;
        return;
    }

    if (Desktop::focusState()->window() != PWINDOW)
        Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

    if (PWINDOW->m_isFloating)
        Desktop::windowState()->raise(PWINDOW);

    info.cancelled   = true;
    m_bCancelledDown = true;

    if (doButtonPress(BARPADDING, BARBUTTONPADDING, HEIGHT, COORDS, BUTTONSRIGHT))
        return;

    if (!ON_DOUBLE_CLICK.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastMouseDown).count() < 400 /* Arbitrary delay I found suitable */) {
        if (ON_DOUBLE_CLICK == "maximize")
            runTouchbarAction(PWINDOW, eTouchbarButtonAction::MAXIMIZE);
        else
            Config::Supplementary::executor()->spawn(ON_DOUBLE_CLICK);
        m_bDragPending = false;
    } else {
        m_lastMouseDown = Time::steadyNow();
        m_bDragPending  = true;
    }
}

void CHyprBar::handleUpEvent(Event::SCallbackInfo& info, bool snap) {
    if (m_pWindow.lock() != Desktop::focusState()->window() && !m_bDraggingThis)
        return;

    if (m_bCancelledDown)
        info.cancelled = true;

    m_bCancelledDown = false;

    if (m_bDraggingThis) {
        g_pKeybindManager->changeMouseBindMode(MBIND_INVALID);
        m_bDraggingThis = false;
        if (m_bPinnedForTouchDrag)
            (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE, m_pWindow.lock());
        m_bPinnedForTouchDrag = false;

        if (snap && m_bTouchEv && g_pGlobalState->topEdgeSnap)
            g_pGlobalState->topEdgeSnap->finish(m_pWindow.lock(), m_lastTouchPosition);

        Log::logger->log(Log::DEBUG, "[hyprtouchbar] Dragging ended on {:x}", (uintptr_t)m_pWindow.lock().get());
    }

    m_bDragPending = false;
    m_bTouchEv     = false;
    m_touchId      = 0;
    m_touchMonitor.reset();
}

void CHyprBar::handleMovement() {
    // The first motion may already be outside the bar. Drag the window that
    // received the press, not whichever window is now under the pointer.
    if (!validMapped(m_pWindow) || g_layoutManager->dragController()->target())
        return;
    if (g_pGlobalState->maximizeManager)
        g_pGlobalState->maximizeManager->prepareDrag(m_pWindow.lock(), g_pInputManager->getMouseCoordsInternal());
    g_layoutManager->beginDragTarget(m_pWindow->layoutTarget(), MBIND_MOVE);
    m_bDraggingThis = true;
    Log::logger->log(Log::DEBUG, "[hyprtouchbar] Dragging initiated on {:x}", (uintptr_t)m_pWindow.lock().get());
    return;
}

bool CHyprBar::doButtonPress(Config::INTEGER barPadding, Config::INTEGER barButtonPadding, Config::INTEGER barHeight, Vector2D COORDS, const bool BUTTONSRIGHT) {
    //check if on a button
    float offset = barPadding;

    for (auto& b : g_pGlobalState->buttons) {
        if (!buttonEnabled(b))
            continue;
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, barHeight};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - barButtonPadding - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

        // Use the full titlebar height as the touch target while keeping the
        // visible control compact.
        if (VECINRECT(COORDS, currentPos.x, 0, currentPos.x + b.size + barButtonPadding, barHeight)) {
            runTouchbarAction(m_pWindow.lock(), b.action, b.cmd);
            return true;
        }

        offset += barButtonPadding + b.size;
    }
    return false;
}

void CHyprBar::renderBarTitle(const Vector2D& bufferSize, const float scale) {
    const auto COLORVAL         = g_pGlobalState->config.textColor->value();
    const auto SIZE             = g_pGlobalState->config.barTextSize->value();
    const auto WEIGHT           = g_pGlobalState->config.barTextWeight->value();
    const auto FONT             = g_pGlobalState->config.barTextFont->value();
    const auto ALIGN            = g_pGlobalState->config.barTextAlign->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();

    float      buttonSizes = BARBUTTONPADDING;
    for (auto& b : g_pGlobalState->buttons) {
        if (buttonEnabled(b))
            buttonSizes += b.size + BARBUTTONPADDING;
    }

    const int  scaledSize        = std::round(SIZE * scale);
    const auto scaledButtonsSize = buttonSizes * scale;
    const auto scaledBarPadding  = BARPADDING * scale;
    const auto scaledIconSpace   = m_pAppIconTex ? (g_pGlobalState->config.appIconSize->value() + 6) * scale : 0;
    const int  paddingTotal      = scaledBarPadding * 2 + scaledButtonsSize + scaledIconSpace + (ALIGN != "left" ? scaledButtonsSize : 0);
    const int  maxWidth          = std::clamp(static_cast<int>(bufferSize.x - paddingTotal), 0, INT_MAX);

    if (m_szLastTitle.empty() || maxWidth < 1) {
        m_pTextTex = nullptr;
        return;
    }

    const CHyprColor COLOR = m_bForcedTitleColor.value_or(configColor(COLORVAL));
    m_pTextTex             = g_pHyprRenderer->renderText(m_szLastTitle, COLOR, scaledSize, false, FONT, maxWidth, WEIGHT.m_value);
}

void CHyprBar::updateAppIcon(const float scale) {
    if (!g_pGlobalState->config.showAppIcon->value()) {
        if (m_pAppIconTex)
            m_pTextTex = nullptr;
        m_pAppIconTex = nullptr;
        return;
    }

    const auto PWINDOW   = m_pWindow.lock();
    const auto ICONCLASS = PWINDOW->m_class + "\n" + PWINDOW->m_initialClass;
    const auto THEME     = g_pGlobalState->config.appIconTheme->value();
    const int  PIXELSIZE = std::max(1, static_cast<int>(std::round(g_pGlobalState->config.appIconSize->value() * scale)));
    if (ICONCLASS == m_szLastIconClass && THEME == m_szLastIconTheme && PIXELSIZE == m_iLastIconSize)
        return;

    m_szLastIconClass = ICONCLASS;
    m_szLastIconTheme = THEME;
    m_iLastIconSize   = PIXELSIZE;
    m_pAppIconTex     = AppIcon::load(PWINDOW->m_class, PWINDOW->m_initialClass, THEME, PIXELSIZE);
    m_pTextTex        = nullptr;
}

SP<Render::ITexture> CHyprBar::builtinButtonIcon(eTouchbarButtonAction action, const float scale) {
    const auto THEME     = g_pGlobalState->config.buttonIconTheme->value();
    const auto COLOR     = g_pGlobalState->config.buttonIconColor->value();
    const int  PIXELSIZE = static_cast<int>(std::ceil(std::clamp<double>(g_pGlobalState->config.buttonIconSize->value() * scale, 1.0, 256.0)));

    if (THEME != m_szLastButtonIconTheme || COLOR != m_iLastButtonIconColor || PIXELSIZE != m_iLastButtonIconSize) {
        m_szLastButtonIconTheme = THEME;
        m_iLastButtonIconColor  = COLOR;
        m_iLastButtonIconSize   = PIXELSIZE;

        const auto tint    = configColor(COLOR);
        m_pCloseIconTex    = AppIcon::loadNamed("window-close-symbolic", THEME, PIXELSIZE, tint);
        m_pMinimizeIconTex = AppIcon::loadNamed("window-minimize-symbolic", THEME, PIXELSIZE, tint);
        m_pMaximizeIconTex = AppIcon::loadNamed("window-maximize-symbolic", THEME, PIXELSIZE, tint);
        m_pRestoreIconTex  = AppIcon::loadNamed("window-restore-symbolic", THEME, PIXELSIZE, tint);
    }

    switch (action) {
        case eTouchbarButtonAction::CLOSE: return m_pCloseIconTex;
        case eTouchbarButtonAction::MINIMIZE: return m_pMinimizeIconTex;
        case eTouchbarButtonAction::MAXIMIZE:
            return Fullscreen::controller()->getFullscreenModes(m_pWindow.lock()).client == Fullscreen::FSMODE_MAXIMIZED ? m_pRestoreIconTex : m_pMaximizeIconTex;
        default: return nullptr;
    }
}

size_t CHyprBar::getVisibleButtonCount(Config::INTEGER barButtonPadding, Config::INTEGER barPadding, const Vector2D& bufferSize, const float scale) {
    float  availableSpace = bufferSize.x - barPadding * scale * 2 - (m_pAppIconTex ? (g_pGlobalState->config.appIconSize->value() + 6) * scale : 0);
    size_t count          = 0;

    for (const auto& button : g_pGlobalState->buttons) {
        if (!buttonEnabled(button))
            continue;
        const float buttonSpace = (button.size + barButtonPadding) * scale;
        if (availableSpace >= buttonSpace) {
            count++;
            availableSpace -= buttonSpace;
        } else
            break;
    }

    return count;
}

void CHyprBar::renderBarButtons(CBox* barBox, const float scale, const float a) {
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const auto INACTIVECOLOR    = g_pGlobalState->config.inactiveButtonColor->value();

    const bool BUTTONSRIGHT    = ALIGNBUTTONS != "left";
    const auto visibleCount    = getVisibleButtonCount(BARBUTTONPADDING, BARPADDING, Vector2D{barBox->w, barBox->h}, scale);
    const bool INVALIDATEICONS = m_bButtonsDirty || m_bWindowSizeChanged;

    int        offset   = BARPADDING * scale;
    size_t     rendered = 0;
    for (size_t i = 0; i < g_pGlobalState->buttons.size() && rendered < visibleCount; ++i) {
        auto& button = g_pGlobalState->buttons[i];
        if (!buttonEnabled(button))
            continue;
        ++rendered;
        const auto scaledButtonSize = button.size * scale;
        const auto scaledButtonsPad = BARBUTTONPADDING * scale;

        auto       color    = button.bgcol;
        const bool hovering = (m_iButtonHoverState & (1U << i)) != 0;
        if (hovering)
            color = button.action == eTouchbarButtonAction::CLOSE ? configColor(g_pGlobalState->config.closeHoverColor->value()) :
                                                                    configColor(g_pGlobalState->config.buttonHoverColor->value());

        if (INACTIVECOLOR > 0 && !hovering) {
            color = m_bWindowHasFocus ? color : configColor(INACTIVECOLOR);
            if (INVALIDATEICONS && button.userfg && button.iconTex)
                button.iconTex = nullptr;
        }

        color.a *= a;

        CBox buttonBox = {barBox->x + (BUTTONSRIGHT ? barBox->w - offset - scaledButtonSize : offset), barBox->y + (barBox->h - scaledButtonSize) / 2.0, scaledButtonSize,
                          scaledButtonSize};
        buttonBox.round();

        g_pHyprOpenGL->renderRect(
            buttonBox, color, {.round = static_cast<int>(std::min<double>(g_pGlobalState->config.cornerRadius->value() * scale, scaledButtonSize / 2.0)), .roundingPower = 2.F});

        offset += scaledButtonsPad + scaledButtonSize;
    }
}

void CHyprBar::renderBarButtonsText(CBox* barBox, const float scale, const float a) {
    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const auto ICONONHOVER      = g_pGlobalState->config.iconOnHover->value();

    const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
    const auto visibleCount = getVisibleButtonCount(BARBUTTONPADDING, BARPADDING, Vector2D{barBox->w, barBox->h}, scale);
    const auto COORDS       = cursorRelativeToBar();

    int        offset        = BARPADDING * scale;
    float      noScaleOffset = BARPADDING;
    size_t     rendered      = 0;

    for (size_t i = 0; i < g_pGlobalState->buttons.size() && rendered < visibleCount; ++i) {
        auto& button = g_pGlobalState->buttons[i];
        if (!buttonEnabled(button))
            continue;
        ++rendered;
        const auto scaledButtonSize = button.size * scale;
        const auto scaledButtonsPad = BARBUTTONPADDING * scale;

        // check if hovering here
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, HEIGHT};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - BARBUTTONPADDING - button.size - noScaleOffset : noScaleOffset), (BARBUF.y - button.size) / 2.0}.floor();
        bool       hovering   = VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + button.size + BARBUTTONPADDING, currentPos.y + button.size);
        noScaleOffset += BARBUTTONPADDING + button.size;

        SP<Render::ITexture> iconTex;
        if (button.action != eTouchbarButtonAction::CUSTOM)
            iconTex = builtinButtonIcon(button.action, scale);

        // Text remains a fallback for custom buttons and icon themes that do
        // not provide Chromium's standard Linux symbolic icon names.
        if (!iconTex && (!button.iconTex || button.iconTex->m_texID == 0) && !button.icon.empty()) {
            auto fgcol     = button.userfg ? button.fgcol : (button.bgcol.r + button.bgcol.g + button.bgcol.b < 1) ? CHyprColor(0xFFFFFFFF) : CHyprColor(0xFF000000);
            button.iconTex = g_pHyprRenderer->renderText(button.icon, fgcol, std::round(button.size * 0.62 * scale), false, "sans", scaledButtonSize);
        }
        if (!iconTex)
            iconTex = button.iconTex;
        if (!iconTex || iconTex->m_texID == 0)
            continue;

        const auto  iconX = barBox->x + (BUTTONSRIGHT ? barBox->width - offset - scaledButtonSize / 2.0 : offset + scaledButtonSize / 2.0) - iconTex->m_size.x / 2.0;
        const auto  iconY = barBox->y + barBox->height / 2.0 - iconTex->m_size.y / 2.0;
        CBox        pos   = {iconX, iconY, iconTex->m_size.x, iconTex->m_size.y};

        const float inactiveOpacity = std::clamp<float>(g_pGlobalState->config.buttonInactiveOpacity->value(), 0.F, 1.F);
        const float iconAlpha       = button.action == eTouchbarButtonAction::CUSTOM || m_bWindowHasFocus || hovering ? a : a * inactiveOpacity;
        if (!ICONONHOVER || hovering)
            g_pHyprOpenGL->renderTexture(iconTex, pos, {.a = iconAlpha});
        offset += scaledButtonsPad + scaledButtonSize;

        bool currentBit = (m_iButtonHoverState & (1 << i)) != 0;
        if (hovering != currentBit) {
            m_iButtonHoverState ^= (1 << i);
            // damage to get rid of some artifacts when icons are "hidden"
            damageEntire();
        }
    }
}

void CHyprBar::draw(PHLMONITOR pMonitor, const float& a) {
    const auto ENABLED = g_pGlobalState->config.enabled->value();

    if (m_bLastEnabledState != ENABLED) {
        m_bLastEnabledState = ENABLED;
        g_pDecorationPositioner->repositionDeco(this);
    }

    if (shouldHide() || !validMapped(m_pWindow) || !ENABLED)
        return;

    const auto PWINDOW = m_pWindow.lock();

    if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault())
        return;

    auto data = CBarPassElement::SBarData{this, a};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(data));
}

void CHyprBar::renderPass(PHLMONITOR pMonitor, const float& a) {
    const auto  PWINDOW = m_pWindow.lock();

    static auto PENABLEBLURGLOBAL = CConfigValue<Config::BOOL>("decoration:blur:enabled");
    const auto  BARCOLOR          = g_pGlobalState->config.barColor->value();
    const auto  INACTIVEBARCOLOR  = g_pGlobalState->config.inactiveBarColor->value();
    const auto  HEIGHT            = g_pGlobalState->config.barHeight->value();
    const auto  PRECEDENCE        = g_pGlobalState->config.barPrecedenceOverBorder->value();
    const auto  ALIGNBUTTONS      = g_pGlobalState->config.barButtonsAlignment->value();
    const auto  ENABLETITLE       = g_pGlobalState->config.barTitleEnabled->value();
    const auto  ENABLEBLUR        = g_pGlobalState->config.barBlur->value();
    const auto  INACTIVECOLOR     = g_pGlobalState->config.inactiveButtonColor->value();

    const bool  currentWindowFocus = PWINDOW == Desktop::focusState()->window();
    if (currentWindowFocus != m_bWindowHasFocus) {
        m_bWindowHasFocus = currentWindowFocus;
        m_bButtonsDirty   = true;
    }

    const CHyprColor DEST_COLOR = m_bForcedBarColor.value_or(configColor(currentWindowFocus ? BARCOLOR : INACTIVEBARCOLOR));
    if (DEST_COLOR != m_cRealBarColor->goal())
        *m_cRealBarColor = DEST_COLOR;

    CHyprColor color = m_cRealBarColor->value();

    color.a *= a;
    const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
    const bool SHOULDBLUR   = ENABLEBLUR && *PENABLEBLURGLOBAL && color.a < 1.F;

    if (HEIGHT < 1) {
        m_iLastHeight = HEIGHT;
        return;
    }

    const auto PWORKSPACE      = PWINDOW->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !PWINDOW->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    // Match Hyprland's outer window/shadow radius exactly. The inherited
    // hyprbars code subtracted two physical pixels here, making the titlebar
    // more square than the window mask and exposing blurred corner triangles.
    const auto ROUNDINGBASE     = PWINDOW->rounding();
    const auto ROUNDINGPOWER    = PWINDOW->roundingPower();
    const auto OUTERBORDER      = PRECEDENCE ? 0 : PWINDOW->getRealBorderSize();
    const auto CORRECTIONOFFSET = OUTERBORDER * (std::numbers::sqrt2 - 1) * std::max(2.0 - ROUNDINGPOWER, 0.0);
    const auto ROUNDING         = ROUNDINGBASE > 0 ? ROUNDINGBASE + OUTERBORDER - CORRECTIONOFFSET : 0;
    const auto scaledRounding   = ROUNDING * pMonitor->m_scale;

    m_seExtents = {{0, HEIGHT}, {}};

    const auto DECOBOX = assignedBoxGlobal();

    const auto BARBUF = DECOBOX.size() * pMonitor->m_scale;

    CBox       titleBarBox = {DECOBOX.x - pMonitor->m_position.x, DECOBOX.y - pMonitor->m_position.y, DECOBOX.w,
                              DECOBOX.h + ROUNDING * 3 /* to fill the bottom cuz we can't disable rounding there */};

    titleBarBox.translate(PWINDOW->m_floatingOffset).scale(pMonitor->m_scale).round();

    if (titleBarBox.w < 1 || titleBarBox.h < 1)
        return;

    g_pHyprOpenGL->scissor(titleBarBox);

    if (ROUNDING) {
        // the +1 is a shit garbage temp fix until renderRect supports an alpha matte
        CBox windowBox = {PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + PWINDOW->m_floatingOffset.x - pMonitor->m_position.x + 1,
                          PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y + PWINDOW->m_floatingOffset.y - pMonitor->m_position.y + 1,
                          PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x - 2, PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y - 2};

        if (windowBox.w < 1 || windowBox.h < 1)
            return;

        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);

        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, true);

        glStencilFunc(GL_ALWAYS, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        windowBox.translate(WORKSPACEOFFSET).scale(pMonitor->m_scale).round();
        g_pHyprOpenGL->renderRect(windowBox, CHyprColor(0, 0, 0, 0), {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glStencilFunc(GL_NOTEQUAL, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }

    if (SHOULDBLUR)
        g_pHyprOpenGL->renderRect(titleBarBox, color, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower(), .blur = true, .blurA = a});
    else
        g_pHyprOpenGL->renderRect(titleBarBox, color, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});

    updateAppIcon(pMonitor->m_scale);

    // render title
    if (ENABLETITLE && (m_szLastTitle != PWINDOW->m_title || m_bWindowSizeChanged || !m_pTextTex || m_pTextTex->m_texID == 0 || m_bTitleColorChanged)) {
        m_szLastTitle = PWINDOW->m_title;
        renderBarTitle(BARBUF, pMonitor->m_scale);
    }

    if (ROUNDING) {
        // cleanup stencil
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
        glStencilMask(-1);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
    }

    CBox       textBox = {titleBarBox.x, titleBarBox.y, (int)BARBUF.x, (int)BARBUF.y};

    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    float      buttonSizes      = BARBUTTONPADDING;
    for (auto& b : g_pGlobalState->buttons) {
        if (buttonEnabled(b))
            buttonSizes += b.size + BARBUTTONPADDING;
    }
    const auto scaledButtonsSize = buttonSizes * pMonitor->m_scale;
    const auto scaledBarPadding  = BARPADDING * pMonitor->m_scale;
    const auto scaledIconSize    = g_pGlobalState->config.appIconSize->value() * pMonitor->m_scale;
    const auto iconOffset        = m_pAppIconTex ? scaledIconSize + 6 * pMonitor->m_scale : 0;

    if (m_pAppIconTex) {
        CBox iconBox = {textBox.x + scaledBarPadding + (BUTTONSRIGHT ? 0 : scaledButtonsSize), textBox.y + std::round((BARBUF.y - scaledIconSize) / 2.0), scaledIconSize,
                        scaledIconSize};
        g_pHyprOpenGL->renderTexture(m_pAppIconTex, iconBox, {.a = a});
    }

    if (ENABLETITLE && m_pTextTex) {
        const auto ALIGN            = g_pGlobalState->config.barTextAlign->value();
        const auto scaledBorderSize = PWINDOW->getRealBorderSize() * pMonitor->m_scale;
        const auto xOffset          = ALIGN == "left" ? std::round(scaledBarPadding + (BUTTONSRIGHT ? 0 : scaledButtonsSize) + iconOffset) :
                                                        std::round(((BARBUF.x - scaledBorderSize) / 2.0 - m_pTextTex->m_size.x / 2.0));
        const auto yOffset          = std::round((BARBUF.y - m_pTextTex->m_size.y) / 2.0);
        CBox       titleBox         = {textBox.x + xOffset, textBox.y + yOffset, m_pTextTex->m_size.x, m_pTextTex->m_size.y};

        g_pHyprOpenGL->renderTexture(m_pTextTex, titleBox, {.a = a});
    }

    renderBarButtons(&textBox, pMonitor->m_scale, a);
    m_bButtonsDirty = false;

    g_pHyprOpenGL->scissor(nullptr);

    renderBarButtonsText(&textBox, pMonitor->m_scale, a);

    m_bWindowSizeChanged = false;
    m_bTitleColorChanged = false;

    // dynamic updates change the extents
    if (m_iLastHeight != HEIGHT) {
        PWINDOW->layoutTarget()->recalc();
        m_iLastHeight = HEIGHT;
    }
}

eDecorationType CHyprBar::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CHyprBar::updateWindow(PHLWINDOW pWindow) {
    damageEntire();
}

void CHyprBar::onConfigReloaded() {
    m_bButtonsDirty      = true;
    m_bTitleColorChanged = true;
    m_pTextTex           = nullptr;
    m_pAppIconTex        = nullptr;
    m_pCloseIconTex      = nullptr;
    m_pMinimizeIconTex   = nullptr;
    m_pMaximizeIconTex   = nullptr;
    m_pRestoreIconTex    = nullptr;
    m_szLastIconClass.clear();
    m_szLastButtonIconTheme.clear();

    g_pDecorationPositioner->repositionDeco(this);
    damageEntire();
}

void CHyprBar::damageEntire() {
    g_pHyprRenderer->damageBox(assignedBoxGlobal());
}

Vector2D CHyprBar::cursorRelativeToBar() {
    return g_pInputManager->getMouseCoordsInternal() - assignedBoxGlobal().pos();
}

eDecorationLayer CHyprBar::getDecorationLayer() {
    return DECORATION_LAYER_UNDER;
}

uint64_t CHyprBar::getDecorationFlags() {
    return DECORATION_ALLOWS_MOUSE_INPUT | (g_pGlobalState->config.barPartOfWindow->value() ? DECORATION_PART_OF_MAIN_WINDOW : 0);
}

CBox CHyprBar::assignedBoxGlobal() {
    if (!validMapped(m_pWindow))
        return {};

    CBox box = m_bAssignedBox;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_pWindow.lock()));

    const auto PWORKSPACE      = m_pWindow->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !m_pWindow->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    return box.translate(WORKSPACEOFFSET);
}

PHLWINDOW CHyprBar::getOwner() {
    return m_pWindow.lock();
}

bool CHyprBar::containsPoint(const Vector2D& global) {
    if (!g_pGlobalState->config.enabled->value() || shouldHide() || !validMapped(m_pWindow))
        return false;
    const auto box = assignedBoxGlobal();
    return VECINRECT(global, box.x, box.y, box.x + box.w, box.y + box.h);
}

void CHyprBar::updateRules() {
    const auto PWINDOW              = m_pWindow.lock();
    auto       prevHidden           = m_hidden;
    auto       prevForcedTitleColor = m_bForcedTitleColor;

    m_bForcedBarColor   = std::nullopt;
    m_bForcedTitleColor = std::nullopt;
    m_hidden            = false;

    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->nobarRuleIdx))
        m_hidden = truthy(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->nobarRuleIdx)->effect);
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->barColorRuleIdx))
        m_bForcedBarColor = CHyprColor(Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->barColorRuleIdx)->effect).value_or(0));
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->titleColorRuleIdx))
        m_bForcedTitleColor = CHyprColor(Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->titleColorRuleIdx)->effect).value_or(0));

    const bool currentCSD = windowHasCSD(PWINDOW);
    if (prevHidden != m_hidden || currentCSD != m_bLastCSDState)
        g_pDecorationPositioner->repositionDeco(this);
    m_bLastCSDState = currentCSD;
    if (prevForcedTitleColor != m_bForcedTitleColor)
        m_bTitleColorChanged = true;
}

void CHyprBar::damageOnButtonHover() {
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const bool BUTTONSRIGHT     = ALIGNBUTTONS != "left";

    float      offset = BARPADDING;

    const auto COORDS = cursorRelativeToBar();

    for (size_t i = 0; i < g_pGlobalState->buttons.size(); ++i) {
        auto& b = g_pGlobalState->buttons[i];
        if (!buttonEnabled(b))
            continue;
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, HEIGHT};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - BARBUTTONPADDING - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

        const bool hover    = VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + b.size + BARBUTTONPADDING, currentPos.y + b.size);
        const bool oldHover = (m_iButtonHoverState & (1U << i)) != 0;
        if (hover != oldHover) {
            m_iButtonHoverState ^= (1U << i);
            damageEntire();
        }

        offset += BARBUTTONPADDING + b.size;
    }
}
