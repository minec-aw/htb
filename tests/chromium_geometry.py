#!/usr/bin/env python3
"""Reproduce/repair the 1.6.0 CSD-offset regression with a real Chromium client.
Run from a Wayland session: python3 tests/chromium_geometry.py [plugin.so]
HTB_BROWSER may specify a Chromium-family executable (default helium-browser).
All input/IPC targets a separate nested compositor and a temporary browser profile.
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess as sp
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "hyprtouchbar.so"
BROWSER = shutil.which(os.environ.get("HTB_BROWSER", "helium-browser"))
if not BROWSER:
    sys.exit("Install helium-browser or set HTB_BROWSER to a Chromium executable")


def wait_for(fn, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = fn()
        if value:
            return value
        time.sleep(0.2)
    raise AssertionError("Timed out waiting for Chromium/compositor state")


with tempfile.TemporaryDirectory(prefix="htb-chromium-geometry-") as temp:
    tmp = Path(temp)
    driver = tmp / "input-driver.so"
    cflags = shlex.split(sp.check_output(["pkg-config", "--cflags", "hyprland", "pixman-1", "libdrm", "libinput", "wayland-server"], text=True))
    sp.run([os.environ.get("CXX", "g++"), "-std=c++23", "-shared", "-fPIC", "-fno-gnu-unique", *cflags,
            str(ROOT / "tests/input-driver.cpp"), "-o", str(driver)], check=True)
    browser_command = shlex.join([BROWSER, "--user-data-dir=" + str(tmp / "profile"), "--no-first-run", "--no-default-browser-check",
                                  "--disable-background-networking", "--ozone-platform=wayland", "about:blank"])
    config = tmp / "hyprland.conf"
    config.write_text(f"""monitor = ,1800x1200@60,auto,1.875
animations:enabled = false
decoration:rounding = 8
misc:disable_hyprland_logo = true
misc:disable_splash_rendering = true
xwayland:enabled = false
debug:disable_logs = false
debug:enable_stdout_logs = true
plugin = {driver}
exec-once = {browser_command}
""")
    env = os.environ.copy()
    env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
    log = (tmp / "compositor.log").open("w+")
    proc = sp.Popen(["Hyprland", "--config", str(config)], env=env, stdout=log, stderr=sp.STDOUT)
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
            time.sleep(0.15)
        def input_(kind):
            dispatch("htb-test.input", kind)
        def client():
            clients = json.loads(ctl("clients", "-j"))
            return clients[0] if clients else None
        def geometry_ready(kind):
            return ctl("dispatch", "htb-test.input", kind) == "ok"
        w = wait_for(client)
        dispatch("focuswindow", "address:" + w["address"])
        if not w["floating"]:
            dispatch("togglefloating")
        dispatch("resizewindowpixel", "exact 700 500,address:" + w["address"])
        dispatch("movewindowpixel", "exact 100 100,address:" + w["address"])
        wait_for(lambda: geometry_ready("expect-no-csd-offset"))
        input_("expect-client-maximized")
        print("PASS stock compositor: normal Chromium has the compatibility hint and no surface offset")

        input_("clear-csd-hint")
        wait_for(lambda: geometry_ready("expect-csd-offset"))
        print("PASS reproduced: clearing only the hint makes real Chromium add geometry margins")

        assert ctl("plugin", "load", str(PLUGIN)) == "ok"
        wait_for(lambda: geometry_ready("expect-no-csd-offset"))
        input_("expect-client-maximized")
        assert client()["fullscreen"] == 0 and client()["fullscreenClient"] == 0, client()
        original = client()
        print("PASS loading corrected plugin repairs an already-affected Chromium window")

        for _ in range(2):
            input_("client-unmaximize")  # what the hinted caption button sends
            wait_for(lambda: client()["fullscreenClient"] == 1)
            assert client()["fullscreen"] == 0, client()
            input_("expect-square")
            wait_for(lambda: geometry_ready("expect-no-csd-offset"))
            input_("client-unmaximize")
            wait_for(lambda: client()["fullscreenClient"] == 0)
            input_("expect-rounded")
            wait_for(lambda: geometry_ready("expect-no-csd-offset"))
            assert client()["size"] == original["size"], client()
        print("PASS repeated caption maximize/restore retains aligned Chromium content and floating size")

        assert ctl("plugin", "unload", str(PLUGIN)) == "ok"
        wait_for(lambda: geometry_ready("expect-no-csd-offset"))
        input_("expect-client-maximized")
        print("PASS unload leaves stock decoration compatibility intact")
        dispatch("killactive")
        wait_for(lambda: not client())
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
