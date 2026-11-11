// SPDX-License-Identifier: GPL-3.0-or-later
// NSO0 loader: LZ4 segments, MOD0, ELF dynamic section, relocations (RELA, JMPREL, RELR), cross-module binding by name.
// File-format facts from the Switchbrew NSO page and the ELF / AArch64 ELF ABI; the code is written for this project.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pkp/platform.h"
#include "pkp/rt.h"

namespace pkp {

namespace {

constexpr int kMaxModules = 8;
LoadedModule g_mods[kMaxModules];
int g_nmods;

// relocations against symbols, bound after every module is loaded
struct PendingReloc {
    uint64_t where;
    int64_t addend;
    uint32_t type, sym;
    int module;
};
PendingReloc* g_pending;
size_t g_npending, g_cap_pending;

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// LZ4 block format (lz4 doc/lz4_Block_format.md): token, literals, 2-byte offset, match; the last sequence has literals only.
bool lz4_block(const uint8_t* src, size_t n, uint8_t* dst, size_t out_size) {
    size_t i = 0, o = 0;
    while (i < n) {
        const unsigned token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do { if (i >= n) return false; b = src[i++]; lit += b; } while (b == 255);
        }
        if (i + lit > n || o + lit > out_size) return false;
        std::memcpy(dst + o, src + i, lit);
        i += lit;
        o += lit;
        if (i >= n) break;
        if (i + 2 > n) return false;
        const size_t off = (size_t)src[i] | ((size_t)src[i + 1] << 8);
        i += 2;
        size_t mlen = (token & 15) + 4;
        if ((token & 15) == 15) {
            unsigned b;
            do { if (i >= n) return false; b = src[i++]; mlen += b; } while (b == 255);
        }
        if (off == 0 || off > o || o + mlen > out_size) return false;
        const uint8_t* m = dst + o - off;
        if (off >= mlen) {
            std::memcpy(dst + o, m, mlen);
        } else {
            for (size_t k = 0; k < mlen; k++) dst[o + k] = m[k];   // overlapping: byte order matters
        }
        o += mlen;
    }
    return o == out_size;
}

uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

void add_pending(const PendingReloc& r) {
    if (g_npending == g_cap_pending) {
        g_cap_pending = g_cap_pending ? g_cap_pending * 2 : 1024;
        g_pending = static_cast<PendingReloc*>(std::realloc(g_pending, g_cap_pending * sizeof(PendingReloc)));
        if (!g_pending) { log("[loader] out of host memory"); std::abort(); }
    }
    g_pending[g_npending++] = r;
}

}  // namespace

int module_count() { return g_nmods; }
const LoadedModule* module_at(int i) { return i >= 0 && i < g_nmods ? &g_mods[i] : nullptr; }

uint64_t find_symbol(const char* name) {
    for (int m = 0; m < g_nmods; m++) {
        const NxModule* d = g_mods[m].desc;
        for (int i = 0; i < d->nsyms; i++)
            if (d->syms[i].defined && d->syms[i].name && !std::strcmp(d->syms[i].name, name)) return d->syms[i].value;
    }
    return 0;
}

bool load_module(const NxModule* desc, const char* nso_path) {
    if (g_nmods == kMaxModules) return false;
    uint8_t* f = nullptr;
    size_t fsize = 0;
    if (!plat::read_file(nso_path, &f, &fsize)) {
        log("[loader] %s: cannot read %s", desc->name, nso_path);
        return false;
    }
    bool ok = false;
    do {
        if (fsize < 0x100 || std::memcmp(f, "NSO0", 4)) { log("[loader] %s: not an NSO0 file", desc->name); break; }
        const uint32_t flags = rd32(f + 0x0C);
        if (flags & 0x80) { log("[loader] %s: zstd (ZBIC) segments are not supported", desc->name); break; }
        uint32_t fo[3], mo[3], sz[3], cs[3];
        for (int s = 0; s < 3; s++) {
            fo[s] = rd32(f + 0x10 + s * 0x10);
            mo[s] = rd32(f + 0x14 + s * 0x10);
            sz[s] = rd32(f + 0x18 + s * 0x10);
            cs[s] = rd32(f + 0x60 + s * 4);
        }
        const uint64_t bss = rd32(f + 0x3C);
        const uint64_t image = (uint64_t)mo[2] + sz[2];
        const uint64_t granule = plat::vm_granule();
        const uint64_t mapped = round_up(image + bss, granule);
        LoadedModule& lm = g_mods[g_nmods];
        lm = LoadedModule{};
        lm.desc = desc;
        lm.base = desc->base;
        lm.image_size = image;
        lm.bss_size = bss;
        lm.mapped_size = mapped;
        for (int k = 0; k < 20; k++) std::snprintf(lm.build_id + 2 * k, 3, "%02X", f[0x40 + k]);
        char how[160];
        if (!plat::vm_reserve(lm.base, mapped, how, sizeof how) ||
            !plat::vm_commit(lm.base, mapped, plat::kReadWrite, how, sizeof how)) {
            log("[loader] %s: cannot map %#llx bytes at %#llx: %s", desc->name, (unsigned long long)mapped,
                (unsigned long long)lm.base, how);
            break;
        }
        uint8_t* base = reinterpret_cast<uint8_t*>(lm.base);
        bool seg_ok = true;
        for (int s = 0; s < 3 && seg_ok; s++) {
            const bool compressed = (flags >> s) & 1;
            const uint32_t stored = compressed ? cs[s] : sz[s];
            if ((uint64_t)fo[s] + stored > fsize) { seg_ok = false; break; }
            if (compressed) seg_ok = lz4_block(f + fo[s], stored, base + mo[s], sz[s]);
            else std::memcpy(base + mo[s], f + fo[s], sz[s]);
        }
        if (!seg_ok) { log("[loader] %s: a segment does not decompress", desc->name); break; }
        // MOD0 -> dynamic
        const uint32_t mod0 = rd32(base + 4);
        if (std::memcmp(base + mod0, "MOD0", 4)) { log("[loader] %s: MOD0 missing", desc->name); break; }
        const uint64_t dynamic = mod0 + (int32_t)rd32(base + mod0 + 4);
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, relr = 0, relrsz = 0;
        for (uint64_t d = dynamic;; d += 16) {
            const int64_t tag = (int64_t)rd64(base + d);
            const uint64_t val = rd64(base + d + 8);
            if (tag == 0) break;
            switch (tag) {
            case 7: rela = val; break;
            case 8: relasz = val; break;
            case 23: jmprel = val; break;
            case 2: pltrelsz = val; break;
            case 36: relr = val; break;
            case 35: relrsz = val; break;
            default: break;
            }
        }
        auto apply_rela = [&](uint64_t start, uint64_t size) {
            for (uint64_t o = start; o + 24 <= start + size; o += 24) {
                const uint64_t r_off = rd64(base + o);
                const uint64_t info = rd64(base + o + 8);
                const int64_t add = (int64_t)rd64(base + o + 16);
                const uint32_t type = (uint32_t)info, sym = (uint32_t)(info >> 32);
                uint8_t* where = base + r_off;
                lm.relocs++;
                if (type == 1027) {   // R_AARCH64_RELATIVE
                    const uint64_t v = lm.base + (uint64_t)add;
                    std::memcpy(where, &v, 8);
                    lm.relative++;
                } else if (type == 257 || type == 1025 || type == 1026) {   // ABS64, GLOB_DAT, JUMP_SLOT
                    add_pending(PendingReloc{lm.base + r_off, add, type, sym, g_nmods});
                } else if (type == 1030 || type == 1031) {   // TLS_TPREL64, TLSDESC: the HLE kernel owns TLS (later stage)
                    lm.tls++;
                } else if (type != 0) {
                    log("[loader] %s: relocation type %u at %#llx not handled", desc->name, type,
                        (unsigned long long)r_off);
                }
            }
        };
        if (rela && relasz) apply_rela(rela, relasz);
        if (jmprel && pltrelsz) apply_rela(jmprel, pltrelsz);
        if (relr && relrsz) {   // RELR: an address, then bitmaps of the following 63 words; the addend is in place
            uint64_t next = 0;
            for (uint64_t o = relr; o < relr + relrsz; o += 8) {
                const uint64_t e = rd64(base + o);
                if (!(e & 1)) {
                    uint64_t v = rd64(base + e) + lm.base;
                    std::memcpy(base + e, &v, 8);
                    lm.relative++;
                    next = e + 8;
                } else {
                    uint64_t bits = e >> 1;
                    for (int k = 0; bits; k++, bits >>= 1)
                        if (bits & 1) {
                            uint64_t v = rd64(base + next + 8 * (uint64_t)k) + lm.base;
                            std::memcpy(base + next + 8 * (uint64_t)k, &v, 8);
                            lm.relative++;
                        }
                    next += 63 * 8;
                }
            }
        }
        log("[loader] %s: build %.16s, %#llx bytes + bss %#llx at %#llx (%s), %u relocations (%u relative, %u TLS)",
            desc->name, lm.build_id, (unsigned long long)image, (unsigned long long)bss,
            (unsigned long long)lm.base, how, lm.relocs, lm.relative, lm.tls);
        g_nmods++;
        ok = true;
    } while (false);
    std::free(f);
    return ok;
}

void link_modules() {
    for (size_t i = 0; i < g_npending; i++) {
        const PendingReloc& r = g_pending[i];
        LoadedModule& lm = g_mods[r.module];
        const NxModule* d = lm.desc;
        if ((int)r.sym >= d->nsyms) { lm.unresolved++; continue; }
        const DynSym& s = d->syms[r.sym];
        uint64_t target = s.defined ? s.value : (s.name ? find_symbol(s.name) : 0);
        if (!target && s.name && s.name[0]) target = hle_register(s.name, nullptr);   // logged when called
        if (target) {
            const uint64_t v = target + (r.type == 257 ? (uint64_t)r.addend : 0);
            std::memcpy(reinterpret_cast<void*>(r.where), &v, 8);
            lm.bound++;
        } else {
            lm.unresolved++;
        }
    }
    for (int m = 0; m < g_nmods; m++)
        log("[loader] %s: %u symbol relocations bound, %u unresolved", g_mods[m].desc->name, g_mods[m].bound,
            g_mods[m].unresolved);
    std::free(g_pending);
    g_pending = nullptr;
    g_npending = g_cap_pending = 0;
}

}  // namespace pkp
