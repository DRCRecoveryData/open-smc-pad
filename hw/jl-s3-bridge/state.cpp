#include "state.h"

#include <Preferences.h>

#include "link.h"

BridgeState g_state;
static Preferences s_prefs;

void stateInit() {
  s_prefs.begin("s3bridge", false);
  uint8_t mode = s_prefs.getUChar("mode", MODE_MIDI);
  g_state.mode = (mode == MODE_UBOOT) ? MODE_UBOOT : MODE_MIDI;
  g_state.dirty = true;
}

void stateRequestMode(uint8_t mode) {
  if (mode != MODE_MIDI && mode != MODE_UBOOT) return;
  s_prefs.putUChar("mode", mode);
  linkLog("mode %s saved, restarting", mode == MODE_UBOOT ? "uboot" : "midi");
  delay(50);
  ESP.restart();
}

void stateEmit(bool force) {
  static uint32_t last = 0;
  if (!force && !g_state.dirty && millis() - last < 1000) return;
  g_state.dirty = false;
  last = millis();
  StateMsg msg;
  msg.mode = g_state.mode;
  msg.pad = g_state.pad;
  msg.vid = g_state.vid;
  msg.pid = g_state.pid;
  msg.rx_cables = g_state.rx_cables;
  msg.tx_cables = g_state.tx_cables;
  msg.keyer = g_state.keyer;
  msg.reserved = 0;
  linkSend(MSG_STATE, &msg, sizeof(msg));
}

void stateTick() { stateEmit(false); }
