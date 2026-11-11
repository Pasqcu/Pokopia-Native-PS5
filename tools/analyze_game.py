#!/usr/bin/env python3
"""Survey of the game dump: modules, segments, symbols, imports between modules, relocations, instruction classes,
SDK / engine strings, NPDM and RomFS. Writes build/analysis/analysis.json and prints a summary.

    .venv/bin/python tools/analyze_game.py [dump folder]
"""
import collections
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nx.nso import Nso, RELOC_NAMES, STT_NAMES  # noqa: E402
from nx.npdm import Npdm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULES = ('rtld', 'main', 'subsdk0', 'sdk')

# A64 top-level encoding groups, op0 = bits 28:25 (Arm ARM C4.1)
GROUPS = {
    0b0000: 'reserved/undef', 0b0001: 'unallocated', 0b0010: 'sve', 0b0011: 'unallocated',
    0b1000: 'dp-imm', 0b1001: 'dp-imm', 0b1010: 'branch/sys', 0b1011: 'branch/sys',
    0b0100: 'ldst', 0b0110: 'ldst', 0b1100: 'ldst', 0b1110: 'ldst',
    0b0101: 'dp-reg', 0b1101: 'dp-reg', 0b0111: 'simd-fp', 0b1111: 'simd-fp',
}


def classify(w):
    g = GROUPS[(w >> 25) & 0xF]
    if g == 'ldst' and (w >> 26) & 1:
        return 'ldst-simd'
    if g == 'ldst' and (w & 0x3F000000) == 0x08000000:
        return 'ldst-exclusive/atomic' if (w >> 21) & 1 == 0 else 'ldst'
    if g == 'ldst' and (w & 0x3B200C00) == 0x38200000:
        return 'atomic-lse'
    if g == 'branch/sys' and (w & 0xFFC00000) == 0xD5000000:
        return 'system'
    if w == 0xD503201F:
        return 'nop'
    return g


def survey_module(path, name, cache):
    m = Nso(path, name, cache_dir=cache)
    text_off, text_size = m.text
    words = struct.unpack_from(f'<{text_size // 4}I', m.image, text_off)
    classes = collections.Counter(classify(w) for w in words)
    svc = collections.Counter((w >> 5) & 0xFFFF for w in words if (w & 0xFFE0001F) == 0xD4000001)
    mrs = collections.Counter((w >> 5) & 0x7FFF for w in words if (w & 0xFFF00000) == 0xD5300000)
    defined = [s for s in m.symbols if s.defined and s.name]
    undefined = [s for s in m.symbols if not s.defined and s.name]
    relocs = collections.Counter(RELOC_NAMES.get(r.type, str(r.type)) for r in m.relocs)
    strings = m.strings(min_len=8)
    sdk_ver = sorted({s for _, s in strings if 'NintendoSDK' in s or 'nnSdk' in s})[:8]
    engine = sorted({s for _, s in strings if re.search(r'KTGL|ktgl|KTSL|Koei|KT_', s)})[:12]
    nvn = sorted({s for _, s in strings if re.fullmatch(r'nvn[A-Z]\w+', s)})
    return m, {
        'build_id': m.build_id[:20].hex().upper(),
        'text': [hex(text_off), hex(text_size)], 'ro': [hex(m.ro[0]), hex(m.ro[1])],
        'data': [hex(m.data[0]), hex(m.data[1])], 'bss': hex(m.bss_size),
        'image_size': hex(m.size), 'image_plus_bss': hex(m.size + m.bss_size),
        'instruction_words': len(words),
        'eh_frame_functions': len(m.eh_frame_function_starts()),
        'symbols_defined': len(defined), 'symbols_undefined': len(undefined),
        'func_symbols_defined': sum(1 for s in defined if s.type == 2),
        'relocations': dict(relocs),
        'instruction_classes': dict(classes.most_common()),
        'svc_numbers': {hex(k): v for k, v in sorted(svc.items())},
        'mrs_sysregs': {hex(k): v for k, v in mrs.most_common(12)},
        'module_path': m.module_path_string(),
        'needed': m.needed, 'soname': m.soname,
        'sdk_strings': sdk_ver, 'engine_strings': engine,
        'nvn_entry_names': len(nvn), 'nvn_sample': nvn[:10],
    }


def imports_between(mods):
    exports = {}
    for name, m in mods.items():
        for s in m.symbols:
            if s.defined and s.name and s.bind in (1, 2):
                exports.setdefault(s.name, name)
    table = {}
    for name, m in mods.items():
        c = collections.Counter()
        unresolved = []
        for s in m.symbols:
            if not s.defined and s.name:
                src = exports.get(s.name)
                if src:
                    c[src] += 1
                else:
                    unresolved.append(s.name)
        table[name] = {'from': dict(c), 'unresolved': len(unresolved), 'unresolved_sample': unresolved[:15]}
    return table


def romfs_survey(romfs):
    out = []
    if not os.path.isdir(romfs):
        return out
    for entry in sorted(os.listdir(romfs)):
        if entry.startswith('.'):
            continue
        p = os.path.join(romfs, entry)
        if os.path.isdir(p):
            total = files = 0
            for dp, _, fs in os.walk(p):
                for f in fs:
                    if not f.startswith('.'):
                        files += 1
                        total += os.path.getsize(os.path.join(dp, f))
            out.append({'name': entry + '/', 'files': files, 'bytes': total})
        else:
            out.append({'name': entry, 'files': 1, 'bytes': os.path.getsize(p)})
    return out


def main():
    dump = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'Pokemon Pokopia [0400A3A01EE4C000]')
    cache = os.path.join(ROOT, 'build', 'nso_cache')
    report = {'dump': dump, 'versions': {}}
    versions = [('base', os.path.join(dump, 'base'))]
    upd = os.path.join(dump, 'updates')
    if os.path.isdir(upd):
        for v in sorted((d for d in os.listdir(upd) if d.isdigit()), key=int):
            versions.append((f'update {v}', os.path.join(upd, v)))
    for label, vdir in versions:
        exefs = os.path.join(vdir, 'exefs')
        mods, info = {}, {}
        for name in MODULES:
            p = os.path.join(exefs, name)
            if os.path.exists(p):
                mods[name], info[name] = survey_module(p, name, cache)
                print(f'[{label}] {name:8s} build {info[name]["build_id"][:16]} text {info[name]["text"][1]:>10s} '
                      f'insns {info[name]["instruction_words"]:>9,d} fde {info[name]["eh_frame_functions"]:>7,d} '
                      f'syms {info[name]["symbols_defined"]:>6,d}/{info[name]["symbols_undefined"]:<5,d}', flush=True)
        entry = {'modules': info, 'imports': imports_between(mods)}
        npdm_path = os.path.join(exefs, 'main.npdm')
        if os.path.exists(npdm_path):
            n = Npdm(npdm_path)
            entry['npdm'] = {
                'name': n.name, 'program_id': f'{n.program_id:016X}', 'flags': hex(n.flags),
                'address_space': n.address_space, 'main_thread_priority': n.main_thread_priority,
                'main_thread_core': n.main_thread_core, 'main_thread_stack': hex(n.main_thread_stack_size),
                'system_resource_size': hex(n.system_resource_size), 'version': n.version,
                'services': [s for s, server in n.services if not server],
                'kernel_thread': n.kernel.get('thread'), 'kernel_version': n.kernel.get('kernel_version'),
                'svcs_allowed': sorted(n.kernel['svcs']),
            }
        romfs = os.path.join(vdir, 'romfs')
        if os.path.isdir(romfs):
            entry['romfs'] = romfs_survey(romfs)
        report['versions'][label] = entry
    out_dir = os.path.join(ROOT, 'build', 'analysis')
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'analysis.json'), 'w') as f:
        json.dump(report, f, indent=1, default=str)
    print('wrote', os.path.join(out_dir, 'analysis.json'))


if __name__ == '__main__':
    main()
