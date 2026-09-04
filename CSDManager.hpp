#pragma once

#include "globals.hpp"

#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>

#include <functional>
#include <unordered_map>

class CXdgToplevel;

namespace Event {
    struct SCallbackInfo;
}

class CCSDManager {
  public:
    CCSDManager();
    ~CCSDManager();

    void registerWindow(const PHLWINDOW& window);
    void refresh();
    bool beginProtocolTouchMove(const PHLWINDOW& window);

  private:
    struct SWindowHooks {
        CHyprSignalListener                                        stateChanged;
        SP<CXdgToplevel>                                           xdgResource;
        std::function<void(CXdgToplevel*, wl_resource*, uint32_t)> originalMove;
        bool                                                       moveOverridden       = false;
        bool                                                       listening            = false;
        bool                                                       maxSuppressionWanted = false;
        bool                                                       ownsMaxSuppression   = false;
    };

    std::unordered_map<Desktop::View::CWindow*, SWindowHooks> m_windows;

    CHyprSignalListener                                       m_windowOpen;
    CHyprSignalListener                                       m_windowDestroy;
    CHyprSignalListener                                       m_mouseButton;
    CHyprSignalListener                                       m_mouseMove;
    CHyprSignalListener                                       m_touchDown;
    CHyprSignalListener                                       m_touchMove;
    CHyprSignalListener                                       m_touchUp;

    PHLWINDOWREF                                              m_dragWindow;
    Vector2D                                                  m_dragOrigin;
    bool                                                      m_dragPending = false;
    bool                                                      m_dragging    = false;
    bool                                                      m_touchDrag   = false;
    int                                                       m_touchID     = -1;
    PHLMONITORREF                                             m_touchMonitor;
    Vector2D                                                  m_dragGrabOffset;
    bool                                                      m_pinnedForTouchDrag   = false;
    bool                                                      m_touchActive          = false;
    bool                                                      m_clientTouchCancelled = false;

    void                                                      unregisterWindow(Desktop::View::CWindow* window);
    void                                                      applySuppression(const PHLWINDOW& window);
    void                                                      handleClientState(const PHLWINDOWREF& window);

    PHLWINDOW                                                 csdWindowAt(const Vector2D& position) const;
    bool                                                      inDragRegion(const PHLWINDOW& window, const Vector2D& position) const;
    Vector2D                                                  touchPosition(const ITouch::SDownEvent& event) const;
    Vector2D                                                  touchPosition(const ITouch::SMotionEvent& event) const;
    bool                                                      armDrag(const Vector2D& position, bool touch, int touchID);
    void                                                      updateDrag(Event::SCallbackInfo& info, const Vector2D& position, bool touch, int touchID);
    void                                                      finishDrag(Event::SCallbackInfo& info, bool touch, int touchID);

    void                                                      onMouseButton(IPointer::SButtonEvent event, Event::SCallbackInfo& info);
    void                                                      onMouseMove(const Vector2D& position, Event::SCallbackInfo& info);
    void                                                      onTouchDown(ITouch::SDownEvent event, Event::SCallbackInfo& info);
    void                                                      onTouchMove(ITouch::SMotionEvent event, Event::SCallbackInfo& info);
    void                                                      onTouchUp(ITouch::SUpEvent event, Event::SCallbackInfo& info);
};
