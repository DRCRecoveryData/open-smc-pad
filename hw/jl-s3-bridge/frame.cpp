#include "frame.h"

#include <string.h>

static size_t cobsEncode(const uint8_t *input, size_t length, uint8_t *output) {
  size_t readIndex = 0, writeIndex = 1, codeIndex = 0;
  uint8_t code = 1;
  while (readIndex < length) {
    if (input[readIndex] == 0) {
      output[codeIndex] = code;
      code = 1;
      codeIndex = writeIndex++;
      readIndex++;
    } else {
      output[writeIndex++] = input[readIndex++];
      if (++code == 0xFF) {
        output[codeIndex] = code;
        code = 1;
        codeIndex = writeIndex++;
      }
    }
  }
  output[codeIndex] = code;
  return writeIndex;
}

static size_t cobsDecode(const uint8_t *input, size_t length, uint8_t *output, size_t maxOut) {
  size_t readIndex = 0, writeIndex = 0;
  while (readIndex < length) {
    uint8_t code = input[readIndex];
    if (code == 0 || readIndex + code > length) return (size_t)-1;
    readIndex++;
    for (uint8_t i = 1; i < code; i++) {
      if (writeIndex >= maxOut) return (size_t)-1;
      output[writeIndex++] = input[readIndex++];
    }
    if (code != 0xFF && readIndex < length) {
      if (writeIndex >= maxOut) return (size_t)-1;
      output[writeIndex++] = 0;
    }
  }
  return writeIndex;
}

uint16_t frameCrc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

void FrameReceiver::reset() {
  wireLen_ = 0;
  overflow_ = false;
}

bool FrameReceiver::push(uint8_t b, Frame &out) {
  if (b != 0) {
    if (wireLen_ < FRAME_MAX_WIRE) {
      wire_[wireLen_++] = b;
    } else {
      overflow_ = true;
    }
    return false;
  }
  if (wireLen_ == 0) return false;
  bool ok = false;
  if (!overflow_) {
    uint8_t body[FRAME_MAX_BODY];
    size_t n = cobsDecode(wire_, wireLen_, body, sizeof(body));
    if (n >= 6 && n <= FRAME_MAX_BODY) {
      uint16_t len = (uint16_t)body[2] | ((uint16_t)body[3] << 8);
      if ((size_t)len + 6 == n) {
        uint16_t crc = (uint16_t)body[4 + len] | ((uint16_t)body[5 + len] << 8);
        if (crc == frameCrc16(body, 4 + len)) {
          out.type = body[0];
          out.seq = body[1];
          out.len = len;
          if (len) memcpy(out.payload, body + 4, len);
          ok = true;
        }
      }
    }
  }
  reset();
  return ok;
}

const uint8_t *FrameSender::encode(uint8_t type, const void *payload, uint16_t len, size_t &wireLen) {
  if (len > FRAME_MAX_PAYLOAD) len = FRAME_MAX_PAYLOAD;
  body_[0] = type;
  body_[1] = nextSeq();
  body_[2] = (uint8_t)(len & 0xFF);
  body_[3] = (uint8_t)(len >> 8);
  if (len) memcpy(body_ + 4, payload, len);
  uint16_t crc = frameCrc16(body_, 4 + len);
  body_[4 + len] = (uint8_t)(crc & 0xFF);
  body_[5 + len] = (uint8_t)(crc >> 8);
  wireLen = cobsEncode(body_, (size_t)len + 6, wire_);
  return wire_;
}
