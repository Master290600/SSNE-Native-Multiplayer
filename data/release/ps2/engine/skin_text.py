from ps2_content import SKINS
from prepare_ps2_pack import database_records, key
import struct
def extend(original, ps2, character_label):
    old = database_records(original, '>')
    new = {h: (p, flag) for p, flag, h in database_records(ps2, '<')}
    hashes = {h for _, _, h in old}
    additions = []
    for name in SKINS:
        h = key(name + '_DESC_BODY')
        if h not in hashes:
            p, flag = new[h]
            additions.append((h, flag, ps2[p:ps2.index(0, p) + 1]))
    n, nv = struct.unpack_from('>II', original)
    shift = len(additions) * 12
    old_end = 8 + n * 12 + nv * 4
    label_index = 168
    label_text = character_label.encode('latin1') + b'\0'
    label_pointer = len(original) + shift + sum(len(value) for _, _, value in additions)
    out = bytearray(struct.pack('>II', n + len(additions), nv))
    for index, (p, flag, h) in enumerate(old):
        out.extend(struct.pack('>III', label_pointer if index == label_index else p + shift, flag, h))
    text = bytearray()
    for h, flag, value in additions:
        out.extend(struct.pack('>III', len(original) + shift + len(text), flag, h))
        text.extend(value)
    for i in range(nv):
        out.extend(struct.pack('>I', struct.unpack_from('>I', original, 8 + n * 12 + i * 4)[0] + shift))
    out.extend(original[old_end:])
    out.extend(text)
    out.extend(label_text)
    after = database_records(out, '>')
    for index, (p, flag, h) in enumerate(old):
        q, new_flag, new_h = after[index]
        assert (flag, h) == (new_flag, new_h)
        expected = label_text[:-1] if index == label_index else original[p:original.index(0, p)]
        assert expected == out[q:out.index(0, q)]
    assert len({h for _, _, h in after}) == len(after)
    for name in SKINS:
        assert key(name + '_DESC_BODY') in {h for _, _, h in after}
    return bytes(out), len(additions), old[label_index][2]

