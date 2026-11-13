/*
 * Note maps from the Spectra guide, Appendix 1 and the virtual-bank section.
 *
 * Pads 0..15 are bottom-left, row by row upwards, exactly like the Spectra's triggers
 * 13,14,15,16 / 9..12 / 5..8 / 1..4 (Fig 1), so the physical layout needs no remap.
 */
#pragma once
#include <stdint.h>

// Arcade trigger note for bank 1..4, first trigger: 36, 52, 68, 84 (note = base + pad).
static const uint8_t SPECTRA_ARCADE_BASE[4] = {36, 52, 68, 84};

// Bank-selection notes on the channel after the triggers (guide: C-1, C#-1, D-1, D#-1).
static const uint8_t SPECTRA_BANK_NOTE[4] = {0, 1, 2, 3};

// Side-button notes per bank (Left 3,2,1 / Right 3,2,1) and the CCs with the same numbers on
// the channel after that; the Pocket has neither the buttons nor the CC channel counterpart.
static const uint8_t SPECTRA_SIDE_BASE[4] = {20, 26, 32, 38};

// Ground LEDs: ch4 notes 16..19 (E0, F0, F#0, G0); the Pocket has no ground LEDs.
#define SPECTRA_GROUND_FIRST 16
#define SPECTRA_GROUND_LAST 19

static inline int spectraPadNote(int pad, int bank) {
    return SPECTRA_ARCADE_BASE[bank & 3] + pad;
}

// Pad for a note in this bank's arcade range, else -1.
static inline int spectraNotePad(int note, int bank) {
    int pad = note - SPECTRA_ARCADE_BASE[bank & 3];
    return (pad >= 0 && pad < 16) ? pad : -1;
}

static inline int spectraBankNote(int bank) {
    return SPECTRA_BANK_NOTE[bank & 3];
}

// Notes a configured side button sends in this bank (the Pocket sends none, but must not
// mistake them for animations).
static inline bool spectraIsSideNote(int note, int bank) {
    int base = SPECTRA_SIDE_BASE[bank & 3];
    return note >= base && note <= base + 5;
}
