#!/usr/bin/env python3
"""Round-trip test for the PocketDemo SysEx backup/restore, on the emulator.

Build the simulator first, then run this:

  arduino/sim/build.sh arduino/sketches/PocketDemo /tmp/pd
  python3 host/backup_test.py [/tmp/pd]

It restores a prepared image (settings, a preset, a take), dumps it back, and checks
every chunk, the acks (0 ok, 1 bad sum, 2 bad address, 3 busy mid-pass) and that the
restored take plays.
"""
import json
import struct
import subprocess
import sys

HDR = bytes([0x7D, 0x53, 0x4D, 0x50])
CHUNK_DATA = 126


def pack7(data):
    out, acc, nbits = bytearray(), 0, 0
    for b in data:
        acc |= b << nbits
        nbits += 8
        while nbits >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nbits -= 7
    if nbits:
        out.append(acc & 0x7F)
    return bytes(out)


def unpack7(data):
    out, acc, nbits = bytearray(), 0, 0
    for b in data:
        acc |= (b & 0x7F) << nbits
        nbits += 7
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8
    return bytes(out)


def msg(body):
    return bytes([0xF0]) + HDR + pack7(bytes(body)) + bytes([0xF7])


# ---- the image we push: settings, preset 0, a one-bar take
image = bytearray(b"\xFF" * 4096)
# Settings v8: magic, root 5 (F), major (3), triad (1), latch, bpm 120, arpOct 1, gate 50
image[64:80] = struct.pack("<IBBBBBBHBBBB", 0x50444B08, 5, 3, 1, 1, 0, 0, 120, 0, 0, 1, 50)
# Preset 0: root 7, pentatonic (1), seventh (2), bpm 100, set marker
image[512:525] = struct.pack("<BBBBBBBBBBBH", 7, 1, 2, 0, 0, 0, 0, 0, 1, 50, 0xA6, 100)
# Take: 4 events at clock 0/48, 1 bar, 1 layer
ev = [0 | (1 << 9) | (60 << 10) | (100 << 17), 0 | (60 << 10),
      48 | (1 << 9) | (64 << 10) | (100 << 17), 48 | (64 << 10)]
for i, w in enumerate(ev):
    image[1024 + 4 * i:1028 + 4 * i] = struct.pack("<I", w)
# LoopHeader: magic, count 4, layerStart[8], bars 1, layers 1, transpose 12 (as recorded)
image[3072:3097] = struct.pack("<IH8HBBB", 0x50444C50, 4, 0, 0, 0, 0, 0, 0, 0, 0,
                               1, 1, 12)

sum7 = sum(image[64:]) & 0x7F
lines = []
t = 100
for addr in range(64, 4096, CHUNK_DATA):
    body = bytes([0x02, addr >> 7, addr & 0x7F]) + bytes(image[addr:addr + CHUNK_DATA])
    lines.append(f"{t} midi " + " ".join(f"{b:02X}" for b in msg(body)))
lines.append(f"400 midi " + " ".join(f"{b:02X}" for b in msg([0x03, sum7])))
lines.append("500 midi " + " ".join(f"{b:02X}" for b in msg([0x01])))     # dump
addr = 64
one = bytes([0x02, addr >> 7, addr & 0x7F]) + bytes(image[addr:addr + CHUNK_DATA])
staged_sum = sum(image[64:190]) & 0x7F
lines.append("1100 midi " + " ".join(f"{b:02X}" for b in msg(one)))      # a staged image...
lines.append("1200 midi " + " ".join(f"{b:02X}" for b in msg([0x03, (staged_sum + 1) & 0x7F])))  # wrong sum
lines.append("1400 midi " + " ".join(f"{b:02X}" for b in msg([0x02, 0, 0, 1, 2, 3])))     # addr < 64
lines.append("1600 midi " + " ".join(f"{b:02X}" for b in msg([0x01])))    # a second dump
lines.append("2400 midi B0 1E 01")                                       # CC 30 = 1: play the take
lines.append("6000 midi B0 1E 02")                                       # CC 30 = 2: record a pass
lines.append("7000 midi " + " ".join(f"{b:02X}" for b in msg(one)))      # staged mid-pass...
lines.append("7100 midi " + " ".join(f"{b:02X}" for b in msg([0x03, staged_sum])))  # commit: busy
lines.append("8200 midi B0 1E 00")                                       # CC 30 = 0: stop
open("/tmp/backup.scn", "w").write("\n".join(lines) + "\n")

sim = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pd"
out = subprocess.run([sim, "/tmp/backup.scn", "9000"], capture_output=True, text=True)
if out.returncode != 0:
    print(out.stderr)
    sys.exit(1)

stream = bytearray()
chunks = {}
acks = []
notes = []
for line in out.stdout.splitlines():
    try:
        e = json.loads(line)
    except json.JSONDecodeError:
        continue
    if "sysex" in e:
        stream.extend(e["sysex"])
        while 0xF7 in stream:
            end = stream.index(0xF7)
            m = bytes(stream[:end + 1])
            del stream[:end + 1]
            if m[:5] != b"\xF0" + HDR:
                print("bad message:", m.hex(" "))
                continue
            body = unpack7(m[5:-1])
            if body[0] == 0x41:
                addr = (body[1] << 7) | body[2]
                chunks[addr] = body[3:]
            elif body[0] == 0x42:
                acks.append(body[1])
    elif "midi" in e:
        b = e["midi"]
        if len(b) == 3 and b[0] & 0xF0 == 0x90:
            notes.append((e["t"], b[1], b[2]))

got = bytearray(image)
for addr, data in chunks.items():
    got[addr:addr + len(data)] = data
print("chunks:", len(chunks), "acks:", acks)
print("note-ons:", [(round(t), n, v) for t, n, v in notes])
ok = True
if sorted(chunks) != list(range(64, 4096, CHUNK_DATA)):
    print("FAIL: chunk addresses", sorted(chunks))
    ok = False
if got[64:] != image[64:]:
    bad = next(a for a in range(64, 4096) if got[a] != image[a])
    print(f"FAIL: backup differs at {bad}")
    ok = False
if acks[:1] != [0]:
    print("FAIL: commit ack", acks)
    ok = False
if acks[1:3] != [1, 2]:
    print("FAIL: error acks", acks)
    ok = False
if acks[3:4] != [3]:
    print("FAIL: busy ack", acks)
    ok = False
if not any(n == 60 and v == 100 for _, n, v in notes):
    print("FAIL: the restored take did not play")
    ok = False
if not any(n == 64 for _, n, v in notes):
    print("FAIL: the second note did not play")
    ok = False
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
