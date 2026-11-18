import struct
from prepare_ps2_test_data import files, fst_tree, serialize
def next_extents(fst, boot, sizes):
    tree = fst_tree(fst)
    current = dict(files(tree))
    end = max(n['offset']+n['size'] for n in current.values())
    for name, size in sorted(sizes.items()):
        directory, parts = tree,name.split('/')
        for part in parts[:-1]:
            directory, = [child for child in directory['children'] if child['name'].lower()==part.lower() and child['directory']]
        matching = [child for child in directory['children'] if child['name'].lower()==parts[-1].lower()]
        if matching:
            entry, = matching
        else:
            entry = dict(directory=False,name=parts[-1],children=[])
            directory['children'].append(entry)
        end = (end+31)&~31
        entry.update(offset=end,size=size)
        end += size
    out=serialize(tree)
    b=bytearray(boot)
    struct.pack_into('>3I',b,0x424,(end+31)&~31,len(out),len(out))
    return out,bytes(b)
