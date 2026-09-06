# hyprtouchbar

A modern, touch-friendly fork of Hyprland's official **hyprbars** plugin.

It adds server-side titlebars only when an application does not already have client-side decorations (CSD), while also filling the common CSD gaps in Hyprland:

- protocol-based CSD detection (no application-name blacklist)
- protocol-driven touchscreen dragging using each application's exact CSD draggable regions
- working CSD minimize requests with a configurable Hyprland dispatcher action
- desktop-style floating maximization and layout-native tiled maximization
- drag-to-top maximization on release (mouse, touch, and stylus)
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
    button_icon_theme = chromium
    button_icon_size = 12
    button_icon_color = rgb(e8eaed)
    button_inactive_opacity = 0.38
    button_hover_color = rgba(ffffff33)
    close_hover_color = rgb(e5484d)

    # Touchscreen and stylus handling
    stylus_drag_enabled = true
    top_edge_maximize = true
    top_edge_distance = 12
    csd_detection = auto
    csd_drag_enabled = true
    csd_touch_emulation = true

    # Optional legacy forced-drag approximation; normally leave disabled
    csd_drag_fallback = false
    csd_titlebar_height = 48
    csd_controls_left = 0
    csd_controls_right = 150
    csd_drag_threshold = 8

    maximize_mode = auto
    on_double_click = maximize

    # Dispatcher name + arguments in legacy notation (also works with Lua configs)
    minimize_action = movetoworkspacesilent special:minimized
    maximize_action =
    close_action =
}
```

The default controls use **Chromium's original built-in caption vector paths**,
not Adwaita symbols or font characters. Both the 12px and 24px representations
are embedded, with Chromium's representation-selection policy for fractional
and HiDPI scales. The default canvas size is 12 **logical** pixels; these vectors
have much less internal padding than Adwaita, so this is not equivalent to
shrinking the old icons. Touch targets and button spacing are unchanged.

Inactive built-in glyphs dim to `button_inactive_opacity` (0.38, as in Chromium),
and brighten on focus/hover. The maximize glyph switches to the overlapping
restore icon when maximized. Set `button_icon_theme` to a freedesktop theme such
as `Adwaita` to keep themed symbolic icons instead; custom text buttons remain
supported.

If you previously set `button_icon_size = 16` following the old guidance, remove
that override or set it to **12** to use the browser's default metrics. See
[the original vector sources and license](third_party/chromium_caption/README.md).
Helium can use different controls with GTK themes or its newer rounded-icons
feature; this default targets its classic Chromium/Linux caption design.

`maximize_action` being empty uses `maximize_mode`. Setting an action replaces maximize button behavior with that dispatcher instead. `close_action` behaves similarly for the plugin's server-side close button. The default `on_double_click = maximize` uses the same maximize handler; any other nonempty value remains a shell command.

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

## Maximization and tiling

`maximize_mode = auto` is the default:

- **Floating windows:** fill the monitor's usable area (excluding panels), while
  staying in normal floating z-order. Clicking or explicitly activating another
  floating window raises it normally; clicking the maximized window raises it
  over other unpinned floating windows. There is no workspace fullscreen input
  priority and multiple floating windows can be maximized independently.
- **Tiled windows and window groups:** use Hyprland's native layout maximization.
  Restoring returns to the tiled layout; the plugin does not force them floating
  or replace the tiling algorithm.
- Both paths remove compositor borders, rounding and outer shadows while
  maximized, **without removing the titlebar or buttons**. Restoring removes only
  the temporary overrides so your normal rules and current configuration apply.
- Floating geometry is saved/restored. Dragging a desktop-maximized window
  restores its floating size under the pointer/finger/pen. Switching it to tiled
  mode transfers maximization to the native layout; restoring then returns to
  tiling. The reverse transition uses desktop maximization.
- CSD requests, built-in buttons, double-click and top-edge snapping share this
  handling. Explicit `set_maximized` requests are idempotent. On Hyprland 0.56,
  Wayland caption requests retain the compatibility behavior described below;
  XWayland's EWMH maximize state tracks the real state.
- Actual fullscreen (F11) remains native. A desktop-maximized window temporarily
  entering fullscreen returns to its maximized work-area size when it exits.
- Panel/monitor changes update maximized floating bounds. Unloading the plugin
  restores its desktop-maximized windows rather than leaving client-only state
  behind.

In your existing Lua plugin configuration:

```lua
maximize_mode = "auto",
on_double_click = "maximize",
```

Use `maximize_mode = "native"` to retain Hyprland's workspace-fullscreen-style
maximization even for floating windows. This compatibility mode retains core
stacking/input behavior; it is not the floating click-through fix. Tiling works
in **both** modes.

In `hyprctl clients`, a desktop-maximized floating window intentionally shows
`fullscreen: 0, fullscreenClient: 1`. A natively maximized tiled window shows
`fullscreen: 1, fullscreenClient: 1`; true fullscreen is `2`.

**Hyprland 0.56 decoration compatibility:** core Hyprland deliberately keeps the
XDG `maximized` wire hint set even for normal windows, suppressing client shadow
margins that its renderer cannot correctly offset. Hyprtouchbar preserves this
hint; it is separate from the actual compositor maximize state above. A CSD
button may therefore send `unset_maximized` on a normal window; like core
Hyprland, we interpret that as a caption toggle. Apps may show a restore glyph
even when floating, as under stock Hyprland. Version 1.6.0 incorrectly cleared
this hint, causing shifted/clipped Chromium content. Version 1.6.1 restores the
compatibility hint, including for already-affected windows, without renderer
hooks or border/rounding workarounds.

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

## Drag to the top to maximize

Drag a window to a monitor's top edge and **release** to maximize it. Enabled by default for plugin titlebars and compositor window moves initiated by CSDs (including native tablet/touch requests and emulated pointer input). Modifier + mouse window drags also work.

In your existing Lua plugin configuration:

```lua
hyprtouchbar = {
    top_edge_maximize = true,
    top_edge_distance = 12,
}
```

`top_edge_distance` is the release zone's height in **logical pixels**, clamped to 0–128. `0` requires the exact edge. It uses each monitor's actual top edge, including on scaled/offset outputs; maximized geometry respects Hyprland's workspace work area, panels and gaps. Disable the feature with `top_edge_maximize = false`.

Only completed window-move gestures qualify. Clicking a titlebar button, resizing, dragging tabs/widgets without a compositor window move, cancellation, or moving away from the top before releasing will not trigger it. Originally pinned windows are left alone. Top-edge maximize is independent of `maximize_action` and uses `maximize_mode`, not true fullscreen. The normal maximize/restore button restores the saved floating geometry (or the tiled layout for tiled windows).

## Touch and stylus dragging notes

Direct finger/stylus window moves follow input immediately, using the same
position/size warp as Hyprland's native mouse dragging. This does not disable
normal move, maximize, or workspace animations. The drag threshold applies only
when starting a drag, not when moving back toward its starting point.

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

This opens Hyprland as a window inside the existing Wayland session (Plasma or Hyprland). Press `Super+M` inside it to exit.

Regression tests:

```bash
make test
python3 tests/nested_top_edge.py
python3 tests/chromium_geometry.py  # real Helium; or set HTB_BROWSER=chromium
```

The Python tests start their own nested compositor and inject input **only there**. See [tests/README.md](tests/README.md) for coverage and limitations.
