/* SMC-PAD Pocket browser protocol module (classic script, no DOM).
 *
 * Exposes window/globalThis.SMCPAD: the app-area validator and CRC from device.py, the
 * Spectra light messages from lights.py, the build settings from settings.py, and the DFU
 * 1.1 engine that replaces dfu-util.  The page drives it with WebUSB / Web Serial / Web MIDI.
 */
(function (root) {
  "use strict";

  var APP_VID = 0x1209;
  var APP_PID = 0x0001;
  var DFU_PID = 0x0002;
  var APP_ENTRY = 0x1E08120;
  var APP_AREA_MAX = 0x68000;
  var FLASH_SIZE = 0x80000;
  var TRANSFER_SIZE = 4096;

  var ARCADE_BASE = [36, 52, 68, 84];

  var PALETTE_RGB = [0x000000, 0xED1F24, 0x760F12, 0xF26722, 0x793311, 0xFFC907, 0x7F6403,
                     0xC4D82E, 0x626C17, 0x00C700, 0x006300, 0x00FFFF, 0x007F7F, 0x0000FF,
                     0x00007F, 0x8500FF, 0x42007F, 0xD2009F, 0x69004F, 0xFFFFFF];
  var PALETTE_NAMES = ["black", "red", "red dim", "orange", "orange dim", "yellow",
                       "yellow dim", "lime", "lime dim", "green", "green dim", "cyan",
                       "cyan dim", "blue", "blue dim", "purple", "purple dim", "magenta",
                       "magenta dim", "white"];
  var PALETTE = PALETTE_NAMES.map(function (name, i) {
    return { index: i, velocity: 1 + 6 * i, name: name, rgb: PALETTE_RGB[i] };
  });

  function crc16(bytes, crc) {
    crc = crc || 0;
    for (var i = 0; i < bytes.length; i++) {
      crc ^= bytes[i] << 8;
      for (var k = 0; k < 8; k++)
        crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
    }
    return crc;
  }

  function u16(bytes, off) { return bytes[off] | (bytes[off + 1] << 8); }
  function u32(bytes, off) {
    return (bytes[off] | (bytes[off + 1] << 8) | (bytes[off + 2] << 16) |
            (bytes[off + 3] << 24)) >>> 0;
  }

  function validateAppArea(data, name) {
    if (name && /\.fwsc$/i.test(name))
      return { ok: false, reason: "a .fwsc is the vendor full-firmware container, not an app area" };
    if (data.length >= FLASH_SIZE)
      return { ok: false, reason: "this is a full raw flash dump, not an app area" };
    if (data.length < 0x120)
      return { ok: false, reason: "too short to hold an app-area directory" };
    if (crc16(data.subarray(2, 32)) !== u16(data, 0))
      return { ok: false, reason: "directory header CRC mismatch" };
    if (u32(data, 4) !== APP_ENTRY)
      return { ok: false, reason: "directory entry is not the application entry 0x1E08120" };
    var size = u32(data, 8);
    if (!(size > 0x120 && size <= APP_AREA_MAX))
      return { ok: false, reason: "declared app-area size " + size + " is out of range" };
    if (size > data.length)
      return { ok: false, reason: "the directory declares more data than the file holds" };
    if (crc16(data.subarray(0x20, size)) !== u16(data, 2))
      return { ok: false, reason: "app-area CRC mismatch (truncated or corrupted file)" };
    return { ok: true, reason: "", size: size };
  }

  function validateBootPackage(size) {
    return (size >= 0x1000 && size <= 0xC000)
      ? { ok: true, reason: "" }
      : { ok: false, reason: "bootloader package must be 4 KiB..48 KiB (got " + size + " bytes)" };
  }

  function rgbForVelocity(velocity) {
    if (velocity === 0 || velocity >= 121) return null;
    return PALETTE[Math.floor((velocity - 1) / 6)].rgb;
  }

  function buildLights(action, baseCh, bank, pads, velocity) {
    if (!(baseCh >= 1 && baseCh <= 14)) throw new Error("base channel out of range");
    if (action === "bank") {
      if (!(velocity >= 0 && velocity <= 3)) throw new Error("bank must be 0..3");
      return [[0x90 | baseCh, velocity, 127]];
    }
    if (!pads || !pads.length || pads.length > 16 ||
        pads.some(function (p) { return !(p >= 0 && p <= 15); }))
      throw new Error("pads must be 0..15");
    if (action === "color") {
      if (!(velocity >= 0 && velocity <= 120))
        throw new Error("colour velocity must be 0..120");
    } else if (action === "force") {
      velocity = 127;
    } else if (action === "anim") {
      if (!(velocity >= 0 && velocity <= 49))
        throw new Error("animation velocity must be 0..49");
    } else {
      throw new Error("unknown action");
    }
    var trig = 0x90 | (baseCh - 1);
    var ctrl = 0x90 | baseCh;
    var base = ARCADE_BASE[bank & 3];
    return pads.map(function (p) {
      return [action === "anim" ? ctrl : trig, base + p, velocity];
    });
  }

  function tapFromMessage(data, baseCh, bank) {
    if (data.length < 3) return null;
    var status = data[0], note = data[1], vel = data[2];
    if ((status & 0xF0) !== 0x90) return null;
    if ((status & 0x0F) !== ((baseCh - 1) & 0x0F)) return null;
    var pad = note - ARCADE_BASE[bank & 3];
    return (pad >= 0 && pad < 16) ? { pad: pad, down: vel > 0 } : null;
  }

  /* ------------------------------------------------------------- settings */

  var BANKS = 4;

  var FIELDS = [
    { key: "base_ch", label: "MIDI base channel", type: "int", min: 1, max: 14, def: 3,
      help: "triggers and colour notes; bank notes and animations use the next channel, " +
            "side-button CCs the one after" },
    { key: "key_velocity", label: "Key MIDI velocity", type: "int", min: 1, max: 127, def: 127,
      help: "fixed trigger velocity (the Pocket's pads are not used for it)" },
    { key: "momentary_cc", label: "Momentary CC on triggers", type: "bool", def: false,
      help: "also send CC 127 on press / 0 on release, on the next channel" },
    { key: "spark", label: "Press spark", type: "bool", def: true,
      help: "flash a hit pad white for 40 ms" },
    { key: "bank_wave", label: "Bank-change wave", type: "bool", def: true,
      help: "diagonal white wave across the grid when the bank changes" },
    { key: "sleep_min", label: "Sleep after (minutes)", type: "int", min: 0, max: 240, def: 0,
      help: "0 = never; idle pads show a dim colour wheel after this long" },
    { key: "side_mode", label: "Side-button mode", type: "int", min: 0, max: 2, def: 0,
      help: "0 factory (Left2/Right2 change bank), 1 all notes, 2 all CCs" },
    { key: "side_bank_buttons", label: "Bank-aware side buttons", type: "bool", def: true,
      help: "side notes/CCs change with the bank" },
    { key: "side_cc_release", label: "Side CC release value", type: "int", min: 0, max: 127,
      def: 0, help: "sent when a side button configured for CC is released" },
  ];

  for (var b = 0; b < BANKS; b++) {
    FIELDS.push({ key: "active_b" + b, label: "Bank " + (b + 1) + " pressed", type: "color",
                  min: 0, max: 0xFFFFFF, def: 0xFFFFFF });
  }
  for (b = 0; b < BANKS; b++) {
    FIELDS.push({ key: "inactive_b" + b, label: "Bank " + (b + 1) + " idle", type: "color",
                  min: 0, max: 0xFFFFFF, def: 0x000000 });
  }

  var DEFAULTS = {};
  FIELDS.forEach(function (f) { DEFAULTS[f.key] = f.def; });

  function asInt(value) {
    if (typeof value === "boolean") return value ? 1 : 0;
    if (typeof value === "number" && Number.isInteger(value)) return value;
    if (typeof value === "string" && value.trim()) {
      var n = Number(value.trim());
      return Number.isInteger(n) ? n : null;
    }
    return null;
  }

  function validateSettings(input) {
    var values = {}, errors = {};
    input = input || {};
    FIELDS.forEach(function (f) {
      var raw = Object.prototype.hasOwnProperty.call(input, f.key) ? input[f.key] : f.def;
      if (f.type === "bool") {
        values[f.key] = (typeof raw === "string") ? /^(1|true|yes|on)$/i.test(raw.trim())
                                                  : !!raw;
        return;
      }
      var n = asInt(raw);
      if (n === null) { errors[f.key] = f.label + ": not a number"; return; }
      if (n < f.min || n > f.max) {
        errors[f.key] = f.label + ": " + n + " is outside " + f.min + ".." + f.max;
        return;
      }
      values[f.key] = n;
    });
    return { values: values, errors: errors };
  }

  function hex6(n) { return "0x" + ("000000" + (n >>> 0).toString(16).toUpperCase()).slice(-6); }

  function settingsDefines(input) {
    var v = validateSettings(input);
    var bad = Object.keys(v.errors).sort();
    if (bad.length) throw new Error(bad.map(function (k) { return v.errors[k]; }).join("; "));
    var out = [
      "-DSPECTRA_BASE_CH=" + v.values.base_ch,
      "-DSPECTRA_KEY_VELOCITY=" + v.values.key_velocity,
      "-DSPECTRA_MOMENTARY_CC=" + (v.values.momentary_cc ? 1 : 0),
      "-DSPECTRA_SPARK=" + (v.values.spark ? 1 : 0),
      "-DSPECTRA_BANK_WAVE=" + (v.values.bank_wave ? 1 : 0),
      "-DSPECTRA_SLEEP_MIN=" + v.values.sleep_min,
      "-DSPECTRA_SIDE_MODE=" + v.values.side_mode,
      "-DSPECTRA_SIDE_BANK_BUTTONS=" + (v.values.side_bank_buttons ? 1 : 0),
      "-DSPECTRA_SIDE_CC_RELEASE=" + v.values.side_cc_release,
    ];
    for (var i = 0; i < BANKS; i++) {
      out.push("-DSPECTRA_ACTIVE_B" + i + "=" + hex6(v.values["active_b" + i]) + "u");
      out.push("-DSPECTRA_INACTIVE_B" + i + "=" + hex6(v.values["inactive_b" + i]) + "u");
    }
    return out;
  }

  /* ---------------------------------------------------------------- hashes */

  function sha256Hex(bytes) {
    var buf = (bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength)
      ? bytes.buffer : bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    return crypto.subtle.digest("SHA-256", buf).then(function (d) {
      return Array.from(new Uint8Array(d)).map(function (x) {
        return ("0" + x.toString(16)).slice(-2);
      }).join("");
    });
  }

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  /* ------------------------------------------------------------------ DFU */

  var DFU = {
    DETACH: 0, DNLOAD: 1, UPLOAD: 2, GETSTATUS: 3, CLRSTATUS: 4, GETSTATE: 5, ABORT: 6,
    STATE: { appIdle: 0, appDetach: 1, idle: 2, dnloadSync: 3, dnloadBusy: 4, dnloadIdle: 5,
             manifestSync: 6, manifest: 7, manifestWaitReset: 8, uploadIdle: 9, error: 10 },
    STATUS_NAMES: ["OK", "errTARGET", "errFILE", "errWRITE", "errERASE", "errCHECK_ERASED",
                   "errPROG", "errVERIFY", "errADDRESS", "errNOTDONE", "errFIRMWARE",
                   "errVENDOR", "errUSBR", "errPOR", "errUNKNOWN", "errSTALLEDPKT"],
    TRANSFER_SIZE: TRANSFER_SIZE,
  };

  function Dfu(device, opts) {
    opts = opts || {};
    this.device = device;
    this.iface = 0;
    this.timeoutMs = opts.timeoutMs || 20000;
  }

  Dfu.prototype.open = function () {
    var self = this;
    return Promise.resolve().then(function () {
      if (!self.device.opened) return self.device.open();
    }).then(function () {
      if (!self.device.configuration) {
        var conf = self.device.configurations && self.device.configurations[0];
        return self.device.selectConfiguration(conf ? conf.configurationValue : 1);
      }
    }).then(function () {
      return self.device.claimInterface(self.iface);
    });
  };

  Dfu.prototype.close = function () {
    var self = this;
    return Promise.resolve()
      .then(function () { return self.device.releaseInterface(self.iface); })
      .catch(function () {})
      .then(function () { if (self.device.opened) return self.device.close(); })
      .catch(function () {});
  };

  Dfu.prototype.selectAlt = function (alt) {
    return this.device.selectAlternateInterface(this.iface, alt);
  };

  Dfu.prototype._out = function (request, value, data) {
    return this.device.controlTransferOut({
      requestType: "class", recipient: "interface",
      request: request, value: value, index: this.iface,
    }, data || new Uint8Array(0));
  };

  Dfu.prototype._in = function (request, value, length) {
    return this.device.controlTransferIn({
      requestType: "class", recipient: "interface",
      request: request, value: value, index: this.iface,
    }, length);
  };

  Dfu.prototype.status = function () {
    return this._in(DFU.GETSTATUS, 0, 6).then(function (r) {
      if (r.status !== "ok" || !r.data || r.data.byteLength < 6)
        throw new Error("DFU status request failed (" + r.status + ")");
      var d = new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
      return {
        status: d[0],
        pollMs: d[1] | (d[2] << 8) | (d[3] << 16),
        state: d[4],
      };
    });
  };

  function dfuError(s) {
    var name = DFU.STATUS_NAMES[s.status] || ("status " + s.status);
    return new Error("DFU " + name + " (state " + s.state + ")");
  }

  Dfu.prototype.waitIdle = function () {
    var self = this, start = Date.now();
    function loop() {
      return self.status().then(function (s) {
        if (s.status !== 0) throw dfuError(s);
        if (s.state !== DFU.STATE.dnloadBusy) return s;
        if (Date.now() - start > self.timeoutMs) throw new Error("DFU timed out");
        return sleep(s.pollMs + 5).then(loop);
      });
    }
    return loop();
  };

  Dfu.prototype.clearError = function (depth) {
    var self = this;
    depth = depth || 0;
    if (depth > 3) return Promise.resolve(null);
    return self.status().then(function (s) {
      if (s.status === 0) return s;
      return self._out(DFU.CLRSTATUS, 0).then(function () {
        return self.clearError(depth + 1);
      });
    }).catch(function () { return null; });
  };

  Dfu.prototype.abort = function () {
    return this._out(DFU.ABORT, 0).catch(function () {});
  };

  Dfu.prototype.detach = function () {
    return this._out(DFU.DETACH, 0).catch(function () {});
  };

  Dfu.prototype.waitManifest = function (onState) {
    var self = this, start = Date.now();
    function loop() {
      return self.status().then(function (s) {
        if (onState) onState(s.state);
        if (s.status !== 0) throw dfuError(s);
        if (s.state === DFU.STATE.idle || s.state === DFU.STATE.manifest ||
            s.state === DFU.STATE.manifestWaitReset) return s;
        if (Date.now() - start > self.timeoutMs) throw new Error("DFU manifestation timed out");
        return sleep(Math.max(s.pollMs, 15)).then(loop);
      }, function () {
        return { status: 0, state: DFU.STATE.manifestWaitReset };
      });
    }
    return loop();
  };

  Dfu.prototype.download = function (alt, bytes, onBlock) {
    var self = this;
    if (!bytes.length) return Promise.reject(new Error("nothing to write"));
    return self.selectAlt(alt).then(function () {
      return self.clearError();
    }).then(function () {
      var blocks = Math.ceil(bytes.length / TRANSFER_SIZE), sent = 0;
      function next() {
        if (sent === blocks) {
          return self._out(DFU.DNLOAD, blocks, new Uint8Array(0)).then(function (r) {
            if (r.status !== "ok" && r.status !== "stall")
              throw new Error("DFU end-of-transfer request failed (" + r.status + ")");
            return self.waitManifest(onBlock && onBlock.manifest);
          });
        }
        var off = sent * TRANSFER_SIZE;
        var chunk = bytes.subarray(off, Math.min(off + TRANSFER_SIZE, bytes.length));
        return self._out(DFU.DNLOAD, sent, chunk).then(function (r) {
          if (r.status !== "ok")
            throw new Error("DFU write failed at block " + sent + " (" + r.status + ")");
          return self.waitIdle();
        }).then(function () {
          sent++;
          if (onBlock) onBlock.block(sent, blocks, Math.min(off + chunk.length, bytes.length),
                                     bytes.length);
          return next();
        });
      }
      return next();
    }).then(function (s) {
      return s;
    });
  };

  Dfu.prototype.upload = function (alt, maxBytes, onBlock) {
    var self = this;
    var limit = maxBytes || Infinity;
    return self.selectAlt(alt).then(function () {
      return self.clearError();
    }).then(function () {
      var parts = [], total = 0, blockNum = 0;
      function next() {
        var want = Math.min(TRANSFER_SIZE, limit - total);
        if (want <= 0) return Promise.resolve(join(parts));
        return self._in(DFU.UPLOAD, blockNum, want).then(function (r) {
          if (r.status === "stall") return join(parts);
          if (r.status !== "ok")
            throw new Error("DFU read failed at block " + blockNum + " (" + r.status + ")");
          var d = new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
          if (!d.length) return join(parts);
          parts.push(d); total += d.length; blockNum++;
          if (onBlock) onBlock(total, limit === Infinity ? null : limit);
          if (d.length < want) return join(parts);
          return next();
        });
      }
      return next();
    });
  };

  function join(parts) {
    var total = 0;
    parts.forEach(function (p) { total += p.length; });
    var out = new Uint8Array(total), off = 0;
    parts.forEach(function (p) { out.set(p, off); off += p.length; });
    return out;
  }

  function isDfuDevice(d) { return d.vendorId === APP_VID && d.productId === DFU_PID; }
  function isAppDevice(d) { return d.vendorId === APP_VID && d.productId === APP_PID; }

  function findDfu(usb) {
    return usb.getDevices().then(function (list) {
      return list.find(isDfuDevice) || null;
    });
  }

  function findSerial(serial) {
    return serial.getPorts().then(function (list) {
      return list.find(function (p) {
        var i = p.getInfo();
        return i.usbVendorId === APP_VID && i.usbProductId === APP_PID;
      }) || null;
    });
  }

  function touch1200(port) {
    return port.open({ baudRate: 1200, dataBits: 8, stopBits: 1,
                       parity: "none", flowControl: "none" })
      .then(function () {
        return port.setSignals({ dataTerminalReady: true, requestToSend: false });
      })
      .then(function () { return sleep(100); })
      .then(function () { return port.setSignals({ dataTerminalReady: false }); })
      /* Close at once: the app reboots on DTR low, and Chrome's serial service has crashed
       * (taking the browser with it) when the port was still being closed as the device
       * re-enumerated. Once the DTR-low transfer is done, the touch has been delivered. */
      .then(function () { return port.close(); });
  }

  function enterDfu(usb, port) {
    return findDfu(usb).then(function (present) {
      if (present) return present;
      if (!port)
        throw new Error("select the serial port first (Status tab)");
      return touch1200(port).then(function () {
        var start = Date.now();
        function poll() {
          return findDfu(usb).then(function (d) {
            if (d) return d;
            if (Date.now() - start > 10000)
              throw new Error("the pad did not enter DFU. On first use click " +
                              "\u201cSelect DFU device\u201d while the pad is in DFU, " +
                              "or replug the pad and try again");
            return sleep(250).then(poll);
          });
        }
        return poll();
      });
    });
  }

  root.SMCPAD = {
    APP_VID: APP_VID, APP_PID: APP_PID, DFU_PID: DFU_PID,
    APP_ENTRY: APP_ENTRY, APP_AREA_MAX: APP_AREA_MAX, FLASH_SIZE: FLASH_SIZE,
    TRANSFER_SIZE: TRANSFER_SIZE,
    ARCADE_BASE: ARCADE_BASE,
    PALETTE: PALETTE,
    DEFAULTS: DEFAULTS,
    FIELDS: FIELDS,
    BANKS: BANKS,
    crc16: crc16,
    validateAppArea: validateAppArea,
    validateBootPackage: validateBootPackage,
    rgbForVelocity: rgbForVelocity,
    buildLights: buildLights,
    tapFromMessage: tapFromMessage,
    validateSettings: validateSettings,
    settingsDefines: settingsDefines,
    sha256Hex: sha256Hex,
    sleep: sleep,
    DFU: DFU,
    Dfu: Dfu,
    isDfuDevice: isDfuDevice,
    isAppDevice: isAppDevice,
    findDfu: findDfu,
    findSerial: findSerial,
    touch1200: touch1200,
    enterDfu: enterDfu,
  };
})(typeof globalThis !== "undefined" ? globalThis : this);
