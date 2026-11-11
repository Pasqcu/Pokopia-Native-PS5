"""NSO0 module loader: header, segments, MOD0, ELF dynamic info, symbols,
relocations (RELA / JMPREL / RELR) and .eh_frame_hdr function starts.

The memory image is text + ro + data at their memory offsets; bss is only a
size. Offsets in this module are image-relative (module base = 0).
"""
import hashlib
import os
import struct

from .lz4 import decompress_block

PAGE = 0x1000

# ELF dynamic tags
DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_PLTGOT, DT_HASH, DT_STRTAB, DT_SYMTAB = 0, 1, 2, 3, 4, 5, 6
DT_RELA, DT_RELASZ, DT_RELAENT, DT_STRSZ, DT_SYMENT, DT_INIT, DT_FINI = 7, 8, 9, 10, 11, 12, 13
DT_SONAME, DT_REL, DT_PLTREL, DT_JMPREL = 14, 17, 20, 23
DT_INIT_ARRAY, DT_FINI_ARRAY, DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ = 25, 26, 27, 28
DT_RELRSZ, DT_RELR, DT_RELRENT = 35, 36, 37
DT_GNU_HASH, DT_RELACOUNT = 0x6FFFFEF5, 0x6FFFFFF9

# AArch64 relocation types
R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027
R_AARCH64_TLS_TPREL64 = 1030
R_AARCH64_TLSDESC = 1031
R_AARCH64_RELR = -1  # pseudo type for RELR-encoded relative relocations

RELOC_NAMES = {
    R_AARCH64_ABS64: 'ABS64', R_AARCH64_GLOB_DAT: 'GLOB_DAT', R_AARCH64_JUMP_SLOT: 'JUMP_SLOT',
    R_AARCH64_RELATIVE: 'RELATIVE', R_AARCH64_TLS_TPREL64: 'TLS_TPREL64', R_AARCH64_TLSDESC: 'TLSDESC',
    R_AARCH64_RELR: 'RELR',
}

STT_NAMES = {0: 'NOTYPE', 1: 'OBJECT', 2: 'FUNC', 3: 'SECTION', 4: 'FILE', 6: 'TLS', 10: 'IFUNC'}
STB_NAMES = {0: 'LOCAL', 1: 'GLOBAL', 2: 'WEAK', 10: 'GNU_UNIQUE'}


class NsoError(ValueError):
    pass


class Symbol:
    __slots__ = ('index', 'name', 'value', 'size', 'type', 'bind', 'shndx')

    def __init__(self, index, name, value, size, info, shndx):
        self.index, self.name, self.value, self.size = index, name, value, size
        self.type, self.bind, self.shndx = info & 0xF, info >> 4, shndx

    @property
    def defined(self):
        return self.shndx != 0

    def __repr__(self):
        return f'<Sym {self.name} {self.value:#x} {STT_NAMES.get(self.type, self.type)} {"def" if self.defined else "undef"}>'


class Reloc:
    __slots__ = ('offset', 'type', 'sym', 'addend')

    def __init__(self, offset, rtype, sym, addend):
        self.offset, self.type, self.sym, self.addend = offset, rtype, sym, addend


class Nso:
    def __init__(self, path, name=None, verify_hash=True, cache_dir=None):
        self.path = path
        self.name = name or os.path.basename(path)
        with open(path, 'rb') as f:
            d = f.read()
        if d[:4] != b'NSO0':
            raise NsoError(f'{path}: not an NSO0 file')
        self.version, _, self.flags = struct.unpack_from('<III', d, 4)
        segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]  # file_off, mem_off, size
        self.module_name_off, self.module_name_size = struct.unpack_from('<I', d, 0x1C)[0], struct.unpack_from('<I', d, 0x2C)[0]
        self.bss_size = struct.unpack_from('<I', d, 0x3C)[0]
        self.build_id = bytes(d[0x40:0x60])
        csizes = struct.unpack_from('<III', d, 0x60)
        self.api_info = struct.unpack_from('<II', d, 0x88)
        self.dynstr_info = struct.unpack_from('<II', d, 0x90)
        self.dynsym_info = struct.unpack_from('<II', d, 0x98)
        hashes = [bytes(d[0xA0 + i * 0x20:0xC0 + i * 0x20]) for i in range(3)]
        if self.flags & 0x80:
            raise NsoError(f'{path}: ZBIC (zstd) compressed segments are not supported by this loader')
        end = segs[2][1] + segs[2][2]
        img = None
        cache = None
        if cache_dir:
            os.makedirs(cache_dir, exist_ok=True)
            cache = os.path.join(cache_dir, f'{self.build_id[:16].hex()}.img')
            if os.path.exists(cache) and os.path.getsize(cache) == end:
                with open(cache, 'rb') as f:
                    img = bytearray(f.read())
        if img is None:
            img = bytearray(end)
            for i, ((fo, mo, sz), csz) in enumerate(zip(segs, csizes)):
                compressed = (self.flags >> i) & 1
                raw = d[fo:fo + (csz if compressed else sz)]
                if compressed:
                    raw = decompress_block(raw, sz)
                if len(raw) != sz:
                    raise NsoError(f'{path}: segment {i} is {len(raw):#x} bytes, header says {sz:#x}')
                if verify_hash and (self.flags >> (3 + i)) & 1:
                    if hashlib.sha256(raw).digest() != hashes[i]:
                        raise NsoError(f'{path}: segment {i} SHA-256 mismatch (damaged file)')
                img[mo:mo + sz] = raw
            if cache:
                with open(cache, 'wb') as f:
                    f.write(img)
        self.image = img
        self.text = (segs[0][1], segs[0][2])
        self.ro = (segs[1][1], segs[1][2])
        self.data = (segs[2][1], segs[2][2])
        self.size = end  # image size without bss
        self._parse_mod0()
        self._parse_dynamic()

    # ------------------------------------------------------------------ helpers
    def u32(self, off):
        return struct.unpack_from('<I', self.image, off)[0]

    def i32(self, off):
        return struct.unpack_from('<i', self.image, off)[0]

    def u64(self, off):
        return struct.unpack_from('<Q', self.image, off)[0]

    def cstr(self, off, limit=4096):
        e = self.image.find(b'\0', off, off + limit)
        return bytes(self.image[off:e if e >= 0 else off + limit]).decode('utf-8', 'replace')

    def in_text(self, off):
        return self.text[0] <= off < self.text[0] + self.text[1]

    def segment_of(self, off):
        for name, (o, s) in (('text', self.text), ('ro', self.ro), ('data', self.data)):
            if o <= off < o + s:
                return name
        if self.data[0] + self.data[1] <= off < self.bss_end:
            return 'bss'
        return None

    # ------------------------------------------------------------------ MOD0
    def _parse_mod0(self):
        mod0 = self.u32(4)
        self.mod0 = mod0
        if self.image[mod0:mod0 + 4] != b'MOD0':
            raise NsoError(f'{self.path}: MOD0 header missing at {mod0:#x}')
        rel = [self.i32(mod0 + 4 + 4 * i) for i in range(6)]
        self.dynamic = mod0 + rel[0]
        self.bss_start = mod0 + rel[1]
        self.bss_end = mod0 + rel[2]
        self.eh_frame_hdr = (mod0 + rel[3], mod0 + rel[4])
        self.module_object = mod0 + rel[5]

    # ------------------------------------------------------------------ dynamic
    def _parse_dynamic(self):
        dyn = {}
        needed = []
        off = self.dynamic
        while off + 16 <= len(self.image):
            tag, val = struct.unpack_from('<qQ', self.image, off)
            off += 16
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                needed.append(val)
            else:
                dyn[tag] = val
        self.dyn = dyn
        strtab = dyn.get(DT_STRTAB)
        self.strtab = strtab
        self.needed = [self.cstr(strtab + v) for v in needed] if strtab is not None else []
        self.soname = self.cstr(strtab + dyn[DT_SONAME]) if DT_SONAME in dyn and strtab is not None else None
        self.init = dyn.get(DT_INIT)
        self.fini = dyn.get(DT_FINI)
        self.init_array = (dyn.get(DT_INIT_ARRAY), dyn.get(DT_INIT_ARRAYSZ, 0))
        self.fini_array = (dyn.get(DT_FINI_ARRAY), dyn.get(DT_FINI_ARRAYSZ, 0))
        self.pltgot = dyn.get(DT_PLTGOT)
        self._parse_symbols()
        self._parse_relocs()

    def _nsyms(self):
        dyn = self.dyn
        if DT_HASH in dyn:
            return self.u32(dyn[DT_HASH] + 4)  # nchain
        if DT_GNU_HASH in dyn:
            h = dyn[DT_GNU_HASH]
            nbuckets, symoffset, bloom_size, _ = struct.unpack_from('<IIII', self.image, h)
            buckets = h + 16 + bloom_size * 8
            chains = buckets + nbuckets * 4
            last = 0
            for b in range(nbuckets):
                last = max(last, self.u32(buckets + b * 4))
            if last < symoffset:
                return symoffset
            while not (self.u32(chains + (last - symoffset) * 4) & 1):
                last += 1
            return last + 1
        if DT_SYMTAB in dyn and DT_STRTAB in dyn and dyn[DT_STRTAB] > dyn[DT_SYMTAB]:
            return (dyn[DT_STRTAB] - dyn[DT_SYMTAB]) // 24
        return 0

    def _parse_symbols(self):
        self.symbols = []
        symtab = self.dyn.get(DT_SYMTAB)
        if symtab is None:
            return
        for i in range(self._nsyms()):
            st_name, info, _other, shndx, value, size = struct.unpack_from('<IBBHQQ', self.image, symtab + i * 24)
            name = self.cstr(self.strtab + st_name) if st_name else ''
            self.symbols.append(Symbol(i, name, value, size, info, shndx))

    def _parse_relocs(self):
        relocs = []
        dyn = self.dyn

        def rela(start, size):
            for o in range(start, start + size, 24):
                r_off, r_info, r_add = struct.unpack_from('<QQq', self.image, o)
                relocs.append(Reloc(r_off, r_info & 0xFFFFFFFF, r_info >> 32, r_add))

        if DT_RELA in dyn:
            rela(dyn[DT_RELA], dyn.get(DT_RELASZ, 0))
        if DT_JMPREL in dyn:
            if dyn.get(DT_PLTREL, 7) != 7:
                raise NsoError(f'{self.path}: DT_PLTREL is not RELA')
            rela(dyn[DT_JMPREL], dyn.get(DT_PLTRELSZ, 0))
        if DT_REL in dyn:
            raise NsoError(f'{self.path}: REL relocations are not expected on AArch64')
        if DT_RELR in dyn:
            # RELR: even entry = address, odd entry = bitmap of the next 63 words
            start, size = dyn[DT_RELR], dyn.get(DT_RELRSZ, 0)
            where = 0
            for o in range(start, start + size, 8):
                e = self.u64(o)
                if not e & 1:
                    relocs.append(Reloc(e, R_AARCH64_RELR, 0, None))
                    where = e + 8
                else:
                    bits = e >> 1
                    k = 0
                    while bits:
                        if bits & 1:
                            relocs.append(Reloc(where + k * 8, R_AARCH64_RELR, 0, None))
                        bits >>= 1
                        k += 1
                    where += 63 * 8
        self.relocs = relocs

    # ------------------------------------------------------------------ eh_frame
    def eh_frame_function_starts(self):
        """Function start offsets from the .eh_frame_hdr binary search table."""
        lo, hi = self.eh_frame_hdr
        if hi <= lo:
            return []
        version, eh_ptr_enc, fde_count_enc, table_enc = self.image[lo:lo + 4]
        if version != 1:
            return []
        p = lo + 4

        def read(enc, at):
            fmt = enc & 0x0F
            app = enc & 0x70
            if fmt == 0x0B:  # sdata4
                v, n = struct.unpack_from('<i', self.image, at)[0], 4
            elif fmt == 0x03:  # udata4
                v, n = struct.unpack_from('<I', self.image, at)[0], 4
            elif fmt == 0x0C:  # sdata8
                v, n = struct.unpack_from('<q', self.image, at)[0], 8
            elif fmt == 0x04:  # udata8
                v, n = struct.unpack_from('<Q', self.image, at)[0], 8
            else:
                raise NsoError(f'unsupported eh_frame_hdr encoding {enc:#x}')
            if app == 0x10:  # pcrel
                v += at
            elif app == 0x30:  # datarel (relative to eh_frame_hdr start)
                v += lo
            return v, n

        _eh_frame, n = read(eh_ptr_enc, p)
        p += n
        count, n = read(fde_count_enc, p)
        p += n
        if table_enc != 0x3B:  # datarel sdata4 is what every linker writes
            raise NsoError(f'unsupported eh_frame_hdr table encoding {table_enc:#x}')
        starts = []
        for i in range(count):
            initial = struct.unpack_from('<i', self.image, p + i * 8)[0] + lo
            starts.append(initial)
        return starts

    # ------------------------------------------------------------------ misc
    def module_path_string(self):
        """The module path that the linker records at the start of .ro (e.g. a build path)."""
        ro = self.ro[0]
        length = self.u32(ro + 4)
        if 0 < length < 0x200 and self.u32(ro) == 0:
            return self.cstr(ro + 8, length)
        return None

    def strings(self, min_len=6, seg=('ro', 'data')):
        """Printable ASCII strings in the given segments (offset, text)."""
        import re
        out = []
        pat = re.compile(rb'[\x20-\x7e]{%d,}' % min_len)
        for s in seg:
            o, sz = getattr(self, s)
            for m in pat.finditer(bytes(self.image[o:o + sz])):
                out.append((o + m.start(), m.group().decode('ascii')))
        return out
