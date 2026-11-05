#!/usr/bin/env python3
"""A long-lived virtual pointer for wlroots compositors (zwlr_virtual_pointer_v1), driven from standard input.

    tools/wayland_pointer.py [--width W --height H]      reads one command per line:
        move X Y         absolute position in the output layout's logical pixels
        press [BUTTON]   BUTTON is left (default), right or middle
        release [BUTTON]
        click [BUTTON]
        scroll DY        vertical wheel notches (positive down)
        sleep SECONDS
        quit

Unlike a one-shot `wlrctl pointer`, the device stays attached to the seat for the whole session, so a client that binds
wl_pointer on the seat's capability change keeps its pointer focus between commands. Plain Wayland wire protocol over
$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY, standard library only. Prints "ready" once the device exists, "ok" after each command.
"""
import argparse
import os
import socket
import struct
import sys
import time

BUTTONS = {"left": 0x110, "right": 0x111, "middle": 0x112}


class Wire:
    def __init__(self):
        path = os.environ["WAYLAND_DISPLAY"]
        if not path.startswith("/"):
            path = os.path.join(os.environ["XDG_RUNTIME_DIR"], path)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.buffer = b""
        self.next_id = 3  # 1 is wl_display, 2 the registry

    def new_id(self):
        self.next_id += 1
        return self.next_id - 1

    @staticmethod
    def string(text):
        data = text.encode() + b"\0"
        return struct.pack("<I", len(data)) + data + b"\0" * (-len(data) % 4)

    def send(self, object_id, opcode, payload=b""):
        self.sock.sendall(struct.pack("<II", object_id, ((8 + len(payload)) << 16) | opcode) + payload)

    def messages(self):
        """The messages that have arrived, as (object, opcode, payload); blocks until there is at least one."""
        while True:
            found = []
            while len(self.buffer) >= 8:
                object_id, word = struct.unpack_from("<II", self.buffer)
                size = word >> 16
                if len(self.buffer) < size:
                    break
                found.append((object_id, word & 0xFFFF, self.buffer[8:size]))
                self.buffer = self.buffer[size:]
            if found:
                return found
            data = self.sock.recv(65536)
            if not data:
                raise SystemExit("the compositor closed the connection")
            self.buffer += data

    def roundtrip(self):
        callback = self.new_id()
        self.send(1, 0, struct.pack("<I", callback))  # wl_display.sync
        globals_seen = []
        while True:
            for object_id, opcode, payload in self.messages():
                if object_id == 1 and opcode == 0:
                    code, length = struct.unpack_from("<II", payload, 4)
                    raise SystemExit("wayland error %d: %s" % (code, payload[12:12 + length - 1].decode()))
                if object_id == callback and opcode == 0:
                    return globals_seen
                if object_id == 2 and opcode == 0:  # wl_registry.global
                    name, length = struct.unpack_from("<II", payload)
                    interface = payload[8:8 + length - 1].decode()
                    version = struct.unpack_from("<I", payload, 8 + length + (-length % 4))[0]
                    globals_seen.append((name, interface, version))


def fixed(value):
    return struct.pack("<i", int(value * 256))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=800)
    options = parser.parse_args()
    wire = Wire()
    wire.send(1, 1, struct.pack("<I", 2))  # wl_display.get_registry -> id 2
    found = {interface: (name, version) for name, interface, version in wire.roundtrip()}
    if "zwlr_virtual_pointer_manager_v1" not in found:
        raise SystemExit("the compositor has no zwlr_virtual_pointer_manager_v1")
    name, version = found["zwlr_virtual_pointer_manager_v1"]
    manager = wire.new_id()
    interface = "zwlr_virtual_pointer_manager_v1"
    wire.send(2, 0, struct.pack("<I", name) + Wire.string(interface) + struct.pack("<II", 1, manager))
    pointer = wire.new_id()
    wire.send(manager, 0, struct.pack("<II", 0, pointer))  # create_virtual_pointer(null seat, id)
    wire.roundtrip()

    def now():
        return int(time.monotonic() * 1000) & 0xFFFFFFFF

    print("ready", flush=True)
    for line in sys.stdin:
        words = line.split()
        if not words:
            continue
        command, arguments = words[0], words[1:]
        if command == "move":
            wire.send(pointer, 1, struct.pack("<IIIII", now(), int(arguments[0]), int(arguments[1]), options.width,
                                              options.height))
            wire.send(pointer, 4)
        elif command in ("press", "release", "click"):
            button = BUTTONS[arguments[0] if arguments else "left"]
            if command in ("press", "click"):
                wire.send(pointer, 2, struct.pack("<III", now(), button, 1))
                wire.send(pointer, 4)
            if command == "click":
                time.sleep(0.05)
            if command in ("release", "click"):
                wire.send(pointer, 2, struct.pack("<III", now(), button, 0))
                wire.send(pointer, 4)
        elif command == "scroll":
            notches = int(arguments[0])
            wire.send(pointer, 5, struct.pack("<I", 0))  # axis_source: wheel
            wire.send(pointer, 3, struct.pack("<II", now(), 0) + fixed(15 * notches))
            wire.send(pointer, 7, struct.pack("<II", now(), 0) + fixed(15 * notches) + struct.pack("<i", notches))
            wire.send(pointer, 4)
        elif command == "sleep":
            time.sleep(float(arguments[0]))
        elif command == "quit":
            break
        else:
            print("unknown command: " + command, file=sys.stderr, flush=True)
        wire.roundtrip()
        print("ok", flush=True)
    wire.send(pointer, 8)  # destroy
    wire.roundtrip()


if __name__ == "__main__":
    main()
