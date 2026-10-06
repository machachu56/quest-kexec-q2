#!/usr/bin/env python3
# Print the logical partitions (name, [(start sector, sectors)]) of an
# Android dynamic-partitions "super" device. Usage: lpdump.py /dev/sdaN
import struct, sys
f = open(sys.argv[1], "rb")
f.seek(4096 + 2 * 4096)  # reserved + primary/backup geometry
h = f.read(256)
magic, major, minor, hsize = struct.unpack_from("<IHHI", h, 0)
assert magic == 0x414C5030, hex(magic)
tables_size, = struct.unpack_from("<I", h, 44)
descs = struct.unpack_from("<12I", h, 80)
f.seek(4096 + 2 * 4096 + hsize); t = f.read(tables_size)
po, pn, ps, eo, en, es = descs[:6]
ext = [struct.unpack_from("<QIQI", t, eo + i * es) for i in range(en)]
for i in range(pn):
    name, attr, fe, ne, grp = struct.unpack_from("<36sIIII", t, po + i * ps)
    name = name.rstrip(b"\0").decode()
    print(name, [(e[2], e[0]) for e in ext[fe:fe + ne]])
