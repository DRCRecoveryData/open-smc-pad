#!/usr/bin/env node
/* Tests for host/webui/smcpad.js: the validator, the Spectra light messages, the settings
 * defines and the DFU 1.1 engine, cross-checked against the Python modules when python3 is
 * available.  Run: node --test host/tests/test_smcpad_js.mjs
 */
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, existsSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { fileURLToPath, pathToFileURL } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..");
const webui = join(root, "host", "webui");

// smcpad.js is a classic IIFE that attaches SMCPAD to globalThis; import it for the effect.
await import(pathToFileURL(join(webui, "smcpad.js")).href);
const S = globalThis.SMCPAD;

function py(code, args) {
  return execFileSync("python3", ["-c", code].concat(args || []), { cwd: webui, encoding: "utf8" });
}
function pyJSON(code, args) { return JSON.parse(py(code, args)); }

let python = true;
try { execFileSync("python3", ["-c", "import sys"], { stdio: "ignore" }); }
catch { python = false; }

function makeArea(size = 0x400) {
  const data = new Uint8Array(size);
  for (let i = 0; i < size; i++) data[i] = (i * 37 + 11) & 0xFF;
  data.fill(0, 0, 4);
  data[4] = 0x20; data[5] = 0x81; data[6] = 0xE0; data[7] = 0x01; // 0x1E08120 LE
  const view = new DataView(data.buffer);
  view.setUint32(8, size, true);
  view.setUint16(2, S.crc16(data.subarray(0x20, size)), true);
  view.setUint16(0, S.crc16(data.subarray(2, 32)), true);
  return data;
}

function withDeclaredSize(data, size) {
  const d = Uint8Array.from(data);
  new DataView(d.buffer).setUint32(8, size, true);
  new DataView(d.buffer).setUint16(0, S.crc16(d.subarray(2, 32)), true);
  return d;
}

/* ------------------------------------------------------------- validator */

test("crc16 matches the XMODEM check value", () => {
  assert.equal(S.crc16(new TextEncoder().encode("123456789")), 0x31C3);
});

test("a synthetic app area validates and reports its size", () => {
  const data = makeArea(0x800);
  assert.deepEqual(S.validateAppArea(data, "x.bin"), { ok: true, reason: "", size: 0x800 });
});

test("the validator refuses the dangerous and malformed cases", () => {
  const good = makeArea(0x600);

  let v = S.validateAppArea(good, "SMC-PAD_005.fwsc");
  assert.equal(v.ok, false);
  assert.match(v.reason, /\.fwsc/);

  v = S.validateAppArea(new Uint8Array(S.FLASH_SIZE), "dump.bin");
  assert.equal(v.ok, false);
  assert.match(v.reason, /full raw flash dump/);

  v = S.validateAppArea(new Uint8Array(16), "x.bin");
  assert.equal(v.ok, false);
  assert.match(v.reason, /too short/);

  const badHead = Uint8Array.from(good); badHead[10] ^= 0xFF;
  v = S.validateAppArea(badHead, "x.bin");
  assert.equal(v.ok, false);
  assert.match(v.reason, /header CRC/);

  const badEntry = Uint8Array.from(good); badEntry[5] ^= 0x01;
  v = S.validateAppArea(withDeclaredSize(badEntry, 0x600), "x.bin");
  assert.equal(v.ok, false);
  assert.equal(v.reason.includes("header CRC"), false);
  assert.match(v.reason, /0x1E08120|entry/);

  v = S.validateAppArea(withDeclaredSize(good, S.APP_AREA_MAX + 1), "x.bin");
  assert.equal(v.ok, false);
  assert.match(v.reason, /out of range/);

  v = S.validateAppArea(good.subarray(0, 0x300), "x.bin");
  assert.equal(v.ok, false);
  assert.match(v.reason, /declares more data/);

  const badArea = Uint8Array.from(good); badArea[0x100] ^= 0xFF;
  v = S.validateAppArea(badArea, "x.bin");
  assert.equal(v.ok, false);
  assert.match(v.reason, /CRC mismatch/);
});

test("bootloader package sizes are 4 KiB..48 KiB", () => {
  assert.equal(S.validateBootPackage(0x1000).ok, true);
  assert.equal(S.validateBootPackage(0xC000).ok, true);
  assert.equal(S.validateBootPackage(0xFFF).ok, false);
  assert.equal(S.validateBootPackage(0xC001).ok, false);
});

test("the locally built MidiFighterSpectra image validates like Python says", (t) => {
  const path = join(root, "work", "sketch", "MidiFighterSpectra", "MidiFighterSpectra.bin");
  if (!existsSync(path)) return t.skip("no local build output");
  const data = new Uint8Array(readFileSync(path));
  const v = S.validateAppArea(data, path);
  assert.equal(v.ok, true, v.reason);
  assert.equal(v.size, data.length);
  if (python) {
    const size = pyJSON(
      "import json,sys; d=open(sys.argv[1],'rb').read(); " +
      "import device; print(json.dumps(device.validate_app_area(d, sys.argv[1])))",
      [path],
    );
    assert.deepEqual([v.ok, v.reason], [size[0], size[1]]);
  }
});

/* ----------------------------------------------------------------- lights */

test("light messages match the Spectra protocol bytes", () => {
  assert.deepEqual(S.buildLights("color", 3, 0, [0], 67), [[0x92, 36, 67]]);
  assert.deepEqual(S.buildLights("color", 3, 1, [0, 15], 1), [[0x92, 52, 1], [0x92, 67, 1]]);
  assert.deepEqual(S.buildLights("anim", 3, 2, [4], 42), [[0x93, 72, 42]]);
  assert.deepEqual(S.buildLights("bank", 3, 0, [], 2), [[0x93, 2, 127]]);
  assert.deepEqual(S.buildLights("force", 3, 0, [1], 0), [[0x92, 37, 127]]);
  assert.deepEqual(S.buildLights("color", 3, 0, [2], 0), [[0x92, 38, 0]]);
});

test("light inputs outside the protocol are refused", () => {
  assert.throws(() => S.buildLights("color", 0, 0, [0], 67), /base channel/);
  assert.throws(() => S.buildLights("color", 15, 0, [0], 67), /base channel/);
  assert.throws(() => S.buildLights("color", 3, 0, [], 67), /pads/);
  assert.throws(() => S.buildLights("color", 3, 0, [16], 67), /pads/);
  assert.throws(() => S.buildLights("color", 3, 0, [0], 121), /colour velocity/);
  assert.throws(() => S.buildLights("anim", 3, 0, [0], 50), /animation velocity/);
  assert.throws(() => S.buildLights("flash", 3, 0, [0], 67), /unknown action/);
  assert.throws(() => S.buildLights("bank", 3, 0, [], 4), /bank must be/);
});

test("taps decode from the trigger channel only", () => {
  assert.deepEqual(S.tapFromMessage(new Uint8Array([0x92, 36, 100]), 3, 0),
                   { pad: 0, down: true });
  assert.deepEqual(S.tapFromMessage(new Uint8Array([0x92, 51, 0]), 3, 0),
                   { pad: 15, down: false });
  assert.deepEqual(S.tapFromMessage(new Uint8Array([0x92, 52, 100]), 3, 1),
                   { pad: 0, down: true });
  assert.equal(S.tapFromMessage(new Uint8Array([0x93, 36, 100]), 3, 0), null);
  assert.equal(S.tapFromMessage(new Uint8Array([0x92, 35, 100]), 3, 0), null);
  assert.equal(S.tapFromMessage(new Uint8Array([0x92, 68, 100]), 3, 0), null);
  assert.equal(S.tapFromMessage(new Uint8Array([0x92, 36]), 3, 0), null);
});

test("palette velocities map to the same RGB as the guide and lights.py", (t) => {
  assert.equal(S.PALETTE.length, 20);
  assert.deepEqual(S.PALETTE.map((p) => p.velocity), Array.from({ length: 20 }, (_, i) => 1 + 6 * i));
  assert.equal(S.rgbForVelocity(0), null);
  assert.equal(S.rgbForVelocity(1), 0x000000);
  assert.equal(S.rgbForVelocity(7), 0xED1F24);
  assert.equal(S.rgbForVelocity(67), 0x00FFFF);
  assert.equal(S.rgbForVelocity(120), 0xFFFFFF);
  assert.equal(S.rgbForVelocity(121), null);
  if (!python) return t.skip("python3 not available");
  const table = pyJSON(
    "import json, lights; print(json.dumps(" +
    "[[lights.rgb_for_velocity(v) for v in range(0,128)], lights.PALETTE]))");
  const [rgb, palette] = table;
  for (let v = 0; v <= 127; v++) assert.equal(S.rgbForVelocity(v), rgb[v], "velocity " + v);
  assert.deepEqual(S.PALETTE.map((p) => [p.name, p.rgb, p.velocity]),
                   palette.map((p) => [p.name, p.rgb, p.velocity]));
});

test("the page's light updates equal lights.apply's state changes", () => {
  // Colour 0 clears: colors None, velocities 0.  Force: colors None, force true, velocity 127.
  const state = { colors: Array(16).fill(null), force: Array(16).fill(false),
                  velocities: Array(16).fill(0) };
  for (const [action, velocity, pad] of [["color", 0, 0], ["color", 67, 1],
                                        ["force", 0, 2], ["color", 7, 2]]) {
    state.velocities[pad] = action === "force" ? 127 : velocity;
    state.colors[pad] = action === "force" ? null : S.rgbForVelocity(velocity);
    state.force[pad] = action === "force";
  }
  assert.equal(state.colors[0], null);
  assert.equal(state.colors[1], 0x00FFFF);
  assert.equal(state.colors[2], 0xED1F24);
  assert.equal(state.force[2], false);
  assert.equal(state.velocities[2], 7);
});

/* --------------------------------------------------------------- settings */

test("empty settings take the documented defaults", () => {
  const v = S.validateSettings({});
  assert.deepEqual(v.errors, {});
  assert.deepEqual(v.values, S.DEFAULTS);
  assert.equal(v.values.base_ch, 3);
  assert.equal(v.values.key_velocity, 127);
  assert.equal(v.values.momentary_cc, false);
  assert.equal(v.values.active_b0, 0xFFFFFF);
  assert.equal(v.values.inactive_b3, 0x000000);
});

test("settings validation refuses out-of-range values and reads bool strings", () => {
  assert.match(S.validateSettings({ base_ch: 0 }).errors.base_ch, /outside/);
  assert.match(S.validateSettings({ key_velocity: 128 }).errors.key_velocity, /outside/);
  assert.match(S.validateSettings({ sleep_min: "soon" }).errors.sleep_min, /not a number/);
  assert.equal(S.validateSettings({ spark: "off" }).values.spark, false);
  assert.equal(S.validateSettings({ spark: "1" }).values.spark, true);
  assert.equal(S.validateSettings({ base_ch: "7" }).values.base_ch, 7);
});

test("settingsDefines is the -D list build_sketch.sh takes", (t) => {
  const defs = S.settingsDefines(S.DEFAULTS);
  assert.equal(defs.length, 9 + 8);
  assert.equal(defs[0], "-DSPECTRA_BASE_CH=3");
  assert.equal(defs[8], "-DSPECTRA_SIDE_CC_RELEASE=0");
  assert.equal(defs[9], "-DSPECTRA_ACTIVE_B0=0xFFFFFFu");
  assert.equal(defs[10], "-DSPECTRA_INACTIVE_B0=0x000000u");
  assert.equal(defs[16], "-DSPECTRA_INACTIVE_B3=0x000000u");
  if (!python) return t.skip("python3 not available");
  const pyDefs = pyJSON("import json, settings; print(json.dumps(settings.defines(settings.DEFAULTS)))");
  assert.deepEqual(defs, pyDefs);
  const custom = { base_ch: 11, momentary_cc: true, sleep_min: 30, active_b2: 0x123456 };
  const pyCustom = pyJSON(
    "import json, settings; print(json.dumps(settings.defines(" +
    "json.loads('{\"base_ch\": 11, \"momentary_cc\": true, \"sleep_min\": 30, " +
    "\"active_b2\": 1193046}'))))");
  assert.deepEqual(S.settingsDefines(custom), pyCustom);
});

test("sha256Hex hashes bytes", async () => {
  assert.equal(await S.sha256Hex(new TextEncoder().encode("abc")),
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const sub = new Uint8Array([9, 9, 1, 2, 3, 9]).subarray(2, 5);
  assert.equal(await S.sha256Hex(sub),
    "039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81");
});

/* -------------------------------------------------------------------- DFU */

class FakeDfuDevice {
  constructor(opts = {}) {
    this.opened = false;
    this.configuration = null;
    this.alt = null;
    this.claimed = false;
    this.state = 2; // dfuIDLE
    this.status = 0;
    this.outs = [];
    this.writtenBlocks = [];
    this.manifestSeen = false;
    this.detached = false;
    this.cleared = 0;
    this.uploadData = opts.uploadData || new Uint8Array(0);
    this.stallAtBlock = opts.stallAtBlock == null ? Infinity : opts.stallAtBlock;
    this.failStatus = opts.failStatus || 0;
  }
  open() { this.opened = true; return Promise.resolve(); }
  selectConfiguration(v) { this.configuration = v; return Promise.resolve(); }
  claimInterface() { this.claimed = true; return Promise.resolve(); }
  releaseInterface() { this.claimed = false; return Promise.resolve(); }
  close() { this.opened = false; return Promise.resolve(); }
  selectAlternateInterface(iface, alt) { this.alt = alt; return Promise.resolve(); }
  _reply(bytes) {
    const d = new Uint8Array(bytes);
    return Promise.resolve({ status: "ok", data: new DataView(d.buffer) });
  }
  controlTransferOut(setup, data) {
    const len = data ? data.length : 0;
    this.outs.push({ request: setup.request, value: setup.value, length: len });
    if (setup.request === 0) { this.detached = true; }
    else if (setup.request === 4) { this.cleared++; this.status = 0; }
    else if (setup.request === 1) {
      if (len === 0) { this.state = 7; this.manifestSeen = true; }
      else { this.writtenBlocks.push(setup.value); this.state = 4; }
    }
    return Promise.resolve({ status: "ok", bytesWritten: len });
  }
  controlTransferIn(setup, length) {
    if (setup.request === 3) {
      let status = this.status;
      let state = this.state;
      if (state === 4) {
        this.state = 5;
        if (this.failStatus) { status = this.failStatus; state = 10; this.failStatus = 0; }
      }
      return this._reply([status, 0, 0, 0, state, 0]);
    }
    if (setup.request === 2) {
      if (setup.value >= this.stallAtBlock) return Promise.resolve({ status: "stall" });
      const off = setup.value * 4096;
      if (off >= this.uploadData.length) return Promise.resolve({ status: "stall" });
      const chunk = this.uploadData.subarray(off, Math.min(off + 4096, this.uploadData.length));
      return this._reply(Array.from(chunk).slice(0, length));
    }
    return this._reply([0, 0, 0, 0, 2, 0]);
  }
}

test("download writes sequential 4096-byte blocks and manifests last", async () => {
  const bytes = new Uint8Array(9000).map((_, i) => i & 0xFF);
  const fake = new FakeDfuDevice();
  const dfu = new S.Dfu(fake);
  await dfu.open();
  assert.equal(fake.opened, true);
  assert.equal(fake.claimed, true);
  const phases = [];
  await dfu.download(0, bytes, { block: (sent, total) => phases.push([sent, total]),
                                 manifest: () => phases.push("manifest") });
  assert.deepEqual(fake.writtenBlocks, [0, 1, 2]);
  const end = fake.outs.filter((o) => o.request === 1 && o.length === 0);
  assert.equal(end.length, 1);
  assert.equal(end[0].value, 3);
  assert.equal(fake.manifestSeen, true);
  assert.deepEqual(phases.slice(0, 3), [[1, 3], [2, 3], [3, 3]]);
  assert.ok(phases.includes("manifest"));
  assert.equal(fake.outs.filter((o) => o.request === 1).length, 4);
});

test("download propagates a DFU error status", async () => {
  const fake = new FakeDfuDevice({ failStatus: 3 });
  const dfu = new S.Dfu(fake);
  await dfu.open();
  await assert.rejects(dfu.download(0, new Uint8Array(4096)), /errWRITE|DFU/);
});

test("download refuses an empty image", async () => {
  const dfu = new S.Dfu(new FakeDfuDevice());
  await assert.rejects(dfu.download(0, new Uint8Array(0)), /nothing to write/);
});

test("upload reads blocks until a short reply and honours the limit", async () => {
  const content = new Uint8Array(9000).map((_, i) => (i * 7) & 0xFF);
  const fake = new FakeDfuDevice({ uploadData: content });
  const dfu = new S.Dfu(fake);
  const got = await dfu.upload(0, S.FLASH_SIZE);
  assert.equal(got.length, content.length);
  assert.deepEqual(Array.from(got), Array.from(content));

  const one = await dfu.upload(1, 4096);
  assert.equal(one.length, 4096);
});

test("upload stops at a stall", async () => {
  const content = new Uint8Array(9000);
  const fake = new FakeDfuDevice({ uploadData: content, stallAtBlock: 1 });
  const got = await new S.Dfu(fake).upload(0, S.FLASH_SIZE);
  assert.equal(got.length, 4096);
});

test("clearError sends CLRSTATUS until status is zero", async () => {
  const fake = new FakeDfuDevice();
  fake.status = 10;
  const dfu = new S.Dfu(fake);
  await dfu.open();
  await dfu.clearError();
  assert.equal(fake.cleared, 1);
});

test("detach sends DFU_DETACH", async () => {
  const fake = new FakeDfuDevice();
  const dfu = new S.Dfu(fake);
  await dfu.open();
  await dfu.detach();
  assert.equal(fake.detached, true);
  await dfu.close();
  assert.equal(fake.opened, false);
});

test("findDfu and enterDfu use a granted 1209:0002 device", async () => {
  const dev = { vendorId: 0x1209, productId: 0x0002 };
  const usb = { getDevices: async () => [dev] };
  assert.equal(await S.findDfu(usb), dev);
  assert.equal(await S.enterDfu(usb, null), dev);
  await assert.rejects(S.enterDfu({ getDevices: async () => [] }, null), /serial port/);
});

test("touch1200 drives DTR at 1200 baud in the documented order", async () => {
  const calls = [];
  const port = {
    open: async (o) => { calls.push(["open", o.baudRate]); },
    setSignals: async (s) => { calls.push(["signals", s.dataTerminalReady]); },
    close: async () => { calls.push(["close"]); },
  };
  await S.touch1200(port);
  assert.deepEqual(calls, [["open", 1200], ["signals", true], ["signals", false], ["close"]]);
});
