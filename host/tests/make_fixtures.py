#!/usr/bin/env python3
"""Write synthetic app-area fixtures for the offline tests when the real build output is
absent (a fresh CI checkout has no work/, which is gitignored).  Existing files are never
touched, so a real MidiFighterSpectra build is always preferred.

    python3 host/tests/make_fixtures.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "webui"))
import device  # noqa: E402

PATHS = [os.path.join(device.ROOT, "work", "sketch", "MidiFighterSpectra",
                      "MidiFighterSpectra.bin"),
         os.path.join(device.ROOT, "work", "flash", "fw_area.bin")]


def make_area(size=0x2000):
    data = bytearray(size)
    for i in range(size):
        data[i] = (i * 37 + 11) & 0xFF
    data[0:4] = b"\0\0\0\0"
    struct.pack_into("<I", data, device.DIR_OFFSET, device.APP_ENTRY)
    struct.pack_into("<I", data, device.DIR_SIZE, size)
    struct.pack_into("<H", data, device.DIR_DATA_CRC,
                     device.crc16(bytes(data[0x20:size])))
    struct.pack_into("<H", data, 0, device.crc16(bytes(data[2:32])))
    assert device.validate_app_area(bytes(data), "x.bin") == (True, "")
    return bytes(data)


def main():
    area = make_area()
    for path in PATHS:
        if os.path.exists(path):
            print("kept", path)
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(area)
        print("wrote", path)


if __name__ == "__main__":
    main()
