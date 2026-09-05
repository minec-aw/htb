# hyprtouchbar

A modern, touch-friendly fork of Hyprland's official **hyprbars** plugin.

It adds server-side titlebars only when an application does not already have client-side decorations (CSD), while also filling the common CSD gaps in Hyprland:

- protocol-based CSD detection (no application-name blacklist)
- protocol-driven touchscreen dragging using each application's exact CSD draggable regions
- working CSD minimize requests with a configurable Hyprland dispatcher action
- native CSD maximize behavior, or an optional configurable override
- application icons resolved from desktop files and freedesktop icon themes
- large built-in close, maximize, and minimize targets
- translucent active/inactive colors, blur, rounded hover states, and modern defaults
- per-window rules to fix applications that report their decoration mode incorrectly

The titlebar renderer is derived from `hyprwm/hyprland-plugins` (`hyprbars`, compatibility commit `7644cecdb947060682891a0db2a0cdc5c0b9e704`). The protocol-driven touch-move approach was inspired by [`khalid151/csd-titlebar-move`](https://github.com/khalid151/csd-titlebar-move), with additional serial validation, touch tracking, and clean handler restoration. This fork retains hyprbars' BSD-3-Clause license.

## Compatibility

Hyprland plugins are ABI-specific. Always rebuild after a Hyprland update. This revision was built and tested in a nested **Hyprland 0.56.2** session.

## Build and install

The build requires the Hyprland development files plus Cairo and `librsvg`.

```bash
make
hyprctl plugin load "$PWD/hyprtouchbar.so"
```

For persistent loading, use an absolute path in `hyprland.conf`:

```ini
plugin = /absolute/path/to/hyprtouchbar.so
```

Or, after publishing/cloning this directory as a Git repository:

```bash
hyprpm add https://github.com/minec-aw/htb
hyprpm enable hyprtouchbar
hyprpm reload
```

## Suggested configuration

The defaults are usable without configuration:

```ini
plugin:hyprtouchbar {
    enabled = true

    # Modern server-side bar
    bar_height = 42
    bar_color = rgba(1c1c22e6)
    inactive_bar_color = rgba(16161bd9)
    col.text = rgb(f4f4f5)
    bar_text_size = 13
    bar_text_weight = 500
    bar_text_align = left
    bar_blur = true
    bar_padding = 8
    bar_button_padding = 6
    corner_radius = 6
    show_app_icon = true
    app_icon_size = 24
    app_icon_theme = hicolor

    # Built-in touch controls
    builtin_buttons = true
    show_minimize = true
    show_maximize = true
    show_close = true
    # Helium Linux-style caption controls
    bar_buttons_alignment = right
    icon_on_hover = false
    button_icon_theme = Adwaita
    button_icon_size = 20
    button_icon_color = rgb(e8eaed)
    button_hover_color = rgba(ffffff33)
    close_hover_color = rgb(e5484d)

    # Touchscreen and stylus handling
    stylus_drag_enabled = true
    csd_detection = auto
    csd_drag_enabled = true
    csd_touch_emulation = true

    # Optional legacy forced-drag approximation; normally leave disabled
    csd_drag_fallback = false
    csd_titlebar_height = 48
    csd_controls_left = 0
    csd_controls_right = 150
    csd_drag_threshold = 8

    # Arguments to `hyprctl dispatch` (not shell commands)
    minimize_action = movetoworkspacesilent special:minimized
    maximize_action =
    close_action =
}
```

The built-in controls load the same freedesktop symbolic icon names Chromium/Helium requests on Linux: `window-minimize-symbolic`, `window-maximize-symbolic`, `window-restore-symbolic`, and `window-close-symbolic`. The maximize glyph automatically changes to restore while maximized. Text glyphs are used only if the selected icon theme does not provide these assets.

`maximize_action` being empty preserves Hyprland's native maximize/unmaximize response to a CSD button. Setting it suppresses native maximize and dispatches your action instead. `close_action` behaves similarly for the plugin's server-side close button.

Hyprland has no native minimize model. The default sends a window to `special:minimized`; show it again with:

```ini
bind = SUPER, M, togglespecialworkspace, minimized
```

Other useful choices include:

```ini
# Hide instead of using a special workspace
minimize_action = movetoworkspacesilent 9

# Make maximize toggle floating instead
maximize_action = togglefloating
```

## How CSD detection works

In `auto` mode:

- an XDG window with no `xdg-decoration` object is treated as client-decorated
- an explicit `CLIENT_SIDE` request is treated as client-decorated
- an XWayland window with the borderless/Motif hint is treated as client-decorated
- other windows receive the plugin titlebar

This is more reliable than matching `class`, but games and unusual toolkits can still be ambiguous. Use window rules to override detection:

```ini
windowrule {
    name = force-my-app-csd
    match:class = ^(my-app)$
    hyprtouchbar:force_csd = true
}

windowrule {
    name = force-my-app-titlebar
    match:class = ^(legacy-app)$
    hyprtouchbar:force_ssd = true
}

windowrule {
    name = no-bar-on-games
    match:class = ^(steam_app_.*)$
    hyprtouchbar:no_bar = true
}
```

Other dynamic rule effects are `hyprtouchbar:bar_color` and `hyprtouchbar:title_color`.

## Touch and stylus dragging notes

With `stylus_drag_enabled = true`, a pen tip over a plugin titlebar is routed through its pointer-style caption handling. Over a detected CSD window, tablet input remains native until the application identifies the exact pixel as draggable with `xdg_toplevel.move`; the plugin then accepts the tablet-tool serial and tracks the pen directly. Motion follows the tablet's output and active-area mapping. Stylus input outside titlebars remains native, including pressure and tilt.

With `csd_touch_emulation = true` (the default), touches in the top `csd_titlebar_height` pixels are forwarded to the client as pointer input rather than being turned directly into a window drag. Chromium and Qt therefore perform their own exact hit testing: tabs reorder/detach, navigation buttons click, and only genuinely draggable regions issue `xdg_toplevel.move`. The region is an input-compatibility scope, not a forced drag overlay.

Setting `csd_touch_emulation = false` retains raw touch delivery. Hyprtouchbar can then accept an application-issued `xdg_toplevel.move` with a valid touch-down serial. Hyprland 0.56 normally validates only pointer-button serials. Native pointer validation is retained, and Hyprland's original request handlers are restored when the plugin unloads.

`csd_drag_fallback = true` enables the older forced-drag approximation for clients that issue no move request at all. In that mode `csd_controls_left` and `csd_controls_right` exclude controls from the fallback. It sends `wl_touch.cancel` when taking over, but can conflict with tab/tool-bar gestures, so it remains disabled by default.

## Lua custom buttons

The inherited custom-button API remains available for Lua configs. Custom actions are shell commands:

```lua
hl.plugin.hyprtouchbar.add_button({
    bg_color = "rgba(ffffff22)",
    fg_color = "rgb(ffffff)",
    size = 30,
    icon = "★",
    action = "notify-send 'hyprtouchbar'",
})
```

## Nested testing from Plasma

`test-nested.conf` is included for development. Build, then run:

```bash
Hyprland --config "$PWD/test-nested.conf"
```

This opens Hyprland as a window inside the existing Wayland Plasma session. Press `Super+M` inside it to exit.
