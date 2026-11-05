#!/usr/bin/env python3
"""Drives the Linux client (MaiCord) on sway through a list of steps and takes screenshots along the way.

    tools/client_flow.py [--compositor headless|nested] [--scale S] [--binary PATH] [--gallium NAME] [--out DIR]
                         STEP...

Steps (one command-line argument each):
    wait:SECONDS          sleep
    shot:NAME             grim screenshot of the whole output to DIR/NAME.png
    click:X,Y             move the pointer to layout pixel X,Y and click the left button (tools/wayland_pointer.py)
    move:X,Y              move the pointer to layout pixel X,Y (hover)
    drag:X0,Y0,X1,Y1      press at X0,Y0, move to X1,Y1, release
    scroll:X,Y,DELTA      move to X,Y and scroll the wheel (positive down)
    type:TEXT             type text with wtype
    key:NAME              press a key (wtype -k NAME, such as Return, Escape, Tab)
    chord:MODS,KEY        press KEY with modifiers (ctrl, shift, alt, logo), such as chord:ctrl,a
    sway:COMMAND          any swaymsg command (output scale, fullscreen, resize, floating, ...)
    tree                  print the client's window rectangle in sway's tree
    metrics               CPU time, voluntary context switches, VmRSS of the client (since the previous metrics step)
    alive                 fail unless the client still runs
    close                 swaymsg kill on the window, then expect a clean exit (status 0) within five seconds
    log                   print the client's log lines that are not info

The client runs with a private, empty profile under DIR/profile (data, state, cache, config), the Secret Service off, and
headless sway (pixman) as its compositor; `nested` runs sway inside the running compositor (WSLg's Weston). The output is
1280x800 at --scale. Exit status: nonzero when a step fails, the client dies or logs an error line.
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import time

APP_ID = "io.github.maj113.MaiCord"


class Flow:
    def __init__(self, options):
        self.options = options
        self.out = os.path.abspath(options.out)
        os.makedirs(self.out, exist_ok=True)
        self.runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
        if not os.path.isdir(self.runtime) or not os.access(self.runtime, os.W_OK):
            self.runtime = os.path.join(self.out, "runtime")
            os.makedirs(self.runtime, mode=0o700, exist_ok=True)
        self.sway = None
        self.pointer = None
        self.client = None
        self.env = dict(os.environ, XDG_RUNTIME_DIR=self.runtime, LANG=os.environ.get("LANG", "C.UTF-8"))
        self.env.pop("DISPLAY", None)
        self.failures = 0
        self.last_metrics = None

    # -- compositor and client ------------------------------------------------------------------------------------
    def start_sway(self):
        scale = self.options.scale
        config = os.path.join(self.out, "sway.conf")
        with open(config, "w") as handle:
            handle.write(f"output * resolution {int(1280 * scale)}x{int(800 * scale)} scale {scale}\n"
                         "default_border none\ndefault_floating_border none\nfocus_follows_mouse no\n")
        env = dict(self.env, WLR_RENDERER="pixman")
        if self.options.compositor == "headless":
            env.update(WLR_BACKENDS="headless", WLR_LIBINPUT_NO_DEVICES="1")
        else:
            env.update(WLR_BACKENDS="wayland")
        # -d: sway names its Wayland display in the log (a stale socket of an earlier run may have the next name, so
        # the directory cannot tell); its IPC socket is named after its pid.
        log_path = os.path.join(self.out, "sway.log")
        log = open(log_path, "w")
        self.sway = subprocess.Popen(["sway", "-d", "-c", config], env=env, stdout=log, stderr=subprocess.STDOUT)
        display = None
        for _ in range(100):
            with open(log_path, errors="replace") as handle:
                for line in handle:
                    if "Running compositor on wayland display '" in line:
                        display = line.split("'")[1]
            socket = os.path.join(self.runtime, f"sway-ipc.{os.getuid()}.{self.sway.pid}.sock")
            if display and os.path.exists(socket):
                self.env["SWAYSOCK"] = socket
                break
            time.sleep(0.1)
        else:
            raise SystemExit("sway did not start; see sway.log")
        self.env["WAYLAND_DISPLAY"] = display
        time.sleep(0.5)
        # One virtual pointer for the whole run: the client keeps the wl_pointer it binds when the device appears.
        helper = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wayland_pointer.py")
        self.pointer = subprocess.Popen([sys.executable, helper, "--width", "1280", "--height", "800"], env=self.env,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        if self.pointer.stdout.readline().strip() != "ready":
            raise SystemExit("the virtual pointer helper did not start")

    def start_client(self):
        profile = os.path.join(self.out, "profile")
        shutil.rmtree(profile, ignore_errors=True)
        env = dict(self.env, XDG_DATA_HOME=profile + "/data", XDG_STATE_HOME=profile + "/state",
                   XDG_CACHE_HOME=profile + "/cache", XDG_CONFIG_HOME=profile + "/config", MAICORD_SECRET_SERVICE="off")
        if self.options.gallium:
            env["GALLIUM_DRIVER"] = self.options.gallium
        self.state = profile + "/state/maicord/logs/maicord.log"
        log = open(os.path.join(self.out, "client.out"), "w")
        self.client = subprocess.Popen([self.options.binary], env=env, stdout=log, stderr=subprocess.STDOUT)
        for _ in range(100):
            if self.window():
                break
            if self.client.poll() is not None:
                raise SystemExit(f"the client exited early with status {self.client.returncode}")
            time.sleep(0.1)
        time.sleep(1.5)

    def stop(self):
        if self.pointer and self.pointer.poll() is None:
            self.pointer.stdin.write("quit\n")
            self.pointer.stdin.flush()
            try:
                self.pointer.wait(3)
            except subprocess.TimeoutExpired:
                self.pointer.kill()
        for process in (self.client, self.sway):
            if process and process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(5)
                except subprocess.TimeoutExpired:
                    process.kill()

    # -- steps ---------------------------------------------------------------------------------------------------
    def msg(self, *arguments, check=True):
        result = subprocess.run(["swaymsg", *arguments], env=self.env, capture_output=True, text=True)
        if check and result.returncode != 0:
            self.fail(f"swaymsg {' '.join(arguments)}: {result.stderr.strip() or result.stdout.strip()}")
        return result.stdout

    def window(self):
        import json
        try:
            tree = json.loads(self.msg("-t", "get_tree", check=False))
        except ValueError:
            return None

        def walk(node):
            if node.get("app_id") == APP_ID:
                return node
            for child in node.get("nodes", []) + node.get("floating_nodes", []):
                found = walk(child)
                if found:
                    return found
            return None
        return walk(tree)

    def fail(self, text):
        self.failures += 1
        print("FAIL", text)

    def run(self, step):
        name, _, argument = step.partition(":")
        getattr(self, "step_" + name)(argument)

    def step_wait(self, argument):
        time.sleep(float(argument))

    def step_shot(self, argument):
        path = os.path.join(self.out, argument + ".png")
        result = subprocess.run(["grim", path], env=self.env, capture_output=True, text=True)
        if result.returncode != 0:
            self.fail("grim: " + result.stderr.strip())
        else:
            print("screenshot:", path)

    def pointer_command(self, command):
        self.pointer.stdin.write(command + "\n")
        self.pointer.stdin.flush()
        if self.pointer.stdout.readline().strip() != "ok":
            self.fail("the pointer helper refused: " + command)
        time.sleep(0.2)

    def move(self, x, y):
        self.pointer_command(f"move {x} {y}")

    def step_click(self, argument):
        x, y = (int(v) for v in argument.split(","))
        self.move(x, y)
        self.pointer_command("click")
        time.sleep(0.3)

    def step_move(self, argument):
        x, y = (int(v) for v in argument.split(","))
        self.move(x, y)

    def step_scroll(self, argument):
        x, y, delta = (int(v) for v in argument.split(","))
        self.move(x, y)
        self.pointer_command(f"scroll {delta}")
        time.sleep(0.3)

    def step_drag(self, argument):
        x0, y0, x1, y1 = (int(v) for v in argument.split(","))
        self.move(x0, y0)
        self.pointer_command("press")
        for i in range(1, 11):
            self.move(x0 + (x1 - x0) * i // 10, y0 + (y1 - y0) * i // 10)
        self.pointer_command("release")
        time.sleep(0.3)

    def wtype(self, *arguments):
        # Each wtype is a new virtual keyboard: the client must receive its keymap and focus before the first key.
        result = subprocess.run(["wtype", "-s", "300", *arguments], env=self.env, capture_output=True, text=True)
        if result.returncode != 0:
            self.fail("wtype: " + result.stderr.strip())
        time.sleep(0.4)

    def step_type(self, argument):
        self.wtype(argument)

    def step_key(self, argument):
        self.wtype("-k", argument)

    def step_chord(self, argument):
        *mods, key = argument.split(",")
        arguments = []
        for mod in mods:
            arguments += ["-M", mod]
        arguments += ["-k", key]
        for mod in reversed(mods):
            arguments += ["-m", mod]
        self.wtype(*arguments)

    def step_sway(self, argument):
        print(self.msg(*argument.split()).strip())
        time.sleep(0.7)

    def step_tree(self, _):
        node = self.window()
        if not node:
            self.fail("no client window in sway's tree")
            return
        rect, inner = node["rect"], node["window_rect"]
        print(f"window: rect {rect['width']}x{rect['height']} at {rect['x']},{rect['y']}, content "
              f"{inner['width']}x{inner['height']}, floating={node.get('type') == 'floating_con'}, "
              f"fullscreen={node.get('fullscreen_mode')}, focused={node.get('focused')}")

    def sample(self):
        pid = self.client.pid
        with open(f"/proc/{pid}/stat") as handle:
            fields = handle.read().rsplit(")", 1)[1].split()
        ticks = int(fields[11]) + int(fields[12])
        switches = 0
        threads = {}
        for task in os.listdir(f"/proc/{pid}/task"):
            try:
                with open(f"/proc/{pid}/task/{task}/status") as handle:
                    for line in handle:
                        if line.startswith("voluntary_ctxt_switches"):
                            switches += int(line.split()[1])
                with open(f"/proc/{pid}/task/{task}/stat") as handle:
                    text = handle.read()
                name = text[text.index("(") + 1:text.rindex(")")]
                rest = text.rsplit(")", 1)[1].split()
                threads[task] = (name, int(rest[11]) + int(rest[12]))
            except (OSError, ValueError):
                pass  # the thread ended
        rss = 0
        with open(f"/proc/{pid}/status") as handle:
            for line in handle:
                if line.startswith("VmRSS"):
                    rss = int(line.split()[1])
        return time.monotonic(), ticks, switches, rss, threads

    def step_metrics(self, _):
        now = self.sample()
        if self.last_metrics:
            seconds = now[0] - self.last_metrics[0]
            hertz = os.sysconf("SC_CLK_TCK")
            print(f"cpu {100 * (now[1] - self.last_metrics[1]) / hertz / seconds:.2f}%  wakeups "
                  f"{(now[2] - self.last_metrics[2]) / seconds:.1f}/s  VmRSS {now[3]} kB  over {seconds:.1f} s")
            busy = sorted(((ticks - self.last_metrics[4].get(task, ("", 0))[1], name, task)
                           for task, (name, ticks) in now[4].items()), reverse=True)[:3]
            for ticks, name, task in busy:
                if ticks:
                    print(f"  thread {task} ({name}): {100 * ticks / hertz / seconds:.1f}% cpu")
        else:
            print(f"VmRSS {now[3]} kB (the next metrics step reports the rates)")
        self.last_metrics = now

    def step_alive(self, _):
        if self.client.poll() is not None:
            self.fail(f"the client exited with status {self.client.returncode}")

    def step_close(self, _):
        self.msg("[app_id=\"%s\"]" % APP_ID, "kill")
        try:
            status = self.client.wait(5)
        except subprocess.TimeoutExpired:
            self.fail("the client did not exit within five seconds of the close request")
            return
        print(f"the client exited with status {status}")
        if status != 0:
            self.fail(f"the client's exit status is {status}")

    def step_log(self, _):
        if os.path.exists(self.state):
            for line in open(self.state, errors="replace"):
                if "[I]" not in line:
                    print(line.rstrip())

    def report_errors(self):
        if os.path.exists(self.state):
            for line in open(self.state, errors="replace"):
                if "[E]" in line:
                    print("log error:", line.rstrip())
                    self.failures += 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compositor", choices=("headless", "nested"), default="headless")
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("--binary", default=os.path.expanduser("~/mc-build/MaiCord"))
    parser.add_argument("--gallium", default="")
    parser.add_argument("--out", default=os.path.expanduser("~/mc-client-flow"))
    parser.add_argument("steps", nargs="*")
    options = parser.parse_args()
    flow = Flow(options)
    try:
        flow.start_sway()
        flow.start_client()
        for step in options.steps:
            flow.run(step)
            if flow.client.poll() is not None and not step.startswith("close"):
                flow.fail(f"the client exited with status {flow.client.returncode} during {step}")
                break
        flow.report_errors()
    finally:
        flow.stop()
    return 1 if flow.failures else 0


if __name__ == "__main__":
    sys.exit(main())
