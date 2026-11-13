#pragma once
#include <Arduino.h>

void mscInit();
void mscHandleRequest(const uint8_t *payload, uint16_t len);
void mscTick();
