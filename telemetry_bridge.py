"""
Animus Engine — Public Live Telemetry Bridge
=============================================

Connects docs/dashboard.html (the public telemetry page) to the REAL
compiled AnimusCore engine (animus/AnimusNative.dll) via the existing
ctypes SDK (animus.bindings). Zero third-party dependencies — the
WebSocket server below is a minimal, from-scratch RFC 6455
implementation using only the standard library, same philosophy as
animus.bindings itself.

This is a separate, git-tracked script from command_center_bridge.py
on purpose: that one feeds the private, gitignored command-center.html
(CRM/financial/roadmap data that must never leave this machine). This
script and docs/dashboard.html carry no such data — they're safe to
run on a box that's reachable from the public internet.

Run from the repo root:

    python telemetry_bridge.py

Then open docs/dashboard.html (or docs/index.html). It connects to
ws://127.0.0.1:8766 by default and renders whatever this process
actually measures. If this script isn't running, the dashboard falls
back to the "OFFLINE — showing verified historical benchmarks" state.
It never invents live numbers.

WHAT IS GENUINELY LIVE (see command_center_bridge.py's own docstring
for the identical reasoning — this script mirrors it):
  - Throughput / per-batch push latency / byte rate: real, timed with
    time.perf_counter_ns() around animus_record_events_batch.
  - Ring-buffer overflow: real (record_events_batch's return value is
    the actual number of events the native ring accepted).
  - Rule-engine fault signals: real, evaluated by the native engine
    against the measured per-batch latency.
  - CPU core count, license status, core-pinning result: real ctypes
    calls against the compiled binary.
  - This process's own working-set memory: real (direct ctypes call
    into the Windows API, no psutil dependency).

WHAT IS EXPLICITLY NOT INSTRUMENTED (reported as such, never invented):
  - Live ring-buffer occupancy / fill percentage — animus_init's ring
    exposes push-accept/reject counts, not a live occupancy query. Any
    "fill %" would be a guess, so it isn't shown as one.
  - L1/L2/L3 cache-miss rates — no hardware performance-counter hook.
  - Live exchange packet loss / kernel-bypass NIC status — no live
    market-data feed connection in this repo, only synthetic benchmark
    generators (adapters/itch50).

PUBLIC EXPOSURE:
  This process binds to 127.0.0.1 by default — it is NOT reachable
  from the internet until you put it behind a tunnel (Cloudflare
  Tunnel, ngrok, etc. — see README / the deployment notes for the
  exact commands). The tunnel terminates TLS, which is what lets an
  https:// page open a wss:// connection to it without a mixed-content
  block. Use --allow-origin to restrict which page origins may connect
  once you have a public URL for the dashboard.
"""

import argparse
import base64
import ctypes
import hashlib
import json
import os
import socket
import statistics
import struct
import sys
import threading
import time
from collections import deque

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)

from animus.bindings import AnimusBindings, LicenseStatus, RuleComparator, NativeEvent  # noqa: E402

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
BATCH_SIZE = 2000
RING_CAPACITY = 65536
LATENCY_RULE_EVENT_ID = 1
LATENCY_RULE_THRESHOLD_NS = 5000
LATENCY_WINDOW = 60  # ~1 minute of rolling samples at 1/sec

EVENT_SIZE_BYTES = ctypes.sizeof(NativeEvent)  # real, from the mirrored C-ABI struct

NOT_INSTRUMENTED = [
    "Live ring-buffer occupancy / fill % (no native occupancy query exported — only push-accept/reject counts)",
    "L1/L2/L3 cache-miss rates (no hardware perf-counter hook in this codebase)",
    "Live exchange packet loss / kernel-bypass NIC status (no live market-data feed connected)",
]

CANDIDATE_LICENSE_PATHS = ["customer.lic", "license.lic", "animus.lic"]


# ---------------------------------------------------------------------
# Minimal RFC 6455 WebSocket server (stdlib only)
# ---------------------------------------------------------------------

def _recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def _recv_headers(conn):
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            return None
        buf += chunk
        if len(buf) > 16384:
            return None
    return buf


def ws_handshake(conn, allowed_origins):
    request = _recv_headers(conn)
    if request is None:
        return False
    headers = {}
    for line in request.split(b"\r\n")[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.strip().lower()] = v.strip()
    if allowed_origins is not None:
        origin = headers.get(b"origin", b"").decode("utf-8", "ignore")
        if origin not in allowed_origins:
            conn.sendall(b"HTTP/1.1 403 Forbidden\r\n\r\n")
            return False
    key = headers.get(b"sec-websocket-key")
    if not key:
        return False
    accept = base64.b64encode(hashlib.sha1(key + WS_GUID.encode()).digest()).decode()
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    )
    conn.sendall(response.encode())
    return True


def ws_send(conn, opcode, payload=b""):
    length = len(payload)
    if length <= 125:
        header = struct.pack("!BB", 0x80 | opcode, length)
    elif length <= 65535:
        header = struct.pack("!BBH", 0x80 | opcode, 126, length)
    else:
        header = struct.pack("!BBQ", 0x80 | opcode, 127, length)
    conn.sendall(header + payload)


def ws_send_text(conn, text):
    ws_send(conn, 0x1, text.encode("utf-8"))


def ws_read_frame(conn):
    hdr = _recv_exact(conn, 2)
    if not hdr:
        return None
    b0, b1 = hdr[0], hdr[1]
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    length = b1 & 0x7F
    if length == 126:
        ext = _recv_exact(conn, 2)
        if ext is None:
            return None
        length = struct.unpack("!H", ext)[0]
    elif length == 127:
        ext = _recv_exact(conn, 8)
        if ext is None:
            return None
        length = struct.unpack("!Q", ext)[0]
    mask_key = _recv_exact(conn, 4) if masked else b""
    if masked and mask_key is None:
        return None
    payload = _recv_exact(conn, length) if length else b""
    if payload is None:
        return None
    if masked and payload:
        payload = bytes(payload[i] ^ mask_key[i % 4] for i in range(len(payload)))
    return opcode, payload


class Client:
    def __init__(self, conn, addr):
        self.conn = conn
        self.addr = addr
        self.alive = True
        self.lock = threading.Lock()

    def send(self, text):
        with self.lock:
            if not self.alive:
                return False
            try:
                ws_send_text(self.conn, text)
                return True
            except OSError:
                self.alive = False
                return False


clients = set()
clients_lock = threading.Lock()


def client_reader(client):
    """Read-only telemetry push — this bridge doesn't need any data
    *from* the browser, so anything other than ping/close/pong is
    ignored."""
    try:
        while True:
            frame = ws_read_frame(client.conn)
            if frame is None:
                break
            opcode, payload = frame
            if opcode == 0x8:  # close
                break
            elif opcode == 0x9:  # ping -> pong
                try:
                    ws_send(client.conn, 0xA, payload)
                except OSError:
                    break
    except OSError:
        pass
    finally:
        client.alive = False
        with clients_lock:
            clients.discard(client)
        try:
            client.conn.close()
        except OSError:
            pass


def accept_loop(server_sock, allowed_origins):
    while True:
        try:
            conn, addr = server_sock.accept()
        except OSError:
            return
        conn.settimeout(None)
        try:
            if not ws_handshake(conn, allowed_origins):
                conn.close()
                continue
        except OSError:
            continue
        client = Client(conn, addr)
        with clients_lock:
            clients.add(client)
        print(f"[bridge] dashboard connected from {addr[0]}:{addr[1]} ({len(clients)} total)")
        threading.Thread(target=client_reader, args=(client,), daemon=True).start()


def broadcast(obj):
    text = json.dumps(obj)
    with clients_lock:
        dead = []
        for c in clients:
            if not c.send(text):
                dead.append(c)
        for c in dead:
            clients.discard(c)


# ---------------------------------------------------------------------
# Windows process memory (real, no psutil)
# ---------------------------------------------------------------------

def get_process_rss_bytes():
    if os.name != "nt":
        return None
    try:
        class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
            _fields_ = [
                ("cb", ctypes.c_uint32),
                ("PageFaultCount", ctypes.c_uint32),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]
        ctypes.windll.kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        ctypes.windll.psapi.GetProcessMemoryInfo.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), ctypes.c_uint32
        ]
        ctypes.windll.psapi.GetProcessMemoryInfo.restype = ctypes.c_int

        counters = PROCESS_MEMORY_COUNTERS()
        counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS)
        handle = ctypes.windll.kernel32.GetCurrentProcess()
        ok = ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb)
        return int(counters.WorkingSetSize) if ok else None
    except Exception:
        return None


def percentile(sorted_samples, pct):
    if not sorted_samples:
        return None
    k = (len(sorted_samples) - 1) * (pct / 100.0)
    f, c = int(k), min(int(k) + 1, len(sorted_samples) - 1)
    if f == c:
        return sorted_samples[f]
    return sorted_samples[f] + (sorted_samples[c] - sorted_samples[f]) * (k - f)


# ---------------------------------------------------------------------
# Real engine telemetry loop
# ---------------------------------------------------------------------

def resolve_license_status(bindings):
    for name in CANDIDATE_LICENSE_PATHS:
        path = os.path.join(REPO_ROOT, name)
        if os.path.isfile(path):
            status = bindings.check_license_status(path)
            return LicenseStatus(status).name, path
    status = bindings.check_license_status(os.path.join(REPO_ROOT, "__no_license_file__.lic"))
    return LicenseStatus(status).name, None


def parse_license_payload(path):
    """Display-only: reads issued_at/expires_at/max_cores straight out
    of the license file's own payload. animus_check_license_status via
    the native RSA verify above remains the sole source of truth for
    VALID/INVALID."""
    try:
        with open(path, "rb") as f:
            payload = f.read(64)
        if len(payload) < 28:
            return None
        _magic, _version, issued_at, expires_at, max_cores = struct.unpack_from("<IIQQI", payload, 0)
        return {"issued_at": issued_at, "expires_at": expires_at, "max_cores": max_cores}
    except OSError:
        return None


def telemetry_loop(tick_seconds):
    bindings = AnimusBindings()
    native = bindings.using_native_engine
    print(f"[bridge] native engine loaded: {native}")

    if not native:
        print("[bridge] FATAL: no compiled AnimusNative binary found -- build it first (see README Quick Start).")

    cpu_count = bindings.get_cpu_count() if native else 0
    license_status_name, license_path = resolve_license_status(bindings) if native else ("UNSUPPORTED_PLATFORM", None)
    license_payload = parse_license_payload(license_path) if license_path else None

    engine_ready = False
    if native:
        engine_ready = bindings.init(RING_CAPACITY)
        if engine_ready:
            null_path = "NUL" if os.name == "nt" else "/dev/null"
            bindings.start_logging(null_path)
            bindings.add_rule(
                rule_id=1,
                event_id=LATENCY_RULE_EVENT_ID,
                threshold=LATENCY_RULE_THRESHOLD_NS,
                comparator=RuleComparator.GREATER_THAN,
                severity=2,
            )

    core_pin_result = bindings.pin_current_thread_to_core(0) if native else False

    latency_samples = deque(maxlen=LATENCY_WINDOW)
    signals_total = 0
    tick = 0

    print(f"[bridge] cpu_count={cpu_count} license={license_status_name} core_pin={core_pin_result}")

    while True:
        tick += 1
        faults = []

        if engine_ready:
            events = [(LATENCY_RULE_EVENT_ID, tick * BATCH_SIZE + i, 0) for i in range(BATCH_SIZE)]
            t0 = time.perf_counter_ns()
            pushed = bindings.record_events_batch(events)
            t1 = time.perf_counter_ns()
            elapsed_ns = t1 - t0
            ns_per_event = elapsed_ns / pushed if pushed else None
            overflow = pushed < len(events)

            if ns_per_event is not None:
                latency_samples.append(ns_per_event)
                bindings.record_events_batch([(LATENCY_RULE_EVENT_ID, tick, int(ns_per_event))])

            time.sleep(0.05)
            new_signals = bindings.poll_signals(4096)
            signals_total += len(new_signals)

            if overflow:
                faults.append({
                    "severity": "CRITICAL",
                    "code": "RING_BUFFER_OVERFLOW",
                    "detail": f"record_events_batch accepted {pushed}/{len(events)} events -- native ring is full.",
                })
            if new_signals:
                faults.append({
                    "severity": "OPERATIONAL",
                    "code": "LATENCY_RULE_TRIGGERED",
                    "detail": f"{len(new_signals)} batch(es) exceeded the {LATENCY_RULE_THRESHOLD_NS}ns push-latency rule this tick.",
                })

            throughput = (pushed / (elapsed_ns / 1e9)) if elapsed_ns > 0 else None
            sorted_samples = sorted(latency_samples)
            batch_block = {
                "requested": len(events),
                "pushed": pushed,
                "overflow": overflow,
                "elapsed_ns": elapsed_ns,
                "ns_per_event": ns_per_event,
                "throughput_events_sec": throughput,
                "throughput_bytes_sec": throughput * EVENT_SIZE_BYTES if throughput else None,
                "event_size_bytes": EVENT_SIZE_BYTES,
            }
            latency_block = {
                "samples": len(sorted_samples),
                "p50_ns": percentile(sorted_samples, 50),
                "p99_ns": percentile(sorted_samples, 99),
                "p999_ns": percentile(sorted_samples, 99.9),
                "jitter_ns": statistics.pstdev(sorted_samples) if len(sorted_samples) >= 2 else None,
            }
        else:
            batch_block = None
            latency_block = None

        rss = get_process_rss_bytes()

        msg = {
            "type": "telemetry",
            "simulated": False,
            "ts": int(time.time() * 1000),
            "tick": tick,
            "engine": {
                "native": native,
                "engine_ready": engine_ready,
                "cpu_count": cpu_count,
                "license_status": license_status_name,
                "licensed_max_cores": bindings.licensed_max_cores() if native else 0,
                "license_issued_at": license_payload["issued_at"] if license_payload else 0,
                "license_expires_at": license_payload["expires_at"] if license_payload else 0,
                "core_pin_requested_core": 0,
                "core_pin_result": core_pin_result,
            },
            "batch": batch_block,
            "latency_window": latency_block,
            "rules": {
                "signals_total": signals_total,
            },
            "process": {
                "rss_bytes": rss,
            },
            "not_instrumented": NOT_INSTRUMENTED,
        }
        broadcast(msg)

        for f in faults:
            f["type"] = "fault"
            f["ts"] = int(time.time() * 1000)
            f["tick"] = tick
            broadcast(f)

        time.sleep(max(0.0, tick_seconds - 0.05))


def main():
    parser = argparse.ArgumentParser(description="Animus Engine public live telemetry bridge")
    parser.add_argument("--host", default="127.0.0.1", help="Bind address (default: 127.0.0.1 — put a tunnel in front for public access, don't bind 0.0.0.0 directly to the internet)")
    parser.add_argument("--port", type=int, default=8766, help="Bind port (default: 8766 — deliberately different from command_center_bridge.py's 8765 so both can run at once)")
    parser.add_argument("--tick-seconds", type=float, default=1.0, help="Seconds between telemetry broadcasts (default: 1.0)")
    parser.add_argument("--allow-origin", action="append", default=None, help="Restrict WebSocket handshakes to this Origin header (repeatable). Omit to allow any origin.")
    args = parser.parse_args()

    allowed_origins = set(args.allow_origin) if args.allow_origin else None

    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_sock.bind((args.host, args.port))
    server_sock.listen(8)

    print(f"[bridge] listening on ws://{args.host}:{args.port} -- open docs/dashboard.html now")
    if allowed_origins:
        print(f"[bridge] restricting handshakes to origins: {sorted(allowed_origins)}")
    else:
        print("[bridge] no --allow-origin set: any page origin may connect once this port is reachable")

    accept_thread = threading.Thread(target=accept_loop, args=(server_sock, allowed_origins), daemon=True)
    accept_thread.start()

    try:
        telemetry_loop(args.tick_seconds)
    except KeyboardInterrupt:
        print("\n[bridge] shutting down")
    finally:
        server_sock.close()


if __name__ == "__main__":
    main()
