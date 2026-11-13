#include "midi_bridge.h"

#include <USBHost.h>
#include <string.h>

#include "link.h"
#include "proto.h"
#include "state.h"

static volatile uint8_t s_idx = 0xFF;
static volatile bool s_mounted = false;

// USB-MIDI packets from the PC, drained by midiTick(). Cable numbers travel in the packet's
// high nibble, so all pad ports survive the round trip.
constexpr uint16_t MIDI_RING = 1024;
static uint8_t s_ring[MIDI_RING];
static volatile uint32_t s_head = 0, s_tail = 0;

void midiInit() {}

void midiHandleOut(const uint8_t *packets, uint16_t len) {
  len &= ~3;
  for (uint16_t i = 0; i < len; i += 4) {
    if (s_head - s_tail >= MIDI_RING) break;   // PC is ahead of the bus: drop the excess
    uint8_t *p = &s_ring[s_head % MIDI_RING];
    memcpy(p, packets + i, 4);
    s_head += 4;
  }
}

void midiTick() {
  if (!s_mounted) return;

  uint8_t pkt[240];
  uint32_t n = tuh_midi_packet_read_n(s_idx, pkt, sizeof(pkt));
  if (n) linkSend(MSG_MIDI_IN, pkt, (uint16_t)n);

  while (s_head - s_tail >= 4) {
    uint32_t avail = s_head - s_tail;
    uint16_t take = (uint16_t)(avail > 60 ? 60 : avail) & ~3;
    uint8_t tmp[60];
    for (uint16_t i = 0; i < take; i++) tmp[i] = s_ring[(s_tail + i) % MIDI_RING];
    uint32_t queued = tuh_midi_packet_write_n(s_idx, tmp, take);
    s_tail += queued;
    if (queued < take) break;
  }
  tuh_midi_write_flush(s_idx);
}

extern "C" void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *mount) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(mount->daddr, &vid, &pid);
  s_idx = idx;
  s_mounted = true;
  g_state.rx_cables = mount->rx_cable_count;
  g_state.tx_cables = mount->tx_cable_count;
  g_state.vid = vid;
  g_state.pid = pid;
  g_state.pad = PAD_MIDI;
  g_state.dirty = true;
}

extern "C" void tuh_midi_umount_cb(uint8_t idx) {
  if (idx != s_idx) return;
  s_mounted = false;
  if (g_state.pad == PAD_MIDI) g_state.pad = PAD_UNKNOWN;
  g_state.dirty = true;
}
