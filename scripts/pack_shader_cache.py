#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Packs the shader cache (scripts/build_shader_cache.sh: build/shader_cache/spirv, ~99,000 files) into one file,
# spirv.pack, for the PS5: one upload instead of a hundred thousand, and a quarter of the size. The runtime reads it
# through runtime/nxrt_port/gfx/shader_pack.cpp (nvn_shaders.cpp's read_file, transformed). Format, little-endian:
#   header   "PKPSPK01" | u32 count | u32 reserved | u64 index_offset
#   data     each file, LZ4 block (or stored when that is not smaller)
#   index    count x { char name[40] (NUL padded) | u64 offset | u32 stored_size | u32 size | u32 flags (1 = LZ4) | u32 0 }
#            sorted by name
# The pack is derived from the game: it stays local (build/), like the cache.
#
#   scripts/pack_shader_cache.py [build/shader_cache/spirv] [build/shader_cache/spirv.pack]
import os
import struct
import sys

import lz4.block

src = sys.argv[1] if len(sys.argv) > 1 else "build/shader_cache/spirv"
dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(src.rstrip("/")), "spirv.pack")
names = sorted(n for n in os.listdir(src) if n.endswith(".spv") or n == "results.tsv")
if any(len(n.encode()) >= 40 for n in names):
    sys.exit("a file name is too long for the pack index")
entries = []
total_in = total_out = 0
with open(dst + ".part", "wb") as out:
    out.write(b"PKPSPK01" + struct.pack("<IIQ", len(names), 0, 0))
    for i, n in enumerate(names):
        data = open(os.path.join(src, n), "rb").read()
        comp = lz4.block.compress(data, store_size=False, mode="high_compression", compression=9)
        flags = 1
        if len(comp) >= len(data):
            comp, flags = data, 0
        off = out.tell()
        out.write(comp)
        entries.append((n.encode(), off, len(comp), len(data), flags))
        total_in += len(data)
        total_out += len(comp)
        if i % 5000 == 0:
            print(f"  {i} of {len(names)}", flush=True)
    index_offset = out.tell()
    for name, off, stored, size, flags in entries:
        out.write(name.ljust(40, b"\0") + struct.pack("<QIIII", off, stored, size, flags, 0))
    out.seek(8)
    out.write(struct.pack("<IIQ", len(names), 0, index_offset))
os.replace(dst + ".part", dst)
print(f"{dst}: {len(names)} files, {total_in / 2**30:.2f} GiB -> {total_out / 2**30:.2f} GiB")
