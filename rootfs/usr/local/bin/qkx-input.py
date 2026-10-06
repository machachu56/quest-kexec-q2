#!/usr/bin/env python3
"""Forward a host input device (keyboard, gamepad) to the headset via uinput.

Until Bluetooth and the Touch controllers work in Linux, this lets a PC
keyboard or USB/Bluetooth gamepad drive games on the headset over the USB
Ethernet link.

  headset:  qkx-input.py recv [--port 5555]
  host:     sudo tools/qkx-input.py send /dev/input/eventN [--host 10.42.0.2]
            tools/qkx-input.py list

The device is grabbed on the host while forwarding (Ctrl+C releases it).
Protocol: one JSON line with name/ids/capabilities, then fixed 8-byte events
(type u16, code u16, value s32).
"""
import argparse
import json
import socket
import struct

import evdev
from evdev import ecodes

EVENT = struct.Struct("<HHi")


def cmd_list(_):
    for path in evdev.list_devices():
        d = evdev.InputDevice(path)
        print(f"{path}\t{d.name}")


def cmd_send(a):
    dev = evdev.InputDevice(a.device)
    caps = dev.capabilities(absinfo=True)
    # ABS caps arrive as (code, AbsInfo); flatten for JSON.
    jcaps = {}
    for etype, codes in caps.items():
        if etype == ecodes.EV_SYN:
            continue
        out = []
        for c in codes:
            if isinstance(c, tuple):
                code, info = c
                out.append([code, list(info)])
            else:
                out.append(c)
        jcaps[str(etype)] = out
    hello = {"name": dev.name, "vendor": dev.info.vendor, "product": dev.info.product,
             "version": dev.info.version, "bustype": dev.info.bustype, "caps": jcaps}
    s = socket.create_connection((a.host, a.port))
    s.sendall((json.dumps(hello) + "\n").encode())
    print(f"forwarding {dev.name} to {a.host}:{a.port} (Ctrl+C to stop)")
    dev.grab()
    try:
        for ev in dev.read_loop():
            s.sendall(EVENT.pack(ev.type, ev.code, ev.value))
    except KeyboardInterrupt:
        pass
    finally:
        dev.ungrab()


def cmd_recv(a):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(4)
    print(f"listening on :{a.port}")
    while True:
        conn, peer = srv.accept()
        f = conn.makefile("rb")
        hello = json.loads(f.readline())
        caps = {}
        for etype, codes in hello["caps"].items():
            caps[int(etype)] = [(c[0], evdev.AbsInfo(*c[1])) if isinstance(c, list) else c
                                for c in codes]
        ui = evdev.UInput(caps, name=hello["name"], vendor=hello["vendor"],
                          product=hello["product"], version=hello["version"],
                          bustype=hello["bustype"])
        print(f"{peer[0]}: created {ui.device.path} for {hello['name']}")
        try:
            while True:
                data = f.read(EVENT.size)
                if len(data) < EVENT.size:
                    break
                ui.write(*EVENT.unpack(data))
        finally:
            ui.close()
            conn.close()
            print(f"{peer[0]}: {hello['name']} disconnected")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list").set_defaults(fn=cmd_list)
    s = sub.add_parser("send")
    s.add_argument("device")
    s.add_argument("--host", default="10.42.0.2")
    s.add_argument("--port", type=int, default=5555)
    s.set_defaults(fn=cmd_send)
    r = sub.add_parser("recv")
    r.add_argument("--port", type=int, default=5555)
    r.set_defaults(fn=cmd_recv)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
