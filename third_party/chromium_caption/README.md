# Chromium caption vectors

These unmodified `.icon` sources are from Chromium tag **152.0.7977.75**,
matching the Chromium base of the Helium build used for comparison:

https://github.com/chromium/chromium/tree/152.0.7977.75/ui/views/window/vector_icons

The BSD license is retained in `LICENSE`. These are Chromium's built-in legacy
caption vectors, not GTK/Adwaita symbolic icons. `OpaqueBrowserFrameView` selects
these when rounded icons are disabled (the default in that Chromium tag):

https://github.com/chromium/chromium/blob/152.0.7977.75/chrome/browser/ui/views/frame/opaque_browser_frame_view.cc

Chromium defaults to the last representation's canvas size (12 logical pixels).
Its rasterizer selects an exact pixel-size representation, otherwise the largest
exact divisor, otherwise the next larger representation, otherwise the largest.
The 24px paths have different stroke/padding geometry, so always scaling the
12px paths is not equivalent at fractional/HiDPI scales.

`tools/generate_caption_icons.py` converts these paths directly into embedded
SVGs in `ChromiumCaptionIcons.hpp`. No SVG files are needed beside the installed
plugin. The paths retain their original coordinates and even-odd fill rule.
Run `python3 tools/generate_caption_icons.py --check` to verify the generated file.

Inactive opacity 0.38 follows Chromium's `FrameCaptionButton` implementation.
Helium can use different controls when configured to use a GTK theme or newer
rounded icons; this bundle targets its built-in classic Linux caption design.
