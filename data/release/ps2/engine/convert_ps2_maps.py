"""Experimental, isolated PS2-to-GX world conversion; no installation side effects."""
import argparse
import collections
import hashlib
import json
import re
import struct
from pathlib import Path
from ps2_content import Container
from convert_ps2_skins import Writer, decode_palette_texture, gx_rgba8, gx_rgb565, strips


def matrix_offsets(c):
    result = set()
    for field, offset in ((0x40, 4), (0x58, 4), (0x48, 8), (0x44, 8), (0x4c, 4)):
        for node in c.chain(field):
            if node + offset in c.fields and c.u(node + offset):
                result.add(c.u(node + offset))
    for group in c.chain(0x48):
        node, seen = c.u(group + 12), set()
        while node and node not in seen:
            seen.add(node)
            if c.u(node + 4):
                result.add(c.u(node + 4))
            node = c.u(node)
    planes = set()
    for record in effect_region_offsets(c):
        if record + 12 in c.fields and c.u(record + 12):
            planes.add(c.u(record + 12))
    for surface in surface_offsets(c):
        pair = c.u(surface + 68)
        if pair and pair + 4 in c.fields and c.u(pair + 4):
            planes.add(c.u(pair + 4))
    for plane in planes:
        if plane + 4 in c.fields and c.u(plane + 4):
            result.add(c.u(plane + 4))
    for entity in c.chain(0x40):
        if c.u(entity + 12) == 5020:
            result.add(c.u(c.u(entity + 16) + 4))
    return result


def surface_offsets(c):
    result = set(c.chain(8))
    for group in c.chain(0x48):
        result.update(c.chain(group + 16))
    sources = set(c.chain(0x5c))
    for instance in c.chain(0x58):
        if c.u(instance + 16):
            sources.add(c.u(instance + 16))
        for frame in c.chain(instance + 44):
            if c.u(frame + 8):
                sources.add(c.u(frame + 8))
    for source in sources:
        result.update(c.chain(source + 4))
    if c.u(0x70):
        result.update(c.chain(c.u(0x70)))
    return result


def effect_region_offsets(c):
    records = set()
    if header := c.u(0x80):
        pending, seen = [c.u(header)], set()
        while pending:
            node = pending.pop()
            if not node or node in seen:
                continue
            seen.add(node)
            if c.u(node):
                pending.extend((c.u(node + 4), c.u(node + 8)))
            else:
                records.add(node)
    for field, offset in ((0x48, 48), (0x58, 40), (0x4c, 24)):
        for node in c.chain(field):
            if node + offset in c.fields and c.u(node + offset):
                records.add(c.u(node + offset))
    for group in c.chain(0x48):
        node, seen = c.u(group + 12), set()
        while node and node not in seen:
            seen.add(node)
            if c.u(node + 16):
                records.add(c.u(node + 16))
            node = c.u(node)
    return records


def convert_regions(c, w):
    records = effect_region_offsets(c)
    for record in records:
        # These are four independent bytes: region kind/priority/gravity/flags.
        # Treating them as one endian-swapped integer changes gravity0 into13
        # in Medieval's water region and leaves pickup orientation undefined.
        w.data[record + 4:record + 8] = c.image[record + 4:record + 8]
    for entity in c.chain(0x40):
        if c.u(entity + 12) == 5020:
            payload = c.u(entity + 16)
            struct.pack_into('>2H', w.data, payload, *struct.unpack_from('<2H', c.image, payload))
    return len(records)


def convert_matrices(c, w):
    offsets = matrix_offsets(c)
    for offset in offsets:
        values = struct.unpack_from('<16I', c.image, offset)
        if any(values[i] & 0x7fffffff for i in (3, 7, 11)) or values[15] != 0x3f800000:
            raise ValueError('Unexpected PS2 affine matrix layout')
        # PS2 stores four SIMD columns; GC uses three rows with translation.
        packed = [values[column * 4 + row] for row in range(3) for column in range(4)]
        struct.pack_into('>12I', w.data, offset, *packed)
    return len(offsets)


def append_base_resources(c, w, base):
    """Mount the PC engine's common effects/model archive, not its map scene.

    PS2 multiplayer worlds embed only Flag; the GC runtime also requires its
    common particles, distortion textures and debris. Copy the owned native
    backing intact so every internal pointer and compressed texture stays exact.
    Only its archive and texture list become reachable from the PS2 world root.
    """
    original = c.u(0x7c)
    names = [c.text(c.u(r + 4)) for r in c.chain(original + 8)] if original else []
    archive = base.u(0x7c)
    available = {base.text(base.u(r + 4)).lower() for r in base.chain(archive + 8)}
    if any(name.lower() not in available for name in names):
        raise ValueError('PS2 map has a unique embedded model requiring conversion')
    delta = w.append(base.image)
    for field in base.fields:
        w.pointer(delta + field, delta + base.u(field))
    w.pointer(0x7c, delta + archive)
    textures = c.chain(12)
    w.pointer(textures[-1] if textures else 12, delta + base.u(12))
    return len(base.image)


def convert_portals(c, w):
    header = c.u(0x74)
    if not header:
        return 0, 0
    cells, visited, surfaces, flags = [c.u(header + i) for i in (4, 8, 12, 16)]
    w.data[visited:visited + cells] = c.image[visited:visited + cells]
    w.shorts(flags, surfaces)
    pending, nodes, portals = [c.u(header)], set(), set()
    while pending:
        node = pending.pop()
        if not node or node in nodes:
            continue
        nodes.add(node)
        if c.u(node):
            pending.extend((c.u(node + 4), c.u(node + 8)))
            continue
        entries, count = c.u(node + 8), c.u(node + 12)
        w.shorts(entries, count * 2)
        links, count = c.u(node + 16), c.u(node + 20)
        for i in range(count):
            portal = c.u(links + i * 4)
            if portal in portals:
                continue
            portals.add(portal)
            vertices, edges = struct.unpack_from('<2H', c.image, portal + 16)
            if vertices > 255 or edges > 255:
                raise ValueError('Unexpected portal polygon counts')
            w.shorts(portal + 16, 2)
            edge_data = c.u(portal + 12)
            w.data[edge_data:edge_data + edges * 2] = c.image[edge_data:edge_data + edges * 2]
            pending.extend((c.u(portal), c.u(portal + 4)))
    return len(nodes), len(portals)


class Geometry:
    def __init__(self, c, w, fields):
        self.c, self.w, self.fields = c, w, fields
        p, n, stride = c.array(fields[0])
        if stride != 12:
            raise ValueError('World position stride')
        self.positions = {c.image[p + i * 12:p + i * 12 + 12]: i for i in range(n)}
        self.normal_ids, self.normals = {}, []
        self.uv_ids, self.uvs = [{} for _ in range(8)], [[] for _ in range(8)]
        self.triangles = 0

    def batch(self, desc, wide, material):
        c, w = self.c, self.w
        layers = c.chain(material + 28) if wide else []
        requested = [(0, 8)]
        if wide:
            requested += [(i + 1, 16 + i * 8) for i in range(min(2, len(layers)))]
            if c.u(material + 32):
                requested.append((3, 32))
        streams = {i: strips(c, desc, off, True) for i, off in requested
                   if desc + off in c.fields and c.u(desc + off)}
        base = streams[0]
        if any(len(value) != len(base) for value in streams.values()):
            raise ValueError('PS2 world layer strip count differs')
        result = bytearray()
        for si, strip in enumerate(base):
            values = []
            for vi, (point, normal, uv) in enumerate(strip):
                pi = self.positions[point]
                key = normal[:3]
                if key not in self.normal_ids:
                    self.normal_ids[key] = len(self.normals)
                    self.normals.append(tuple(v / 127 for v in struct.unpack('3b', key)))
                ids = []
                for channel in range(8 if wide else 1):
                    if channel in streams:
                        other = streams[channel][si][vi]
                        if other[0] != point or other[1] != normal:
                            raise ValueError('PS2 material layer vertex order differs')
                        value = other[2]
                    else:
                        value = bytes(8)
                    if value not in self.uv_ids[channel]:
                        self.uv_ids[channel][value] = len(self.uvs[channel])
                        self.uvs[channel].append(value)
                    ids.append(self.uv_ids[channel][value])
                if max(pi, self.normal_ids[key], *ids) > 65535:
                    raise ValueError('GX world array exceeds 16-bit indices')
                vertex = struct.pack('>HHB', pi, self.normal_ids[key], 0)
                if wide:
                    vertex += bytes(1)
                vertex += struct.pack('>' + str(len(ids)) + 'H', *ids)
                values.append((vertex, normal[3] & 128, pi))
            vertices = []
            for i in range(2, len(values)):
                if values[i][1]:
                    continue
                a, b, d = values[i - 2:i + 1]
                if i & 1:
                    a, b = b, a
                if len({a[2], b[2], d[2]}) == 3:
                    vertices.extend((a[0], b[0], d[0]))
            if vertices:
                result.extend(struct.pack('>BH', 0x90, len(vertices)))
                result.extend(b''.join(vertices))
                self.triangles += len(vertices) // 3
        used = len(result)
        result.extend(bytes((-used) % 32))
        payload = w.append(result)
        descriptor = w.append(struct.pack('>4I', payload, len(result), used, 3 if wide else 5))
        w.fields.add(descriptor)
        return descriptor

    def finish(self):
        w = self.w
        w.pointer(self.fields[1], w.array(b''.join(struct.pack('>3f', *n) for n in self.normals), len(self.normals), 12))
        for field in self.fields[2:4]:
            if field is not None:
                w.pointer(field, w.array(bytes([255] * 4), 1, 4))
        for i, field in enumerate(self.fields[4:]):
            values = self.uvs[i] or [bytes(8)]
            if i >= 3:
                pairs = [struct.unpack('<2f', b) for b in values]
                if any(not 0 <= v <= 1 for pair in pairs for v in pair):
                    raise ValueError(f'Lightmap UV outside unsigned fixed-point range: {min(v for pair in pairs for v in pair)}..{max(v for pair in pairs for v in pair)}')
                data = b''.join(struct.pack('>2H', *(min(65535, round(v * 65536)) for v in pair))
                                for pair in pairs)
                w.pointer(field, w.array(data, len(values), 4))
            else:
                data = b''.join(b[j:j + 4][::-1] for b in values for j in (0, 4))
                w.pointer(field, w.array(data, len(values), 8))


def convert(c, base=None):
    w = Writer(c)
    # Preserve strings at actual relocatable string targets, not arbitrary bytes.
    strings = set()
    for f in sorted(c.fields):
        p = c.u(f)
        try:
            text = c.text(p)
        except (ValueError, UnicodeDecodeError):
            continue
        if re.fullmatch(r'[A-Za-z_\\][A-Za-z0-9_ .:()\\/#,+=\-]{2,1023}', text):
            w.string(p)
            strings.add(p)
    for entity in c.chain(0x40):
        if entity + 8 in c.fields and c.u(entity + 8):
            w.string(c.u(entity + 8))
            strings.add(c.u(entity + 8))
    compact_textures = 0
    for t in c.chain(12):
        if c.u(t + 4) == 2:
            width, height = c.u(t + 8), c.u(t + 12)
            p = c.u(t + 24)
            if not p:
                p = c.u(c.u(t + 48) + 4)
            rgba = bytearray(c.image[p:p + width * height * 4])
            for i in range(3, len(rgba), 4):
                rgba[i] = min(255, rgba[i] * 2)
        else:
            width, height, rgba = decode_palette_texture(c, t)
        # PS2 full-opacity exporters use alpha127 or128 (expanded254/255).
        # Native RGB565 halves opaque map texture storage in the 64-MiB heap.
        opaque = min(rgba[3::4]) >= 254
        compact_textures += int(opaque)
        pixels = w.append(gx_rgb565(rgba, width, height) if opaque else gx_rgba8(rgba, width, height))
        for i in range(4, 40, 4):
            w.pointer(t + i, 0)
        for off, value in [(4, 0 if opaque else 2), (8, width), (12, height)]:
            w.word(t + off, value)
        w.pointer(t + 24, pixels)
    root_fields = [0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x60, 0x64, 0x68, 0x6c]
    root = Geometry(c, w, root_fields)
    surfaces = set()

    def direct(field, geometry, wide):
        for mesh in c.chain(field):
            surfaces.add(mesh)
            w.pointer(mesh + 12, geometry.batch(c.u(mesh + 12), wide, mesh))

    for material in c.chain(8):
        surfaces.add(material)
        for record in c.chain(material + 12):
            w.pointer(record + 4, root.batch(c.u(record + 4), True, material))
    for group in c.chain(0x48):
        direct(group + 16, root, True)
    sources = set(c.chain(0x5c))
    for instance in c.chain(0x58):
        color_fields = [instance + 20]
        if c.u(instance + 16):
            sources.add(c.u(instance + 16))
        for frame in c.chain(instance + 44):
            if c.u(frame + 8):
                sources.add(c.u(frame + 8))
            color_fields.append(frame + 12)
        for field in color_fields:
            if c.u(field):
                p, count, stride = c.array(field)
                if stride != 4:
                    raise ValueError('Unexpected instance colour stride')
                w.data[p:p + count * 4] = c.image[p:p + count * 4]
    extras = []
    for source in sorted(sources):
        geo = Geometry(c, w, [source + 16, source + 20, source + 24, None, source + 28])
        direct(source + 4, geo, False)
        geo.finish()
        extras.append(geo)
    if sky := c.u(0x70):
        geo = Geometry(c, w, [sky + 12, sky + 16, sky + 20, None, sky + 24])
        direct(sky, geo, False)
        geo.finish()
        extras.append(geo)
    root.finish()
    # Authored PS2 batches mix triangle winding (also confirmed against the
    # unchanged collision topology). Preserve their geometry and render both
    # sides through the native material flag rather than dropping half a wall.
    for surface in surfaces:
        w.word(surface + 40, c.u(surface + 40) | 1)
    # Numeric word reversal already moves PS2 wrap U/V into GX V/U byte slots.

    hierarchies = {c.u(0x50)}
    for group in c.chain(0x48):
        if c.u(group + 40):
            hierarchies.add(c.u(group + 40))
    for source in sources:
        if c.u(source + 36):
            hierarchies.add(c.u(source + 36))
    for h in hierarchies:
        for i in range(c.u(h + 60)):
            w.shorts(c.u(h + 4) + i * 24, 2)
        for pointer, count, stride in [(12, 72, 5), (20, 80, 1), (24, 88, 3)]:
            if c.u(h + pointer):
                w.shorts(c.u(h + pointer), c.u(h + count) * stride)
    portal_nodes, portal_count = convert_portals(c, w)
    matrices = convert_matrices(c, w)
    regions = convert_regions(c, w)
    resource_bytes = append_base_resources(c, w, base) if base else 0
    result = w.finish()
    check = Container(result, '>')
    if len(check.chain(64)) != len(c.chain(64)):
        raise ValueError('World entity count changed')
    return result, dict(render_triangles=root.triangles,
                        prop_triangles=sum(g.triangles for g in extras),
                        textures=len(c.chain(12)), entities=len(c.chain(64)),
                        hierarchies=len(hierarchies), preserved_strings=len(strings),
                        portal_nodes=portal_nodes, portals=portal_count,
                        native_resource_bytes=resource_bytes, matrices=matrices,
                        native_two_sided_materials=len(surfaces), rgb565_textures=compact_textures,
                        effect_regions=regions)
