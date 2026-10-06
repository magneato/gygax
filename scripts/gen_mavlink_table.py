"""Regenerates src/gygax/robotics/mavlink_messages.inc from the pymavlink common dialect.

Usage: python3 scripts/gen_mavlink_table.py > src/gygax/robotics/mavlink_messages.inc
The script recomputes CRC_EXTRA for every message and aborts on any mismatch with pymavlink.
"""
import sys
from pymavlink.dialects.v20 import common as m

TYPES = {
    "uint8_t": "U8", "int8_t": "I8", "uint16_t": "U16", "int16_t": "I16", "uint32_t": "U32", "int32_t": "I32",
    "uint64_t": "U64", "int64_t": "I64", "float": "F32", "double": "F64", "char": "CHAR",
    "uint8_t_mavlink_version": "U8",
}
SIZES = {"U8": 1, "I8": 1, "U16": 2, "I16": 2, "U32": 4, "I32": 4, "U64": 8, "I64": 8, "F32": 4, "F64": 8, "CHAR": 1}
X25_CRC_INITIAL_VALUE = 0xFFFF
X25_CRC_WORD_MASK = 0xFFFF
CRC_BYTE_MASK = 0xFF
CRC_EXTRA_BYTE_MASK = 0xFF


def x25(data):
    crc = X25_CRC_INITIAL_VALUE
    for byte in data:
        tmp = byte ^ (crc & CRC_BYTE_MASK)
        tmp = (tmp ^ (tmp << 4)) & CRC_BYTE_MASK
        crc = ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & X25_CRC_WORD_MASK
    return crc


def crc_extra(name, fields):
    data = bytearray(name.encode() + b" ")
    for ftype, fname, alen in fields:
        base = ftype.replace("_mavlink_version", "")
        data += base.encode() + b" " + fname.encode() + b" "
        if alen:
            data.append(alen)
    crc = x25(data)
    return (crc & CRC_EXTRA_BYTE_MASK) ^ (crc >> 8)


out = []
for mid in sorted(m.mavlink_map):
    cls = m.mavlink_map[mid]
    name = cls.msgname
    wire = []
    for wire_index, fname in enumerate(cls.ordered_fieldnames):
        idx = cls.fieldnames.index(fname)
        wire.append((cls.fieldtypes[idx], fname, cls.array_lengths[wire_index]))
    base = None
    for k in range(len(wire), 0, -1):
        if crc_extra(name, wire[:k]) == cls.crc_extra:
            base = k
            break
    if base is None:
        sys.exit(f"cannot reproduce crc_extra for {name}")
    offset = 0
    base_len = 0
    fields = []
    for i, (ftype, fname, alen) in enumerate(wire):
        t = TYPES[ftype]
        size = SIZES[t] * (alen if alen else 1)
        fields.append((fname, t, alen, offset, i >= base))
        offset += size
        if i < base:
            base_len = offset
    out.append((mid, name, cls.crc_extra, base_len, offset, fields))

for mid, name, crc, base_len, max_len, fields in out:
    fl = ", ".join('{"%s", FieldType::%s, %d, %d, %s}' % (f[0], f[1], f[2], f[3], "true" if f[4] else "false") for f in fields)
    print('{%d, "%s", %d, %d, %d, {%s}},' % (mid, name, crc, base_len, max_len, fl))
