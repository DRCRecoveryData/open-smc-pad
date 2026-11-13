#pragma once
#include <Arduino.h>

#include "proto.h"

struct BridgeState {
  volatile uint8_t mode = MODE_MIDI;
  volatile uint8_t pad = PAD_UNKNOWN;
  volatile uint16_t vid = 0;
  volatile uint16_t pid = 0;
  volatile uint8_t rx_cables = 0;
  volatile uint8_t tx_cables = 0;
  volatile uint8_t keyer = KEYER_IDLE;
  volatile bool dirty = true;
};

extern BridgeState g_state;

void stateInit();
void stateRequestMode(uint8_t mode);
void stateEmit(bool force);
void stateTick();
