/* Which USB stack the firmware uses: 1 = TinyUSB with our BR25 driver (usb/dcd_br25.c),
 * 0 = the JieLi SDK's. fw/build.sh can override it (SMCPAD_TINYUSB=0|1). */
#ifndef SMCPAD_USB_CFG_H
#define SMCPAD_USB_CFG_H
#include "smcpad_usb_build.h"               /* written by fw/build.sh */
#ifndef SMCPAD_TINYUSB
#define SMCPAD_TINYUSB 0
#endif
/* 1: the Arduino runtime (fw/smcpad/arduino) runs setup()/loop() instead of the console. */
#ifndef SMCPAD_ARDUINO
#define SMCPAD_ARDUINO 0
#endif
/* Flash offset of our app area's directory (0x1000, or 0x9000 behind the bootloader). */
#ifndef SMCPAD_APP_DIR
#define SMCPAD_APP_DIR 0x1000
#endif
/* 1: the board is the SMC-PAD Pocket (SK16 V03): one HCF4051, no key matrix, no encoders,
 * no button LEDs. The Pocket is the default; fw/build.sh can override it (SMCPAD_POCKET=0|1),
 * SMCPAD_POCKET=0 being the full-size SMC-PAD reference board. */
#ifndef SMCPAD_POCKET
#define SMCPAD_POCKET 1
#endif
#endif
