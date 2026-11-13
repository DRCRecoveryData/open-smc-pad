/*
 * Compile-time settings for the Spectra mode.  The Spectra's settings live in DJ TechTools'
 * Utility (a proprietary app with no documented protocol), so they are constants here.
 */
#pragma once
#include <stdint.h>

// MIDI channels as numbered in the guide (1-based).  Triggers and colour notes use this
// channel, CCs/side buttons/bank notes/animations/ground LEDs the next, side-button CCs the
// one after (reserved: the Pocket has no side buttons).
#ifndef SPECTRA_BASE_CH
#define SPECTRA_BASE_CH 3
#endif

// Spectra triggers are not velocity sensitive; this is the guide's "Key MIDI Velocity".
#ifndef SPECTRA_KEY_VELOCITY
#define SPECTRA_KEY_VELOCITY 127
#endif

// Optional Momentary CC: CC 127 on press / 0 on release on the channel after the triggers.
#ifndef SPECTRA_MOMENTARY_CC
#define SPECTRA_MOMENTARY_CC 0
#endif

// Cosmetic extras from the Utility, on by default to match the full feature set.
#ifndef SPECTRA_SPARK
#define SPECTRA_SPARK 1
#endif
#ifndef SPECTRA_BANK_WAVE
#define SPECTRA_BANK_WAVE 1
#endif
#ifndef SPECTRA_SLEEP_MIN
#define SPECTRA_SLEEP_MIN 0            // 0 = never sleep
#endif

// Side buttons, standing in for the Spectra's 6 buttons (which have no LEDs):
//   0 factory defaults (Left2/Right2 change banks, the rest send notes),
//   1 all six send notes, 2 all six send CCs.
#ifndef SPECTRA_SIDE_MODE
#define SPECTRA_SIDE_MODE 0
#endif
// The Utility's "Bank Side Buttons": side notes/CCs change with the bank.
#ifndef SPECTRA_SIDE_BANK_BUTTONS
#define SPECTRA_SIDE_BANK_BUTTONS 1
#endif
// The guide documents only the pressed value (127) for a side-button CC; release is assumed.
#ifndef SPECTRA_SIDE_CC_RELEASE
#define SPECTRA_SIDE_CC_RELEASE 0
#endif

// Utility colours per virtual bank: active while a trigger is held, inactive while released.
// The Spectra's factory colours are device state; these are ours and MIDI colour control
// (velocity 1..120) overrides both.  The web UI rebuilds the sketch with these overridden.
#ifndef SPECTRA_ACTIVE_B0
#define SPECTRA_ACTIVE_B0 0xFFFFFFu
#endif
#ifndef SPECTRA_ACTIVE_B1
#define SPECTRA_ACTIVE_B1 0xFFFFFFu
#endif
#ifndef SPECTRA_ACTIVE_B2
#define SPECTRA_ACTIVE_B2 0xFFFFFFu
#endif
#ifndef SPECTRA_ACTIVE_B3
#define SPECTRA_ACTIVE_B3 0xFFFFFFu
#endif
#ifndef SPECTRA_INACTIVE_B0
#define SPECTRA_INACTIVE_B0 0x000000u
#endif
#ifndef SPECTRA_INACTIVE_B1
#define SPECTRA_INACTIVE_B1 0x000000u
#endif
#ifndef SPECTRA_INACTIVE_B2
#define SPECTRA_INACTIVE_B2 0x000000u
#endif
#ifndef SPECTRA_INACTIVE_B3
#define SPECTRA_INACTIVE_B3 0x000000u
#endif

static inline uint32_t spectraActiveColor(int bank, int pad) {
    (void)pad;
    switch (bank & 3) {
        case 0:  return SPECTRA_ACTIVE_B0;
        case 1:  return SPECTRA_ACTIVE_B1;
        case 2:  return SPECTRA_ACTIVE_B2;
        default: return SPECTRA_ACTIVE_B3;
    }
}

static inline uint32_t spectraInactiveColor(int bank, int pad) {
    (void)pad;
    switch (bank & 3) {
        case 0:  return SPECTRA_INACTIVE_B0;
        case 1:  return SPECTRA_INACTIVE_B1;
        case 2:  return SPECTRA_INACTIVE_B2;
        default: return SPECTRA_INACTIVE_B3;
    }
}
