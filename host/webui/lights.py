#!/usr/bin/env python3
"""Live pad LEDs over the Spectra MIDI protocol.

The sketch sets a ring colour from channel-3 Note On with the pad's own note and a
palette velocity (1..120, 0 clears, 121..127 forces the Utility colour), and an
animation from channel-4 Note On velocity 18..49.  Templates use the configured base
channel, so this module builds its messages from the current settings.

A reader thread watches the same rawmidi node for the notes the sketch sends when a
pad is pressed, so the page can highlight real taps.  Everything here is in-memory
plus non-blocking writes to /dev/snd/midiC*D0; no DFU, no files.
"""
import os
import select
import threading
import time

import device
import settings

ARCADE_BASE = (36, 52, 68, 84)

# Midi Fighter Spectra palette (User Guide Ver 1.01, Fig 2): velocity = 1 + 6*index.
_PALETTE_RGB = (0x000000, 0xED1F24, 0x760F12, 0xF26722, 0x793311, 0xFFC907, 0x7F6403,
                0xC4D82E, 0x626C17, 0x00C700, 0x006300, 0x00FFFF, 0x007F7F, 0x0000FF,
                0x00007F, 0x8500FF, 0x42007F, 0xD2009F, 0x69004F, 0xFFFFFF)
_PALETTE_NAMES = ("black", "red", "red dim", "orange", "orange dim", "yellow", "yellow dim",
                  "lime", "lime dim", "green", "green dim", "cyan", "cyan dim", "blue",
                  "blue dim", "purple", "purple dim", "magenta", "magenta dim", "white")
PALETTE = [{"index": i, "velocity": 1 + 6 * i, "name": _PALETTE_NAMES[i], "rgb": _PALETTE_RGB[i]}
           for i in range(len(_PALETTE_NAMES))]

LOCK = threading.Lock()
STATE = {
    "node": None,
    "bank": 0,
    "colors": [None] * 16,      # palette RGB, or None = Utility colour
    "force": [False] * 16,      # velocity 121..127
    "velocities": [0] * 16,     # last control velocity per pad
    "anims": [0] * 16,
    "pressed": [False] * 16,
    "last_hit": [0.0] * 16,
    "error": None,
    "midi_ok": False,
}
STOP = threading.Event()


def node():
    port = device.midi_port()
    if not port:
        return None
    try:
        card = int(port.split(":")[1].split(",")[0])
    except (IndexError, ValueError):
        return None
    path = f"/dev/snd/midiC{card}D0"
    return path if os.path.exists(path) else None


def rgb_for_velocity(velocity):
    if velocity == 0 or velocity >= 121:
        return None
    return PALETTE[(velocity - 1) // 6]["rgb"]


def build(action, base_ch, bank, pads, velocity):
    """MIDI messages for one lights action; raises ValueError on bad input."""
    if not 1 <= base_ch <= 14:
        raise ValueError("base channel out of range")
    if action == "bank":
        if not 0 <= velocity <= 3:
            raise ValueError("bank must be 0..3")
        return [bytes((0x90 | base_ch, velocity, 127))]
    if not pads or len(pads) > 16 or any(not 0 <= p <= 15 for p in pads):
        raise ValueError("pads must be 0..15")
    if action == "color":
        if not 0 <= velocity <= 120:
            raise ValueError("colour velocity must be 0..120")
    elif action == "force":
        velocity = 127
    elif action == "anim":
        if not 0 <= velocity <= 49:
            raise ValueError("animation velocity must be 0..49")
    else:
        raise ValueError("unknown action")
    trig = 0x90 | (base_ch - 1)
    ctrl = 0x90 | base_ch
    base = ARCADE_BASE[bank & 3]
    if action == "anim":
        return [bytes((ctrl, base + p, velocity)) for p in pads]
    return [bytes((trig, base + p, velocity)) for p in pads]


def write(data):
    path = node()
    if not path:
        raise OSError("no SMC-PAD MIDI port")
    fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
    try:
        for msg in data:
            os.write(fd, msg)
    finally:
        os.close(fd)


def apply(action, pads, velocity, base_ch):
    with LOCK:
        bank = STATE["bank"]
    msgs = build(action, base_ch, bank, pads, velocity)
    write(msgs)
    with LOCK:
        if action == "bank":
            STATE["bank"] = velocity
        elif action == "anim":
            for p in pads:
                STATE["anims"][p] = velocity
        else:
            for p in pads:
                STATE["velocities"][p] = 127 if action == "force" else velocity
                STATE["colors"][p] = (None if action == "force"
                                      else rgb_for_velocity(velocity))
                STATE["force"][p] = action == "force"
        STATE["error"] = None
        STATE["midi_ok"] = True
    return msgs


def _consume(buf, base_ch):
    """Take whole channel messages off the front of buf; return (status, note, vel)."""
    events = []
    i = 0
    while i < len(buf):
        b = buf[i]
        if b >= 0xF0 or not b & 0x80:
            i += 1
        elif i + 2 >= len(buf):
            break
        else:
            events.append((b, buf[i + 1], buf[i + 2]))
            i += 3
    del buf[:i]
    return events


def _hits(events, base_ch):
    trig = 0x90 | (base_ch - 1)
    with LOCK:
        bank = STATE["bank"]
    hits = []
    for status, note, vel in events:
        kind = status & 0xF0
        if kind == 0x90 and (status & 0x0F) == (trig & 0x0F):
            pad = note - ARCADE_BASE[bank & 3]
            if 0 <= pad < 16:
                hits.append((pad, vel > 0))
    return hits


def reader():
    """Watch the rawmidi input for pad notes; reconnect when the card comes and goes."""
    buf = bytearray()
    while not STOP.wait(0.2):
        path = node()
        if not path:
            with LOCK:
                STATE["midi_ok"] = False
            continue
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError as exc:
            with LOCK:
                STATE["midi_ok"] = False
                STATE["error"] = str(exc)
            continue
        with LOCK:
            STATE["midi_ok"] = True
            STATE["error"] = None
        del buf[:]
        try:
            while not STOP.is_set():
                r, _, _ = select.select([fd], [], [], 0.3)
                if not r:
                    continue
                chunk = os.read(fd, 4096)
                if not chunk:
                    break
                buf.extend(chunk)
                base_ch = settings.load()["base_ch"]
                for pad, down in _hits(_consume(buf, base_ch), base_ch):
                    with LOCK:
                        STATE["pressed"][pad] = down
                        if down:
                            STATE["last_hit"][pad] = time.time()
        except OSError as exc:
            with LOCK:
                STATE["error"] = str(exc)
        finally:
            os.close(fd)


def start_reader():
    if not any(t.name == "smcpad-midi" for t in threading.enumerate()):
        threading.Thread(target=reader, name="smcpad-midi", daemon=True).start()


def snapshot():
    with LOCK:
        node_path = node()
        return {
            "node": node_path,
            "bank": STATE["bank"],
            "colors": [None if c is None else f"{c:06X}" for c in STATE["colors"]],
            "force": list(STATE["force"]),
            "velocities": list(STATE["velocities"]),
            "anims": list(STATE["anims"]),
            "pressed": list(STATE["pressed"]),
            "last_hit": list(STATE["last_hit"]),
            "midi_ok": STATE["midi_ok"] and node_path is not None,
            "error": STATE["error"],
            "palette": PALETTE,
            "actions": {"color": "1..120 (0 clears, 121..127 forces the Utility colour)",
                        "anim": "18..33 brightness, 34..41 gate, 42..49 pulse"},
        }
