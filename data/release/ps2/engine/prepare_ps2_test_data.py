"""Create an isolated test installation with hardlinks and an updated virtual FST.

Existing data is read only. Overrides replace links atomically, so no installed
file can be changed through a shared inode.
"""
import argparse
import os
import shutil
import struct
from pathlib import Path


def fst_tree(data):
    count = struct.unpack_from('>I', data, 8)[0]
    strings = count * 12
    def read(index):
        kind, offset, size = struct.unpack_from('>3I', data, index * 12)
        start = strings + (kind & 0xffffff)
        name = data[start:data.index(0, start)].decode('ascii')
        node = dict(name=name, directory=bool(kind >> 24), offset=offset, size=size, children=[])
        following = index + 1
        if node['directory']:
            while following < size:
                child, following = read(following)
                node['children'].append(child)
        return node, following
    root, end = read(0)
    if end != count:
        raise ValueError('FST root count mismatch')
    return root


def files(node, prefix=''):
    for child in node['children']:
        path = prefix + child['name']
        if child['directory']:
            yield from files(child, path + '/')
        else:
            yield path, child


def serialize(root):
    rows = []
    names = bytearray(b'\0')
    def walk(node, parent):
        at = len(rows)
        name = 0 if at == 0 else len(names)
        if at:
            names.extend(node['name'].encode('ascii') + b'\0')
        rows.append([name | (0x1000000 if node['directory'] else 0),
                     parent if node['directory'] else node['offset'], node['size']])
        if node['directory']:
            for child in node['children']:
                walk(child, at)
            rows[at][2] = len(rows)
    walk(root, 0)
    return b''.join(struct.pack('>3I', *r) for r in rows) + names
