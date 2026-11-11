"""NPDM (META / ACI0 / ACID) reader: program flags, main thread parameters,
service access list and kernel capabilities."""
import struct

ADDRESS_SPACES = {0: '32-bit', 1: '64-bit (36-bit, old)', 2: '32-bit (no reserved)', 3: '64-bit (39-bit)'}


def _sac(blob):
    """Service access control: [ctrl byte (len-1 | 0x80 server)] [name]..."""
    out = []
    i = 0
    while i < len(blob):
        ctrl = blob[i]
        n = (ctrl & 7) + 1
        name = blob[i + 1:i + 1 + n].decode('ascii', 'replace')
        out.append((name, bool(ctrl & 0x80)))
        i += 1 + n
    return out


def _kac(blob):
    """Kernel access control descriptors (the low set bits give the type)."""
    caps = {'svcs': set(), 'raw': []}
    for (d,) in struct.iter_unpack('<I', blob[:len(blob) // 4 * 4]):
        if d == 0xFFFFFFFF:
            continue
        low = (~d & (d + 1))  # lowest clear bit, as a mask
        t = low.bit_length() - 1
        if t == 3:  # thread info: lowest/highest priority, min/max core
            caps['thread'] = {
                'highest_prio': (d >> 4) & 0x3F, 'lowest_prio': (d >> 10) & 0x3F,
                'min_core': (d >> 16) & 0xFF, 'max_core': (d >> 24) & 0xFF,
            }
        elif t == 4:  # enable system calls: 24 bits mask, 3-bit index
            mask = (d >> 5) & 0xFFFFFF
            idx = (d >> 29) & 7
            for b in range(24):
                if mask >> b & 1:
                    caps['svcs'].add(idx * 24 + b)
        elif t == 14:
            caps['kernel_version'] = ((d >> 19) & 0x1FFF, (d >> 15) & 0xF)
        elif t == 15:
            caps['handle_table_size'] = (d >> 16) & 0x3FF
        elif t == 16:
            caps['debug_flags'] = (d >> 17) & 0x7
        elif t == 13:
            caps['program_type'] = (d >> 14) & 0x7
        else:
            caps['raw'].append((t, d))
    return caps


class Npdm:
    def __init__(self, path):
        d = open(path, 'rb').read()
        if d[:4] != b'META':
            raise ValueError(f'{path}: not an NPDM')
        self.flags = d[0x0C]
        self.is_64bit = bool(self.flags & 1)
        self.address_space = ADDRESS_SPACES.get((self.flags >> 1) & 7, f'type {(self.flags >> 1) & 7}')
        self.main_thread_priority = d[0x0E]
        self.main_thread_core = d[0x0F]
        self.system_resource_size = struct.unpack_from('<I', d, 0x14)[0]
        self.version = struct.unpack_from('<I', d, 0x18)[0]
        self.main_thread_stack_size = struct.unpack_from('<I', d, 0x1C)[0]
        self.name = d[0x20:0x30].split(b'\0')[0].decode('ascii', 'replace')
        self.product_code = d[0x30:0x40].split(b'\0')[0].decode('ascii', 'replace')
        aci_off, aci_size, acid_off, acid_size = struct.unpack_from('<IIII', d, 0x70)
        aci = d[aci_off:aci_off + aci_size]
        if aci[:4] != b'ACI0':
            raise ValueError(f'{path}: ACI0 missing')
        self.program_id = struct.unpack_from('<Q', aci, 0x10)[0]
        sac_off, sac_size, kac_off, kac_size = struct.unpack_from('<IIII', aci, 0x28)
        self.services = _sac(aci[sac_off:sac_off + sac_size])
        self.kernel = _kac(aci[kac_off:kac_off + kac_size])
        self.acid_present = d[acid_off + 0x200:acid_off + 0x204] == b'ACID' if acid_size else False
