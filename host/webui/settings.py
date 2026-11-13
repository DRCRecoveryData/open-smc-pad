#!/usr/bin/env python3
"""Spectra compile-time settings, editable from the web UI.

The sketch has no runtime settings channel; every option in SpectraConfig.h is a
build-time define.  This module owns their schema, validation, persistence
(work/webui/settings.json) and the -D list that host/build_sketch.sh consumes via
SMCPAD_DEFS.  Applying settings = rebuild + flash (server.py jobs).
"""
import json
import os

from device import ROOT

PATH = os.path.join(ROOT, "work", "webui", "settings.json")

FIELDS = [
    {"key": "base_ch", "label": "MIDI base channel", "type": "int", "min": 1, "max": 14,
     "default": 3, "help": "triggers and colour notes; bank notes and animations use the "
                           "next channel, side-button CCs the one after"},
    {"key": "key_velocity", "label": "Key MIDI velocity", "type": "int", "min": 1, "max": 127,
     "default": 127, "help": "fixed trigger velocity (the Pocket's pads are not used for it)"},
    {"key": "momentary_cc", "label": "Momentary CC on triggers", "type": "bool",
     "default": False, "help": "also send CC 127 on press / 0 on release, on the next channel"},
    {"key": "spark", "label": "Press spark", "type": "bool", "default": True,
     "help": "flash a hit pad white for 40 ms"},
    {"key": "bank_wave", "label": "Bank-change wave", "type": "bool", "default": True,
     "help": "diagonal white wave across the grid when the bank changes"},
    {"key": "sleep_min", "label": "Sleep after (minutes)", "type": "int", "min": 0, "max": 240,
     "default": 0, "help": "0 = never; idle pads show a dim colour wheel after this long"},
    {"key": "side_mode", "label": "Side-button mode", "type": "int", "min": 0, "max": 2,
     "default": 0, "help": "0 factory (Left2/Right2 change bank), 1 all notes, 2 all CCs"},
    {"key": "side_bank_buttons", "label": "Bank-aware side buttons", "type": "bool",
     "default": True, "help": "side notes/CCs change with the bank"},
    {"key": "side_cc_release", "label": "Side CC release value", "type": "int", "min": 0,
     "max": 127, "default": 0, "help": "sent when a side button configured for CC is released"},
]

BANKS = 4


def _color_field(key, label):
    return {"key": key, "label": label, "type": "color", "min": 0, "max": 0xFFFFFF,
            "default": 0xFFFFFF if key.startswith("active") else 0x000000}


COLOR_FIELDS = []
for _b in range(BANKS):
    COLOR_FIELDS.append(_color_field(f"active_b{_b}", f"Bank {_b + 1} pressed"))
for _b in range(BANKS):
    COLOR_FIELDS.append(_color_field(f"inactive_b{_b}", f"Bank {_b + 1} idle"))

ALL_FIELDS = FIELDS + COLOR_FIELDS
DEFAULTS = {f["key"]: f["default"] for f in ALL_FIELDS}


def _as_int(value):
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.strip():
        try:
            return int(value.strip(), 0)
        except ValueError:
            return None
    return None


def validate(values):
    """Return (clean_values, errors).  Missing keys take their defaults."""
    clean, errors = {}, {}
    for field in ALL_FIELDS:
        key = field["key"]
        raw = values.get(key, field["default"])
        if field["type"] == "bool":
            if isinstance(raw, str):
                raw = raw.strip().lower() in ("1", "true", "yes", "on")
            clean[key] = bool(raw)
            continue
        n = _as_int(raw)
        if n is None:
            errors[key] = f"{field['label']}: not a number"
            continue
        if n < field["min"] or n > field["max"]:
            errors[key] = (f"{field['label']}: {n} is outside "
                           f"{field['min']}..{field['max']}")
            continue
        clean[key] = n
    return clean, errors


def defines(values):
    """The -D list for host/build_sketch.sh (SMCPAD_DEFS)."""
    v, errors = validate(values)
    if errors:
        raise ValueError("; ".join(errors[k] for k in sorted(errors)))
    out = [
        f"-DSPECTRA_BASE_CH={v['base_ch']}",
        f"-DSPECTRA_KEY_VELOCITY={v['key_velocity']}",
        f"-DSPECTRA_MOMENTARY_CC={int(v['momentary_cc'])}",
        f"-DSPECTRA_SPARK={int(v['spark'])}",
        f"-DSPECTRA_BANK_WAVE={int(v['bank_wave'])}",
        f"-DSPECTRA_SLEEP_MIN={v['sleep_min']}",
        f"-DSPECTRA_SIDE_MODE={v['side_mode']}",
        f"-DSPECTRA_SIDE_BANK_BUTTONS={int(v['side_bank_buttons'])}",
        f"-DSPECTRA_SIDE_CC_RELEASE={v['side_cc_release']}",
    ]
    for b in range(BANKS):
        out.append(f"-DSPECTRA_ACTIVE_B{b}=0x{v[f'active_b{b}']:06X}u")
        out.append(f"-DSPECTRA_INACTIVE_B{b}=0x{v[f'inactive_b{b}']:06X}u")
    return out


def load():
    try:
        with open(PATH, "r") as f:
            stored = json.load(f)
    except (OSError, ValueError):
        stored = {}
    clean, _ = validate(stored if isinstance(stored, dict) else {})
    return clean


def save(values):
    clean, errors = validate(values)
    if errors:
        raise ValueError("; ".join(errors[k] for k in sorted(errors)))
    os.makedirs(os.path.dirname(PATH), exist_ok=True)
    tmp = PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump(clean, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, PATH)
    return clean


def payload():
    return {"fields": ALL_FIELDS, "banks": BANKS, "defaults": DEFAULTS,
            "values": load()}
