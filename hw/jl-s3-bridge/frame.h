#pragma once
#include <Arduino.h>

// Wire format: 0x00 | COBS(type, seq, len LE16, payload, crc16 LE16) | 0x00

constexpr uint16_t FRAME_MAX_PAYLOAD = 2048;
constexpr uint16_t FRAME_MAX_BODY = FRAME_MAX_PAYLOAD + 6;
constexpr uint16_t FRAME_MAX_WIRE = FRAME_MAX_BODY + (FRAME_MAX_BODY / 254) + 1;

struct Frame {
  uint8_t type = 0;
  uint8_t seq = 0;
  uint16_t len = 0;
  uint8_t payload[FRAME_MAX_PAYLOAD];
};

uint16_t frameCrc16(const uint8_t *p, size_t n);

class FrameReceiver {
 public:
  bool push(uint8_t b, Frame &out);

 private:
  void reset();
  uint8_t wire_[FRAME_MAX_WIRE];
  uint16_t wireLen_ = 0;
  bool overflow_ = false;
};

class FrameSender {
 public:
  const uint8_t *encode(uint8_t type, const void *payload, uint16_t len, size_t &wireLen);
  uint8_t nextSeq() { return seq_++; }

 private:
  uint8_t seq_ = 0;
  uint8_t body_[FRAME_MAX_BODY];
  uint8_t wire_[FRAME_MAX_WIRE];
};
