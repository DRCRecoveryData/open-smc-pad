// Midi Fighter Spectra mode for the SMC-PAD Pocket: a clean-room implementation of the
// official Spectra MIDI protocol (User Guide Ver 1.01), not DJ TechTools' firmware.
//
//   triggers     Note On/Off on channel 3, fixed velocity 127, 16 notes per virtual bank
//                (36..51, 52..67, 68..83, 84..99); optional Momentary CC on channel 4
//   banks        switched by incoming channel-4 Note On 0..3 (C-1..D#-1, "Advanced Bank
//                Control"); the previous bank's Note Off is always followed by the new One
//   ring colours channel-3 Note On with the trigger's own pitch; velocity 1..120 selects one
//                of 10 colours (dim/bright), 0 returns to the Utility colours, 121..127
//                force the Utility active colour
//   animations   channel-4 Note On with the trigger's pitch: 18..33 brightness, 34..41 gate,
//                42..49 pulse, timed by MIDI clock or 120 BPM
//   ground LEDs  channel-4 notes 16..19 (accepted; the Pocket has no ground LEDs)
//
// The Pocket has no side buttons, no ground LEDs and no Utility app, so bank selection comes
// from the host (the documented bank notes) and settings are compile-time.  The pads are
// velocity sensitive but the Spectra's triggers are not: the key velocity is fixed.
#include <SMCPad.h>
#include "SpectraConfig.h"
#include "SpectraMap.h"
#include "SpectraPalette.h"
#include "SpectraMidi.h"
#include "SpectraLeds.h"

static uint8_t spectraBank;
static int8_t spectraSentNote[16];
static int8_t spectraSideSent[6] = {-1, -1, -1, -1, -1, -1};   // number held per side button

static void spectraSetBank(int bank, bool echo) {
    if (bank == spectraBank) return;
    // The guide is silent about a bank change while triggers are held; releasing them with
    // their old-bank notes keeps the host free of stuck notes.
    for (int pad = 0; pad < 16; pad++) {
        if (spectraSentNote[pad] < 0) continue;
        spectraNoteOff(SPECTRA_BASE_CH, spectraSentNote[pad]);
#if SPECTRA_MOMENTARY_CC
        spectraCc(SPECTRA_BASE_CH + 1, spectraSentNote[pad], 0);
#endif
        spectraSentNote[pad] = -1;
    }
    int old = spectraBank;
    spectraBank = bank;
    if (echo) {
        spectraNoteOff(SPECTRA_BASE_CH + 1, spectraBankNote(old));
        spectraNoteOn(SPECTRA_BASE_CH + 1, spectraBankNote(bank), 127);
    }
    spectraBeginWave();
    spectraTouchInput();
}

// ---------------------------------------------------------------- side buttons

static void spectraSidePressed(int button) {
    spectraTouchInput();
    for (int i = 0; i < 6; i++) {
        if (SPECTRA_SIDE_BUTTON[i] != button) continue;
        uint8_t action = SPECTRA_SIDE_ACTION[i];
        if (action == SPECTRA_SIDE_PREV_BANK) {
            spectraSetBank((spectraBank + 3) & 3, true);        // bank 1 wraps to 4
        } else if (action == SPECTRA_SIDE_NEXT_BANK) {
            spectraSetBank((spectraBank + 1) & 3, true);        // bank 4 wraps to 1
        } else if (action >= SPECTRA_SIDE_BANK_1) {
            spectraSetBank(action - SPECTRA_SIDE_BANK_1, true);
        } else {
            int number = spectraSideNumber(i, spectraBank);
            spectraSideSent[i] = number;
            if (action == SPECTRA_SIDE_CC)
                spectraCc(SPECTRA_BASE_CH + 2, number, 127);
            else
                spectraNoteOn(SPECTRA_BASE_CH + 1, number, SPECTRA_KEY_VELOCITY);
        }
        return;
    }
}

static void spectraSideReleased(int button) {
    spectraTouchInput();
    for (int i = 0; i < 6; i++) {
        if (SPECTRA_SIDE_BUTTON[i] != button) continue;
        int number = spectraSideSent[i];
        if (number < 0) return;                     // a bank button sent nothing
        spectraSideSent[i] = -1;
        if (SPECTRA_SIDE_ACTION[i] == SPECTRA_SIDE_CC)
            spectraCc(SPECTRA_BASE_CH + 2, number, SPECTRA_SIDE_CC_RELEASE);
        else
            spectraNoteOff(SPECTRA_BASE_CH + 1, number);
        return;
    }
}

// ---------------------------------------------------------------- triggers

static void spectraPadPressed(int pad, int) {
    spectraTouchInput();
    spectraHeld[pad] = true;
    int note = spectraPadNote(pad, spectraBank);
    spectraSentNote[pad] = note;
    spectraNoteOn(SPECTRA_BASE_CH, note, SPECTRA_KEY_VELOCITY);
#if SPECTRA_MOMENTARY_CC
    spectraCc(SPECTRA_BASE_CH + 1, note, 127);
#endif
    spectraBeginSpark(pad);
}

static void spectraPadReleased(int pad) {
    spectraTouchInput();
    spectraHeld[pad] = false;
    int note = spectraSentNote[pad];
    if (note < 0) return;
    spectraNoteOff(SPECTRA_BASE_CH, note);
#if SPECTRA_MOMENTARY_CC
    spectraCc(SPECTRA_BASE_CH + 1, note, 0);
#endif
    spectraSentNote[pad] = -1;
}

// ---------------------------------------------------------------- MIDI input

static void spectraMidiIn() {
    for (int i = 0; i < 32; i++) {
        midiEventPacket_t p = MidiUSB.read();
        if (p.header == 0) return;
        uint8_t status = p.byte1;
        if (status >= 0xF0) {
            if (status == 0xF8) spectraClockPulse();
            else if (status == 0xFA) spectraClockReset();
            continue;
        }
        if ((status & 0xF0) != 0x90) continue;      // Note On only (velocity 0 counts)
        int channel = (status & 0x0F) + 1;
        uint8_t note = p.byte2 & 0x7F;
        uint8_t velocity = p.byte3 & 0x7F;
        if (channel == SPECTRA_BASE_CH) {
            int pad = spectraNotePad(note, spectraBank);
            if (pad >= 0) spectraApplyColor(pad, velocity);
        } else if (channel == SPECTRA_BASE_CH + 1) {
            if (note <= 3) {
                if (velocity > 0) spectraSetBank(note, true);
            } else if (note >= SPECTRA_GROUND_FIRST && note <= SPECTRA_GROUND_LAST) {
                spectraApplyGround(note - SPECTRA_GROUND_FIRST, velocity);
            } else if (spectraIsSideNote(note, spectraBank)) {
                // side buttons: the Pocket has none
            } else {
                int pad = spectraNotePad(note, spectraBank);
                if (pad >= 0) spectraApplyAnimation(pad, velocity);
            }
        }
    }
}

// ---------------------------------------------------------------- main

void setup() {
    for (int pad = 0; pad < 16; pad++) spectraSentNote[pad] = -1;
    spectraLedsBegin();
    SMCPad.onPadPress(spectraPadPressed);
    SMCPad.onPadRelease(spectraPadReleased);
    SMCPad.onButtonPress(spectraSidePressed);
    SMCPad.onButtonRelease(spectraSideReleased);
    spectraLedsShow(spectraBank, true);
}

void loop() {
    SMCPad.update();
    spectraMidiIn();
    spectraClockAdvance();
    spectraLedsShow(spectraBank, false);
    spectraGroundLedsShow();
}
