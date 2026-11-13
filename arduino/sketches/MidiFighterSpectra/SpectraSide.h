/*
 * The Spectra's 6 side buttons and 4 ground LEDs on the Pocket's buttons.
 *
 * The Spectra's side buttons have no LEDs; its ground LEDs are four dimmable white lights
 * at the base.  The Pocket has 10 buttons, each with a binary LED: six act as the side
 * buttons (LEDs stay off, as on the Spectra) and four LEDs stand in for the ground LEDs.
 */
#pragma once
#include <SMCPad.h>
#include "SpectraConfig.h"
#include "SpectraMap.h"

enum {
    SPECTRA_SIDE_NOTE = 0,
    SPECTRA_SIDE_CC,
    SPECTRA_SIDE_PREV_BANK,
    SPECTRA_SIDE_NEXT_BANK,
    SPECTRA_SIDE_BANK_1,
    SPECTRA_SIDE_BANK_2,
    SPECTRA_SIDE_BANK_3,
    SPECTRA_SIDE_BANK_4,
};

// Slots match the guide: Left Side 1..3 then Right Side 1..3, top to bottom.
static const uint8_t SPECTRA_SIDE_BUTTON[6] = {
    SMCPadClass::PAD_BANK, SMCPadClass::LEFT, SMCPadClass::KNOB_BANK,
    SMCPadClass::PLAY, SMCPadClass::RIGHT, SMCPadClass::PAUSE,   // PAUSE is the panel's STOP
};

#if SPECTRA_SIDE_MODE == 1
static const uint8_t SPECTRA_SIDE_ACTION[6] = {
    SPECTRA_SIDE_NOTE, SPECTRA_SIDE_NOTE, SPECTRA_SIDE_NOTE,
    SPECTRA_SIDE_NOTE, SPECTRA_SIDE_NOTE, SPECTRA_SIDE_NOTE,
};
#elif SPECTRA_SIDE_MODE == 2
static const uint8_t SPECTRA_SIDE_ACTION[6] = {
    SPECTRA_SIDE_CC, SPECTRA_SIDE_CC, SPECTRA_SIDE_CC,
    SPECTRA_SIDE_CC, SPECTRA_SIDE_CC, SPECTRA_SIDE_CC,
};
#else
// Factory defaults: the middle left/right buttons change banks, the others send notes.
static const uint8_t SPECTRA_SIDE_ACTION[6] = {
    SPECTRA_SIDE_NOTE, SPECTRA_SIDE_PREV_BANK, SPECTRA_SIDE_NOTE,
    SPECTRA_SIDE_NOTE, SPECTRA_SIDE_NEXT_BANK, SPECTRA_SIDE_NOTE,
};
#endif

// Ground LED 1..4 (ch4 notes 16..19) on the four button LEDs the side buttons do not use.
static const uint8_t SPECTRA_GROUND_BUTTON[4] = {
    SMCPadClass::BLUETOOTH, SMCPadClass::REC, SMCPadClass::SHIFT, SMCPadClass::NOTE_REPEAT,
};

// Appendix 1: Left1 22, Left2 21, Left3 20, Right1 25, Right2 24, Right3 23.
static const uint8_t SPECTRA_SIDE_OFFSET[6] = {2, 1, 0, 5, 4, 3};

static inline int spectraSideNumber(int slot, int bank) {
#if SPECTRA_SIDE_BANK_BUTTONS
    return SPECTRA_SIDE_BASE[bank & 3] + SPECTRA_SIDE_OFFSET[slot];
#else
    (void)bank;
    return SPECTRA_SIDE_BASE[0] + SPECTRA_SIDE_OFFSET[slot];
#endif
}
