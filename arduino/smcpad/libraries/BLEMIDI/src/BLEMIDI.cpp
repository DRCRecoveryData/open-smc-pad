#include "BLEMIDI.h"

BLEMIDI_ BLEMIDI;

/* bytes of a message by its status */
static int midi_length(uint8_t status)
{
    if (status < 0xF0) {
        return (status & 0xE0) == 0xC0 ? 2 : 3;     /* program change / channel pressure: 2 */
    }
    return status == 0xF2 ? 3 : (status == 0xF1 || status == 0xF3) ? 2 : 1;
}

/* USB-MIDI code index number of a message */
static uint8_t cin(uint8_t status)
{
    if (status < 0xF0) {
        return status >> 4;
    }
    if (status >= 0xF8) {
        return 0x0F;                                /* real time: single byte */
    }
    return midi_length(status) == 3 ? 0x03 : midi_length(status) == 2 ? 0x02 : 0x05;
}

/* bytes of a message by its code index number (0 = not one we send) */
static int cin_length(uint8_t c)
{
    switch (c & 0x0F) {
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: case 0x3:
        return 3;
    case 0xC: case 0xD: case 0x2:
        return 2;
    case 0x5: case 0xF:
        return 1;
    default:
        return 0;
    }
}

int BLEMIDI_::begin(const char *name)
{
    static bool added;

    if (!BLE.begin()) {
        return 0;
    }
    if (!added) {
        _io.setRawWriteCallback(onWrite, this);
        _service.addCharacteristic(_io);
        BLE.addService(_service);
        added = true;
    }
    BLE.setLocalName(name);
    BLE.setDeviceName(name);
    BLE.setAdvertisedService(_service);
    BLE.setConnectionInterval(6, 12);       /* 7.5-15 ms, as the BLE-MIDI spec asks */
    smcpad_yield_hook(yieldHook);
    BLE.advertise();
    return 1;
}

void BLEMIDI_::yieldHook()
{
    BLEMIDI.poll();
}

bool BLEMIDI_::connected()
{
    return BLE.connected() && _io.subscribed();
}

/* ------------------------------------------------------------------ outgoing */

/* The waiting packet to the stack. Waits only if the stack's queue has no room at all. */
bool BLEMIDI_::sendPacket()
{
    bool ok;

    _sending = true;                                /* writeValue may yield: no poll() inside */
    ok = _io.writeValue(_tx, _txLen);
    _sending = false;
    _txLen = 0;
    return ok;
}

void BLEMIDI_::poll()
{
    if (!_txLen || _sending) {
        return;
    }
    if (!_io.subscribed()) {
        _txLen = 0;                                 /* nobody listens any more */
    } else if (smcpad_ble_queued() == 0) {
        sendPacket();
    }
}

/* Packet: header (1, timestamp bits 12..7), then per message timestamp low (1, bits 6..0),
 * status, data. A low part smaller than the previous one tells the host the high part went up
 * by one: so a packet spans less than 128 ms. */
bool BLEMIDI_::send(uint8_t status, uint8_t data1, uint8_t data2)
{
    uint16_t ts = millis() & 0x1FFF;
    int n = midi_length(status);
    int room = smcpad_ble_mtu() - 3;

    if (!(status & 0x80) || status == 0xF0 || status == 0xF7) {
        return false;                               /* not a status, or SysEx */
    }
    if (!_io.subscribed() || _sending) {
        return false;
    }
    if (room > PACKET_MAX) {
        room = PACKET_MAX;
    }
    if (_txLen && (_txLen + 1 + n > room || ((ts - _txFirst) & 0x1FFF) >= 128)) {
        sendPacket();                               /* full (or old): out now, queue or not */
    }
    if (!_txLen) {
        _tx[_txLen++] = 0x80 | (ts >> 7);
        _txFirst = ts;
    }
    _tx[_txLen++] = 0x80 | (ts & 0x7F);
    _tx[_txLen++] = status;
    if (n > 1) {
        _tx[_txLen++] = data1 & 0x7F;
    }
    if (n > 2) {
        _tx[_txLen++] = data2 & 0x7F;
    }
    poll();                                         /* the radio is free: at once */
    return true;
}

/* A whole SysEx message to the stack, split into packets: header, timestamp, data; the F0
 * travels in the first packet, the F7 in the last. Blocking (a packet waits for queue room):
 * meant for user-asked dumps, not for the note path. */
bool BLEMIDI_::sendSysEx(const uint8_t *data, int len)
{
    uint16_t ts = millis() & 0x1FFF;
    int room = smcpad_ble_mtu() - 3;

    if (!_io.subscribed() || _sending || len < 2) {
        return false;
    }
    if (room > PACKET_MAX) {
        room = PACKET_MAX;
    }
    if (room < 3) {
        return false;
    }
    if (_txLen && !sendPacket()) {                  /* a note packet is waiting: let it go */
        return false;
    }
    for (int i = 0; i < len; ) {
        int n = len - i;

        if (n > room - 2) {
            n = room - 2;
        }
        _tx[0] = 0x80 | (ts >> 7);
        _tx[1] = 0x80 | (ts & 0x7F);
        memcpy(_tx + 2, data + i, n);
        _txLen = 2 + n;
        if (!sendPacket()) {
            return false;
        }
        i += n;
    }
    return true;
}

void BLEMIDI_::sendMIDI(midiEventPacket_t event)
{
    if (cin_length(event.header)) {
        send(event.byte1, event.byte2, event.byte3);
    }
}

size_t BLEMIDI_::write(const uint8_t *buffer, size_t size)
{
    for (size_t i = 0; i + 4 <= size; i += 4) {
        sendMIDI({buffer[i], buffer[i + 1], buffer[i + 2], buffer[i + 3]});
    }
    return size - size % 4;
}

/* ------------------------------------------------------------------ incoming (BT task) */

void BLEMIDI_::received(const uint8_t *msg, int length)
{
    uint8_t next = (_head + 1) % 32;

    if (next == _tail) {
        return;                                     /* full: drop */
    }
    _rx[_head] = { cin(msg[0]), msg[0], (uint8_t)(length > 1 ? msg[1] : 0), (uint8_t)(length > 2 ? msg[2] : 0) };
    _head = next;
}

/* ------------------------------------------------------------------ SysEx (BT task) */

/* A message starts: its bytes go into the slot the sketch is not reading */
void BLEMIDI_::sysexStart()
{
    uint8_t pending = (uint8_t)(_sxHead - _sxTail);   /* messages not read yet */

    _sxBuild = _sxHead % SYSEX_SLOTS;
    _sxFill = 0;
    _sxDrop = pending >= SYSEX_SLOTS;                 /* both slots taken: drop this one */
    _sxActive = true;
    sysexAppend(0xF0);
}

void BLEMIDI_::sysexAppend(uint8_t b)
{
    if (_sxFill < SYSEX_MAX) {
        _sx[_sxBuild][_sxFill++] = b;
    } else {
        _sxDrop = true;
    }
}

void BLEMIDI_::sysexFinish()
{
    if (!_sxDrop && _sxFill >= 2) {
        _sxLen[_sxBuild] = _sxFill;
        _sxHead++;
    }
    _sxActive = false;
}

/* The rest of the message from p[i]. Data bytes are 7-bit; a byte with bit 7 set is a
 * packet header (skipped, with its timestamp), the F7 ends the message, real time is
 * ignored and the assembly goes on. */
void BLEMIDI_::sysexFeed(const uint8_t *p, int i, int n)
{
    while (i < n) {
        uint8_t b = p[i];

        if (b < 0x80) {
            sysexAppend(b);
            i++;
        } else if (b == 0xF7) {
            sysexAppend(b);
            sysexFinish();
            return;
        } else if (b >= 0xF8) {
            i++;                                    /* real time */
        } else if (b < 0xC0) {
            i++;                                    /* header ... */
            if (i < n && (p[i] & 0x80)) {
                i++;                                /* ... and its timestamp */
            }
        } else {
            i++;                                    /* not a header: skip */
        }
    }
}

/* header, then [timestamp] status data... / [timestamp] data (running status) / data
 * (running status, same timestamp). An F0 starts a SysEx that the later writes continue. */
void BLEMIDI_::onWrite(const uint8_t *p, int n, void *arg)
{
    BLEMIDI_ *self = (BLEMIDI_ *)arg;
    uint8_t status = 0, msg[3];
    int i = 1, have = 0;

    if (n < 2 || (p[0] & 0xC0) != 0x80) {
        return;                                     /* not a BLE-MIDI header */
    }
    if (self->_sxActive) {                          /* the message goes on from here */
        self->sysexFeed(p, 0, n);
        return;
    }
    while (i < n) {
        uint8_t b = p[i++];

        if (b & 0x80) {                             /* a timestamp byte */
            if (i < n && (p[i] & 0x80)) {           /* ... followed by a status byte */
                uint8_t st = p[i++];

                if (st == 0xF0) {                   /* SysEx: this write goes on in it */
                    self->sysexStart();
                    self->sysexFeed(p, i, n);
                    return;
                }
                if (st >= 0xF8) {                   /* real time: one byte, status unchanged */
                    self->received(&st, 1);
                    continue;
                }
                status = st == 0xF7 ? 0 : st;       /* a stray F7: skip */
                have = 0;
                if (status && midi_length(status) == 1) {
                    self->received(&status, 1);
                    status = 0;
                }
            }
            continue;
        }
        if (!status) {
            continue;                               /* data with no status (or SysEx) */
        }
        if (!have) {
            msg[have++] = status;
        }
        msg[have++] = b;
        if (have == midi_length(status)) {
            self->received(msg, have);
            have = 0;
        }
    }
}

int BLEMIDI_::available()
{
    return (_head - _tail + 32) % 32;
}

midiEventPacket_t BLEMIDI_::read()
{
    midiEventPacket_t p = {0, 0, 0, 0};

    BLE.poll();
    if (_tail != _head) {
        p = _rx[_tail];
        _tail = (_tail + 1) % 32;
    }
    return p;
}

int BLEMIDI_::readSysEx(uint8_t *out, int max)
{
    BLE.poll();
    if (_sxHead == _sxTail) {
        return 0;
    }
    uint8_t slot = _sxTail % SYSEX_SLOTS;
    int n = _sxLen[slot];

    if (n > max) {
        n = max;                                    /* short buffer: the tail is lost */
    }
    memcpy(out, _sx[slot], n);
    _sxTail++;
    return n;
}
