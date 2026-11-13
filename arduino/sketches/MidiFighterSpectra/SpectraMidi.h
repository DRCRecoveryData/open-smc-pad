// USB-MIDI out/in for the Spectra mode (the Spectra has no BLE).
#pragma once
#include <MIDIUSB.h>

static inline uint8_t spectraStatus(uint8_t type, int channel) {
    return type | (uint8_t)((channel - 1) & 0x0F);
}

static inline void spectraSend(uint8_t status, uint8_t d1, uint8_t d2) {
    MidiUSB.sendMIDI({(uint8_t)(status >> 4), status, d1, d2});
    MidiUSB.flush();
}

static inline void spectraNoteOn(int channel, uint8_t note, uint8_t velocity) {
    spectraSend(spectraStatus(0x90, channel), note, velocity);
}

static inline void spectraNoteOff(int channel, uint8_t note) {
    spectraSend(spectraStatus(0x80, channel), note, 0);
}

static inline void spectraCc(int channel, uint8_t control, uint8_t value) {
    spectraSend(spectraStatus(0xB0, channel), control, value);
}
