"""Animus ITCH 5.0 Adapter -- zero-copy NumPy consumption verification.

Proves the claim in adapters/itch50/README.md's "Python Consumer
Compatibility" section: that decoded ItchFrame records (adapters/itch50/
include/itch50_ring_frame.hpp), pushed into a named shared-memory ring by
a separate C++ process (adapters/itch50/itch50_shm_bridge.cpp), can be
read from Python as a real NumPy structured array with NO per-record
parsing and NO copy of the underlying bytes -- using the animus-engine-sdk
/ animus-native-stream packages' EXISTING schema-agnostic mechanism
(animus._animus_shm_native.SharedSchemaChannel + animus.dynamic_schema.
to_structured_array), not a new, ItchFrame-specific binding. ItchFrame
qualifies for that existing mechanism purely by being a registered
ANIMUS_DEFINE_SCHEMA wire schema (see itch50_ring_frame.hpp) -- no new
compiled extension code was needed to make this script work.

Prerequisite: the compiled animus-native-stream extension
(bindings/animus_shm_py.cpp, built via `pip install ./bindings`) must be
importable as `animus._animus_shm_native`. See bindings/README.md /
docs/EVALUATION_KIT.md if it isn't built yet.

Usage:
    1. In one terminal, start the bridge and leave it running:
           build/itch50_shm_bridge --name animus_itch50_demo --messages 100000 --hold-seconds 60
       (Windows note: the segment is destroyed the instant the bridge
       process exits -- it MUST still be running, inside its
       --hold-seconds wait, when this script runs. See
       itch50_shm_bridge.cpp's own header comment.)
    2. While it's still holding the segment open, in a second terminal:
           python adapters/itch50/verify_zero_copy_numpy.py --name animus_itch50_demo
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, _REPO_ROOT)


# Field order/meaning must track adapters/itch50/include/itch50_ring_frame.hpp's
# ItchFrame struct exactly -- wire_format_to_dtype() names them generically
# (f0, f1, ...) in declaration order since struct.calcsize carries no field
# names, so this script re-attaches the real names for readability.
_FIELD_NAMES = [
    "sequence_id", "recv_timestamp_ns", "itch_timestamp_ns", "order_ref_number",
    "secondary_ref_number", "stock_ticker_raw", "price_ticks", "shares",
    "stock_locate", "msg_type", "side_or_flag",
]

_MSG_TYPE_NAMES = {
    ord("S"): "SystemEvent", ord("A"): "AddOrderNoMPID", ord("F"): "AddOrderWithMPID",
    ord("E"): "OrderExecuted", ord("C"): "OrderExecutedWithPrice", ord("X"): "OrderCancel",
    ord("D"): "OrderDelete", ord("U"): "OrderReplace",
}


def _decode_ticker(raw_u64: int) -> str:
    """stock_ticker_raw is an opaque 8-byte blob (see ItchFrame's own decode
    note), not a numeric value -- struct.pack("<Q", ...) recovers the exact
    original bytes on this little-endian host, matching how the C++ side
    wrote it with a plain memcpy."""
    return struct.pack("<Q", raw_u64 & 0xFFFFFFFFFFFFFFFF).decode("ascii", errors="replace").rstrip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", default="animus_itch50_shm_demo",
                         help="shared-memory segment name (must match itch50_shm_bridge's --name)")
    parser.add_argument("--rows", type=int, default=10, help="number of decoded records to print (default: 10)")
    args = parser.parse_args()

    try:
        import numpy as np
    except ImportError:
        print("error: this verification script requires numpy (pip install numpy)", file=sys.stderr)
        return 1

    try:
        from animus._animus_shm_native import SharedSchemaChannel
    except ImportError as exc:
        print(
            "error: animus._animus_shm_native (the animus-native-stream compiled extension) "
            "is not importable -- build it first via `pip install ./bindings` "
            f"(see docs/EVALUATION_KIT.md). Original error: {exc}", file=sys.stderr)
        return 1

    from animus.dynamic_schema import to_structured_array  # noqa: E402 -- see sys.path.insert above

    try:
        channel = SharedSchemaChannel.open(args.name)
    except RuntimeError as exc:
        print(
            f"error: could not open segment '{args.name}': {exc}\n"
            "Is adapters/itch50/itch50_shm_bridge.cpp still running and holding it open? "
            "See this script's own module docstring for the two-terminal launch sequence.",
            file=sys.stderr)
        return 1

    print(f"=== Animus ITCH 5.0 Adapter -- Zero-Copy NumPy Verification ===")
    print(f"Segment              : {channel.name}")
    print(f"Wire format          : {channel.wire_format}")
    print(f"Payload size / stride: {channel.payload_size} / {channel.stride} bytes")
    print(f"Ring capacity        : {channel.capacity} frames")
    print(f"head / tail          : {channel.head} / {channel.tail}")
    print(f"Dropped (overwritten): {channel.dropped_count}")
    print()

    if channel.wire_format != "<QQQQQQqIHBB":
        print(
            f"warning: wire_format '{channel.wire_format}' does not match ItchFrame's expected "
            "'<QQQQQQqIHBB' -- this segment may not actually hold ItchFrame records "
            "(a stale segment from a different schema/build?). Proceeding anyway.",
            file=sys.stderr)

    arr = to_structured_array(channel)  # zero-copy: np.frombuffer over channel.raw_view()

    # Zero-copy proof, the same two checks tests/test_dynamic_schema.py's
    # own test_raw_view_is_actually_zero_copy relies on: an array built by
    # np.frombuffer never owns its backing buffer, and its declared shape
    # covers the WHOLE ring (capacity), not just the records actually
    # pushed so far -- neither would be true of a copying decode.
    assert arr.flags["OWNDATA"] is False, "expected a view, not an owned copy"
    assert arr.shape[0] == channel.capacity, "expected the raw view to cover the entire ring, not just filled slots"
    print(f"Zero-copy check      : arr.flags['OWNDATA']=False, arr.shape={arr.shape} == channel.capacity  [OK]")
    print()

    head, tail, capacity = channel.head, channel.tail, channel.capacity
    filled = min(head - tail, capacity)
    n = min(args.rows, filled)
    print(f"Decoding the first {n} of {filled} live records (ring holds up to {capacity}):")
    print(f"{'seq':>10} {'msg_type':>18} {'stock':>8} {'price':>12} {'shares':>8} {'order_ref':>12}")
    for i in range(n):
        row = arr[(tail + i) & (capacity - 1)]
        rec = dict(zip(_FIELD_NAMES, (row[f"f{j}"] for j in range(len(_FIELD_NAMES)))))
        msg_type_byte = int(rec["msg_type"])
        msg_type_name = _MSG_TYPE_NAMES.get(msg_type_byte, f"0x{msg_type_byte:02x}")
        ticker = _decode_ticker(int(rec["stock_ticker_raw"])) if msg_type_byte in (ord("A"), ord("F")) else "-"
        price = f"{int(rec['price_ticks']) / 10000.0:.4f}" if rec["price_ticks"] else "-"
        print(f"{int(rec['sequence_id']):>10} {msg_type_name:>18} {ticker:>8} {price:>12} "
              f"{int(rec['shares']):>8} {int(rec['order_ref_number']):>12}")

    print()
    print("VERIFIED: decoded ItchFrame records via a zero-copy NumPy structured array "
          "with no ItchFrame-specific compiled binding.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
