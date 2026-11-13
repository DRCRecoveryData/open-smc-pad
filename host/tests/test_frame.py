import os
import random
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'jl-uboot-tool'))

from scsiio import relayport as rp  # noqa: E402


def feed_byte_by_byte(wire):
    """Split a wire stream into frames the way the bridge's FrameReceiver does."""
    frames = []
    acc = bytearray()
    for b in wire:
        if b == 0:
            if acc:
                frame = rp.parse_frame(bytes(acc))
                if frame:
                    frames.append(frame)
                acc = bytearray()
        else:
            acc.append(b)
    return frames


class FrameTest(unittest.TestCase):
    def test_round_trip(self):
        payload = bytes(range(32)) * 3
        frame = rp.build_frame(0x30, 7, payload)
        self.assertEqual(frame[0], 0)
        self.assertEqual(frame[-1], 0)
        frames = feed_byte_by_byte(frame)
        self.assertEqual(frames, [(0x30, 7, payload)])

    def test_zero_bytes_survive(self):
        payload = bytes([0, 1, 0, 0, 2, 0, 0xFF, 0])
        frames = feed_byte_by_byte(rp.build_frame(0x31, 0, payload))
        self.assertEqual(frames, [(0x31, 0, payload)])

    def test_max_payload(self):
        payload = bytes(random.getrandbits(8) for _ in range(2048))
        frames = feed_byte_by_byte(rp.build_frame(0x20, 255, payload))
        self.assertEqual(frames, [(0x20, 255, payload)])

    def test_crc_rejects_corruption(self):
        wire = bytearray(rp.build_frame(0x10, 1, b'state'))
        wire[3] ^= 0x01
        self.assertEqual(feed_byte_by_byte(bytes(wire)), [])

    def test_garbage_resync(self):
        stream = b'\x01\x00garbage' + rp.build_frame(0x50, 9, b'ping') + b'\x00'
        frames = feed_byte_by_byte(stream)
        self.assertIn((0x50, 9, b'ping'), frames)

    def test_decoded_length_and_crc(self):
        data = b'\x00\x00\x00'
        enc = rp.cobs_encode(data)
        self.assertNotIn(0, enc)
        self.assertEqual(rp.cobs_decode(enc), data)
        self.assertEqual(rp.crc16(b'123456789'), 0x29B1)   # CCITT-FALSE


if __name__ == '__main__':
    unittest.main()
