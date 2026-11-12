#!/usr/bin/env python3
"""Back up or restore a PocketDemo pad over USB-MIDI or BLE-MIDI.

The pad answers F0 7D 53 4D 50 <body> F7 with the body 7-bit packed (LSB first):
01 dumps EEPROM 64..4095 in 126-byte 0x41 chunks, 02/03 stage and commit a restore, 0x42
is an ack (0 ok, 1 bad image/checksum, 2 bad chunk, 3 busy). Bytes 0..63 (the stock
firmware's settings) are neither read nor written.

Usage:
  python3 pocket_backup.py backup OUT.bin [--port NAME]
  python3 pocket_backup.py restore IN.bin [--port NAME]

Needs mido: uv run --with mido --with python-rtmidi host/pocket_backup.py ...
"""
import argparse
import sys
import time

import mido

HDR = (0x7D, 0x53, 0x4D, 0x50)                   # non-commercial id, "SMP"
DUMP, RESTORE, COMMIT, ABORT = 0x01, 0x02, 0x03, 0x04
CHUNK_OUT, ACK = 0x41, 0x42
BASE, SIZE = 64, 4096                             # the image the pad keeps
CHUNK_DATA = 126                                  # 148 packed bytes: a message stays < 160
ACK_TEXT = {0: "ok", 1: "bad image or checksum", 2: "bad chunk", 3: "busy: stop recording"}


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


def find_port(name=None):
    ports = mido.get_input_names()
    for port in ports:
        if name and name.lower() in port.lower():
            return port
        if not name and "SMC-PAD" in port:
            return port
    want = f"matching {name!r}" if name else "with 'SMC-PAD' in the name"
    raise SystemExit(f"no MIDI input port {want}; have: {ports}")


class Pad:
    def __init__(self, port):
        self.inp = mido.open_input(port)
        self.out = mido.open_output(port)

    def send(self, body, delay=0.0):
        self.out.send(mido.Message("sysex", data=list(HDR) + list(pack7(body))))
        if delay:
            time.sleep(delay)

    def recv(self, timeout=1.0):
        end = time.time() + timeout
        while time.time() < end:
            for m in self.inp.iter_pending():
                if m.type == "sysex" and tuple(m.data[:4]) == HDR:
                    return unpack7(m.data[4:])
            time.sleep(0.002)
        return None

    def drain(self, seconds=0.2):
        end = time.time() + seconds
        while time.time() < end:
            self.recv(0.02)

    def ack(self, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            body = self.recv(max(0.05, end - time.time()))
            if body and body[0] == ACK and len(body) >= 2:
                return body[1]
        return None


def backup(pad, path):
    pad.drain()
    pad.send((DUMP,))
    image = bytearray(SIZE)
    got = bytearray(SIZE)
    end = time.time() + 15
    while time.time() < end and not all(got[BASE:]):
        body = pad.recv(0.5)
        if not body or body[0] != CHUNK_OUT or len(body) < 4:
            continue
        addr = (body[1] << 7) | body[2]
        data = body[3:]
        if addr < BASE or addr + len(data) > SIZE:
            continue
        image[addr:addr + len(data)] = data
        got[addr:addr + len(data)] = b"\x01" * len(data)
    for addr in range(BASE, SIZE):
        if not got[addr]:
            raise SystemExit(f"backup incomplete: no data at {addr}")
    with open(path, "wb") as f:
        f.write(image)
    print(f"saved {path}: {SIZE} bytes ({BASE}..{SIZE - 1} from the pad)")


def restore(pad, path):
    with open(path, "rb") as f:
        image = f.read()
    if len(image) < SIZE:
        raise SystemExit(f"{path} is {len(image)} bytes, expected {SIZE}")
    pad.drain()
    total = sum(image[BASE:SIZE]) & 0x7F
    for addr in range(BASE, SIZE, CHUNK_DATA):
        data = image[addr:addr + CHUNK_DATA]
        pad.send((RESTORE, (addr >> 7) & 0x7F, addr & 0x7F) + tuple(data), delay=0.005)
    pad.send((COMMIT, total))
    code = pad.ack()
    if code is None:
        raise SystemExit("no ack from the pad")
    if code != 0:
        raise SystemExit(f"the pad refused the image: {ACK_TEXT.get(code, code)}")
    print(f"restored {path}: bytes {BASE}..{SIZE - 1} written and applied")


def main():
    ap = argparse.ArgumentParser(description="Back up or restore a PocketDemo pad.")
    ap.add_argument("action", choices=["backup", "restore"])
    ap.add_argument("file")
    ap.add_argument("--port", help="MIDI port name (default: the one with 'SMC-PAD')")
    args = ap.parse_args()

    port = find_port(args.port)
    pad = Pad(port)
    print(f"port: {port}")
    if args.action == "backup":
        backup(pad, args.file)
    else:
        restore(pad, args.file)


if __name__ == "__main__":
    sys.exit(main())
