#pragma once
#include <Arduino.h>

constexpr uint8_t BRIDGE_PROTO_VERSION = 1;
constexpr uint8_t BRIDGE_FW_VERSION = 1;

constexpr uint32_t LINK_BAUD = 921600;
constexpr uint32_t LINK_TIMEOUT_MS = 600000;

// Key taps. 0 = the proven GPIO4/5 taps on the data lines, so the pad's already tapped
// cable keeps working. 1 = bit-bang the native USB pins instead (only valid while the host
// PHY is off).
#define BRIDGE_KEY_ON_NATIVE_PINS 0
#if BRIDGE_KEY_ON_NATIVE_PINS
constexpr uint8_t PIN_KEY_DP = 19;
constexpr uint8_t PIN_KEY_DM = 20;
#else
constexpr uint8_t PIN_KEY_DP = 4;
constexpr uint8_t PIN_KEY_DM = 5;
#endif

constexpr uint8_t PIN_RGB_S3_V10 = 48;
constexpr uint8_t PIN_RGB_S3_V11 = 38;

constexpr uint16_t USB_KEY = 0x16EF;
constexpr uint32_t HALF_PERIOD_US = 10;   // ~50 kHz key clock
constexpr uint32_t SENSE_US = 3;
constexpr uint32_t PROBE_US = 300;
constexpr uint32_t TIMEOUT_MS = 600000;
constexpr uint32_t ARM_LOW_MS = 10;
constexpr uint32_t TRAIN_PULSES = 150;
