#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FontWeightValue.hpp>

inline HANDLE PHANDLE = nullptr;

enum class eTouchbarButtonAction {
    CUSTOM,
    CLOSE,
    MAXIMIZE,
    MINIMIZE,
};

struct SHyprButton {
    std::string           cmd    = "";
    bool                  userfg = false;
    CHyprColor            fgcol  = CHyprColor(0, 0, 0, 0);
    CHyprColor            bgcol  = CHyprColor(0, 0, 0, 0);
    float                 size   = 10;
    std::string           icon   = "";
    SP<Render::ITexture>  iconTex;
    eTouchbarButtonAction action = eTouchbarButtonAction::CUSTOM;
};

class CHyprBar;
class CCSDManager;
class CTopEdgeSnap;
class CMaximizeManager;

struct SGlobalState {
    std::vector<SHyprButton>  buttons;
    std::vector<WP<CHyprBar>> bars;
    UP<CCSDManager>           csdManager;
    UP<CTopEdgeSnap>          topEdgeSnap;
    UP<CMaximizeManager>      maximizeManager;

    uint32_t                  nobarRuleIdx      = 0;
    uint32_t                  barColorRuleIdx   = 0;
    uint32_t                  titleColorRuleIdx = 0;
    uint32_t                  forceCSDRuleIdx   = 0;
    uint32_t                  forceSSDRuleIdx   = 0;

    struct {
        SP<Config::Values::CColorValue>      barColor, inactiveBarColor, textColor, inactiveButtonColor, buttonHoverColor, closeHoverColor, buttonIconColor;
        SP<Config::Values::CIntValue>        barHeight;
        SP<Config::Values::CIntValue>        barTextSize;
        SP<Config::Values::CFontWeightValue> barTextWeight;
        SP<Config::Values::CIntValue>        barPadding;
        SP<Config::Values::CIntValue>        barButtonPadding;
        SP<Config::Values::CIntValue>        cornerRadius;
        SP<Config::Values::CBoolValue>       topEdgeMaximize;
        SP<Config::Values::CIntValue>        topEdgeDistance;
        SP<Config::Values::CIntValue>        appIconSize, buttonIconSize;
        SP<Config::Values::CIntValue>        csdTitlebarHeight, csdControlsLeft, csdControlsRight, csdDragThreshold;
        SP<Config::Values::CBoolValue>       barBlur, barTitleEnabled, barPartOfWindow, barPrecedenceOverBorder, enabled, iconOnHover;
        SP<Config::Values::CBoolValue>   builtinButtons, showClose, showMaximize, showMinimize, showAppIcon, stylusDragEnabled, csdDragEnabled, csdTouchEmulation, csdDragFallback;
        SP<Config::Values::CStringValue> barTextFont, barTextAlign, barButtonsAlignment, appIconTheme, buttonIconTheme, onDoubleClick;
        SP<Config::Values::CStringValue> csdDetection, minimizeAction, maximizeAction, closeAction, maximizeMode;
    } config;
};

inline UP<SGlobalState> g_pGlobalState;

bool                    windowHasCSD(const PHLWINDOW& window);
void                    runTouchbarAction(const PHLWINDOW& window, eTouchbarButtonAction action, const std::string& custom = "");
