"""Prepare a private content overlay; never writes to an installed game folder."""
import argparse
import hashlib
import json
import shutil
import struct
import zlib
from pathlib import Path
from ps2_content import Container, Iso, SKINS, MAPS


def encode(image, fields):
    packed = zlib.compress(image, 9)
    table = zlib.compress(b''.join(struct.pack('>I', p) for p in sorted(fields)), 9)
    return (b'CLIMAX-\0' + struct.pack('>6I', len(fields), len(image), len(packed),
            len(table), 0, 0) + bytes(32) + packed + table)


def merge_archives(original, additions):
    base = Container(original, '>')
    image, fields = bytearray(base.image), set(base.fields)
    last_record, last_texture = base.chain(8)[-1], base.chain(24)[-1]
    for wire in additions:
        part = Container(wire, '>')
        image.extend(bytes((-len(image)) % 32))
        delta = len(image)
        image.extend(part.image)
        for field in part.fields:
            struct.pack_into('>I', image, delta + field, part.u(field) + delta)
            fields.add(delta + field)
        for old, new in [(last_record, part.u(8)), (last_texture, part.u(24))]:
            struct.pack_into('>I', image, old, delta + new)
            fields.add(old)
        last_record = delta + part.chain(8)[-1]
        last_texture = delta + part.chain(24)[-1]
    wire = encode(image, fields)
    check = Container(wire, '>')
    names = [check.text(check.u(r + 4)).lower() for r in check.chain(8)]
    if len(names) != len(set(names)):
        raise ValueError('Merged skin archive has duplicate names')
    for name in SKINS:
        for part in ('body', 'legs'):
            if name + '_' + part + '_static1' not in names:
                raise ValueError('Missing merged skin model')
    return wire


def key(name):
    value = 0
    for index, ch in enumerate(name.encode('ascii'), 2):
        value = (value + (ch & 0xdf) * index) & 0xffffffff
        value = ((value << 1) | (value >> 31)) & 0xffffffff
    return value


def database_records(data, endian):
    n, variables = struct.unpack_from(endian + 'II', data)
    if 8 + n * 12 + variables * 4 > len(data):
        raise ValueError('TDB header outside file')
    return [struct.unpack_from(endian + 'III', data, 8 + i * 12) for i in range(n)]


def extend_database(original, ps2):
    old = database_records(original, '>')
    new = {h: (p, flag) for p, flag, h in database_records(ps2, '<')}
    hashes = {h for _, _, h in old}
    additions = []
    for name in SKINS:
        h = key(name + '_MODEL_NAME')
        if h in hashes:
            continue
        p, flag = new[h]
        end = ps2.index(0, p)
        additions.append((h, flag, ps2[p:end + 1]))
    n, nv = struct.unpack_from('>II', original)
    shift = len(additions) * 12
    old_end = 8 + n * 12 + nv * 4
    out = bytearray(struct.pack('>II', n + len(additions), nv))
    for p, flag, h in old:
        out.extend(struct.pack('>III', p + shift, flag, h))
    extra_strings = bytearray()
    for h, flag, text in additions:
        out.extend(struct.pack('>III', len(original) + shift + len(extra_strings), flag, h))
        extra_strings.extend(text)
    for i in range(nv):
        out.extend(struct.pack('>I', struct.unpack_from('>I', original, 8 + n * 12 + i * 4)[0] + shift))
    out.extend(original[old_end:])
    out.extend(extra_strings)
    after = database_records(out, '>')
    for i, (p, flag, h) in enumerate(old):
        q, f, digest = after[i]
        if (f, digest) != (flag, h) or original[p:original.index(0, p)] != out[q:out.index(0, q)]:
            raise ValueError('Existing text database entry changed')
    return out
