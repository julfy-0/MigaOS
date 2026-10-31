#!/usr/bin/env python3
"""Проверка PE32+ образа MigaBoot: подсистема EFI app, entry, секции, релок-таблица."""
import struct, sys

d = open(sys.argv[1], "rb").read()
assert d[:2] == b"MZ", "нет MZ"
pe = struct.unpack_from("<I", d, 0x3C)[0]
assert d[pe:pe + 4] == b"PE\0\0", "нет PE сигнатуры"
machine, nsec, _, _, _, optsz, _ = struct.unpack_from("<HHIIIHH", d, pe + 4)
opt = pe + 24
magic, = struct.unpack_from("<H", d, opt)
entry, = struct.unpack_from("<I", d, opt + 16)
image_base, = struct.unpack_from("<Q", d, opt + 24)
subsystem, = struct.unpack_from("<H", d, opt + 68)
reloc_rva, reloc_size = struct.unpack_from("<II", d, opt + 112 + 5 * 8)
secs = []
so = opt + optsz
for i in range(nsec):
    n, vs, va, rs, ro = struct.unpack_from("<8sIIII", d, so + 40 * i)
    secs.append((n.rstrip(b"\0").decode(), va, vs))
ok = machine == 0x8664 and magic == 0x20B and subsystem == 10 and reloc_size > 0
print(f"PE check: machine={machine:#x} magic={magic:#x} subsystem={subsystem} entry={entry:#x} "
      f"imagebase={image_base:#x} reloc_dir={reloc_size}B sections={[s[0] for s in secs]}")
if not any(va <= entry < va + vs for _, va, vs in secs):
    ok = False; print("entry вне секций!")
sys.exit(0 if ok else 1)
