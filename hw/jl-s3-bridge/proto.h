#pragma once
#include <Arduino.h>

enum : uint8_t {
  MSG_HELLO = 0x01,
  MSG_HELLO_ACK = 0x02,
  MSG_STATE = 0x10,
  MSG_MIDI_IN = 0x20,
  MSG_MIDI_OUT = 0x21,
  MSG_SCSI_REQ = 0x30,
  MSG_SCSI_RSP = 0x31,
  MSG_KEYER = 0x40,
  MSG_KEYER_ST = 0x41,
  MSG_MODE = 0x42,
  MSG_PING = 0x50,
  MSG_PONG = 0x51,
  MSG_LOG = 0x60,
};

enum : uint8_t { SCSI_DIR_NONE = 0, SCSI_DIR_OUT = 1, SCSI_DIR_IN = 2 };

enum : uint8_t {
  SCSI_OK = 0,
  SCSI_BOT_FAIL = 1,
  SCSI_TRANSPORT = 2,
  SCSI_NOT_UBOOT = 3,
  SCSI_UNSUPPORTED = 4,
  SCSI_TIMEOUT = 5,
  SCSI_BUSY = 6,
};

enum : uint8_t { PAD_UNKNOWN = 0, PAD_MIDI = 1, PAD_UBOOT = 2 };

enum : uint8_t { MODE_MIDI = 0, MODE_UBOOT = 1 };

enum : uint8_t {
  KEYER_IDLE = 0,
  KEYER_ARMED,
  KEYER_KEYING,
  KEYER_OK,
  KEYER_TIMEOUT,
  KEYER_BLOCKED,
};

enum : uint8_t { KEYER_OP_DISARM = 0, KEYER_OP_ARM = 1, KEYER_OP_REPLUG = 2, KEYER_OP_HOST = 3 };

constexpr uint16_t MAX_SCSI_DATA = 1024;

struct HelloReq {
  uint16_t proto;
  uint16_t want_baud;
  uint8_t flags;
} __attribute__((packed));

struct HelloAck {
  uint16_t proto;
  uint8_t mode;
  uint8_t fw;
  uint16_t max_payload;
} __attribute__((packed));

struct StateMsg {
  uint8_t mode;
  uint8_t pad;
  uint16_t vid;
  uint16_t pid;
  uint8_t rx_cables;
  uint8_t tx_cables;
  uint8_t keyer;
  uint8_t reserved;
} __attribute__((packed));

struct ScsiReqHdr {
  uint32_t req_id;
  uint8_t dir;
  uint8_t cdb_len;
  uint16_t data_len;
  uint32_t timeout_ms;
  uint8_t cdb[16];
} __attribute__((packed));

struct ScsiRspHdr {
  uint32_t req_id;
  uint8_t status;
  uint8_t sense_len;
  uint16_t data_len;
  uint32_t residue;
} __attribute__((packed));

struct KeyerCmd {
  uint8_t op;
  uint8_t reserved;
  uint32_t wait_ms;
} __attribute__((packed));

struct KeyerSt {
  uint8_t phase;
  uint8_t reserved[3];
  uint32_t keys;
  uint32_t ms;
} __attribute__((packed));
