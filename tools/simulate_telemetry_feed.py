"""
Animus Engine — Synthetic Telemetry Feed (local dashboard testing ONLY)
========================================================================

Emits the exact same JSON schema as telemetry_bridge.py, on the same
default port, but every number comes from a synthetic random-walk
generator — there is no compiled engine involved at all. Use this when
you want to see docs/dashboard.html's live charts moving without
building AnimusNative.dll first, or when iterating on the frontend
away from the machine that has the real engine on it.

Every message this script sends sets "simulated": true. The dashboard
is required to render a visible "SIMULATED FEED" state instead of
"LIVE STREAM" when it sees that flag — never remove that distinction
in dashboard.html, since this project's stated rule (see
telemetry_bridge.py and command_center_bridge.py's docstrings) is that
the dashboard never silently presents synthetic numbers as real
engine measurements.

Run from the repo root:

    python tools/simulate_telemetry_feed.py

Then open docs/dashboard.html. Stop this script and start
telemetry_bridge.py instead to see the real engine's numbers on the
same page, no dashboard changes needed.
"""

import argparse
import base64
import hashlib
import json
import math
import random
import socket
import struct
import threading
import time

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
EVENT_SIZE_BYTES = 16  # mirrors animus::RawEvent / animus.bindings.NativeEvent

NOT_INSTRUMENTED = [
    "Live ring-buffer occupancy / fill % (no native occupancy query exported)",
    "L1/L2/L3 cache-miss rates (no hardware perf-counter hook in this codebase)",
    "Live exchange packet loss / kernel-bypass NIC status (no live market-data feed connected)",
    "SIMULATED FEED -- none of these numbers come from the compiled engine (see tools/simulate_telemetry_feed.py)",
]


# ---------------------------------------------------------------------
# Minimal RFC 6455 WebSocket server (stdlib only) -- identical wire
# protocol to telemetry_bridge.py so the same dashboard connects to
# either without any changes.
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


def ws_handshake(conn):
    request = _recv_headers(conn)
    if request is None:
        return False
    headers = {}
    for line in request.split(b"\r\n")[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.strip().lower()] = v.strip()
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


def ws_send_text(conn, text):
    payload = text.encode("utf-8")
    length = len(payload)
    if length <= 125:
        header = struct.pack("!BB", 0x81, length)
    elif length <= 65535:
        header = struct.pack("!BBH", 0x81, 126, length)
    else:
        header = struct.pack("!BBQ", 0x81, 127, length)
    conn.sendall(header + payload)


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
    try:
        while True:
            frame = ws_read_frame(client.conn)
            if frame is None:
                break
            if frame[0] == 0x8:
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


def accept_loop(server_sock):
    while True:
        try:
            conn, addr = server_sock.accept()
        except OSError:
            return
        try:
            if not ws_handshake(conn):
                conn.close()
                continue
        except OSError:
            continue
        client = Client(conn, addr)
        with clients_lock:
            clients.add(client)
        print(f"[simulate] dashboard connected from {addr[0]}:{addr[1]} ({len(clients)} total)")
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
# Synthetic random-walk generator -- clearly fake, clearly labeled
# ---------------------------------------------------------------------

def simulate_loop(tick_seconds, target_throughput):
    tick = 0
    p50 = 53.0
    signals_total = 0
    rng = random.Random()

    print(f"[simulate] emitting synthetic telemetry every {tick_seconds}s -- press Ctrl+C to stop")

    while True:
        tick += 1

        # Slow random walk so the charts look alive without being noise.
        p50 = max(20.0, p50 + rng.uniform(-2.5, 2.5))
        jitter = rng.uniform(4.0, 14.0)
        p99 = p50 + jitter * rng.uniform(1.5, 3.0)
        p999 = p99 + jitter * rng.uniform(2.0, 5.0)

        throughput = target_throughput * (1 + 0.05 * math.sin(tick / 7.0)) * rng.uniform(0.97, 1.03)
        pushed = int(throughput * tick_seconds)
        overflow = rng.random() < 0.03
        requested = int(pushed * (1.08 if overflow else 1.0))

        rule_fired = rng.random() < 0.08
        if rule_fired:
            signals_total += rng.randint(1, 3)

        faults = []
        if overflow:
            faults.append({"severity": "CRITICAL", "code": "RING_BUFFER_OVERFLOW",
                            "detail": f"[SIMULATED] synthetic batch accepted {pushed}/{requested} events."})
        if rule_fired:
            faults.append({"severity": "OPERATIONAL", "code": "LATENCY_RULE_TRIGGERED",
                            "detail": "[SIMULATED] synthetic latency sample exceeded the demo threshold rule."})

        msg = {
            "type": "telemetry",
            "simulated": True,
            "ts": int(time.time() * 1000),
            "tick": tick,
            "engine": {
                "native": False,
                "engine_ready": True,
                "cpu_count": 16,
                "license_status": "SIMULATED",
                "licensed_max_cores": 16,
                "license_issued_at": 0,
                "license_expires_at": 0,
                "core_pin_requested_core": 0,
                "core_pin_result": True,
            },
            "batch": {
                "requested": requested,
                "pushed": pushed,
                "overflow": overflow,
                "elapsed_ns": int(tick_seconds * 1e9),
                "ns_per_event": (1e9 / throughput) if throughput else None,
                "throughput_events_sec": throughput,
                "throughput_bytes_sec": throughput * EVENT_SIZE_BYTES,
                "event_size_bytes": EVENT_SIZE_BYTES,
            },
            "latency_window": {
                "samples": 60,
                "p50_ns": p50,
                "p99_ns": p99,
                "p999_ns": p999,
                "jitter_ns": jitter,
            },
            "rules": {"signals_total": signals_total},
            "process": {"rss_bytes": int(80_000_000 + rng.uniform(-2_000_000, 2_000_000))},
            "not_instrumented": NOT_INSTRUMENTED,
        }
        broadcast(msg)

        for f in faults:
            f["type"] = "fault"
            f["ts"] = int(time.time() * 1000)
            f["tick"] = tick
            broadcast(f)

        time.sleep(tick_seconds)


def main():
    parser = argparse.ArgumentParser(description="Synthetic telemetry feed for local dashboard testing")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8766, help="Same default port as telemetry_bridge.py -- run one or the other, not both")
    parser.add_argument("--tick-seconds", type=float, default=1.0)
    parser.add_argument("--throughput", type=float, default=2_000_000.0, help="Target simulated events/sec (default: 2,000,000)")
    args = parser.parse_args()

    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_sock.bind((args.host, args.port))
    server_sock.listen(8)

    print(f"[simulate] SIMULATED FEED listening on ws://{args.host}:{args.port} -- open docs/dashboard.html now")
    print("[simulate] this is fake data for UI testing -- do not point a public dashboard at this in production")

    threading.Thread(target=accept_loop, args=(server_sock,), daemon=True).start()

    try:
        simulate_loop(args.tick_seconds, args.throughput)
    except KeyboardInterrupt:
        print("\n[simulate] shutting down")
    finally:
        server_sock.close()


if __name__ == "__main__":
    main()
