#!/usr/bin/env python3
"""mkinitrd.py out.img name=path [name=path ...]  -- образ MIGARD01 (см. kernel/include/kernel/initrd.h)"""
import struct, sys

def die(m):
    sys.exit("mkinitrd: " + m)

if len(sys.argv) < 3:
    die("usage: mkinitrd.py out.img name=path ...")
items = []
for a in sys.argv[2:]:
    if "=" not in a:
        die("bad arg: " + a)
    name, path = a.split("=", 1)
    nb = name.encode()
    if not 0 < len(nb) < 40 or b"/" in nb:
        die("bad name: " + name)
    with open(path, "rb") as f:
        items.append((nb, f.read()))

table_end = 16 + 64 * len(items)
off = (table_end + 15) & ~15
ents, blob = b"", b""
for nb, data in items:
    ents += struct.pack("<40sQQII", nb, off + len(blob), len(data), 0, 0)
    blob += data + b"\0" * (-len(data) % 16)
total = off + len(blob)
img = b"MIGARD01" + struct.pack("<II", len(items), total) + ents
img += b"\0" * (off - len(img)) + blob
with open(sys.argv[1], "wb") as f:
    f.write(img)
print("initrd: %d files, %d bytes" % (len(items), total))
