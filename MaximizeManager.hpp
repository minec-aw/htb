#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <optional>
#include <memory>
#include <unordered_map>

// Floating windows: ordinary z-order, work-area geometry, client maximized bit.
// Tiled/grouped windows: native layout maximization, preserving their tile slot.
class CMaximizeManager {
  public:
    CMaximizeManager();
    ~CMaximizeManager();
    void set(const PHLWINDOW& window, bool maximized);
    void toggle(const PHLWINDOW& window);
    bool isDesktopMaximized(const PHLWINDOW& window) const;
    bool prepareDrag(const PHLWINDOW& window, const Vector2D& point);
    void scheduleRefresh();

  private:
    struct SState {
        PHLWINDOWREF           window;
        bool                   desktop   = false;
        bool                   suspended = false; // true fullscreen temporarily covers desktop maximization
        CBox                   restoreBox;
        CBox                   workArea;
        Vector2D               restoreMonitorOrigin;
        std::optional<int64_t> border, rounding;
        std::optional<bool>    shadow;
    };
    std::unordered_map<Desktop::View::CWindow*, SState> m_states;
    UP<SEventLoopDoLaterLock>                           m_pending;
    std::shared_ptr<int>                                m_lifetime = std::make_shared<int>(0);
    bool                                                m_busy     = false;
    bool                                                m_stopping = false;
    CHyprSignalListener                                 m_open, m_close, m_fullscreen, m_floating, m_rules, m_config, m_monitor, m_workspace, m_mouseMove, m_active, m_render;

    bool                                                useDesktop(const PHLWINDOW& window) const;
    void                                                setModes(const PHLWINDOW& window, int internal, int client);
    void                                                notifyClient(const PHLWINDOW& window, bool maximized);
    void                                                stripFrame(const PHLWINDOW& window, SState& state);
    void                                                restoreFrame(const PHLWINDOW& window, const SState& state);
    void                                                fit(const PHLWINDOW& window);
    void                                                restore(const PHLWINDOW& window, bool geometry = true);
    void                                                refresh();
    void                                                onNativeDrag();
};
