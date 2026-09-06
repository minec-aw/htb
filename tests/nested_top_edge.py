#!/usr/bin/env python3
"""Isolated Wayland smoke/regression test; never injects input into the parent.
Requires Hyprland 0.56.2, its headers, a C++ compiler, alacritty, and a Wayland session.
Usage: python3 tests/nested_top_edge.py [path/to/hyprtouchbar.so]
"""
import json
import os
from pathlib import Path
import shlex
import subprocess as sp
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "hyprtouchbar.so"


def wait_for(fn, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = fn()
        if result:
            return result
        time.sleep(0.15)
    raise AssertionError("Timed out waiting for nested compositor/client")


with tempfile.TemporaryDirectory(prefix="htb-edge-test-") as temp:
    tmp = Path(temp)
    driver = tmp / "input-driver.so"
    cflags = shlex.split(sp.check_output(["pkg-config", "--cflags", "hyprland", "pixman-1", "libdrm", "libinput", "wayland-server"], text=True))
    sp.run([os.environ.get("CXX", "g++"), "-std=c++23", "-shared", "-fPIC", "-fno-gnu-unique", *cflags,
            str(ROOT / "tests/input-driver.cpp"), "-o", str(driver)], check=True)
    config = tmp / "hyprland.conf"
    config.write_text(f"""monitor = ,1000x700@60,auto,1
animations:enabled = false
input:follow_mouse = 2
general:gaps_in = 6
general:gaps_out = 10
decoration:rounding = 8
misc:disable_hyprland_logo = true
misc:disable_splash_rendering = true
xwayland:enabled = false
debug:disable_logs = false
debug:enable_stdout_logs = true
plugin = {PLUGIN}
plugin = {driver}
plugin:hyprtouchbar {{
    bar_blur = false
    show_app_icon = false
    top_edge_maximize = true
    top_edge_distance = 12
}}
exec-once = alacritty --class htb-regression
""")
    env = os.environ.copy()
    env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
    log = (tmp / "compositor.log").open("w+")
    proc = sp.Popen(["Hyprland", "--config", str(config)], env=env, stdout=log, stderr=sp.STDOUT)
    instance = None
    try:
        def find_instance():
            instances = json.loads(sp.check_output(["hyprctl", "instances", "-j"], text=True))
            return next((i["instance"] for i in instances if i["pid"] == proc.pid), None)
        instance = wait_for(find_instance)
        def ctl(*args):
            return sp.check_output(["hyprctl", "-i", instance, *args], text=True).strip()
        def dispatch(command, args=""):
            result = ctl("dispatch", command, args)
            assert result == "ok", result
            time.sleep(0.08)
        def input_(kind, x=0, y=0, id_=7):
            dispatch("htb-test.input", f"{kind} {x} {y} {id_}")
        def animation_enabled():
            value = json.loads(ctl("getoption", "animations:enabled", "-j"))
            return bool(value.get("bool", value.get("int", 0)))
        def client(name="htb-regression"):
            return next((w for w in json.loads(ctl("clients", "-j")) if w["class"] == name), None)
        wait_for(client)
        assert not ctl("configerrors"), ctl("configerrors")
        assert "Plugin hyprtouchbar by" in ctl("plugin", "list")
        def reset():
            w = client()
            dispatch("focuswindow", "address:" + w["address"])
            input_("restore")
            w = client()
            if not w["floating"]:
                dispatch("togglefloating")
            dispatch("resizewindowpixel", "exact 500 320,address:" + w["address"])
            dispatch("movewindowpixel", "exact 220 230,address:" + w["address"])
            return client()
        def assert_maximized():
            w = client()
            assert w["fullscreen"] == (0 if w["floating"] else 1) and w["fullscreenClient"] == 1, w
        def assert_normal():
            w = client()
            assert w["fullscreen"] == 0 and w["fullscreenClient"] == 0, w
        def mouse_drag(end_y):
            w = client()
            input_("hover", w["at"][0] + 180, w["at"][1] - 20)
            input_("down")
            input_("move", 410, 170)
            input_("move", 440, end_y)
            input_("up")
        def touch_drag(finish="touch-up", end_y=4):
            w = client()
            input_("touch-down", w["at"][0] + 180, w["at"][1] - 20)
            input_("touch-move", 410, 170)
            input_("touch-move", 440, end_y)
            input_(finish)

        original = reset()
        mouse_drag(4)
        assert_maximized()
        input_("restore")
        assert client()["floating"] and client()["size"] == original["size"], client()
        print("PASS pointer titlebar release -> maximize; original floating size restored")

        reset()
        w = client()
        input_("hover", w["at"][0] + 180, w["at"][1] - 20)
        input_("down")
        input_("up")
        assert_normal()
        print("PASS a titlebar click without motion does not maximize")

        reset()
        mouse_drag(100)
        assert_normal()
        print("PASS release away from top does not maximize")

        reset()
        w = client()
        input_("hover", w["at"][0] + 180, w["at"][1] - 20)
        input_("down")
        input_("move", 420, 4)
        input_("move", 440, 160)
        input_("up")
        assert_normal()
        print("PASS visiting top edge then leaving does not latch maximize")

        reset()
        assert ctl("keyword", "plugin:hyprtouchbar:top_edge_maximize", "false") == "ok"
        mouse_drag(4)
        assert_normal()
        assert ctl("keyword", "plugin:hyprtouchbar:top_edge_maximize", "true") == "ok"
        print("PASS disabling top_edge_maximize")

        reset()
        input_("hover", 400, 260)
        input_("down")
        input_("native-resize")
        input_("move", 440, 4)
        input_("up")
        assert_normal()
        print("PASS resize at top is not a window move")

        original = reset()
        touch_drag()
        assert_maximized()
        input_("restore")
        assert client()["floating"] and not client()["pinned"] and client()["size"] == original["size"], client()
        print("PASS raw touchscreen titlebar (nonzero ID) -> maximize and restore")

        reset()
        touch_drag("touch-cancel")
        assert_normal()
        assert not client()["pinned"], client()
        print("PASS cancelled touch at top does not maximize or leave a pin")

        reset()
        input_("hover", 400, 260)
        input_("down")
        input_("native-move")
        input_("move", 440, 4)
        input_("up")
        assert_maximized()
        print("PASS native compositor move (CSD/modifier drag path)")

        reset()
        dispatch("pin")
        mouse_drag(4)
        assert_normal()
        assert client()["pinned"], client()
        dispatch("pin")
        print("PASS originally pinned windows remain pinned and unmaximized")

        reset()
        dispatch("togglefloating")
        w = client()
        input_("hover", w["at"][0] + 180, w["at"][1] + 50)
        input_("down")
        input_("native-move")
        input_("move", 440, 4)
        input_("up")
        assert_maximized()
        input_("restore")
        assert not client()["floating"], client()
        print("PASS tiled native move -> maximize, then restore to tiling")

        original = reset()
        w = client()
        input_("pen-down", w["at"][0] + 180, w["at"][1] - 20)
        input_("pen-move", 410, 170)
        input_("pen-move", 440, 4)
        input_("pen-up", 440, 4)
        assert_maximized()
        input_("restore")
        assert client()["floating"] and client()["size"] == original["size"], client()
        print("PASS stylus on plugin titlebar -> maximize and restore")

        reset()
        w = client()
        input_("pen-down", w["at"][0] + 180, w["at"][1] - 20)
        input_("pen-move", 410, 170)
        input_("pen-move", 440, 4)
        input_("pen-out", 440, 4)
        assert_normal()
        print("PASS stylus proximity loss cancels without maximizing")

        # Treat the probe as a CSD client to exercise native request completion.
        # No application-name guessing or changes to the parent compositor.
        assert ctl("keyword", "plugin:hyprtouchbar:csd_detection", "all") == "ok"
        assert ctl("keyword", "plugin:hyprtouchbar:csd_touch_emulation", "false") == "ok"
        original = reset()
        w = client()
        input_("pen-down", w["at"][0] + 180, w["at"][1] + 20)
        input_("client-move-request")
        input_("pen-move", 410, 170)
        input_("pen-move", 440, 4)
        input_("pen-up", 440, 4)
        assert_maximized()
        input_("restore")
        assert client()["floating"] and not client()["pinned"] and client()["size"] == original["size"], client()
        print("PASS native CSD tablet request -> maximize and restore")

        reset()
        w = client()
        input_("touch-down", w["at"][0] + 180, w["at"][1] + 20)
        input_("client-move-request")
        input_("touch-move", 410, 170)
        input_("touch-move", 440, 4)
        input_("touch-up")
        assert_maximized()
        assert not client()["pinned"], client()
        print("PASS native CSD touch request -> maximize")

        reset()
        w = client()
        input_("pen-down", w["at"][0] + 180, w["at"][1] + 20)
        input_("pen-move", 440, 4)
        input_("pen-up", 440, 4)
        assert_normal()
        print("PASS CSD widget gesture without a move request stays native")

        input_("restore")
        assert ctl("plugin", "unload", str(PLUGIN)) == "ok"
        assert "Plugin hyprtouchbar by" not in ctl("plugin", "list")
        assert ctl("plugin", "load", str(PLUGIN)) == "ok"
        wait_for(lambda: "Plugin hyprtouchbar by" in ctl("plugin", "list"))
        reset()
        mouse_drag(4)
        assert_maximized()
        print("PASS unload/reload with existing client, then snap again")

        original = reset()
        input_("expect-rounded")
        input_("client-maximize")
        assert_maximized()
        input_("expect-client-maximized")
        input_("expect-square")
        input_("expect-work-area")
        assert not any(ws["hasfullscreen"] for ws in json.loads(ctl("workspaces", "-j"))), ctl("workspaces", "-j")
        first = client()
        input_("client-maximize")
        assert client()["at"] == first["at"] and client()["size"] == first["size"]
        input_("client-unmaximize")
        assert_normal()
        # Normal compositor state, but preserve Hyprland's wire decoration hint.
        input_("expect-client-maximized")
        input_("expect-rounded")
        assert client()["at"] == original["at"] and client()["size"] == original["size"], client()
        print("PASS desktop maximize is non-exclusive, square, explicit set is idempotent, and restore preserves the compatibility hint")
        # With that hint, a real caption button sends unset even when the
        # compositor considers the window normal. Preserve the native toggle.
        input_("client-unmaximize")
        assert_maximized()
        input_("client-unmaximize")
        assert_normal()
        assert client()["size"] == original["size"]
        print("PASS hinted CSD unset requests toggle actual maximize/restore")

        input_("client-maximize")
        input_("fullscreen")
        assert client()["fullscreen"] == 2 and client()["fullscreenClient"] == 2, client()
        input_("restore")
        assert_maximized()
        input_("expect-square")
        input_("client-unmaximize")
        assert_normal()
        input_("expect-rounded")
        assert client()["size"] == original["size"], client()
        print("PASS true fullscreen round-trip preserves preceding desktop-maximized state")

        original = reset()
        input_("client-maximize")
        input_("expect-work-area")
        mon_name = json.loads(ctl("monitors", "-j"))[0]["name"]
        assert ctl("keyword", "monitor", mon_name + ",addreserved,34,0,0,0") == "ok"
        time.sleep(0.5)
        input_("expect-work-area")
        input_("expect-square")
        assert ctl("keyword", "decoration:rounding", "12") == "ok"
        assert ctl("keyword", "general:border_size", "2") == "ok"
        input_("client-unmaximize")
        input_("expect-frame", 12, 2)
        assert client()["size"] == original["size"]
        assert ctl("keyword", "decoration:rounding", "8") == "ok"
        assert ctl("keyword", "general:border_size", "1") == "ok"
        assert ctl("keyword", "monitor", mon_name + ",addreserved,0,0,0,0") == "ok"
        print("PASS panel reservation changes and updated frame configuration survive maximize/restore")

        original = reset()
        input_("client-maximize")
        w = client()
        input_("hover", w["at"][0] + 200, w["at"][1] - 20)
        input_("down")
        input_("move", 400, 250)
        input_("move", 450, 300)
        input_("up")
        assert_normal()
        assert client()["size"] == original["size"], client()
        input_("expect-rounded")
        print("PASS drag down restores floating size and normal decorations")

        original = reset()
        input_("client-maximize")
        input_("hover", 300, 300)
        input_("down")
        input_("native-move")
        input_("move", 320, 330)
        input_("move", 440, 370)
        input_("up")
        assert_normal()
        assert client()["size"] == original["size"], client()
        input_("expect-rounded")
        print("PASS native modifier-drag rebases onto restored floating geometry")

        original = reset()
        input_("client-maximize")
        dispatch("togglefloating")
        assert not client()["floating"], client()
        assert_maximized()
        input_("client-unmaximize")
        assert_normal()
        assert not client()["floating"], client()
        input_("expect-rounded")
        input_("client-maximize")
        assert_maximized()
        input_("expect-square")
        input_("client-unmaximize")
        assert not client()["floating"], client()
        print("PASS switching maximized float to tiled uses native maximize and returns to tiling")

        original = reset()
        input_("client-maximize")
        input_("expect-square")
        dispatch("exec", "alacritty --class htb-overlay")
        wait_for(lambda: client("htb-overlay"))
        overlay = client("htb-overlay")
        dispatch("focuswindow", "address:" + overlay["address"])
        if not client("htb-overlay")["floating"]:
            dispatch("togglefloating")
        dispatch("resizewindowpixel", "exact 300 240,address:" + overlay["address"])
        dispatch("movewindowpixel", "exact 600 250,address:" + overlay["address"])
        # A normal mouse click, not Alt-Tab or a forced hover/refocus, must hit
        # the visible floating overlay even though the browser is maximized.
        input_("move", 720, 330)
        input_("down")
        input_("up")
        assert json.loads(ctl("activewindow", "-j"))["class"] == "htb-overlay"
        input_("expect-pointer-focus")
        # Click exposed browser content: it should rise and cover the overlay,
        # not leave a visible but unclickable ghost on top.
        input_("move", 150, 150)
        input_("down")
        input_("up")
        assert json.loads(ctl("activewindow", "-j"))["class"] == "htb-regression"
        input_("expect-pointer-focus")
        input_("move", 720, 330)
        input_("down")
        input_("up")
        assert json.loads(ctl("activewindow", "-j"))["class"] == "htb-regression"
        input_("expect-pointer-focus")
        dispatch("focuswindow", "address:" + overlay["address"])
        input_("move", 720, 330)
        input_("down")
        input_("up")
        assert json.loads(ctl("activewindow", "-j"))["class"] == "htb-overlay"
        input_("expect-pointer-focus")
        print("PASS normal click/raise and pointer focus agree with overlapping floating windows")

        input_("client-maximize")
        assert client("htb-overlay")["fullscreen"] == 0 and client("htb-overlay")["fullscreenClient"] == 1
        assert_maximized()
        assert not any(ws["hasfullscreen"] for ws in json.loads(ctl("workspaces", "-j")))
        input_("client-unmaximize")
        assert client("htb-overlay")["size"] == [300, 240]
        assert_maximized()
        print("PASS multiple independent floating maximizations do not claim the workspace")

        dispatch("focuswindow", "address:" + original["address"])
        input_("client-unmaximize")
        input_("expect-rounded")
        assert client()["size"] == original["size"]
        input_("client-maximize")
        assert ctl("plugin", "unload", str(PLUGIN)) == "ok"
        assert_normal()
        input_("expect-rounded")
        assert client()["size"] == original["size"]
        print("PASS unload restores desktop-maximized geometry, decorations and client state")
        assert ctl("plugin", "load", str(PLUGIN)) == "ok"
        wait_for(lambda: "Plugin hyprtouchbar by" in ctl("plugin", "list"))
        # Verify actual tile geometry with two participants, not just the flag.
        for name in ("htb-regression", "htb-overlay"):
            w = client(name)
            dispatch("focuswindow", "address:" + w["address"])
            if w["floating"]:
                dispatch("togglefloating")
        dispatch("focuswindow", "address:" + client()["address"])
        tile_boxes = {name: (client(name)["at"], client(name)["size"]) for name in ("htb-regression", "htb-overlay")}
        input_("client-maximize")
        assert_maximized()
        input_("expect-square")
        input_("client-unmaximize")
        for name, geometry in tile_boxes.items():
            w = client(name)
            assert not w["floating"] and (w["at"], w["size"]) == geometry, (name, w, geometry)
        print("PASS two-window tiled layout restores the same slots and sizes")

        reset()
        assert ctl("keyword", "plugin:hyprtouchbar:maximize_mode", "native") == "ok"
        input_("client-maximize")
        assert client()["floating"] and client()["fullscreen"] == 1 and client()["fullscreenClient"] == 1, client()
        input_("expect-square")
        input_("client-unmaximize")
        assert_normal()
        assert client()["floating"]
        assert ctl("keyword", "plugin:hyprtouchbar:maximize_mode", "auto") == "ok"
        print("PASS optional native mode retains layout fullscreen behavior for floating windows")

        # Exaggerated move animation makes any lag obvious in current-vs-goal
        # assertions. Setup remains nonanimated; only the gestures enable it.
        assert ctl("keyword", "plugin:hyprtouchbar:csd_detection", "auto") == "ok"
        assert ctl("keyword", "animation", "windowsMove,1,50,default") == "ok"
        original = reset()
        ox, oy = original["at"]
        assert ctl("keyword", "animations:enabled", "true") == "ok"
        input_("touch-down", ox + 180, oy - 20)
        input_("touch-move", ox + 240.25, oy + 40.5)
        input_("expect-instant-position")
        input_("expect-position", ox + 60, oy + 61)  # native moveTarget rounding
        input_("touch-up")
        assert animation_enabled()
        dispatch("movewindowpixel", "exact 150 180,address:" + original["address"])
        input_("expect-animated-position")
        print("PASS finger titlebar motion is immediate and precise; ordinary move animation still works")

        assert ctl("keyword", "animations:enabled", "false") == "ok"
        assert ctl("keyword", "plugin:hyprtouchbar:csd_detection", "all") == "ok"
        assert ctl("keyword", "plugin:hyprtouchbar:csd_touch_emulation", "false") == "ok"
        original = reset()
        ox, oy = original["at"]
        assert ctl("keyword", "animations:enabled", "true") == "ok"
        input_("touch-down", ox + 100, oy + 20)
        input_("client-move-request")
        input_("touch-move", ox + 160.5, oy + 60.25)
        input_("expect-instant-position")
        input_("expect-position", ox + 61, oy + 40)
        input_("touch-move", ox + 101.25, oy + 20.5)
        input_("expect-position", ox + 1, oy + 1)
        input_("touch-up")
        print("PASS native CSD touch tracks immediately, including movement back inside the initial drag threshold")

        assert ctl("keyword", "animations:enabled", "false") == "ok"
        original = reset()
        ox, oy = original["at"]
        assert ctl("keyword", "animations:enabled", "true") == "ok"
        input_("pen-down", ox + 100, oy + 20)
        input_("client-move-request")
        input_("pen-move", ox + 170.125, oy + 100.5)
        input_("expect-instant-position")
        input_("expect-position", ox + 70, oy + 81)
        input_("pen-up", ox + 170.125, oy + 100.5)
        assert animation_enabled()
        print("PASS native CSD stylus tracks immediately without changing animation settings")
    except Exception:
        log.flush()
        log.seek(0)
        print(log.read()[-10000:], file=sys.stderr)
        raise
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except sp.TimeoutExpired:
                proc.kill()
                proc.wait()
        log.close()
