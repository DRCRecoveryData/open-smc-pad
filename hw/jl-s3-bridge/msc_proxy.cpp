#include "msc_proxy.h"

#include <USBHost.h>
#include <string.h>

#include "link.h"
#include "proto.h"
#include "state.h"

static volatile uint8_t s_daddr = 0;
static volatile bool s_mounted = false;
static uint32_t s_tag = 0x54555342;

struct Pending {
  bool active;
  uint32_t req_id;
  uint8_t dir;
  uint8_t cdb_len;
  uint16_t data_len;
  uint32_t timeout_ms;
  uint8_t cdb[16];
  uint8_t data[MAX_SCSI_DATA];
} __attribute__((aligned(16)));
static Pending s_pending;

struct CmdDone {
  volatile bool done;
  uint8_t status;
  uint32_t residue;
};
static CmdDone s_done;
static volatile uint32_t s_gen = 0;
static SemaphoreHandle_t s_sem = nullptr;

static bool onComplete(uint8_t, const tuh_msc_complete_data_t *cb) {
  if ((uint32_t)cb->user_arg != s_gen) return true;
  s_done.status = cb->csw->status;
  s_done.residue = cb->csw->data_residue;
  s_done.done = true;
  xSemaphoreGive(s_sem);
  return true;
}

// One command, one transfer. Returns false on transport failure or timeout.
static bool runCdb(const uint8_t *cdb, uint8_t cdbLen, uint8_t dir, uint8_t *buf, uint16_t len,
                   uint32_t timeoutMs, uint8_t &status, uint32_t &residue) {
  msc_cbw_t cbw;
  memset(&cbw, 0, sizeof(cbw));
  cbw.signature = MSC_CBW_SIGNATURE;
  cbw.tag = ++s_tag;
  cbw.total_bytes = (dir && len) ? len : 0;
  cbw.dir = (dir == SCSI_DIR_IN) ? TUSB_DIR_IN_MASK : 0;
  cbw.lun = 0;
  cbw.cmd_len = cdbLen;
  memcpy(cbw.command, cdb, cdbLen);

  uint32_t gen = ++s_gen;
  s_done.done = false;
  if (!tuh_msc_scsi_command(s_daddr, &cbw, (dir && len) ? (void *)buf : nullptr, onComplete, gen)) {
    return false;
  }
  if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(timeoutMs + 1000)) != pdTRUE) return false;
  if (!s_done.done) return false;
  status = s_done.status;
  residue = s_done.residue;
  return true;
}

static void reply(uint32_t reqId, uint8_t status, const uint8_t *data, uint16_t dataLen,
                  uint32_t residue, const uint8_t *sense, uint8_t senseLen) {
  static ScsiRspHdr hdr;
  static uint8_t buf[sizeof(ScsiRspHdr) + MAX_SCSI_DATA + 32] __attribute__((aligned(16)));
  hdr.req_id = reqId;
  hdr.status = status;
  hdr.sense_len = senseLen;
  hdr.data_len = dataLen;
  hdr.residue = residue;
  memcpy(buf, &hdr, sizeof(hdr));
  uint16_t n = sizeof(hdr);
  if (dataLen) memcpy(buf + n, data, dataLen);
  n += dataLen;
  if (senseLen) memcpy(buf + n, sense, senseLen);
  n += senseLen;
  linkSend(MSG_SCSI_RSP, buf, n);
}

void mscInit() {
  s_sem = xSemaphoreCreateBinary();
}

void mscHandleRequest(const uint8_t *payload, uint16_t len) {
  if (len < sizeof(ScsiReqHdr)) return;
  ScsiReqHdr h;
  memcpy(&h, payload, sizeof(h));
  if (h.cdb_len > sizeof(h.cdb)) h.cdb_len = sizeof(h.cdb);
  if (h.dir > SCSI_DIR_IN || h.data_len > MAX_SCSI_DATA ||
      (h.dir == SCSI_DIR_OUT && len < sizeof(ScsiReqHdr) + h.data_len)) {
    reply(h.req_id, SCSI_UNSUPPORTED, nullptr, 0, 0, nullptr, 0);
    return;
  }
  if (s_pending.active) {
    reply(h.req_id, SCSI_BUSY, nullptr, 0, 0, nullptr, 0);
    return;
  }
  s_pending.active = true;
  s_pending.req_id = h.req_id;
  s_pending.dir = h.dir;
  s_pending.cdb_len = h.cdb_len;
  s_pending.data_len = (h.dir == SCSI_DIR_NONE) ? 0 : h.data_len;
  s_pending.timeout_ms = h.timeout_ms ? h.timeout_ms : 5000;
  memcpy(s_pending.cdb, h.cdb, sizeof(h.cdb));
  if (s_pending.dir == SCSI_DIR_OUT && s_pending.data_len) {
    memcpy(s_pending.data, payload + sizeof(ScsiReqHdr), s_pending.data_len);
  }
}

void mscTick() {
  if (!s_pending.active) return;
  uint32_t reqId = s_pending.req_id;

  if (!s_mounted) {
    s_pending.active = false;
    reply(reqId, SCSI_NOT_UBOOT, nullptr, 0, 0, nullptr, 0);
    return;
  }

  uint8_t status = 0;
  uint32_t residue = 0;
  if (!runCdb(s_pending.cdb, s_pending.cdb_len, s_pending.dir, s_pending.data, s_pending.data_len,
              s_pending.timeout_ms, status, residue)) {
    s_pending.active = false;
    reply(reqId, SCSI_TIMEOUT, nullptr, 0, 0, nullptr, 0);
    return;
  }

  uint8_t sense[18];
  uint8_t senseLen = 0;
  uint8_t outStatus = SCSI_OK;
  if (status == MSC_CSW_STATUS_PASSED) {
    outStatus = SCSI_OK;
  } else if (status == MSC_CSW_STATUS_FAILED) {
    outStatus = SCSI_BOT_FAIL;
    const uint8_t rs[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t sStatus = 0;
    uint32_t sResidue = 0;
    if (runCdb(rs, sizeof(rs), SCSI_DIR_IN, sense, sizeof(sense), 3000, sStatus, sResidue) &&
        sStatus == MSC_CSW_STATUS_PASSED) {
      senseLen = (sResidue < sizeof(sense)) ? (uint8_t)(sizeof(sense) - sResidue) : 0;
    }
  } else {
    outStatus = SCSI_TRANSPORT;
  }

  uint16_t dataLen = 0;
  if (s_pending.dir == SCSI_DIR_IN && outStatus == SCSI_OK) {
    uint32_t lost = residue < s_pending.data_len ? residue : s_pending.data_len;
    dataLen = (uint16_t)(s_pending.data_len - lost);
  }
  s_pending.active = false;
  reply(reqId, outStatus, s_pending.data, dataLen, residue, sense, senseLen);
}

extern "C" void tuh_msc_mount_cb(uint8_t dev_addr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  s_daddr = dev_addr;
  s_mounted = true;
  g_state.vid = vid;
  g_state.pid = pid;
  g_state.pad = PAD_UBOOT;
  g_state.dirty = true;
}

extern "C" void tuh_msc_umount_cb(uint8_t dev_addr) {
  if (dev_addr != s_daddr) return;
  s_mounted = false;
  if (g_state.pad == PAD_UBOOT) g_state.pad = PAD_UNKNOWN;
  g_state.dirty = true;
}
