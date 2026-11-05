#!/usr/bin/env python3
"""Orca against the running Linux client (tools/test_atspi.sh --orca): does the screen reader speak the login fields?

Orca runs headless on the same private buses and sway as the AT-SPI client test, with speech-dispatcher's dummy output
module, and writes its debug log (every utterance it sends is logged as "SPEECH OUTPUT"). The script turns accessibility
on as a desktop's switch does, moves the focus through the login fields with Component.GrabFocus and reads the log:
focusing the identifier field must speak its name and role, focusing the password field must speak its name and that it is
a password field, and the tree walk Orca makes of the window must not fault.

Needs orca, speech-dispatcher (the dummy module), python3-speechd and python3-gi with gir1.2-atspi-2.0.
"""

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import warnings

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, GLib, Gio  # noqa: E402

warnings.simplefilter("ignore", DeprecationWarning)

failures = 0


def check(condition, label):
    global failures
    print(("ok   " if condition else "FAIL ") + label, flush=True)
    if not condition:
        failures += 1
    return bool(condition)


def pump(seconds):
    end = time.monotonic() + seconds
    context = GLib.MainContext.default()
    while time.monotonic() < end:
        while context.iteration(False):
            pass
        time.sleep(0.01)


def wait_for(predicate, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        pump(0.1)
        value = predicate()
        if value:
            return value
    return predicate()


def set_status(**flags):
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    for name, value in flags.items():
        bus.call_sync("org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Set",
                      GLib.Variant("(ssv)", ("org.a11y.Status", name, GLib.Variant("b", value))), None,
                      Gio.DBusCallFlags.NONE, 5000, None)


def find_application(name="MaiCord"):
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        try:
            application = desktop.get_child_at_index(i)
            if application is not None and application.get_name() == name:
                return application
        except GLib.Error:
            continue
    return None


def find(node, name, role_name):
    try:
        if node.get_name() == name and node.get_role_name() == role_name:
            return node
        for i in range(node.get_child_count()):
            child = node.get_child_at_index(i)
            found = find(child, name, role_name) if child is not None else None
            if found is not None:
                return found
    except GLib.Error:
        pass
    return None


def speech(log_path):
    """The utterances in Orca's debug log, in order."""
    try:
        with open(log_path, errors="replace") as handle:
            return re.findall(r"SPEECH OUTPUT: '(.*?)' \{", handle.read())
    except OSError:
        return []


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--client-pid", type=int, required=True)
    options = parser.parse_args()

    home = os.environ["HOME"]
    # speech-dispatcher with only its dummy module: nothing is played, Orca still believes it speaks.
    speechd = os.path.join(home, ".config", "speech-dispatcher")
    os.makedirs(speechd, exist_ok=True)
    with open(os.path.join(speechd, "speechd.conf"), "w") as handle:
        handle.write('DefaultModule dummy\nAddModule "dummy" "sd_dummy" "" ""\nLogLevel 3\n'
                     'LogDir "%s"\nDefaultLanguage "en"\n' % options.out)
    log = os.path.join(options.out, "orca.log")
    prefs = os.path.join(options.out, "orca-prefs")
    os.makedirs(prefs, exist_ok=True)

    Atspi.init()
    # Orca needs an X display for its modifier handling (xkbcomp) even where the application it serves is a Wayland
    # client: a private Xvfb, for Orca only.
    reader, writer = os.pipe()
    # Only the abstract socket: /tmp/.X11-unix is not writable everywhere (WSLg mounts it read-only), and no TCP.
    xvfb = subprocess.Popen(["Xvfb", "-screen", "0", "1280x800x24", "-nolisten", "tcp", "-nolisten", "unix",
                             "-displayfd", str(writer)],
                            pass_fds=[writer], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(writer)
    display = os.read(reader, 16).decode().strip()
    os.close(reader)
    if not display:
        print("Xvfb did not start", flush=True)
        raise SystemExit(1)
    environment = dict(os.environ, DISPLAY=":" + display, GDK_BACKEND="x11", LANG="C.UTF-8")
    environment.pop("WAYLAND_DISPLAY", None)
    orca = subprocess.Popen(["orca", "--replace", "--debug-file=%s" % log, "--user-prefs=%s" % prefs,
                             "--speech-system=speechdispatcherfactory"],
                            stdout=open(os.path.join(options.out, "orca.out"), "w"), stderr=subprocess.STDOUT,
                            env=environment)
    try:
        pump(4.0)
        check(orca.poll() is None, "Orca started and stays up")
        # What the desktop's accessibility switch and Orca's own startup do.
        set_status(IsEnabled=True, ScreenReaderEnabled=True)
        application = wait_for(find_application, 15)
        if not check(application is not None, "Orca sees the application in the registry"):
            return
        window = application.get_child_at_index(0)
        email = wait_for(lambda: find(window, "Email or phone number", "entry"), 5)
        password = find(window, "Password", "password text")
        if not check(email is not None and password is not None, "the login fields are in the tree"):
            return
        pump(2.0)
        Atspi.Component.grab_focus(password)
        pump(3.0)
        Atspi.EditableText.set_text_contents(password, "correct horse")
        pump(1.0)
        Atspi.Component.grab_focus(email)
        pump(4.0)
        check(orca.poll() is None, "Orca is still running after walking the window")
        check(os.kill(options.client_pid, 0) is None, "the client is still running")
    finally:
        # Orca writes its debug file in blocks: what it spoke last is in the file once it has exited.
        orca.send_signal(signal.SIGTERM)
        try:
            orca.wait(15)
        except subprocess.TimeoutExpired:
            orca.kill()
        xvfb.terminate()
        set_status(IsEnabled=False, ScreenReaderEnabled=False)
    lines = speech(log)
    print("--- what Orca spoke", flush=True)
    for line in lines:
        print("  " + line, flush=True)
    print("---", flush=True)
    check(bool(lines), "Orca produced speech")
    check(any(line.startswith("MaiCord frame") for line in lines), "Orca announces the window")
    check(any("Email or phone number entry" in line for line in lines), "focusing the identifier field speaks its name and role")
    check(any("Password password text" in line for line in lines), "focusing the password field speaks its name and role")
    check(not any("correct horse" in line for line in lines), "nothing spoken discloses a password")
    check(not any("unfocused dialog" in line for line in lines), "the sign-in dialog is not announced as unfocused")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback
        traceback.print_exc()
        failures += 1
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)
