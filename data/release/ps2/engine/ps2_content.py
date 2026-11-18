"""Local PS2 Next Encounter asset reader. Never modifies the supplied disc.

PS2 DMA/VIF packets must be decoded before these assets can be exported to GX.
This module deliberately does not label unconverted assets as playable maps.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path


class Iso:
    def __init__(self, path: Path):
        self.stream = path.open('rb')
        self.path = path
        self.files = {}
        self.stream.seek(16 * 2048)
        pvd = self.stream.read(2048)
        if pvd[:7] != b'\x01CD001\x01':
            raise ValueError('Expected an ISO9660 disc')
        root = pvd[156:190]
        self._walk(struct.unpack_from('<I', root, 2)[0],
                   struct.unpack_from('<I', root, 10)[0], '', set())

    def _walk(self, extent, size, prefix, seen):
        if (extent, size) in seen:
            raise ValueError('Cyclic ISO directory')
        seen.add((extent, size))
        self.stream.seek(extent * 2048)
        raw = self.stream.read(size)
        pos = 0
        while pos < len(raw):
            count = raw[pos]
            if not count:
                pos = (pos // 2048 + 1) * 2048
                continue
            record = raw[pos:pos + count]
            pos += count
            name = record[33:33 + record[32]]
            if name in (b'\0', b'\1'):
                continue
            name = name.decode('ascii').split(';')[0]
            if name in ('.', '..') or '/' in name or '\\' in name:
                raise ValueError('Unsafe ISO path')
            extent = struct.unpack_from('<I', record, 2)[0]
            size = struct.unpack_from('<I', record, 10)[0]
            if record[25] & 2:
                self._walk(extent, size, prefix + name + '/', seen)
            else:
                self.files[prefix + name] = (extent, size)

    def read(self, path):
        extent, size = self.files[path]
        self.stream.seek(extent * 2048)
        data = self.stream.read(size)
        if len(data) != size:
            raise ValueError('Truncated ISO extent')
        return data


class Container:
    def __init__(self, wire, endian='<'):
        if wire[:8] != b'CLIMAX-\0':
            raise ValueError('Expected a CLIMAX container')
        self.endian = endian
        h = struct.unpack_from(endian + '6I', wire, 8)
        count, size, packed, relpacked, _, _ = h
        if size > 128 * 1024 * 1024 or count > 2_000_000:
            raise ValueError('CLIMAX size limit')
        image_end = 64 + (packed or size)
        if image_end + (relpacked or count * 4) != len(wire):
            raise ValueError('CLIMAX extent mismatch')
        self.image = zlib.decompress(wire[64:image_end]) if packed else wire[64:image_end]
        rel = zlib.decompress(wire[image_end:]) if relpacked else wire[image_end:]
        if len(self.image) != size or len(rel) != count * 4:
            raise ValueError('CLIMAX inflated size mismatch')
        words = struct.unpack(endian + str(count) + 'I', rel)
        fields = []
        i = 0
        while i < len(words):
            word = words[i]
            if word & 0x80000000:
                i += 1
                start = words[i]
                fields.extend(start + j * (word & 1023)
                              for j in range((word >> 10) & 0x1fffff))
            else:
                fields.append(word)
            if len(fields) > 2_000_000:
                raise ValueError('CLIMAX relocation limit')
            i += 1
        if any(f + 4 > size or self.u(f) > size for f in fields):
            raise ValueError('CLIMAX relocation outside image')
        self.fields = set(fields)

    def u(self, offset):
        return struct.unpack_from(self.endian + 'I', self.image, offset)[0]

    def text(self, pointer):
        end = self.image.index(0, pointer)
        if end - pointer > 1024:
            raise ValueError('Unbounded asset name')
        return self.image[pointer:end].decode('latin1')

    def chain(self, field):
        out = []
        pointer = self.u(field)
        while pointer:
            if pointer in out or len(out) > 100000:
                raise ValueError('Cyclic asset list')
            out.append(pointer)
            pointer = self.u(pointer)
        return out

    def array(self, field):
        descriptor = self.u(field)
        pointer, size, count, stride = struct.unpack_from(self.endian + '4I', self.image, descriptor)
        if count * stride != size or pointer + size > len(self.image):
            raise ValueError('Invalid asset array')
        return pointer, count, stride

    def dma(self, pointer):
        """Read a bounded source DMA chain; addresses are unrelocated image offsets."""
        seen = set()
        chunks = []
        stack = []
        while True:
            if pointer in seen or len(seen) > 100000:
                raise ValueError('Cyclic/oversized DMA chain')
            seen.add(pointer)
            tag, address, vif0, vif1 = struct.unpack_from('<4I', self.image, pointer)
            qwc = tag & 0xffff
            kind = (tag >> 28) & 7
            source = address & 0x7fffffff if kind in (0, 3, 4) else pointer + 16
            if source + qwc * 16 > len(self.image):
                raise ValueError('DMA payload outside image')
            chunks.append((pointer, kind, vif0, vif1, self.image[source:source + qwc * 16], source))
            if kind in (0, 7):
                break
            if kind in (1,):
                pointer += 16 + qwc * 16
            elif kind in (3, 4):
                pointer += 16
            elif kind == 2:
                pointer = address & 0x7fffffff
            elif kind == 5:
                if len(stack) >= 2:
                    raise ValueError('DMA call stack overflow')
                stack.append(pointer + 16 + qwc * 16)
                pointer = address & 0x7fffffff
            elif kind == 6:
                if not stack:
                    break
                pointer = stack.pop()
            else:
                raise ValueError('Unsupported DMA call/return tag')
        return chunks


SKINS = {
    'ballwraith': 'EB3BB0', 'nesbot': 'EC544', 'superfella': 'EE3AC0',
    'wrestler': '3B953C', 'drivethrough': '3B1F60C', 'fisherman': '761084',
}
MAPS = {name.lower(): name for name in (
    'CANYON', 'EGYPT', 'FLIGHT', 'FORT', 'MAYA', 'MEDIEVAL', 'MILITARY',
    'OFFWORLD', 'RUSSIA', 'TEETH', 'VENICE', 'VERTIGO', 'WESTERN')}
MAPS.update(victorian='3A29C', neo_tokyo='3A2C0')
