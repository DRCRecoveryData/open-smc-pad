#!/usr/bin/env python3
"""Local web UI for the SMC-PAD Pocket: backup, restore, firmware upload.

    python3 host/webui/server.py [--port 8765] [--offline]

The page is served on 127.0.0.1 only and shells out to dfu-util; it never writes
sector 0 and only accepts application app areas (or, for the advanced action,
bootloader update packages).  Backups land in work/webui/backups/.
"""
import argparse
import atexit
import hashlib
import json
import mimetypes
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import device  # noqa: E402
import lights  # noqa: E402
import settings as smcpad_settings  # noqa: E402

MAX_UPLOAD = 512 * 1024
STATIC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "index.html")
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")
BUILD_DIR = os.path.join(device.ROOT, "work", "webui", "build", "MidiFighterSpectra")
LOCKFILE = os.path.join(device.ROOT, "work", "webui", "server.lock")

# The GitHub Pages copy of index.html may call this server; everything else stays refused.
PAGES_ORIGINS = {o.strip().rstrip("/") for o in os.environ.get(
    "SMCPAD_WEBUI_ORIGINS", "https://drcrecoverydata.github.io").split(",") if o.strip()}


def allowed_origins(port):
    return {f"http://127.0.0.1:{port}", f"http://localhost:{port}"} | PAGES_ORIGINS


def origin_allowed(origin, port):
    return not origin or origin in allowed_origins(port)

DEVICE_LOCK = threading.Lock()
STATE_LOCK = threading.Lock()
JOBS = {}
NEXT_ID = [1]
UPLOAD_SEQ = [0]
CURRENT = [None]
Q = queue.Queue()
OFFLINE = False
STOP = threading.Event()
UPLOADS = {}


class JobError(Exception):
    pass


# ---------------------------------------------------------------- jobs

class Job:
    def __init__(self, jid, op, params):
        self.id = jid
        self.op = op
        self.params = params
        self.state = "queued"
        self.detail = ""
        self.progress = None
        self.lines = []
        self.started = time.time()
        self.ended = None
        self.result = None
        self.error = None
        self.abort = False

    def log(self, text):
        for line in str(text).replace("\r", "\n").splitlines():
            if line.strip():
                self.lines.append(line)

    def set(self, state, detail=""):
        self.state = state
        self.detail = detail

    def check_abort(self):
        if self.abort:
            raise JobError("aborted")

    def brief(self):
        return {"id": self.id, "op": self.op, "state": self.state,
                "detail": self.detail, "progress": self.progress,
                "started": self.started, "ended": self.ended,
                "result": self.result, "error": self.error,
                "lines": len(self.lines)}

    def full(self):
        d = self.brief()
        d["log"] = self.lines
        return d


def stamp():
    return time.strftime("%Y%m%d-%H%M%S")


def new_job(op, params):
    global NEXT_ID
    with STATE_LOCK:
        cur = CURRENT[0]
        if cur is not None and cur.state not in ("done", "error"):
            return None, cur
        job = Job(NEXT_ID[0], op, params)
        NEXT_ID[0] += 1
        JOBS[job.id] = job
        for old in sorted(JOBS)[:-20]:
            del JOBS[old]
        CURRENT[0] = job
        Q.put(job.id)
        return job, None


# ---------------------------------------------------------------- dfu-util

def dfu(job, args, timeout=240):
    """Run dfu-util for our device; log output, track progress, return (rc, text)."""
    cmd = ["dfu-util", "-d", device.DFU_ID] + args
    job.log("$ " + " ".join(cmd))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        text = (r.stdout or "") + (r.stderr or "")
        rc = r.returncode
    except subprocess.TimeoutExpired:
        job.log("dfu-util timed out")
        return 1, ""
    for line in text.replace("\r", "\n").splitlines():
        if not line.strip() or "DFU suffix" in line:
            continue
        if line.startswith("dfu-util:") and "Warning" in line:
            continue
        job.log(line)
        m = re.search(r"(\d+)%", line)
        if m:
            job.progress = int(m.group(1)) / 100.0
    return rc, text


def ensure_dfu(job):
    if OFFLINE:
        raise JobError("server started with --offline")
    job.set("entering_dfu", "checking the pad")
    info = device.classify()
    mode = info["mode"]
    if mode == "dfu":
        job.log("the bootloader is already up")
        return
    if mode == "mixed":
        raise JobError("more than one SMC-PAD is on USB; unplug all but one")
    if mode == "stock":
        raise JobError("the pad runs the factory firmware (4353:4b4d): the bootloader "
                       "is not booted, so DFU is unavailable")
    if mode == "absent":
        job.log("no pad on USB yet, waiting up to 10 s")
        if not device.wait_until(lambda: device.classify()["mode"] in ("app", "dfu"), 10):
            raise JobError("neither the bootloader (1209:0002) nor the application is on "
                           "USB; is the pad switched on?")
        return ensure_dfu(job)

    port = device.app_port()
    if not port:
        raise JobError("the application is on USB but its serial port was not found")
    job.log(f"asking the application on {port} into the bootloader (1200-baud touch)")
    try:
        device.touch_1200(port)
    except PermissionError:
        raise JobError(f"no access to {port}: install the udev rule with "
                       f"host/install-udev.sh (or add the user to group dialout)")
    except OSError as exc:
        job.log(f"touch failed: {exc}")
    if device.wait_until(device.dfu_present, 10):
        return
    job.log("touch did not take, sending the console's B command")
    try:
        device.console_B(port)
    except OSError as exc:
        job.log(f"console B failed: {exc}")
    if not device.wait_until(device.dfu_present, 10):
        raise JobError("the bootloader did not show up after the 1200-baud touch")


def wait_app(job, timeout=30):
    job.set("waiting_app", "waiting for the application")
    if device.wait_until(lambda: device.app_port() is not None, timeout):
        return
    if device.dfu_present():
        ver = device.boot_version()
        raise JobError(f"the pad is still in the bootloader" + (f" ({ver})" if ver else ""))
    raise JobError("neither the application nor the bootloader is on USB")


def leave_dfu(job, timeout=30):
    job.set("detach", "leaving the bootloader")
    dfu(job, ["-a", "0", "-e"], timeout=30)
    wait_app(job, timeout)


# ---------------------------------------------------------------- operations

def new_tmpdir(job):
    path = os.path.join(device.TMP, str(job.id))
    os.makedirs(path, exist_ok=True)
    return path


def check_app_file(path, name):
    with open(path, "rb") as f:
        data = f.read()
    ok, why = device.validate_app_area(data, name)
    if not ok:
        raise JobError(f"not a flashable app area: {why}")
    return data


def op_backup_app(job):
    ensure_dfu(job)
    job.check_abort()
    job.set("reading", "reading the application area")
    out = os.path.join(new_tmpdir(job), "app.bin")
    rc, _ = dfu(job, ["-a", "0", "-U", out], timeout=120)
    if not os.path.isfile(out) or os.path.getsize(out) == 0:
        raise JobError(f"dfu-util returned no data (rc {rc})")
    data = open(out, "rb").read()
    ok, why = device.validate_app_area(data)
    if not ok:
        job.log(f"warning: saved image does not validate: {why}")
    os.makedirs(device.BACKUPS, exist_ok=True)
    name = f"app_{stamp()}.bin"
    os.replace(out, os.path.join(device.BACKUPS, name))
    job.result = {"file": name, "bytes": len(data),
                  "sha256": hashlib.sha256(data).hexdigest(), "valid": ok}
    leave_dfu(job)
    job.log(f"saved backups/{name} ({len(data)} bytes)")


def op_backup_full(job):
    ensure_dfu(job)
    job.check_abort()
    job.set("reading", "reading the whole flash (512 KiB)")
    out = os.path.join(new_tmpdir(job), "flash.bin")
    rc, _ = dfu(job, ["-a", "1", "-U", out], timeout=180)
    size = os.path.getsize(out) if os.path.isfile(out) else 0
    if size != device.FLASH_SIZE:
        raise JobError(f"read {size} bytes, expected {device.FLASH_SIZE} (rc {rc})")
    data = open(out, "rb").read()
    os.makedirs(device.BACKUPS, exist_ok=True)
    name = f"flash_{stamp()}.bin"
    os.replace(out, os.path.join(device.BACKUPS, name))
    job.result = {"file": name, "bytes": size,
                  "sha256": hashlib.sha256(data).hexdigest()}
    leave_dfu(job)
    job.log(f"saved backups/{name} ({size} bytes)")


def flash_app(job, path, name):
    data = check_app_file(path, name)
    job.check_abort()
    ensure_dfu(job)
    job.check_abort()
    job.set("writing", f"writing {name}")
    rc, text = dfu(job, ["-a", "0", "-D", path], timeout=240)
    if "dfuERROR" in text:
        raise JobError("the bootloader refused the image; nothing changed and the pad "
                       "stays in the bootloader")
    if rc != 0:
        raise JobError(f"dfu-util failed (rc {rc}); the pad stays in the bootloader")
    wait_app(job)
    job.result = {"file": name, "bytes": len(data),
                  "sha256": hashlib.sha256(data).hexdigest()}
    job.log(f"{name} flashed; the application is back")


def op_restore(job):
    name = job.params.get("file", "")
    path = device.safe_path("backups", name)
    if not path:
        raise JobError("no such backup")
    flash_app(job, path, name)


def op_upload(job):
    uid = job.params.get("upload_id", "")
    entry = UPLOADS.get(uid)
    if not entry or not os.path.isfile(entry["path"]):
        raise JobError("the uploaded file is gone; upload it again")
    flash_app(job, entry["path"], entry["name"])
    os.makedirs(device.BACKUPS, exist_ok=True)
    keep = os.path.join(device.BACKUPS, f"app_{stamp()}_uploaded.bin")
    shutil.copyfile(entry["path"], keep)
    job.log(f"kept a copy as backups/{os.path.basename(keep)}")
    job.result["backup"] = os.path.basename(keep)


def op_boot_update(job):
    src = job.params.get("source", "builtin")
    if src == "builtin":
        path, name = device.BOOT_PACKAGE, os.path.basename(device.BOOT_PACKAGE)
    elif src.startswith("upload:"):
        entry = UPLOADS.get(src.split(":", 1)[1])
        if not entry or not os.path.isfile(entry["path"]):
            raise JobError("the uploaded package is gone; upload it again")
        path, name = entry["path"], entry["name"]
    else:
        raise JobError("unknown source")
    if not os.path.isfile(path):
        raise JobError(f"missing package {path}")
    size = os.path.getsize(path)
    if not 0x1000 <= size <= 0xC000:
        raise JobError(f"package is {size} bytes, expected 4 KiB..48 KiB")
    ensure_dfu(job)
    job.check_abort()
    before = device.boot_version()
    job.log(f"bootloader before: {before}")
    job.set("writing", f"writing {name}")
    rc, text = dfu(job, ["-a", "2", "-D", path], timeout=300)
    if "dfuERROR" in text:
        raise JobError("the bootloader refused the package; nothing changed")
    if rc != 0 and not ("dfuMANIFEST" in text and "dfuERROR" not in text):
        raise JobError(f"dfu-util failed (rc {rc}); the pad stays in the bootloader")
    job.log("package accepted; the bootloader is running the installer")
    wait_app(job, 30)
    time.sleep(4)
    job.set("waiting_app", "checking the new bootloader version")
    try:
        device.touch_1200(device.app_port())
    except OSError:
        pass
    if not device.wait_until(device.dfu_present, 15):
        raise JobError("the bootloader did not come up for the version check")
    after = device.boot_version()
    leave_dfu(job)
    job.log(f"bootloader now: {after}")
    job.result = {"before": before, "after": after}


def op_apply_settings(job):
    clean, errors = smcpad_settings.validate(job.params.get("settings") or {})
    if errors:
        raise JobError("; ".join(errors[k] for k in sorted(errors)))
    smcpad_settings.save(clean)
    job.check_abort()
    job.set("building", "compiling MidiFighterSpectra with the new settings")
    defs = smcpad_settings.defines(clean)
    sketch = os.path.join(device.ROOT, "arduino", "sketches", "MidiFighterSpectra")
    env = dict(os.environ, SMCPAD_DEFS=" ".join(defs), SMCPAD_OUT=BUILD_DIR)
    job.log("$ host/build_sketch.sh arduino/sketches/MidiFighterSpectra")
    try:
        p = subprocess.run(["bash", os.path.join(device.ROOT, "host", "build_sketch.sh"),
                            sketch],
                           capture_output=True, text=True, timeout=900, env=env)
    except subprocess.TimeoutExpired:
        raise JobError("the build timed out after 15 minutes")
    for line in ((p.stdout or "") + (p.stderr or "")).splitlines()[-40:]:
        if line.strip():
            job.log(line)
    if p.returncode != 0:
        raise JobError("the build failed; the pad was not touched")
    path = os.path.join(BUILD_DIR, "MidiFighterSpectra.bin")
    if not os.path.isfile(path):
        raise JobError("the build produced no MidiFighterSpectra.bin")
    data = check_app_file(path, "MidiFighterSpectra.bin")
    job.check_abort()
    job.log(f"built {len(data)} bytes from {len(defs)} defines")
    flash_app(job, path, "MidiFighterSpectra.bin")
    os.makedirs(device.BACKUPS, exist_ok=True)
    keep = os.path.join(device.BACKUPS, f"app_{stamp()}_settings.bin")
    shutil.copyfile(path, keep)
    job.log(f"kept a copy as backups/{os.path.basename(keep)}")
    job.result = dict(job.result or {}, settings=clean,
                      backup=os.path.basename(keep))


def op_detach(job):
    if OFFLINE:
        raise JobError("server started with --offline")
    if not device.dfu_present():
        if device.classify()["mode"] == "app":
            job.log("the application is already running")
            job.result = {"mode": "app"}
            return
        raise JobError("the pad is not in the bootloader")
    leave_dfu(job)
    job.result = {"mode": "app"}


OPS = {"backup_app": op_backup_app, "backup_full": op_backup_full,
       "restore": op_restore, "upload": op_upload,
       "boot_update": op_boot_update, "detach": op_detach,
       "apply_settings": op_apply_settings}
WRITE_OPS = {"restore", "upload", "boot_update", "apply_settings"}


def run_job(job):
    tmp = os.path.join(device.TMP, str(job.id))
    failed = None
    try:
        with DEVICE_LOCK:
            OPS[job.op](job)
    except JobError as exc:
        failed = str(exc)
    except Exception as exc:                      # keep the worker alive
        failed = f"internal error: {exc}"
    finally:
        if failed:
            job.error = failed
            job.log(f"error: {failed}")
            if job.op not in WRITE_OPS and device.dfu_present() and not STOP.is_set():
                try:
                    job.log("leaving the bootloader")
                    leave_dfu(job)
                except JobError as exc:
                    job.log(f"could not leave DFU: {exc}")
            job.set("error", failed)
        else:
            job.result = job.result or {}
            job.set("done", "done")
        job.ended = time.time()
        shutil.rmtree(tmp, ignore_errors=True)


def worker():
    while True:
        jid = Q.get()
        if jid is None:
            break
        job = JOBS.get(jid)
        if job is not None:
            run_job(job)


# ---------------------------------------------------------------- status/files

def device_status():
    if OFFLINE:
        return {"mode": "offline", "id": "", "product": "", "serial": "",
                "port": None, "port_ok": False, "midi": None, "boot_version": None}
    with DEVICE_LOCK:
        info = device.classify()
        out = dict(info)
        out["port"] = device.app_port() if info["mode"] == "app" else None
        out["port_ok"] = bool(out["port"] and os.access(out["port"], os.W_OK))
        out["midi"] = device.midi_port() if info["mode"] == "app" else None
        out["boot_version"] = device.boot_version() if info["mode"] == "dfu" else None
    return out


def current_job():
    with STATE_LOCK:
        return CURRENT[0]


def status_payload():
    job = current_job()
    busy = job is not None and job.state not in ("done", "error")
    if busy and job.state != "done":
        dev = {"mode": "busy", "id": "", "product": "", "serial": "",
               "port": None, "port_ok": False, "midi": None, "boot_version": None}
    else:
        dev = device_status()
    return {"device": dev, "job": job.brief() if job else None, "busy": busy,
            "server": {"offline": OFFLINE}}


def artifacts_payload():
    def info(path):
        if not os.path.isfile(path):
            return {"path": os.path.relpath(path, device.ROOT), "ok": False}
        st = os.stat(path)
        return {"path": os.path.relpath(path, device.ROOT), "ok": True,
                "bytes": st.st_size, "mtime": int(st.st_mtime)}
    return {"boot_update": info(device.BOOT_PACKAGE),
            "known_good_app": info(device.KNOWN_APP),
            "boot_source_mtime": int(os.stat(os.path.join(
                device.ROOT, "fw", "boot", "out", "boot.bin")).st_mtime)
            if os.path.isfile(os.path.join(device.ROOT, "fw", "boot", "out", "boot.bin"))
            else None}


# ---------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    server_version = "smcpad-webui/1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def cors_origin(self):
        origin = self.headers.get("Origin")
        return origin if origin and origin_allowed(origin, self.server.server_address[1]) else None

    def end_headers(self):
        origin = self.cors_origin()
        if origin:
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
        super().end_headers()

    def send_error_json(self, code, msg, **extra):
        self.send_json(code, {"error": msg, **extra})

    def same_origin(self):
        origin = self.headers.get("Origin")
        if origin:
            if origin not in allowed_origins(self.server.server_address[1]):
                return False
            return True
        site = self.headers.get("Sec-Fetch-Site")
        if site and site not in ("same-origin", "none"):
            return False
        return True

    def do_OPTIONS(self):
        if not self.cors_origin():
            return self.send_error_json(403, "cross-origin request refused")
        self.send_response(204)
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Filename")
        self.send_header("Access-Control-Max-Age", "600")
        if self.headers.get("Access-Control-Request-Private-Network"):
            self.send_header("Access-Control-Allow-Private-Network", "true")
        self.end_headers()

    def body_json(self):
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return None
        if n <= 0 or n > 65536:
            return None
        try:
            return json.loads(self.rfile.read(n))
        except (ValueError, json.JSONDecodeError):
            return None

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        path, query = url.path, urllib.parse.parse_qs(url.query)
        if path in ("/", "/index.html"):
            try:
                body = open(STATIC, "rb").read()
            except OSError:
                return self.send_error_json(500, "index.html is missing")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/api/status":
            return self.send_json(200, status_payload())
        if path == "/api/files":
            return self.send_json(200, {"backups": device.list_backups(),
                                        "artifacts": artifacts_payload()})
        if path == "/api/settings":
            return self.send_json(200, smcpad_settings.payload())
        if path == "/api/lights":
            return self.send_json(200, {"lights": lights.snapshot(),
                                        "base_ch": smcpad_settings.load()["base_ch"]})
        m = re.fullmatch(r"/static/([A-Za-z0-9._-]+\.(?:png|jpg|jpeg|svg|webp))", path)
        if m:
            fpath = os.path.join(STATIC_DIR, m.group(1))
            if not os.path.isfile(fpath):
                return self.send_error_json(404, "not found")
            with open(fpath, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type",
                             mimetypes.guess_type(fpath)[0] or "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "max-age=3600")
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/api/files/download":
            name = (query.get("name") or [""])[0]
            fpath = device.safe_path("backups", name)
            if not fpath:
                return self.send_error_json(404, "no such backup")
            body = open(fpath, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        m = re.fullmatch(r"/api/jobs/(\d+)", path)
        if m:
            job = JOBS.get(int(m.group(1)))
            return self.send_json(200, {"job": job.full()}) if job else \
                self.send_error_json(404, "no such job")
        m = re.fullmatch(r"/api/jobs/(\d+)/log", path)
        if m:
            job = JOBS.get(int(m.group(1)))
            if not job:
                return self.send_error_json(404, "no such job")
            since = int(query.get("since", ["0"])[0] or 0)
            lines = job.lines[max(0, since):]
            return self.send_json(200, {"lines": lines, "next": max(0, since) + len(lines),
                                        "state": job.state})
        m = re.fullmatch(r"/api/jobs/(\d+)/download", path)
        if m:
            job = JOBS.get(int(m.group(1)))
            name = (job.result or {}).get("file") if job else None
            fpath = device.safe_path("backups", name) if name else None
            if not fpath:
                return self.send_error_json(404, "no backup for this job")
            body = open(fpath, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error_json(404, "not found")

    def do_POST(self):
        if not self.same_origin():
            return self.send_error_json(403, "cross-origin request refused")
        url = urllib.parse.urlparse(self.path)
        path, query = url.path, urllib.parse.parse_qs(url.query)
        if path == "/api/settings":
            req = self.body_json()
            if not req or not isinstance(req.get("values"), dict):
                return self.send_error_json(400, "bad request")
            clean, errors = smcpad_settings.validate(req["values"])
            if errors:
                return self.send_json(400, {"error": "invalid settings", "fields": errors})
            smcpad_settings.save(clean)
            return self.send_json(200, {"values": clean})
        if path == "/api/upload":
            return self.handle_upload(query)
        if path == "/api/lights":
            return self.handle_lights()
        if path == "/api/jobs":
            return self.handle_new_job()
        m = re.fullmatch(r"/api/jobs/(\d+)/abort", path)
        if m:
            job = JOBS.get(int(m.group(1)))
            if not job:
                return self.send_error_json(404, "no such job")
            job.abort = True
            return self.send_json(200, {"job": job.brief()})
        self.send_error_json(404, "not found")

    def handle_lights(self):
        req = self.body_json()
        if not req or not isinstance(req, dict):
            return self.send_error_json(400, "bad request")
        action = req.get("action")
        pads = req.get("pads", [])
        velocity = req.get("velocity", 0)
        if not isinstance(pads, list) or any(not isinstance(p, int) or isinstance(p, bool)
                                             for p in pads):
            return self.send_error_json(400, "pads must be a list of numbers")
        if not isinstance(velocity, int) or isinstance(velocity, bool):
            return self.send_error_json(400, "velocity must be a number")
        job = current_job()
        if job is not None and job.state not in ("done", "error"):
            return self.send_error_json(409, "an operation is running")
        if OFFLINE:
            return self.send_error_json(409, "server started with --offline")
        with DEVICE_LOCK:
            mode = device.classify()["mode"]
        if mode != "app":
            return self.send_error_json(409, f"the pad is not running the application ({mode})")
        base_ch = smcpad_settings.load()["base_ch"]
        try:
            msgs = lights.apply(action, pads, velocity, base_ch)
        except ValueError as exc:
            return self.send_error_json(400, str(exc))
        except OSError as exc:
            return self.send_error_json(409, f"the MIDI write failed: {exc}")
        return self.send_json(200, {"ok": True, "sent": len(msgs),
                                    "lights": lights.snapshot(), "base_ch": base_ch})

    def handle_new_job(self):
        req = self.body_json()
        if not req or req.get("op") not in OPS:
            return self.send_error_json(400, "unknown operation")
        op = req["op"]
        params = req.get("params") or {}
        if op in WRITE_OPS and not params.get("confirm"):
            return self.send_error_json(400, "confirmation required")
        if op == "restore" and not device.safe_path("backups", params.get("file", "")):
            return self.send_error_json(400, "no such backup")
        if op == "upload" and params.get("upload_id") not in UPLOADS:
            return self.send_error_json(400, "unknown upload")
        if op == "apply_settings":
            _, errors = smcpad_settings.validate(params.get("settings") or {})
            if errors:
                return self.send_json(400, {"error": "invalid settings", "fields": errors})
        job, busy = new_job(op, params)
        if job is None:
            return self.send_error_json(409, "busy", job=busy.brief() if busy else None)
        return self.send_json(201, {"job": job.brief()})

    def handle_upload(self, query):
        kind = (query.get("kind") or ["app"])[0]
        if kind not in ("app", "boot"):
            return self.send_error_json(400, "kind must be app or boot")
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip()
        if ctype not in ("application/octet-stream", ""):
            return self.send_error_json(415, "send raw bytes as application/octet-stream")
        raw_name = self.headers.get("X-Filename", "")
        name = os.path.basename(raw_name)
        if raw_name != name or ".." in name or not device.NAME_RE.match(name):
            return self.send_error_json(400, "file name must be a plain ...bin name")
        if name.lower().endswith(".fwsc"):
            return self.send_error_json(415, "a .fwsc is the vendor container, not an app area")
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        if n <= 0:
            return self.send_error_json(400, "empty upload")
        if n > MAX_UPLOAD:
            return self.send_error_json(413, "file is larger than the 512 KiB flash")
        os.makedirs(device.UPLOADS, exist_ok=True)
        with STATE_LOCK:
            UPLOAD_SEQ[0] += 1
            uid = f"u{UPLOAD_SEQ[0]}"
        dest = os.path.join(device.UPLOADS, f"{uid}_{name}")
        try:
            with open(dest, "wb") as f:
                left = n
                while left:
                    chunk = self.rfile.read(min(65536, left))
                    if not chunk:
                        raise OSError("short read")
                    f.write(chunk)
                    left -= len(chunk)
        except OSError as exc:
            try:
                os.unlink(dest)
            except OSError:
                pass
            return self.send_error_json(400, f"upload failed: {exc}")
        info = {"upload_id": uid, "name": name, "bytes": n, "kind": kind,
                "valid_app_area": None}
        if kind == "app":
            ok, why = device.validate_app_area(open(dest, "rb").read(), name)
            info["valid_app_area"], info["reason"] = ok, why
        else:
            info["valid_app_area"] = 0x1000 <= n <= 0xC000
            info["reason"] = "" if info["valid_app_area"] else "package size out of range"
        if not info["valid_app_area"]:
            os.unlink(dest)
            return self.send_error_json(400, info["reason"] or "invalid file")
        UPLOADS[uid] = {"name": name, "path": dest, "bytes": n}
        return self.send_json(201, info)


# ---------------------------------------------------------------- main

def acquire_lock():
    os.makedirs(os.path.dirname(LOCKFILE), exist_ok=True)
    try:
        fd = os.open(LOCKFILE, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        try:
            pid = int(open(LOCKFILE).read().strip() or 0)
            os.kill(pid, 0)
            sys.exit(f"another server is running (pid {pid}); remove {LOCKFILE} if stale")
        except (OSError, ValueError):
            os.unlink(LOCKFILE)
            return acquire_lock()
    os.write(fd, str(os.getpid()).encode())
    os.close(fd)
    atexit.register(lambda: os.path.exists(LOCKFILE) and os.unlink(LOCKFILE))
    return LOCKFILE


def main():
    global OFFLINE
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--offline", action="store_true",
                    help="serve the UI without touching the device")
    a = ap.parse_args()
    OFFLINE = a.offline

    acquire_lock()
    os.makedirs(device.BACKUPS, exist_ok=True)
    shutil.rmtree(device.UPLOADS, ignore_errors=True)   # uploads do not survive a restart
    os.makedirs(device.UPLOADS, exist_ok=True)
    threading.Thread(target=worker, daemon=True).start()
    if not OFFLINE:
        lights.start_reader()

    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)

    def stop(_sig, _frame):
        STOP.set()
        threading.Thread(target=srv.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    print(f"smcpad web UI on http://127.0.0.1:{a.port}/"
          + ("  (offline)" if OFFLINE else ""))
    try:
        srv.serve_forever()
    finally:
        Q.put(None)
        srv.server_close()
        print("stopped")


if __name__ == "__main__":
    main()
