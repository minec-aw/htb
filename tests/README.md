# Top-edge maximize regression tests

`make test` builds a standalone C++ policy test without Hyprland. It covers the
release zone, clicks/jitter, threshold boundaries, negative monitor offsets,
adjacent outputs, logical versus physical coordinates, and invalid input.

`python3 tests/nested_top_edge.py [plugin.so]` builds `input-driver.cpp` into a
temporary **test-only** plugin and starts a separate nested Hyprland. Requires
matching Hyprland headers, a C++ compiler, pkg-config, alacritty, and an existing
Wayland session. All IPC is explicitly targeted at the child compositor by its
instance signature; it does not inject into the parent or modify user config.
Do not load the test driver into your normal session.

Automated scenarios:

- Pointer dragging the plugin titlebar to the top, then native restore; floating
  size is preserved.
- Release away from the top, including after first visiting the edge.
- Disabling the feature; clicks without movement and resizing at the top must
  not maximize.
- Nonzero touch ID dragging the plugin titlebar, with temporary pin cleanup.
- Touch cancel at the top must not maximize.
- Native compositor move target (the CSD/modifier-drag completion path),
  originally pinned windows, and tiled-window restoration.
- Stylus on a plugin titlebar, and pen proximity loss without a release.
- Native CSD tablet/touch move-request completion and restore.
- A CSD gesture without a move request must not maximize.
- Unload/reload with an existing client, followed by another top-edge drag.

The driver synthesizes events into Hyprland's input paths, not `/dev/uinput`.
For native CSD request tests it marks the probe client as CSD and calls the
installed move handler with a test-issued seat serial. This tests the gesture
completion path, **not** a real toolkit's protocol negotiation or serial
security. Physical device mapping and actual Chromium tab interaction still
need manual testing.

Manual checks on the real desktop:

1. Drag a floating window by its titlebar with mouse, finger and pen; release
   near the top. Restore it and verify the floating size.
2. Repeat using actual Chromium/KDE CSD drag regions, then try tabs and controls.
3. Use outputs above/left of the origin and mixed fractional scales. Only the
   release output should maximize; panels should stay visible.
4. Test tiled and already-pinned windows. No implicit unpinning should occur.
5. Test changing configuration and unloading the plugin during a pending drag.
