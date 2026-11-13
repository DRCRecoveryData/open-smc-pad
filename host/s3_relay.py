#!/usr/bin/env python3
"""Relay for the ESP32-S3 USB-host bridge (hw/jl-s3-bridge).

The S3 is the USB host to the SMC-PAD and talks to this PC over the UART port.  This
process owns that tty (flock) and offers:

  * a Unix socket speaking the jl-uboot-tool macport format, so
        jluboottool --device sock:/run/user/1000/smcpad-s3.sock ...
    runs the normal UBOOT flash/backup flow with the pad never unplugged;
  * a control socket for `status`, `mode midi|uboot` and `keyer ...`;
  * optional MIDI virtual ports, one per USB-MIDI cable, via mido/RTMidi.

Run `serve` for the daemon.  The short commands work without it (they take the tty for a
moment); with the daemon up they are forwarded through the control socket.
"""

import argparse
import fcntl
import glob
import json
import os
import socket
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools', 'jl-uboot-tool'))

from scsiio.relayport import (  # noqa: E402
    build_frame, parse_frame, MSG_HELLO, MSG_HELLO_ACK, MSG_STATE, MSG_MIDI_IN, MSG_MIDI_OUT,
    MSG_SCSI_REQ, MSG_SCSI_RSP, MSG_KEYER, MSG_KEYER_ST, MSG_MODE, MSG_PING, MSG_PONG, MSG_LOG,
)

MODE_NAMES = {0: 'midi', 1: 'uboot'}
PAD_NAMES = {0: 'unknown', 1: 'midi', 2: 'uboot'}
KEYER_NAMES = {0: 'idle', 1: 'armed', 2: 'keying', 3: 'ok', 4: 'timeout', 5: 'blocked'}
KEYER_OPS = {'disarm': 0, 'arm': 1, 'replug': 2, 'host': 3}
SCSI_STATUS = {1: 'command failed', 2: 'transport error', 3: 'pad not in UBOOT mode',
               4: 'unsupported', 5: 'timeout', 6: 'busy'}


def default_port():
    # CP210x and CH343 devkits both ship; CH343 names its by-id node after the chip,
    # not the bridge chip family.
    for pat in ('*CP210*', '*CH343*', '*USB_Single_Serial*'):
        by_id = sorted(glob.glob('/dev/serial/by-id/' + pat))
        if by_id:
            return by_id[0]
    for node in ('/dev/ttyUSB0', '/dev/ttyACM0'):
        if os.path.exists(node):
            return node
    return '/dev/ttyUSB0'


def default_sock(kind):
    return '/run/user/%d/smcpad-s3.%s' % (os.getuid(), kind)


class UsbMidiIn:
    """USB-MIDI 4-byte packets -> complete MIDI byte messages, per cable."""

    def __init__(self):
        self.sysex = None

    def feed(self, pkt):
        cin = pkt[0] & 0x0F
        data = bytes(pkt[1:])
        if cin == 0x4:
            if self.sysex is None:
                self.sysex = bytearray([0xF0])
            self.sysex += data
            return None
        if cin in (0x5, 0x6, 0x7):
            if self.sysex is None:
                return None
            self.sysex += data[:cin - 4]
            self.sysex.append(0xF7)
            msg = bytes(self.sysex)
            self.sysex = None
            return msg
        if 0x8 <= cin <= 0xE:
            n = 2 if cin in (0xC, 0xD) else 3
            return data[:n]
        return None


def message_to_packets(cable, raw):
    """Complete MIDI bytes -> USB-MIDI packets (list of 4-byte bytes)."""
    out = []
    if not raw:
        return out
    if raw[0] == 0xF0:
        body = raw[1:-1] if raw[-1] == 0xF7 else raw[1:]
        chunks = [body[i:i + 3] for i in range(0, len(body), 3)] or [b'']
        for i, chunk in enumerate(chunks):
            last = i == len(chunks) - 1
            cin = (4 + len(chunk)) if last else 4
            out.append(bytes([(cable << 4) | cin]) + chunk.ljust(3, b'\x00'))
        return out
    status = raw[0]
    cin = status >> 4 if status >= 0xF0 else {0x8: 0x8, 0x9: 0x9, 0xA: 0xA, 0xB: 0xB,
                                              0xC: 0xC, 0xD: 0xD, 0xE: 0xE}.get(status >> 4)
    if cin is None:
        return out
    out.append(bytes([(cable << 4) | cin]) + bytes(raw[1:]).ljust(3, b'\x00'))
    return out


class S3Link:
    def __init__(self, port, baud, on_frame):
        import serial
        self.ser = serial.Serial(port, baud, timeout=0.05, write_timeout=5)
        try:
            fcntl.flock(self.ser.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.ser.close()
            raise SystemExit('%s is already owned by another relay' % port)
        self.on_frame = on_frame
        self.seq = 0
        self.rx = bytearray()
        self.lock = threading.Lock()
        self.hello_event = threading.Event()
        self.closed = False
        threading.Thread(target=self._reader, daemon=True).start()
        self.hello()

    def _reader(self):
        while not self.closed:
            try:
                chunk = self.ser.read(max(1, min(256, self.ser.in_waiting or 1)))
            except Exception:
                break
            if not chunk:
                continue
            self.rx += chunk
            while b'\x00' in self.rx:
                end = self.rx.index(0)
                wire = bytes(self.rx[:end])
                del self.rx[:end + 1]
                if not wire:
                    continue
                frame = parse_frame(wire)
                if frame:
                    msg_type, _, payload = frame
                    if msg_type in (MSG_HELLO_ACK, MSG_STATE):
                        self.hello_event.set()
                    self.on_frame(msg_type, payload)

    def send(self, msg_type, payload=b''):
        frame = build_frame(msg_type, self.seq, payload)
        with self.lock:
            self.seq = (self.seq + 1) & 0xFF
            self.ser.write(frame)

    def hello(self, timeout=3):
        deadline = time.time() + timeout
        while time.time() < deadline and not self.hello_event.is_set():
            self.send(MSG_HELLO, struct.pack('<HHB', 1, 0, 0))
            self.hello_event.wait(0.4)
        if not self.hello_event.is_set():
            raise SystemExit('no HELLO_ACK from the bridge (wrong port or baud?)')

    def close(self):
        self.closed = True
        try:
            self.ser.close()
        except Exception:
            pass


class Relay:
    def __init__(self, link, midi_enabled, verbose):
        self.link = link
        self.verbose = verbose
        self.state = {}
        self.keyer = {}
        self.scsi_lock = threading.Lock()
        self.pending = None
        self.req = 0
        self.midi = None
        if midi_enabled:
            self.midi = MidiBridge(link)

    # ---- frames from the S3 ----

    def on_frame(self, msg_type, payload):
        if msg_type == MSG_STATE and len(payload) >= 10:
            mode, pad, vid, pid, rx, tx, keyer, _ = struct.unpack('<BBHHBBBB', payload)
            self.state = {'mode': MODE_NAMES.get(mode, mode), 'pad': PAD_NAMES.get(pad, pad),
                          'vid': vid, 'pid': pid, 'rx_cables': rx, 'tx_cables': tx,
                          'keyer': KEYER_NAMES.get(keyer, keyer)}
            if self.verbose:
                print('STATE %s' % json.dumps(self.state), file=sys.stderr, flush=True)
            if self.midi:
                self.midi.ensure_ports(rx, tx)
        elif msg_type == MSG_KEYER_ST and len(payload) >= 12:
            phase, keys, ms = struct.unpack('<III', payload)
            self.keyer = {'phase': KEYER_NAMES.get(phase, phase), 'keys': keys, 'ms': ms}
            print('KEYER %s' % json.dumps(self.keyer), file=sys.stderr, flush=True)
        elif msg_type == MSG_LOG:
            print('[s3] %s' % payload.decode('utf-8', 'replace'), file=sys.stderr, flush=True)
        elif msg_type == MSG_MIDI_IN and self.midi:
            self.midi.from_s3(payload)
        elif msg_type == MSG_SCSI_RSP and len(payload) >= 12:
            rid, status, sense_len, n, residue = struct.unpack_from('<IBBHI', payload)
            data = payload[12:12 + n]
            sense = payload[12 + n:12 + n + sense_len]
            if self.pending and self.pending[0] == rid:
                self.pending[1]._result = (status, residue, data, sense)
                self.pending[1].set()

    # ---- SCSI for the socket clients ----

    def scsi(self, cdb, direction, data, timeout_ms=30000):
        with self.scsi_lock:
            self.req = (self.req + 1) & 0xFFFFFFFF
            rid = self.req
            ev = threading.Event()
            ev._result = None
            self.pending = (rid, ev)
            hdr = struct.pack('<IBBHI16s', rid, direction, len(cdb), len(data), timeout_ms,
                              bytes(cdb).ljust(16, b'\xff')[:16])
            self.link.send(MSG_SCSI_REQ, hdr + data)
            if not ev.wait(timeout_ms / 1000.0 + 5):
                self.pending = None
                raise TimeoutError('no reply from the bridge')
            self.pending = None
            return ev._result

    def serve_scsi(self, path):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(path)
        os.chmod(path, 0o600)
        srv.listen(4)
        print('SCSI socket %s' % path, file=sys.stderr, flush=True)

        def handle(conn):
            with conn:
                while True:
                    head = recv_exact(conn, 10)
                    if head is None:
                        return
                    direction, cdb_len, length, timeout_ms = struct.unpack('<BBII', head)
                    body = recv_exact(conn, cdb_len + (length if direction == 1 else 0))
                    if body is None:
                        return
                    cdb = body[:cdb_len]
                    data_out = body[cdb_len:]
                    try:
                        status, residue, data, sense = self.scsi(cdb, direction, data_out, timeout_ms)
                    except Exception as exc:
                        print('scsi: %s' % exc, file=sys.stderr, flush=True)
                        conn.sendall(struct.pack('<BII', 1, 0, 0) + struct.pack('<I', 0))
                        continue
                    if status != 0:
                        conn.sendall(struct.pack('<BII', 1, residue, 0) +
                                     struct.pack('<I', len(sense)) + sense)
                    else:
                        conn.sendall(struct.pack('<BII', 0, residue, len(data)) + data +
                                     struct.pack('<I', 0))

        while True:
            conn, _ = srv.accept()
            threading.Thread(target=handle, args=(conn,), daemon=True).start()

    def serve_ctl(self, path):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(path)
        os.chmod(path, 0o600)
        srv.listen(4)
        print('control socket %s' % path, file=sys.stderr, flush=True)

        def handle(conn):
            with conn:
                line = b''
                while True:
                    b = conn.recv(1)
                    if not b:
                        return
                    if b != b'\n':
                        line += b
                        continue
                    reply = self.control(line.decode(), from_daemon=True)
                    conn.sendall((reply + '\n').encode())
                    line = b''

        while True:
            conn, _ = srv.accept()
            threading.Thread(target=handle, args=(conn,), daemon=True).start()

    def control(self, line, from_daemon=False):
        parts = line.strip().split()
        if not parts:
            return 'ERR empty'
        cmd = parts[0].upper()
        if cmd == 'STATUS':
            text = dict(self.state)
            text.update(self.keyer)
            return 'OK ' + json.dumps(text)
        if cmd == 'MODE' and len(parts) == 2 and parts[1] in ('midi', 'uboot'):
            self.link.send(MSG_MODE, bytes([0 if parts[1] == 'midi' else 1]))
            return 'OK mode %s (the bridge restarts)' % parts[1]
        if cmd == 'KEYER' and len(parts) >= 2:
            op = KEYER_OPS.get(parts[1].lower())
            if op is None:
                return 'ERR unknown keyer op'
            self.link.send(MSG_KEYER, struct.pack('<BBI', op, 0, 0))
            return 'OK keyer %s' % parts[1]
        return 'ERR unknown command'


class MidiBridge:
    """One virtual port per USB-MIDI cable, both directions."""

    def __init__(self, link):
        import mido
        self.mido = mido
        self.link = link
        self.inputs = {}
        self.outputs = {}
        self.vinputs = {}
        self.ensure_ports(1, 1)

    def ensure_ports(self, rx_cables, tx_cables):
        for cable in range(max(1, min(4, rx_cables))):
            if cable not in self.outputs:
                try:
                    self.outputs[cable] = self.mido.open_output(
                        'SMC-PAD (S3) from c%d' % cable, virtual=True)
                except Exception as exc:
                    print('midi: cannot open output c%d: %s' % (cable, exc),
                          file=sys.stderr, flush=True)
        for cable in range(max(1, min(4, tx_cables))):
            if cable not in self.vinputs:
                try:
                    self.vinputs[cable] = self.mido.open_input(
                        'SMC-PAD (S3) to c%d' % cable, virtual=True,
                        callback=lambda msg, c=cable: self.to_s3(c, msg))
                except Exception as exc:
                    print('midi: cannot open input c%d: %s' % (cable, exc),
                          file=sys.stderr, flush=True)
        for cable in range(4):
            self.inputs.setdefault(cable, UsbMidiIn())

    def from_s3(self, payload):
        for i in range(0, len(payload) & ~3, 4):
            pkt = payload[i:i + 4]
            raw = self.inputs[pkt[0] >> 4].feed(pkt)
            if not raw:
                continue
            port = self.outputs.get(pkt[0] >> 4)
            if not port:
                continue
            try:
                for msg in self.mido.parse_all(raw):
                    port.send(msg)
            except ValueError:
                pass

    def to_s3(self, cable, msg):
        data = b''.join(message_to_packets(cable, msg.bytes()))
        while data:
            chunk, data = data[:1200], data[1200:]
            self.link.send(MSG_MIDI_OUT, chunk)


def recv_exact(sock, n):
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def run_brief(args, line):
    ctl = args.ctl or default_sock('ctl')
    try:
        conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        conn.connect(ctl)
        conn.sendall((line + '\n').encode())
        reply = b''
        while not reply.endswith(b'\n'):
            chunk = conn.recv(256)
            if not chunk:
                break
            reply += chunk
        print(reply.decode().strip())
        return 0
    except OSError:
        pass

    state = {}

    def on_frame(msg_type, payload):
        if msg_type == MSG_STATE and len(payload) >= 10:
            mode, pad, vid, pid, rx, tx, keyer, _ = struct.unpack('<BBHHBBBB', payload)
            state.update(mode=MODE_NAMES.get(mode, mode), pad=PAD_NAMES.get(pad, pad),
                         vid=vid, pid=pid, rx_cables=rx, tx_cables=tx,
                         keyer=KEYER_NAMES.get(keyer, keyer))
        elif msg_type == MSG_LOG:
            print('[s3] %s' % payload.decode('utf-8', 'replace'), file=sys.stderr)

    link = S3Link(args.port, args.baud, on_frame)
    try:
        if line.upper() == 'STATUS':
            time.sleep(0.5)
            print(json.dumps(state))
        else:
            print(Relay(link, False, args.verbose).control(line))
    finally:
        link.close()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('-p', '--port', default=default_port())
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('--ctl', default=default_sock('ctl'))
    ap.add_argument('--scsi-socket', default=default_sock('sock'))
    ap.add_argument('--no-scsi-socket', action='store_true')
    ap.add_argument('--midi', action='store_true', help='expose MIDI virtual ports')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('cmd', choices=['serve', 'status', 'mode', 'keyer'])
    ap.add_argument('arg', nargs='?')
    args = ap.parse_args()

    if args.cmd == 'status':
        return run_brief(args, 'STATUS')
    if args.cmd == 'mode':
        if args.arg not in ('midi', 'uboot'):
            ap.error('mode needs midi or uboot')
        return run_brief(args, 'MODE ' + args.arg)
    if args.cmd == 'keyer':
        if args.arg not in KEYER_OPS:
            ap.error('keyer needs one of: %s' % ', '.join(KEYER_OPS))
        return run_brief(args, 'KEYER ' + args.arg)

    relay = None

    def on_frame(msg_type, payload):
        if relay is not None:
            relay.on_frame(msg_type, payload)

    link = S3Link(args.port, args.baud, on_frame)
    relay = Relay(link, args.midi, args.verbose)
    threads = []
    if not args.no_scsi_socket:
        threads.append(threading.Thread(target=relay.serve_scsi, args=(args.scsi_socket,),
                                        daemon=True))
    threads.append(threading.Thread(target=relay.serve_ctl, args=(args.ctl,), daemon=True))
    for t in threads:
        t.start()
    print('relay up: %s @ %d' % (args.port, args.baud), file=sys.stderr, flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        for path in (args.scsi_socket, args.ctl):
            try:
                os.unlink(path)
            except OSError:
                pass
        link.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
