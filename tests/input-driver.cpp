// Test-only plugin. Load ONLY in an isolated nested compositor.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <aquamarine/input/Input.hpp>
#define private public
#include <hyprland/src/protocols/core/Seat.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#undef private
#include <sstream>
#include <linux/input-event-codes.h>

class CTestTouch : public ITouch {
  public:
    bool isVirtual() override {
        return true;
    }
    SP<Aquamarine::ITouch> aq() override {
        return nullptr;
    }
};
class CTestTablet : public Aquamarine::ITablet {
  public:
    const std::string& getName() override {
        static const std::string name = "htb-test-tablet";
        return name;
    }
};
class CTestTool : public Aquamarine::ITabletTool {
  public:
    const std::string& getName() override {
        static const std::string name = "htb-test-pen";
        return name;
    }
};
static SP<CTestTouch>  touch;
static SP<CTestTablet> aqTablet;
static SP<CTestTool>   tool;
static SP<CTablet>     tablet;
static uint32_t        timeMs = 100;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash())
        throw std::runtime_error("input-driver: ABI mismatch");
    touch      = makeShared<CTestTouch>();
    aqTablet   = makeShared<CTestTablet>();
    tool       = makeShared<CTestTool>();
    tool->type = Aquamarine::ITabletTool::AQ_TABLET_TOOL_TYPE_PEN;
    tablet     = CTablet::create(aqTablet);
    HyprlandAPI::addDispatcherV2(handle, "htb-test.input", [](std::string args) -> SDispatchResult {
        std::istringstream stream(args);
        std::string        type;
        double             x = 0, y = 0;
        int                id = 7;
        stream >> type >> x >> y >> id;
        timeMs += 20;
        if (type == "move" || type == "hover") {
            Pointer::mgr()->warpTo({x, y});
            if (type == "hover")
                g_pInputManager->refocus();
            else
                g_pInputManager->simulateMouseMovement();
        } else if (type == "down" || type == "up") {
            g_pInputManager->onMouseButton(
                IPointer::SButtonEvent{.timeMs = timeMs, .button = BTN_LEFT, .state = type == "down" ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED},
                nullptr);
        } else if (type == "touch-down") {
            const auto monitor   = Desktop::focusState()->monitor();
            touch->m_boundOutput = monitor->m_name;
            g_pInputManager->onTouchDown(ITouch::SDownEvent{.timeMs = timeMs, .touchID = id, .pos = (Vector2D{x, y} - monitor->m_position) / monitor->m_size, .device = touch});
        } else if (type == "touch-move") {
            const auto monitor = Desktop::focusState()->monitor();
            g_pInputManager->onTouchMove(ITouch::SMotionEvent{.timeMs = timeMs, .touchID = id, .pos = (Vector2D{x, y} - monitor->m_position) / monitor->m_size});
        } else if (type == "touch-up") {
            g_pInputManager->onTouchUp(ITouch::SUpEvent{.timeMs = timeMs, .touchID = id});
        } else if (type == "touch-cancel") {
            Event::SCallbackInfo info;
            Event::bus()->m_events.input.touch.cancel.emit(ITouch::SCancelEvent{.timeMs = timeMs, .touchID = id}, info);
        } else if (type == "native-move" || type == "native-resize") {
            if (const auto w = Desktop::focusState()->window())
                g_layoutManager->beginDragTarget(w->layoutTarget(), type == "native-move" ? MBIND_MOVE : MBIND_RESIZE, std::nullopt, true);
        } else if (type == "pen-down" || type == "pen-up" || type == "pen-move" || type == "pen-out") {
            const auto monitor       = Desktop::focusState()->monitor();
            tablet->m_boundOutput    = monitor->m_name;
            const auto           pos = (Vector2D{x, y} - monitor->m_position) / monitor->m_size;
            Event::SCallbackInfo info;
            if (type == "pen-move")
                Event::bus()->m_events.input.tablet.axis.emit(
                    CTablet::SAxisEvent{
                        .tool = tool, .tablet = tablet, .timeMs = timeMs, .updatedAxes = CTablet::HID_TABLET_TOOL_AXIS_X | CTablet::HID_TABLET_TOOL_AXIS_Y, .axis = pos},
                    info);
            else if (type == "pen-out")
                Event::bus()->m_events.input.tablet.proximity.emit(CTablet::SProximityEvent{.tool = tool, .tablet = tablet, .timeMs = timeMs, .proximity = pos, .in = false}, info);
            else
                Event::bus()->m_events.input.tablet.tip.emit(CTablet::STipEvent{.tool = tool, .tablet = tablet, .timeMs = timeMs, .tip = pos, .in = type == "pen-down"}, info);
        } else if (type == "client-move-request") {
            // Exercise the plugin's native touch/tablet request branch. This
            // trusted test driver mints a seat serial rather than emulating a
            // full client tablet-v2 binding; production code never does this.
            const auto w    = Desktop::focusState()->window();
            const auto top  = w->m_xdgSurface->m_toplevel.lock();
            const auto seat = g_pSeatManager->seatResourceForClient(top->m_resource->client());
            top->m_resource->requests.move(top->m_resource.get(), seat->m_resource->resource(), g_pSeatManager->nextSerial(seat));
        } else if (type == "client-maximize" || type == "client-unmaximize") {
            const auto w   = Desktop::focusState()->window();
            const auto top = w->m_xdgSurface->m_toplevel.lock();
            if (type == "client-maximize")
                top->m_resource->requests.setMaximized(top->m_resource.get());
            else
                top->m_resource->requests.unsetMaximized(top->m_resource.get());
        } else if (type == "expect-client-maximized" || type == "expect-client-normal") {
            const auto w         = Desktop::focusState()->window();
            const auto top       = w->m_xdgSurface->m_toplevel.lock();
            const bool maximized = std::ranges::find(top->m_pendingApply.states, XDG_TOPLEVEL_STATE_MAXIMIZED) != top->m_pendingApply.states.end();
            if (maximized != (type == "expect-client-maximized"))
                return {.success = false, .error = "unexpected XDG maximized hint"};
        } else if (type == "clear-csd-hint") {
            // Reproduce 1.6.0's regression ONLY in the isolated test compositor.
            const auto w = Desktop::focusState()->window();
            w->m_xdgSurface->m_toplevel->setMaximized(false);
        } else if (type == "expect-csd-offset" || type == "expect-no-csd-offset") {
            const auto w        = Desktop::focusState()->window();
            const auto geometry = w->m_xdgSurface->m_current.geometry;
            const bool offset   = geometry.x != 0 || geometry.y != 0;
            if (offset != (type == "expect-csd-offset"))
                return {.success = false, .error = std::format("unexpected XDG geometry: {},{} {}x{}", geometry.x, geometry.y, geometry.w, geometry.h)};
        } else if (type == "expect-square") {
            const auto w = Desktop::focusState()->window();
            if (w->getRealBorderSize() != 0 || w->rounding() != 0 || !w->m_ruleApplicator->noShadow().valueOrDefault())
                return {.success = false, .error = "maximized frame still decorated"};
        } else if (type == "expect-rounded") {
            const auto w = Desktop::focusState()->window();
            if (w->getRealBorderSize() == 0 || w->rounding() == 0 || w->m_ruleApplicator->noShadow().valueOrDefault())
                return {.success = false, .error = "restored frame still stripped"};
        } else if (type == "expect-frame") {
            const auto w = Desktop::focusState()->window();
            if (w->rounding() != x || w->getRealBorderSize() != y)
                return {.success = false, .error = "frame properties not restored to current configuration"};
        } else if (type == "expect-work-area") {
            const auto w       = Desktop::focusState()->window();
            const auto area    = w->m_monitor->logicalBoxMinusReserved();
            const auto extents = w->getFullWindowReservedArea();
            const auto actual  = w->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL);
            if (actual.pos() != area.pos() + extents.topLeft || actual.size() != area.size() - extents.topLeft - extents.bottomRight)
                return {.success = false, .error = "maximized bounds do not match usable area plus titlebar"};
        } else if (type == "expect-pointer-focus") {
            const auto w = Desktop::focusState()->window();
            if (w->wlSurface()->resource() != g_pSeatManager->m_state.pointerFocus.lock())
                return {.success = false, .error = "keyboard and pointer focus disagree"};
        } else if (type == "fullscreen") {
            Fullscreen::controller()->setFullscreenMode(Desktop::focusState()->window(), Fullscreen::FSMODE_FULLSCREEN, Fullscreen::FSMODE_FULLSCREEN);
        } else if (type == "restore") {
            Fullscreen::controller()->setFullscreenMode(Desktop::focusState()->window(), Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
        }
        return {};
    });
    return {"htb-input-driver", "Nested regression test input only", "hyprtouchbar tests", "1"};
}
APICALL EXPORT void PLUGIN_EXIT() {
    tablet.reset();
    tool.reset();
    aqTablet.reset();
    touch.reset();
}
