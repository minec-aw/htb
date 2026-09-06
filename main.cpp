#define WLR_USE_UNSTABLE

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>
#include <hyprland/src/config/lua/types/LuaConfigColor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>

#include "barDeco.hpp"
#include "CSDManager.hpp"
#include "TopEdgeSnap.hpp"
#include "MaximizeManager.hpp"
#include "globals.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static void addBuiltinButtons() {
    if (!g_pGlobalState->config.builtinButtons->value())
        return;

    // Helium's Linux controls are monochrome caption glyphs on the right.
    // The renderer places the first button nearest that edge, producing the
    // visible order minimize, maximize, close from left to right.
    constexpr auto GLYPH = 0xffe8eaed;
    g_pGlobalState->buttons.push_back(
        SHyprButton{.userfg = true, .fgcol = CHyprColor(GLYPH), .bgcol = CHyprColor(0x00000000), .size = 30, .icon = "×", .action = eTouchbarButtonAction::CLOSE});
    g_pGlobalState->buttons.push_back(
        SHyprButton{.userfg = true, .fgcol = CHyprColor(GLYPH), .bgcol = CHyprColor(0x00000000), .size = 30, .icon = "□", .action = eTouchbarButtonAction::MAXIMIZE});
    g_pGlobalState->buttons.push_back(
        SHyprButton{.userfg = true, .fgcol = CHyprColor(GLYPH), .bgcol = CHyprColor(0x00000000), .size = 30, .icon = "−", .action = eTouchbarButtonAction::MINIMIZE});
}

static void onNewWindow(PHLWINDOW window) {
    if (!window || std::ranges::any_of(window->m_windowDecorations, [](const auto& decoration) { return decoration->getDisplayName() == "Hyprtouchbar"; }))
        return;

    auto bar = makeUnique<CHyprBar>(window);
    g_pGlobalState->bars.emplace_back(bar);
    bar->m_self = bar;
    HyprlandAPI::addWindowDecoration(PHANDLE, window, std::move(bar));
}

static void onPreConfigReload() {
    g_pGlobalState->buttons.clear();
}

static void onConfigReloaded() {
    // Config values are final only after reload. Put built-ins nearest the edge,
    // then retain any custom buttons that Lua added while parsing.
    auto customButtons = std::move(g_pGlobalState->buttons);
    g_pGlobalState->buttons.clear();
    addBuiltinButtons();
    g_pGlobalState->buttons.insert(g_pGlobalState->buttons.end(), std::make_move_iterator(customButtons.begin()), std::make_move_iterator(customButtons.end()));

    for (auto& bar : g_pGlobalState->bars) {
        if (bar)
            bar->onConfigReloaded();
    }
    if (g_pGlobalState->csdManager)
        g_pGlobalState->csdManager->refresh();
}

static void onUpdateWindowRules(PHLWINDOW window) {
    const auto found = std::find_if(g_pGlobalState->bars.begin(), g_pGlobalState->bars.end(), [window](const auto& bar) { return bar && bar->getOwner() == window; });
    if (found != g_pGlobalState->bars.end()) {
        (*found)->updateRules();
        window->updateWindowData();
    }
    if (g_pGlobalState->csdManager)
        g_pGlobalState->csdManager->registerWindow(window);
}

int newLuaButton(lua_State* state) {
    if (!lua_istable(state, 1))
        return Config::Lua::Bindings::Internal::configError(state, "add_button: expected { bg_color, fg_color, size, icon, action }");

    SHyprButton button;
    {
        Hyprutils::Utils::CScopeGuard guard([state] { lua_pop(state, 1); });
        lua_getfield(state, 1, "bg_color");
        Config::Lua::CLuaConfigColor parser(0);
        if (parser.parse(state).errorCode != Config::Lua::PARSE_ERROR_OK)
            return Config::Lua::Bindings::Internal::configError(state, "add_button: invalid bg_color");
        button.bgcol = parser.parsed();
    }
    {
        Hyprutils::Utils::CScopeGuard guard([state] { lua_pop(state, 1); });
        lua_getfield(state, 1, "fg_color");
        Config::Lua::CLuaConfigColor parser(0);
        if (parser.parse(state).errorCode != Config::Lua::PARSE_ERROR_OK)
            return Config::Lua::Bindings::Internal::configError(state, "add_button: invalid fg_color");
        button.fgcol  = parser.parsed();
        button.userfg = true;
    }
    {
        Hyprutils::Utils::CScopeGuard guard([state] { lua_pop(state, 1); });
        lua_getfield(state, 1, "size");
        if (!lua_isnumber(state, -1))
            return Config::Lua::Bindings::Internal::configError(state, "add_button: size must be an integer");
        button.size = lua_tointeger(state, -1);
    }
    {
        Hyprutils::Utils::CScopeGuard guard([state] { lua_pop(state, 1); });
        lua_getfield(state, 1, "icon");
        if (!lua_isstring(state, -1))
            return Config::Lua::Bindings::Internal::configError(state, "add_button: icon must be a string");
        button.icon = lua_tostring(state, -1);
    }
    {
        Hyprutils::Utils::CScopeGuard guard([state] { lua_pop(state, 1); });
        lua_getfield(state, 1, "action");
        if (!lua_isstring(state, -1))
            return Config::Lua::Bindings::Internal::configError(state, "add_button: action must be a string");
        button.cmd = lua_tostring(state, -1);
    }

    g_pGlobalState->buttons.push_back(std::move(button));
    for (auto& bar : g_pGlobalState->bars) {
        if (bar)
            bar->m_bButtonsDirty = true;
    }
    return 0;
}

#define ADD_CONFIG(member, Type, key, description, fallback)                                                                                                                       \
    g_pGlobalState->config.member = makeShared<Config::Values::Type>("plugin:hyprtouchbar:" key, description, fallback);                                                           \
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.member)

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string runtimeHash = __hyprland_api_get_hash();
    const std::string headerHash  = __hyprland_api_get_client_hash();
    if (runtimeHash != headerHash) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprtouchbar] Version mismatch: rebuild the plugin for this Hyprland version", CHyprColor{1.F, .2F, .2F, 1.F}, 6000);
        throw std::runtime_error(std::format("[hyprtouchbar] Hyprland version mismatch: runtime={} headers={}", runtimeHash, headerHash));
    }

    g_pGlobalState                    = makeUnique<SGlobalState>();
    g_pGlobalState->nobarRuleIdx      = Desktop::Rule::windowEffects()->registerEffect("hyprtouchbar:no_bar");
    g_pGlobalState->barColorRuleIdx   = Desktop::Rule::windowEffects()->registerEffect("hyprtouchbar:bar_color");
    g_pGlobalState->titleColorRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprtouchbar:title_color");
    g_pGlobalState->forceCSDRuleIdx   = Desktop::Rule::windowEffects()->registerEffect("hyprtouchbar:force_csd");
    g_pGlobalState->forceSSDRuleIdx   = Desktop::Rule::windowEffects()->registerEffect("hyprtouchbar:force_ssd");

    ADD_CONFIG(barColor, CColorValue, "bar_color", "Active titlebar color", 0xe61c1c22);
    ADD_CONFIG(inactiveBarColor, CColorValue, "inactive_bar_color", "Inactive titlebar color", 0xd916161b);
    ADD_CONFIG(textColor, CColorValue, "col.text", "Title text color", 0xfff4f4f5);
    ADD_CONFIG(inactiveButtonColor, CColorValue, "inactive_button_color", "Inactive button color; transparent means disabled", 0x00000000);
    ADD_CONFIG(buttonHoverColor, CColorValue, "button_hover_color", "Button hover background", 0x33ffffff);
    ADD_CONFIG(closeHoverColor, CColorValue, "close_hover_color", "Close button hover background", 0xffe5484d);
    ADD_CONFIG(buttonIconColor, CColorValue, "button_icon_color", "Linux caption icon color", 0xffe8eaed);
    ADD_CONFIG(barHeight, CIntValue, "bar_height", "Titlebar height", 42);
    ADD_CONFIG(barTextSize, CIntValue, "bar_text_size", "Title text size", 13);
    ADD_CONFIG(barTextWeight, CFontWeightValue, "bar_text_weight", "Title font weight", 500);
    ADD_CONFIG(barPadding, CIntValue, "bar_padding", "Outer titlebar padding", 8);
    ADD_CONFIG(barButtonPadding, CIntValue, "bar_button_padding", "Space between titlebar buttons", 6);
    ADD_CONFIG(cornerRadius, CIntValue, "corner_radius", "Button corner radius", 6);
    ADD_CONFIG(appIconSize, CIntValue, "app_icon_size", "Application icon size", 24);
    ADD_CONFIG(buttonIconSize, CIntValue, "button_icon_size", "Linux caption icon size", 20);
    ADD_CONFIG(barTitleEnabled, CBoolValue, "bar_title_enabled", "Show the window title", true);
    ADD_CONFIG(barBlur, CBoolValue, "bar_blur", "Blur translucent titlebars", true);
    ADD_CONFIG(barPartOfWindow, CBoolValue, "bar_part_of_window", "Reserve titlebar space", true);
    ADD_CONFIG(barPrecedenceOverBorder, CBoolValue, "bar_precedence_over_border", "Draw titlebar above border", false);
    ADD_CONFIG(enabled, CBoolValue, "enabled", "Enable server-side titlebars", true);
    ADD_CONFIG(iconOnHover, CBoolValue, "icon_on_hover", "Only show button glyphs while hovered", false);
    ADD_CONFIG(builtinButtons, CBoolValue, "builtin_buttons", "Add built-in touch-friendly controls", true);
    ADD_CONFIG(showClose, CBoolValue, "show_close", "Show close button", true);
    ADD_CONFIG(showMaximize, CBoolValue, "show_maximize", "Show maximize button", true);
    ADD_CONFIG(showMinimize, CBoolValue, "show_minimize", "Show minimize button", true);
    ADD_CONFIG(showAppIcon, CBoolValue, "show_app_icon", "Show the application icon", true);
    ADD_CONFIG(barTextFont, CStringValue, "bar_text_font", "Title font", "Sans");
    ADD_CONFIG(barTextAlign, CStringValue, "bar_text_align", "Title alignment: left or center", "left");
    ADD_CONFIG(barButtonsAlignment, CStringValue, "bar_buttons_alignment", "Button alignment: left or right", "right");
    ADD_CONFIG(appIconTheme, CStringValue, "app_icon_theme", "Freedesktop icon theme name", "hicolor");
    ADD_CONFIG(buttonIconTheme, CStringValue, "button_icon_theme", "Icon theme for Linux caption controls", "Adwaita");
    ADD_CONFIG(onDoubleClick, CStringValue, "on_double_click", "maximize for built-in toggle, or a shell command on titlebar double click", "maximize");
    ADD_CONFIG(csdDetection, CStringValue, "csd_detection", "CSD detection: auto, all, or off", "auto");
    ADD_CONFIG(stylusDragEnabled, CBoolValue, "stylus_drag_enabled", "Forward stylus tip input through titlebar pointer handling", true);
    ADD_CONFIG(csdDragEnabled, CBoolValue, "csd_drag_enabled", "Enable touch interaction with CSD titlebars", true);
    ADD_CONFIG(csdTouchEmulation, CBoolValue, "csd_touch_emulation", "Forward CSD titlebar touch as pointer input for client-side hit testing", true);
    ADD_CONFIG(csdDragFallback, CBoolValue, "csd_drag_fallback", "Enable the approximate top-strip fallback", false);
    ADD_CONFIG(csdTitlebarHeight, CIntValue, "csd_titlebar_height", "CSD draggable top region height", 48);
    ADD_CONFIG(csdControlsLeft, CIntValue, "csd_controls_left", "Non-draggable CSD control width on the left", 0);
    ADD_CONFIG(csdControlsRight, CIntValue, "csd_controls_right", "Non-draggable CSD control width on the right", 150);
    ADD_CONFIG(csdDragThreshold, CIntValue, "csd_drag_threshold", "Movement before a CSD drag starts", 8);
    ADD_CONFIG(topEdgeMaximize, CBoolValue, "top_edge_maximize", "Maximize on window-drag release at a monitor's top edge", true);
    ADD_CONFIG(topEdgeDistance, CIntValue, "top_edge_distance", "Top-edge release zone in logical pixels (0-128)", 12);
    ADD_CONFIG(minimizeAction, CStringValue, "minimize_action", "Dispatcher action for SSD and CSD minimize", "movetoworkspacesilent special:minimized");
    ADD_CONFIG(maximizeAction, CStringValue, "maximize_action", "Dispatcher action overriding maximize; empty uses maximize_mode", "");
    ADD_CONFIG(maximizeMode, CStringValue, "maximize_mode", "auto: desktop-style floating maximize, native tiling; native: always use layout maximize", "auto");
    ADD_CONFIG(closeAction, CStringValue, "close_action", "Dispatcher action overriding native close; empty keeps native", "");

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprtouchbar", "add_button", ::newLuaButton);

    static auto openListener = Event::bus()->m_events.window.open.listen([](PHLWINDOW window) { onNewWindow(window); });
    static auto ruleListener = Event::bus()->m_events.window.updateRules.listen([](PHLWINDOW window) { onUpdateWindowRules(window); });
    static auto preReload    = Event::bus()->m_events.config.preReload.listen([] { onPreConfigReload(); });
    static auto reloaded     = Event::bus()->m_events.config.reloaded.listen([] { onConfigReloaded(); });

    g_pGlobalState->maximizeManager = makeUnique<CMaximizeManager>();
    g_pGlobalState->topEdgeSnap     = makeUnique<CTopEdgeSnap>();
    g_pGlobalState->csdManager      = makeUnique<CCSDManager>();
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->isHidden() && validMapped(window))
            onNewWindow(window);
    }

    HyprlandAPI::reloadConfig();
    return {"hyprtouchbar", "Modern, touch-friendly and CSD-aware titlebars", "hyprtouchbar contributors", "1.6.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Cancel idle maximization before input cleanup synthesizes releases.
    g_pGlobalState->topEdgeSnap.reset();
    g_pGlobalState->csdManager.reset();
    g_pGlobalState->maximizeManager.reset();
    for (const auto& monitor : State::monitorState()->monitors())
        monitor->m_scheduledRecalc = true;
    g_pHyprRenderer->m_renderPass.removeAllOfType("CBarPassElement");

    Desktop::Rule::windowEffects()->unregisterEffect(g_pGlobalState->forceSSDRuleIdx);
    Desktop::Rule::windowEffects()->unregisterEffect(g_pGlobalState->forceCSDRuleIdx);
    Desktop::Rule::windowEffects()->unregisterEffect(g_pGlobalState->barColorRuleIdx);
    Desktop::Rule::windowEffects()->unregisterEffect(g_pGlobalState->titleColorRuleIdx);
    Desktop::Rule::windowEffects()->unregisterEffect(g_pGlobalState->nobarRuleIdx);
}
