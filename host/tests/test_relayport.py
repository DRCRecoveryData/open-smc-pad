import os
import pty
import select
import socket
import struct
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'jl-uboot-tool'))

from scsiio import SCSIDev  # noqa: E402
from scsiio import relayport as rp  # noqa: E402
from scsiio.common import SCSIException  # noqa: E402


class Target:
    """Minimal UBOOT device: INQUIRY, READ_FLASH, and a 'pad off' status."""

    def __init__(self, pad_present=True):
        self.pad_present = pad_present

    def scsi(self, cdb, data_out):
        if not self.pad_present:
            return 3, b'', b''
        op = cdb[0]
        if op == 0x12:
            resp = bytearray(36)
            resp[1] = 0x80
            resp[4] = 31
            resp[8:16] = b'JieLi   '
            resp[16:32] = b'UBOOT1.00'.ljust(16, b' ')
            resp[32:36] = b'0100'
            return 0, b'', bytes(resp)
        if op == 0xFD and cdb[1] == 0x05:
            addr = int.from_bytes(cdb[2:6], 'big')
            length = int.from_bytes(cdb[6:8], 'big')
            return 0, b'', bytes(((addr + i) & 0xFF) for i in range(length))
        return 1, b'\x70\x00\x00\x00\x00\x00\x00\x0a' + b'\x00' * 10, b''


class FakeSerial(threading.Thread):
    def __init__(self, fd, target):
        super().__init__(daemon=True)
        self.fd = fd
        self.target = target
        self.stop = False
        self.seq = 0

    def send(self, msg_type, payload=b''):
        os.write(self.fd, rp.build_frame(msg_type, self.seq, payload))
        self.seq = (self.seq + 1) & 0xFF

    def run(self):
        rx = bytearray()
        while not self.stop:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                return
            rx += chunk
            while b'\x00' in rx:
                end = rx.index(0)
                wire = bytes(rx[:end])
                del rx[:end + 1]
                if not wire:
                    continue
                frame = rp.parse_frame(wire)
                if frame:
                    self.handle(*frame)

    def handle(self, msg_type, _seq, payload):
        if msg_type == rp.MSG_HELLO:
            self.send(rp.MSG_HELLO_ACK, struct.pack('<HBBH', 1, 1, 1, 2048))
            self.send(rp.MSG_STATE, struct.pack('<BBHHBBBB', 1, 2, 0x4C4A, 0x2542, 0, 0, 0, 0))
        elif msg_type == rp.MSG_SCSI_REQ:
            rid, direction, cdb_len, length, _timeout, cdb = struct.unpack_from('<IBBHI16s', payload)
            data_out = payload[28:28 + length] if direction == 1 else b''
            status, sense, data_in = self.target.scsi(cdb[:cdb_len], data_out)
            rsp = struct.pack('<IBBHI', rid, status, len(sense), len(data_in), 0) + data_in + sense
            self.send(rp.MSG_SCSI_RSP, rsp)


class FakeSocket(threading.Thread):
    def __init__(self, path, target):
        super().__init__(daemon=True)
        self.path = path
        self.target = target
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(1)

    def run(self):
        conn, _ = self.srv.accept()
        with conn:
            while True:
                head = conn.recv(10)
                if len(head) < 10:
                    return
                direction, cdb_len, length, _timeout = struct.unpack('<BBII', head)
                body = conn.recv(cdb_len + (length if direction == 1 else 0))
                cdb = body[:cdb_len]
                status, sense, data_in = self.target.scsi(cdb, body[cdb_len:])
                if status:
                    conn.sendall(struct.pack('<BII', 1, 0, 0) + struct.pack('<I', len(sense)) + sense)
                else:
                    conn.sendall(struct.pack('<BII', 0, 0, len(data_in)) + data_in +
                                 struct.pack('<I', 0))


INQUIRY = b'\x12\x00\x00\x00\x24\x00'
READ_FLASH = struct.pack('>HIH', 0xFD05, 0x100, 512).ljust(16, b'\xff')


class SerialBackendTest(unittest.TestCase):
    def test_inquiry_and_read(self):
        master, slave = pty.openpty()
        fake = FakeSerial(master, Target())
        fake.start()
        try:
            dev = SCSIDev('serial:%s@921600' % os.ttyname(slave))
            buf = bytearray(36)
            self.assertEqual(dev.execute(INQUIRY, None, buf), 0)
            self.assertEqual(bytes(buf[8:16]).strip(), b'JieLi')
            self.assertEqual(bytes(buf[16:32]).strip(), b'UBOOT1.00')
            self.assertEqual(bytes(buf[32:36]), b'0100')

            data = bytearray(512)
            dev.execute(READ_FLASH, None, data)
            self.assertEqual(bytes(data[:8]), bytes((0x100 + i) & 0xFF for i in range(8)))
            self.assertEqual(bytes(data[-8:]), bytes((0x100 + 504 + i) & 0xFF for i in range(8)))
            dev.close()
        finally:
            fake.stop = True
            os.close(master)
            os.close(slave)

    def test_pad_not_in_uboot(self):
        master, slave = pty.openpty()
        fake = FakeSerial(master, Target(pad_present=False))
        fake.start()
        try:
            dev = SCSIDev('serial:%s@921600' % os.ttyname(slave))
            with self.assertRaises(SCSIException) as ctx:
                dev.execute(INQUIRY, None, bytearray(36))
            self.assertIn('pad not in UBOOT', str(ctx.exception))
            dev.close()
        finally:
            fake.stop = True
            os.close(master)
            os.close(slave)


class SocketBackendTest(unittest.TestCase):
    def test_inquiry_over_socket(self):
        path = '/tmp/smcpad-s3-test-%d.sock' % os.getpid()
        fake = FakeSocket(path, Target())
        fake.start()
        try:
            dev = SCSIDev('sock:' + path)
            buf = bytearray(36)
            dev.execute(INQUIRY, None, buf)
            self.assertEqual(bytes(buf[16:32]).strip(), b'UBOOT1.00')
            dev.close()
        finally:
            fake.srv.close()
            try:
                os.unlink(path)
            except OSError:
                pass


if __name__ == '__main__':
    unittest.main()
