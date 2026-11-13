#include "status.h"

#include "bridge_config.h"

void statusInit() { statusRGB(0, 0, 0); }

// Both revision pins are driven: the one without an LED is a free header pin.
void statusRGB(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(PIN_RGB_S3_V10, r, g, b);
  rgbLedWrite(PIN_RGB_S3_V11, r, g, b);
}
