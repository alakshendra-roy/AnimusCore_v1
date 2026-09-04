"""Milestone 1: Dynamic & User-Defined Wire Schemas -- Python-side helpers.

Companion to bindings/animus_shm_py.cpp's SharedSchemaChannel: that binding
attaches to a ShmRing<T> segment without ever naming T at compile time and
exposes whatever wire descriptor (payload_size, stride, schema_version_hash,
wire_format) ShmRing<T>::create() stamped into the header
(include/animus/shm_ipc.hpp). wire_format_to_dtype() below is the other
half -- turning that struct.calcsize-compatible format string into a real
NumPy structured dtype at runtime, so a Python consumer can decode a
segment holding any ANIMUS_DEFINE_SCHEMA-registered C++ struct
(ExecutionEvent, OrderBookL2, or a client's own MarketDepthEvent/
AlphaSignal/...) without a new compiled extension per schema.

Field names are not part of the wire format string (struct.calcsize has no
concept of names), so the resulting dtype uses generic f0, f1, ... field
names in declaration order -- matching the C++ struct's own field order by
construction, since animus::schema::Traits<T>::kWireFormat is always
written by hand to mirror that order (see include/animus/schema.hpp).
"""
from __future__ import annotations

# struct format char -> numpy typestr (endianness applied separately).
_STRUCT_TO_NUMPY = {
    "b": "i1", "B": "u1",
    "h": "i2", "H": "u2",
    "i": "i4", "I": "u4",
    "l": "i4", "L": "u4",
    "q": "i8", "Q": "u8",
    "f": "f4", "d": "f8",
}

_ENDIAN_PREFIXES = {"<": "<", ">": ">", "=": "=", "!": ">"}


def wire_format_to_dtype(wire_format: str):
    """Parses a struct.calcsize-compatible format string (e.g. "<QQqqII",
    the same convention animus::schema::Traits<T>::kWireFormat and
    animus::kExecutionEventWireFormat use) into a NumPy structured dtype
    whose in-memory layout matches the C++ struct field-for-field,
    byte-for-byte -- no padding is assumed beyond what the format string
    itself encodes, matching every wire struct in this codebase
    (execution_event.hpp/schema.hpp both static_assert their structs have
    none).

    Raises ImportError with a clear message if numpy isn't installed, and
    ValueError for an empty or unrecognized format string (e.g. a T with
    no ANIMUS_DEFINE_SCHEMA registration, whose wire_format() is "").
    """
    try:
        import numpy as np
    except ImportError as exc:
        raise ImportError(
            "wire_format_to_dtype() requires numpy, which this package does not "
            "depend on -- pip install numpy, or decode the raw bytes by hand with "
            "the struct module using the same format string."
        ) from exc

    if not wire_format:
        raise ValueError(
            "empty wire_format -- this schema has no ANIMUS_DEFINE_SCHEMA registration "
            "(include/animus/schema.hpp) to derive field types from")

    endian = "<"
    body = wire_format
    if wire_format[0] in _ENDIAN_PREFIXES:
        endian = _ENDIAN_PREFIXES[wire_format[0]]
        body = wire_format[1:]

    fields = []
    for i, ch in enumerate(body):
        if ch not in _STRUCT_TO_NUMPY:
            raise ValueError(f"unsupported struct format character '{ch}' in wire_format '{wire_format}'")
        fields.append((f"f{i}", endian + _STRUCT_TO_NUMPY[ch]))

    return np.dtype(fields)


def to_structured_array(channel):
    """Zero-copy structured NumPy view over a SharedSchemaChannel's entire
    slot array (bindings/animus_shm_py.cpp), decoded via
    wire_format_to_dtype(channel.wire_format). Aliases the live shared
    memory segment -- valid only as long as `channel` is alive, and only
    rows [channel.tail, channel.head) (mod channel.capacity) are data an
    active producer has actually published and not yet overwritten, same
    reader contract as the typed SharedExecutionChannel.poll()/drain().
    """
    dtype = wire_format_to_dtype(channel.wire_format)
    if dtype.itemsize != channel.stride:
        raise ValueError(
            f"wire_format '{channel.wire_format}' decodes to {dtype.itemsize} bytes/record, "
            f"but the segment's stride is {channel.stride} -- possible padding or a stale format string")
    import numpy as np
    return np.frombuffer(channel.raw_view(), dtype=dtype)
