/*
 * BLE-MIDI (Apple / MMA "MIDI over Bluetooth LE" 1.0) on ArduinoBLE, with the MIDIUSB API: code
 * written for MidiUSB works on BLEMIDI by changing the name.
 *
 *   service        03B80E5A-EDE8-4B33-A751-6CE34EC4C700
 *   MIDI I/O char  7772E5DB-3868-4112-A1A9-F2669D106BF3  read | write w/o response | notify
 *
 * Other BLE services can live alongside: add them to BLE before BLEMIDI.begin(). SysEx is
 * handled as whole messages apart from the 3-byte API: readSysEx() hands the sketch the
 * reassembled bytes, sendSysEx() fragments one into packets.
 *
 * Sending, for low latency: a message goes out at once when the radio has nothing waiting;
 * otherwise it joins the next packet (each message keeps its own timestamp, so the host keeps
 * the timing), which leaves as soon as the previous one has gone. A burst (a clock tick and
 * sixteen notes) is then one or two packets, not seventeen queued one behind the other. The
 * waiting packet also leaves from yield() (after each loop(), in delay()); begin() asks the
 * host for a 7.5-15 ms connection interval.
 */
#pragma once
#include <ArduinoBLE.h>

#ifndef MIDI_EVENT_PACKET_T_DEFINED         /* shared with MIDIUSB */
#define MIDI_EVENT_PACKET_T_DEFINED
typedef struct {
    uint8_t header;         /* cable number << 4 | code index number (usually status >> 4) */
    uint8_t byte1;
    uint8_t byte2;
    uint8_t byte3;
} midiEventPacket_t;
#endif

class BLEMIDI_ {
public:
    /* starts BLE (if needed) and advertises as a MIDI device; 1 when BLE is up */
    int begin(const char *name = "SMC-PAD Pocket");
    bool connected();                       /* a central listens (subscribed) */

    /* MIDIUSB API */
    midiEventPacket_t read();               /* header 0 when nothing is waiting */
    void sendMIDI(midiEventPacket_t event);
    size_t write(const uint8_t *buffer, size_t size);   /* whole 4-byte packets */
    void flush() { poll(); }                /* a waiting packet: out if the radio is free */
    int available();

    /* channel 0..15 */
    void noteOn(uint8_t channel, uint8_t note, uint8_t velocity) { send(0x90 | channel, note, velocity); }
    void noteOff(uint8_t channel, uint8_t note, uint8_t velocity = 0) { send(0x80 | channel, note, velocity); }
    void controlChange(uint8_t channel, uint8_t control, uint8_t value) { send(0xB0 | channel, control, value); }
    void programChange(uint8_t channel, uint8_t program) { send(0xC0 | channel, program, 0); }
    void pitchBend(uint8_t channel, int value)          /* -8192..8191 */
    {
        value += 8192;
        send(0xE0 | channel, value & 0x7F, (value >> 7) & 0x7F);
    }
    /* any message of 1..3 bytes (status first) */
    bool send(uint8_t status, uint8_t data1 = 0, uint8_t data2 = 0);

    /* SysEx: a whole message (F0..F7) from the host, and sending one (split into packets) */
    int readSysEx(uint8_t *out, int max);   /* bytes copied, 0 when nothing is waiting */
    bool sendSysEx(const uint8_t *data, int len);

    BLEService &service() { return _service; }

    /* sends the waiting packet if the radio has nothing queued (yield() calls it) */
    void poll();
    bool busy() { return _sending || _txLen; }

private:
    static const int PACKET_MAX = 64;       /* bytes; also bounded by the ATT MTU */
    static const int SYSEX_MAX = 160;       /* bytes of a whole SysEx message */
    static const int SYSEX_SLOTS = 2;       /* complete messages waiting for the sketch */

    static void onWrite(const uint8_t *data, int length, void *arg);
    static void yieldHook();
    void received(const uint8_t *msg, int length);
    bool sendPacket();
    void sysexStart();
    void sysexAppend(uint8_t b);
    void sysexFinish();
    void sysexFeed(const uint8_t *p, int i, int n);

    BLEService _service{"03B80E5A-EDE8-4B33-A751-6CE34EC4C700"};
    BLECharacteristic _io{"7772E5DB-3868-4112-A1A9-F2669D106BF3",
                          BLERead | BLEWriteWithoutResponse | BLENotify, PACKET_MAX};
    midiEventPacket_t _rx[32];
    volatile uint8_t _head = 0, _tail = 0;
    uint8_t _sx[SYSEX_SLOTS][SYSEX_MAX];    /* complete SysEx messages */
    volatile uint8_t _sxLen[SYSEX_SLOTS] = {0};
    volatile uint8_t _sxHead = 0, _sxTail = 0;
    uint8_t _sxBuild = 0, _sxFill = 0;      /* the message being assembled */
    bool _sxActive = false, _sxDrop = false;
    uint8_t _tx[PACKET_MAX];                /* the packet being filled: header, messages */
    uint8_t _txLen = 0;
    uint16_t _txFirst;                      /* its first message's 13-bit timestamp */
    bool _sending = false;
};

extern BLEMIDI_ BLEMIDI;
