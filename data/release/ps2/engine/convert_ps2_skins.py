"""Convert original PS2 skin geometry and rigid bone groups to GC CLIMAX assets.

Output is private, disc-derived data. Conversion does not install or enable it.
"""
from pathlib import Path
import argparse
import collections
import json
import math
import struct
import zlib
from ps2_content import Container, SKINS


class Writer:
    def __init__(self, source):
        self.source = source
        self.data = bytearray(source.image)
        self.fields = set(source.fields)
        for p in range(0, len(self.data) - 3, 4):
            self.data[p:p + 4] = self.data[p:p + 4][::-1]

    def word(self, p, v):
        struct.pack_into('>I', self.data, p, v)

    def pointer(self, p, v):
        self.word(p, v)
        self.fields.discard(p)
        if v:
            self.fields.add(p)

    def append(self, data, alignment=32):
        self.data.extend(bytes((-len(self.data)) % alignment))
        offset = len(self.data)
        self.data.extend(data)
        return offset

    def string(self, p):
        s = self.source.text(p).encode('latin1') + b'\0'
        self.data[p:p + len(s)] = s

    def shorts(self, p, n):
        data = self.source.image[p:p + n * 2]
        if len(data) != n * 2:
            raise ValueError('Short animation array')
        for i in range(n):
            self.data[p + i * 2:p + i * 2 + 2] = data[i * 2:i * 2 + 2][::-1]

    def array(self, data, count, stride):
        assert count * stride == len(data)
        p = self.append(data)
        desc = self.append(struct.pack('>4I', p, len(data), count, stride))
        self.fields.add(desc)
        return desc

    def finish(self):
        image = zlib.compress(self.data, 9)
        rel = b''.join(struct.pack('>I', p) for p in sorted(self.fields))
        table = zlib.compress(rel, 9)
        return (b'CLIMAX-\0' + struct.pack('>6I', len(self.fields), len(self.data),
                len(image), len(table), 0, 0) + bytes(32) + image + table)


def decode_palette_texture(c, t, *, skin_rgb=False):
    width, height = c.u(t + 8), c.u(t + 12)
    if c.u(t + 4) != 0 or width < 16 or height < 16:
        raise ValueError('Unsupported PS2 skin texture format/size')
    palette = c.u(t + 28)
    mip = c.u(t + 48)
    pixels = c.u(mip + 4)
    if c.u(mip + 8) != (height << 16) | width:
        raise ValueError('PS2 texture mip dimensions disagree')
    # The character exporter stores diffuse RGB in seven bits. Expand its
    # palette once; GX samples full-range RGB. Map textures use a different
    # export path, and alpha already has its own GS conversion.
    clut = [tuple(c.image[palette + i * 4:palette + i * 4 + 4]) for i in range(256)]
    if skin_rgb:
        if any(max(color[:3]) > 127 for color in clut):
            raise ValueError('Unexpected full-range entry in PS2 skin palette')
        clut = [tuple((v << 1) | (v >> 6) for v in color[:3]) + color[3:] for color in clut]
    rgba = bytearray()
    for y in range(height):
        for x in range(width):
            # PSMT8 upload swizzle; GS CLUT permutes index bits 3 and 4.
            # Format reference: PS2 Linux HOWTO swizzle textures, 2003.
            block = (y & ~15) * width + (x & ~15) * 2
            selector = (((y + 2) >> 2) & 1) * 4
            row = (((y & ~3) >> 1) + (y & 1)) & 7
            column = row * width * 2 + ((x + selector) & 7) * 4
            byte = ((y >> 1) & 1) + ((x >> 2) & 2)
            index = c.image[pixels + block + column + byte]
            index = (index & ~24) | ((index & 8) << 1) | ((index & 16) >> 1)
            r, g, b, a = clut[index]
            rgba.extend((r, g, b, min(a * 2, 255)))
    return width, height, rgba


def gx_rgba8(rgba, width, height):
    result = bytearray()
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            block = [rgba[((by + y) * width + bx + x) * 4:((by + y) * width + bx + x) * 4 + 4]
                     for y in range(4) for x in range(4)]
            result.extend(v for r, g, b, a in block for v in (a, r))
            result.extend(v for r, g, b, a in block for v in (g, b))
    return result


def gx_rgb565(rgba, width, height):
    result = bytearray()
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            for y in range(4):
                for x in range(4):
                    at = ((by + y) * width + bx + x) * 4
                    r, g, b = rgba[at:at + 3]
                    packed = ((r * 31 + 127) // 255 << 11) | ((g * 63 + 127) // 255 << 5) | ((b * 31 + 127) // 255)
                    result.extend(struct.pack('>H', packed))
    return result


def strips(c, descriptor, dma_field=8, allow_missing_uv=False):
    uv = normals = None
    result = []
    for _, _, _, vif, data, _ in c.dma(c.u(descriptor + dma_field)):
        command = (vif >> 24) & 0x7f
        n = (vif >> 16) & 255 or 256
        if command == 0x64:
            uv = [data[i * 8:i * 8 + 8] for i in range(n)]
        elif command == 0x6e:
            normals = [data[i * 4:i * 4 + 4] for i in range(n)]
        elif command == 0x68:
            if uv is None and allow_missing_uv:
                uv = [bytes(8)] * n
            if uv is None or normals is None or len(uv) != n or len(normals) != n:
                raise ValueError('Unmatched PS2 skin VIF attribute stream')
            result.append([(data[i * 12:i * 12 + 12], normals[i], uv[i]) for i in range(n)])
            uv = normals = None
        elif command not in (0, 0x6c):
            raise ValueError(f'Unknown PS2 skin VIF command {command:x}')
    if not result:
        raise ValueError('Empty PS2 skin mesh')
    return result


def split_drivethrough_cuffs(w, model, bones, points, bone_map):
    # Both cuffs on the PAL mesh share these two bind-pose points at the ankles.
    # Keep their right-hand copies and give the left cuff its own skinning indices.
    c = w.source
    names = [c.text(c.u(b + 4)) for b in bones]
    copies = {}
    left_bones = set()
    for index, part in ((58, 'Lower_Leg'), (64, 'Foot')):
        if index >= len(points) or names[bone_map[index]] != 'R_' + part:
            raise ValueError('Unexpected Drivethrough ankle binding')
        left = names.index('L_' + part)
        copies[index] = len(points)
        left_bones.add(left)
        points.append(points[index])
        bone_map.append(left)
    w.pointer(model + 28, w.array(b''.join(points), len(points), 12))
    w.pointer(model + 72, w.append(bone_map))
    return copies, left_bones


def convert(c):
    w = Writer(c)
    stats = []
    textures = c.chain(24)
    for t in textures:
        width, height, rgba = decode_palette_texture(c, t, skin_rgb=True)
        data = w.append(gx_rgba8(rgba, width, height))
        for i in range(4, 40, 4):
            w.pointer(t + i, 0)
        for i, value in [(4, 2), (8, width), (12, height)]:
            w.word(t + i, value)
        w.pointer(t + 24, data)
    for record in c.chain(8):
        name = c.text(c.u(record + 4))
        w.string(c.u(record + 4))
        model = c.u(record + 12)
        if c.u(record + 8) != 0 or c.u(model + 44) != 5:
            raise ValueError('Expected a rigid animated PS2 skin model')
        # GX type-5 lists consume CPU-skinned streams. PS2 kind 5 selects its
        # GPU bone path; the converted representation uses the GC kind-3 path.
        w.word(model + 44, 3)
        w.word(model + 24, 1)
        bones = c.chain(model + 48)
        for b in bones:
            w.string(c.u(b + 4))
        pool = c.u(model + 68)
        w.shorts(c.u(pool), c.u(pool + 4) * 10)
        for anim in c.chain(model + 52):
            w.string(c.u(anim + 4))
            for i in range(c.u(anim + 12)):
                w.shorts(c.u(c.u(anim + 16) + i * 20), len(bones))
        p, count, stride = c.array(model + 28)
        if stride != 12:
            raise ValueError('Unsupported PS2 skin position layout')
        positions = {c.image[p + i * 12:p + i * 12 + 12]: i for i in range(count)}
        bone_map_pointer = c.u(model + 72)
        bone_map = bytearray(c.image[bone_map_pointer:bone_map_pointer + count])
        w.data[bone_map_pointer:bone_map_pointer + count] = bone_map
        points = [w.data[q:q + 12] for q in range(p, p + count * 12, 12)]
        cuff_copies, left_cuff_bones = {}, set()
        if name.casefold() == 'drivethrough_legs_static1':
            cuff_copies, left_cuff_bones = split_drivethrough_cuffs(w, model, bones, points, bone_map)
        up, un, us = c.array(model + 40)
        if us != 8:
            raise ValueError('Unsupported skin UV layout')
        uvs = {c.image[up + i * 8:up + i * 8 + 8]: i for i in range(un)}
        normal_ids = {}
        packed_normals = []
        normal_bones = []
        def normal_index(raw, bone):
            key = (raw, bone)
            if key not in normal_ids:
                normal_ids[key] = len(packed_normals)
                packed_normals.append(raw)
                normal_bones.append(bone)
            return normal_ids[key]

        triangles = 0
        for mesh in c.chain(model + 8):
            w.word(mesh + 40, c.u(mesh + 40) | 1)
            if c.u(mesh + 4):
                w.string(c.u(mesh + 4))
            stream = bytearray()
            for strip in strips(c, c.u(mesh + 12)):
                vertices = []
                for point, normal, uv in strip:
                    pi = positions[point]
                    bone = normal[3] & 127
                    if bone >= len(bones) or bone_map[pi] != bone:
                        raise ValueError('PS2 VIF bone differs from position bone map')
                    vertices.append((pi, normal_index(normal[:3], bone), uvs[uv], normal[3] & 128))
                tri = []
                for i in range(2, len(vertices)):
                    if vertices[i][3]:
                        continue
                    a, b, d = vertices[i - 2:i + 1]
                    if i & 1:
                        a, b = b, a
                    if len({a[0], b[0], d[0]}) < 3:
                        continue
                    face = (a, b, d)
                    if cuff_copies and any(bone_map[v[0]] in left_cuff_bones for v in face):
                        for pi, ni, ui, flags in face:
                            if pi in cuff_copies:
                                pi = cuff_copies[pi]
                                ni = normal_index(packed_normals[ni], bone_map[pi])
                            tri.append((pi, ni, ui, flags))
                    else:
                        tri.extend(face)
                if len(tri) > 65535:
                    raise ValueError('GX skin primitive count overflow')
                if tri:
                    stream.extend(struct.pack('>BH', 0x90, len(tri)))
                    for pi, ni, ui, _ in tri:
                        stream.extend(struct.pack('>HHBH', pi, ni, 0, ui))
                    triangles += len(tri) // 3
            used = len(stream)
            stream.extend(bytes((-used) % 32))
            payload = w.append(stream)
            rec = w.append(bytes(44))
            w.pointer(rec + 8, rec + 28)
            w.pointer(rec + 28, payload)
            for off, value in [(32, len(stream)), (36, used), (40, 5)]:
                w.word(rec + off, value)
            w.pointer(mesh + 12, rec)
            # Reversing the numeric word moves PS2 U/V at +0x30/+0x31
            # to GX V/U at +0x32/+0x33 without swapping the axes.
        normal_values = [[v / 127 for v in struct.unpack('3b', raw)] for raw in packed_normals]
        normal_data = b''.join(struct.pack('>3f', *v) for v in normal_values)
        w.pointer(model + 32, w.array(normal_data, len(normal_values), 12))
        for bi, bone in enumerate(bones):
            ids = [i for i, b in enumerate(bone_map) if b == bi]
            norms = [i for i, b in enumerate(normal_bones) if b == bi]
            for offset, data, n in [
                (16, b''.join(points[i] for i in ids), len(ids)),
                (20, b''.join(struct.pack('>I', i) for i in ids), len(ids)),
                (24, struct.pack('>' + str(len(ids)) + 'f', *([1.] * len(ids))), len(ids)),
                (44, b''.join(struct.pack('>3f', *normal_values[i]) for i in norms), len(norms)),
                (48, b''.join(struct.pack('>I', i) for i in norms), len(norms))]:
                w.pointer(bone + offset, w.append(data) if n else 0)
            w.word(bone + 12, len(ids))
            w.word(bone + 40, len(norms))
        # One white vertex colour; PS2's packed normal stream is not colour data.
        color = c.u(model + 36)
        cp, cn, cs = c.array(model + 36)
        w.data[cp:cp + cn * cs] = c.image[cp:cp + cn * cs]
        stats.append(dict(name=name, vertices=len(points), normals=len(normal_values),
                          bones=len(bones), triangles=triangles))
    result = w.finish()
    check = Container(result, '>')
    if len(check.chain(8)) != len(stats):
        raise ValueError('Converted archive record count changed')
    return result, stats
