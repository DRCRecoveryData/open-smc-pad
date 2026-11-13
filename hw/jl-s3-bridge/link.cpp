#include "link.h"

#include <string.h>

#include "bridge_config.h"
#include "frame.h"
#include "keyer.h"
#include "midi_bridge.h"
#include "msc_proxy.h"
#include "state.h"

static FrameSender s_tx;
static SemaphoreHandle_t s_txLock = nullptr;

void linkSend(uint8_t type, const void *payload, uint16_t len) {
  if (s_txLock == nullptr) return;
  xSemaphoreTake(s_txLock, portMAX_DELAY);
  size_t wireLen = 0;
  const uint8_t *wire = s_tx.encode(type, payload, len, wireLen);
  Serial.write((uint8_t)0);
  Serial.write(wire, wireLen);
  Serial.write((uint8_t)0);
  xSemaphoreGive(s_txLock);
}

void linkLog(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  linkSend(MSG_LOG, buf, (uint16_t)strlen(buf));
}

static void dispatch(Frame &f) {
  switch (f.type) {
    case MSG_HELLO: {
      if (f.len < sizeof(HelloReq)) return;
      HelloAck ack;
      ack.proto = BRIDGE_PROTO_VERSION;
      ack.mode = g_state.mode;
      ack.fw = BRIDGE_FW_VERSION;
      ack.max_payload = FRAME_MAX_PAYLOAD;
      linkSend(MSG_HELLO_ACK, &ack, sizeof(ack));
      stateEmit(true);
      break;
    }
    case MSG_PING:
      linkSend(MSG_PONG, f.payload, f.len);
      break;
    case MSG_MIDI_OUT:
      midiHandleOut(f.payload, f.len);
      break;
    case MSG_SCSI_REQ:
      mscHandleRequest(f.payload, f.len);
      break;
    case MSG_KEYER:
      if (f.len >= sizeof(KeyerCmd)) {
        KeyerCmd cmd;
        memcpy(&cmd, f.payload, sizeof(cmd));
        keyerHandleCommand(cmd);
      }
      break;
    case MSG_MODE:
      if (f.len >= 1) stateRequestMode(f.payload[0]);
      break;
    default:
      break;
  }
}

static void linkTask(void *) {
  static Frame rx;
  static FrameReceiver recv;
  for (;;) {
    int b = Serial.read();
    if (b < 0) {
      vTaskDelay(1);
      continue;
    }
    if (recv.push((uint8_t)b, rx)) dispatch(rx);
  }
}

void linkInit() {
  s_txLock = xSemaphoreCreateMutex();
  Serial.setRxBufferSize(4096);
  Serial.begin(LINK_BAUD, SERIAL_8N1, 44, 43);
  xTaskCreatePinnedToCore(linkTask, "s3link", 6144, nullptr, 2, nullptr, 1);
}
