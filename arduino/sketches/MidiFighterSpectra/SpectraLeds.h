/*
 * Ring colours and animations (guide: "Setting Arcade Trigger Color" / "... Animation State").
 *
 * One MIDI colour and one animation per pad, shared by all four banks, as in the guide: the
 * bank only changes note numbers and the Utility colours.  Animations share one beat phase,
 * driven by MIDI clock when one is running and by a 0.5 s beat (120 BPM) otherwise.
 */
#pragma once
#include <SMCPad.h>
#include "SpectraConfig.h"
#include "SpectraMap.h"
#include "SpectraPalette.h"
#include "SpectraSide.h"

enum {
    SPECTRA_ANIM_NONE = 0,
    SPECTRA_ANIM_BRIGHT,
    SPECTRA_ANIM_GATE,
    SPECTRA_ANIM_PULSE,
};

struct SpectraPadState {
    uint32_t color;         // MIDI colour, valid when override
    bool override;
    bool forceActive;       // velocity 121..127: Utility active colour, pressed or not
    uint8_t anim;
    uint8_t level;          // brightness 0..15, or rate index 0..7
};

static SpectraPadState spectraPad[16];
static bool spectraHeld[16];

// Ground LEDs: the Pocket's LEDs are binary, so brightness collapses to on/off and gate/pulse
// to blinking; 127 is the guide's plain "turn on".
struct SpectraGroundState {
    uint8_t anim;
    uint8_t level;
    bool steady;
};
static SpectraGroundState spectraGround[4];

// ---------------------------------------------------------------- clock / phase

static uint32_t spectraLastClockUs;
static uint32_t spectraClockIntervalUs = 20833;     // 120 BPM (24 ppqn), liveness only
static bool spectraClockStarted;
static uint32_t spectraLastTickUs;
static uint32_t spectraBeatQ16;                     // 0..65535 = one beat
static uint32_t spectraBeatRemainderUs;
static uint32_t spectraLastInputUs;

// gate: 16, 8, 4, 2, 1, 1/2, 1/4, 1/8 cycles per beat; pulse: 32 .. 1/8 (Q8).
static const uint16_t spectraGateRateQ8[8] = {4096, 2048, 1024, 512, 256, 128, 64, 32};
static const uint16_t spectraPulseRateQ8[8] = {8192, 4096, 2048, 1024, 512, 256, 128, 32};

// (1 - cos(2*pi*i/32)) / 2 * 255: a raised-cosine pulse.
static const uint8_t spectraPulseGain[32] = {
    0, 2, 10, 21, 37, 57, 79, 103, 128, 152, 176, 198, 218, 234, 245, 253,
    255, 253, 245, 234, 218, 198, 176, 152, 128, 103, 79, 57, 37, 21, 10, 2,
};

// brightness level 0..15 (0 = off, 15 = 100%)
static const uint8_t spectraLevelGain[16] = {
    0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255,
};

static void spectraTouchInput() { spectraLastInputUs = micros(); }

static void spectraClockPulse() {
    uint32_t now = micros();
    uint32_t dt = now - spectraLastClockUs;
    if (spectraClockStarted && dt < 200000)
        spectraClockIntervalUs = (spectraClockIntervalUs * 3 + dt) / 4;
    spectraLastClockUs = now;
    spectraClockStarted = true;
    spectraTouchInput();
}

static void spectraClockReset() { spectraBeatQ16 = 0; }

static bool spectraClockAlive() {
    return spectraClockStarted && micros() - spectraLastClockUs < 500000;
}

static void spectraClockAdvance() {
    uint32_t now = micros();
    uint32_t dt = now - spectraLastTickUs;
    spectraLastTickUs = now;
    if (dt > 250000) dt = 250000;                   // a stalled loop must not jump the phase
    uint32_t beatUs = spectraClockAlive() ? 24 * spectraClockIntervalUs : 500000UL;
    // Not wrapped per beat: rates slower than one cycle per beat need the phase to carry on
    // across beat boundaries (wrapping here would restart every slow animation each beat).
    uint64_t acc = (uint64_t)dt * 65536 + spectraBeatRemainderUs;
    spectraBeatQ16 += (uint32_t)(acc / beatUs);
    spectraBeatRemainderUs = (uint32_t)(acc % beatUs);
}

// ---------------------------------------------------------------- MIDI colour/animation

static void spectraApplyColor(int pad, uint8_t velocity) {
    SpectraPadState &s = spectraPad[pad];
    if (velocity == 0) {                            // MIDI colour control off
        s.override = false;
        s.forceActive = false;
        return;
    }
    if (velocity >= 121) {                          // force the Utility active colour
        s.forceActive = true;
        return;
    }
    s.color = SPECTRA_PALETTE[spectraColorIndex(velocity)];
    s.override = true;
    s.forceActive = false;
}

static void spectraApplyAnimation(int pad, uint8_t velocity) {
    SpectraPadState &s = spectraPad[pad];
    if (velocity < 18 || velocity > 49) {
        s.anim = SPECTRA_ANIM_NONE;
    } else if (velocity <= 33) {
        s.anim = SPECTRA_ANIM_BRIGHT;
        s.level = velocity - 18;                    // 0..15
    } else if (velocity <= 41) {
        s.anim = SPECTRA_ANIM_GATE;
        s.level = velocity - 34;                    // 0..7
    } else {
        s.anim = SPECTRA_ANIM_PULSE;
        s.level = velocity - 42;                    // 0..7
    }
}

// Position in the current animation cycle, Q16 (0..65535 = one cycle).
static inline uint32_t spectraPhase(uint16_t rateQ8) {
    return (uint32_t)(((uint64_t)spectraBeatQ16 * rateQ8) >> 8) & 0xFFFF;
}

static void spectraApplyGround(int led, uint8_t velocity) {
    SpectraGroundState &s = spectraGround[led];
    s.steady = false;
    if (velocity == 127) {                          // "simply turn the LED on"
        s.anim = SPECTRA_ANIM_NONE;
        s.level = 0;
        s.steady = true;
    } else if (velocity < 18 || velocity > 49) {    // off; 50..126 undocumented
        s.anim = SPECTRA_ANIM_NONE;
        s.level = 0;
    } else if (velocity <= 33) {
        s.anim = SPECTRA_ANIM_BRIGHT;
        s.level = velocity - 18;
    } else if (velocity <= 41) {
        s.anim = SPECTRA_ANIM_GATE;
        s.level = velocity - 34;
    } else {
        s.anim = SPECTRA_ANIM_PULSE;
        s.level = velocity - 42;
    }
}

static bool spectraGroundOn(int led) {
    const SpectraGroundState &s = spectraGround[led];
    if (s.steady) return true;
    switch (s.anim) {
        case SPECTRA_ANIM_BRIGHT: return s.level != 0;
        case SPECTRA_ANIM_GATE: return spectraPhase(spectraGateRateQ8[s.level]) < 0x8000;
        case SPECTRA_ANIM_PULSE: {
            uint32_t phase = spectraPhase(spectraPulseRateQ8[s.level]);
            return spectraPulseGain[phase >> 11] >= 128;
        }
        default: return false;
    }
}

// ---------------------------------------------------------------- cosmetics (off by default)

static uint32_t spectraSparkUntil[16];
static uint32_t spectraWaveStartUs;
static bool spectraWaveActive;

static void spectraBeginSpark(int pad) {
#if SPECTRA_SPARK
    spectraSparkUntil[pad] = micros() + 40000;
#else
    (void)pad;
#endif
}

static void spectraBeginWave() {
#if SPECTRA_BANK_WAVE
    spectraWaveStartUs = micros();
    spectraWaveActive = true;
#else
    spectraWaveActive = false;
#endif
}

static uint32_t spectraScale(uint32_t rgb, uint8_t gain) {
    uint32_t r = ((rgb >> 16) & 0xFF) * gain / 255;
    uint32_t g = ((rgb >> 8) & 0xFF) * gain / 255;
    uint32_t b = (rgb & 0xFF) * gain / 255;
    return (r << 16) | (g << 8) | b;
}

static uint32_t spectraSleepColor(int pad, uint32_t now) {
    static const uint32_t wheel[8] = {0x400000, 0x404000, 0x004000, 0x004040,
                                      0x000040, 0x400040, 0x202020, 0x402000};
    return wheel[((now / 120000) + pad) & 7];
}

static uint32_t spectraPadColor(int pad, int bank, uint32_t now) {
    SpectraPadState &s = spectraPad[pad];
    uint32_t base;
    if (s.forceActive)
        base = spectraActiveColor(bank, pad);
    else if (s.override)
        base = s.color;
    else
        base = spectraHeld[pad] ? spectraActiveColor(bank, pad)
                                : spectraInactiveColor(bank, pad);

#if SPECTRA_SLEEP_MIN
    if (!s.override && !s.forceActive && !spectraHeld[pad] &&
        now - spectraLastInputUs > (uint32_t)SPECTRA_SLEEP_MIN * 60000000UL)
        return spectraSleepColor(pad, now);
#endif

#if SPECTRA_SPARK
    if ((int32_t)(spectraSparkUntil[pad] - now) > 0) return 0xFFFFFF;
#endif
#if SPECTRA_BANK_WAVE
    if (spectraWaveActive) {
        uint32_t dt = now - spectraWaveStartUs;
        uint32_t start = (uint32_t)pad * 12000;
        if (dt < start + 40000) {
            if (dt >= start) return 0xFFFFFF;
        } else if (pad == 15) {
            spectraWaveActive = false;
        }
    }
#endif

    switch (s.anim) {
        case SPECTRA_ANIM_BRIGHT:
            return spectraScale(base, spectraLevelGain[s.level]);
        case SPECTRA_ANIM_GATE:
            return spectraPhase(spectraGateRateQ8[s.level]) < 0x8000 ? base : 0;
        case SPECTRA_ANIM_PULSE:
            return spectraScale(base, spectraPulseGain[spectraPhase(spectraPulseRateQ8[s.level]) >> 11]);
        default:
            return base;
    }
}

static void spectraLedsBegin() {
    spectraLastTickUs = micros();
    spectraLastInputUs = spectraLastTickUs;
    for (int pad = 0; pad < 16; pad++) {
        spectraPad[pad] = SpectraPadState();
        spectraHeld[pad] = false;
    }
    for (int led = 0; led < 4; led++) spectraGround[led] = SpectraGroundState();
}

// Recompute the 16 rings; push to the LEDs when something changed or an animation runs.
static void spectraLedsShow(int bank, bool force) {
    static uint32_t lastShowUs;
    static uint32_t lastFrame[16];
    uint32_t now = micros();
    uint32_t frame[16];
    bool changed = force;
    for (int pad = 0; pad < 16; pad++) {
        frame[pad] = spectraPadColor(pad, bank, now);
        if (frame[pad] != lastFrame[pad]) changed = true;
    }
    if (!changed || now - lastShowUs < 10000) return;
    for (int pad = 0; pad < 16; pad++) SMCPad.padColor(pad, frame[pad]);
    SMCPad.show();
    for (int pad = 0; pad < 16; pad++) lastFrame[pad] = frame[pad];
    lastShowUs = now;
}

// Button LEDs are separate from SMCPad.show(): they must be refreshed every loop, even when
// the rings are static, so this runs outside spectraLedsShow's early return.
static void spectraGroundLedsShow() {
    static bool last[4];
    for (int led = 0; led < 4; led++) {
        bool on = spectraGroundOn(led);
        if (on != last[led]) {
            SMCPad.buttonLed(SPECTRA_GROUND_BUTTON[led], on);
            last[led] = on;
        }
    }
}
