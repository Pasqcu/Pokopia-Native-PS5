// SPDX-License-Identifier: GPL-3.0-or-later
// CPU self-test: runs functions of the game's own `sdk` module (Nintendo's libc, libm and nn::crypto), recompiled from
// AArch64 to x86-64, and compares every result with a host reference. It exercises integer and flag lifting, NEON loads
// and stores (memcpy), the AArch64 variadic convention and FP formatting (snprintf), correctly rounded parsing (strtod),
// libm (sqrtf, sinf, cosf), guest-to-host indirect calls (qsort with a host comparator), and SHA-256 / HMAC-SHA256.
// Then an address-space probe of the HLE runtime's layout (nxrt memory.h).
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "pkp/platform.h"
#include "pkp/rt.h"

// descriptors of the lifted modules (generated module_<name>.cpp)
extern "C" const NxModule nxmod_sdk;
extern "C" const NxModule nxmod_rtld;

namespace pkp {

namespace {

// ---------------------------------------------------------------- reference SHA-256 (FIPS 180-4)
struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    size_t fill;
};
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void sha_block(Sha256& s, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d; s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += h;
}
void sha_init(Sha256& s) {
    static constexpr uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(s.h, iv, sizeof iv);
    s.len = 0;
    s.fill = 0;
}
void sha_update(Sha256& s, const uint8_t* p, size_t n) {
    s.len += n;
    while (n) {
        const size_t k = n < 64 - s.fill ? n : 64 - s.fill;
        std::memcpy(s.buf + s.fill, p, k);
        s.fill += k; p += k; n -= k;
        if (s.fill == 64) { sha_block(s, s.buf); s.fill = 0; }
    }
}
void sha_final(Sha256& s, uint8_t out[32]) {
    const uint64_t bits = s.len * 8;
    const uint8_t one = 0x80, zero = 0;
    sha_update(s, &one, 1);
    while (s.fill != 56) sha_update(s, &zero, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, lb, 8);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(s.h[i] >> (24 - 8 * j));
}
void sha256(const uint8_t* p, size_t n, uint8_t out[32]) { Sha256 s; sha_init(s); sha_update(s, p, n); sha_final(s, out); }
void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[32]) {
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[32];
    if (klen > 64) sha256(key, klen, k); else std::memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    Sha256 s; sha_init(s); sha_update(s, ipad, 64); sha_update(s, msg, mlen); sha_final(s, inner);
    sha_init(s); sha_update(s, opad, 64); sha_update(s, inner, 32); sha_final(s, out);
}
void hex(const uint8_t* p, size_t n, char* out) { for (size_t i = 0; i < n; i++) std::snprintf(out + 2 * i, 3, "%02x", p[i]); }

// ---------------------------------------------------------------- harness
struct Env {
    uint64_t strlen_, memcpy_, memset_, memcmp_, strcmp_, snprintf_, strtod_, strtol_, sqrtf_, sinf_, cosf_, qsort_,
        sha_, hmac_, hmac_mac_;
    SelfTestReport* r;
};
Env g;

void line(SelfTestReport* r, const char* fmt, ...) {
    if (r->nlines >= 48) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(r->lines[r->nlines++], sizeof r->lines[0], fmt, ap);
    va_end(ap);
    log("%s", r->lines[r->nlines - 1]);
}

uint64_t gstr(const char* s) {
    const size_t n = std::strlen(s) + 1;
    const uint64_t a = arena_alloc(n, 16);
    std::memcpy(reinterpret_cast<void*>(a), s, n);
    return a;
}

void clear_v() { std::memset(guest_ctx()->v, 0, sizeof guest_ctx()->v); }

uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

// -------------------------------------------------------------- tests: return true on success, detail in `d`
bool t_strlen(char* d, size_t n) {
    const char* s[] = {"", "a", "Pokopia Native on PS5", "0123456789abcdef0123456789abcdef!"};
    for (const char* x : s) {
        clear_v();
        const uint64_t a = gstr(x);
        const uint64_t args[] = {a};
        const uint64_t got = call_guest(g.strlen_, args, 1);
        if (got != std::strlen(x)) { std::snprintf(d, n, "strlen(\"%s\") = %llu", x, (unsigned long long)got); return false; }
    }
    std::snprintf(d, n, "4 strings");
    return true;
}

bool t_memcpy(char* d, size_t n) {
    const uint64_t src = arena_alloc(1 << 20, 64), dst = arena_alloc((1 << 20) + 64, 64);
    uint8_t* s = reinterpret_cast<uint8_t*>(src);
    uint32_t seed = 12345;
    for (int i = 0; i < (1 << 20); i++) s[i] = (uint8_t)(lcg(seed) >> 24);
    int cases = 0;
    for (uint32_t len : {1u, 3u, 7u, 15u, 16u, 17u, 31u, 33u, 63u, 64u, 65u, 127u, 129u, 255u, 1000u, 4096u, 65537u, 1u << 20}) {
        for (uint32_t so : {0u, 1u, 7u}) {
            for (uint32_t dofs : {0u, 3u}) {
                if (so + len > (1u << 20)) continue;
                std::memset(reinterpret_cast<void*>(dst), 0xEE, (1 << 20) + 64);
                clear_v();
                const uint64_t args[] = {dst + dofs, src + so, len};
                call_guest(g.memcpy_, args, 3);
                const uint8_t* o = reinterpret_cast<uint8_t*>(dst);
                if (std::memcmp(o + dofs, s + so, len) || (dofs && o[dofs - 1] != 0xEE) || o[dofs + len] != 0xEE) {
                    std::snprintf(d, n, "len %u src+%u dst+%u differs", len, so, dofs);
                    return false;
                }
                cases++;
            }
        }
    }
    std::snprintf(d, n, "%d sizes/alignments up to 1 MiB", cases);
    return true;
}

bool t_memset(char* d, size_t n) {
    if (!g.memset_) { std::snprintf(d, n, "memset not exported"); return true; }
    const uint64_t b = arena_alloc(8192, 64);
    for (uint32_t len : {1u, 5u, 16u, 31u, 64u, 100u, 4097u}) {
        std::memset(reinterpret_cast<void*>(b), 0x11, 8192);
        clear_v();
        const uint64_t args[] = {b + 3, 0xA5, len};
        call_guest(g.memset_, args, 3);
        const uint8_t* p = reinterpret_cast<uint8_t*>(b);
        for (uint32_t i = 0; i < len; i++) if (p[3 + i] != 0xA5) { std::snprintf(d, n, "len %u byte %u", len, i); return false; }
        if (p[2] != 0x11 || p[3 + len] != 0x11) { std::snprintf(d, n, "len %u overran", len); return false; }
    }
    std::snprintf(d, n, "7 lengths");
    return true;
}

int sgn(int64_t v) { return (v > 0) - (v < 0); }

bool t_compare(char* d, size_t n) {
    struct { const char* a; const char* b; } cs[] = {{"abc", "abc"}, {"abc", "abd"}, {"b", "a"}, {"", "x"}, {"Pokopia", "Pokopi"},
                                                     {"\xff", "\x01"}};
    for (auto& c : cs) {
        clear_v();
        const uint64_t a = gstr(c.a), b = gstr(c.b);
        const uint64_t a2[] = {a, b};
        const int gs = sgn((int32_t)call_guest(g.strcmp_, a2, 2));
        const int hs = sgn(std::strcmp(c.a, c.b));
        const size_t len = std::strlen(c.a) + 1 < std::strlen(c.b) + 1 ? std::strlen(c.a) + 1 : std::strlen(c.b) + 1;
        const uint64_t a3[] = {a, b, len};
        const int gm = sgn((int32_t)call_guest(g.memcmp_, a3, 3));
        const int hm = sgn(std::memcmp(c.a, c.b, len));
        if (gs != hs || gm != hm) { std::snprintf(d, n, "\"%s\" vs \"%s\": strcmp %d/%d memcmp %d/%d", c.a, c.b, gs, hs, gm, hm); return false; }
    }
    std::snprintf(d, n, "strcmp+memcmp, 6 pairs");
    return true;
}

bool t_snprintf(char* d, size_t n) {
    const uint64_t buf = arena_alloc(256, 16);
    const uint64_t fmt = gstr("%d|%7.3f|%s|%08x|%lld|%c|%.4e|%g");
    const uint64_t str = gstr("Zen2");
    Ctx* c = guest_ctx();
    clear_v();
    c->v[0].df[0] = 3.14159265358979;
    c->v[1].df[0] = -12345.678e-3;
    c->v[2].df[0] = 0.0001;
    const uint64_t args[] = {buf, 256, fmt, (uint64_t)(int64_t)-42, str, 0xBEEFu, (uint64_t)-9000000000ll, 'P'};
    // call_guest clears x but keeps v: the three doubles go in v0..v2 (AAPCS64 variadic)
    const uint64_t r = call_guest(g.snprintf_, args, 8);
    char want[256];
    const int wr = std::snprintf(want, sizeof want, "%d|%7.3f|%s|%08x|%lld|%c|%.4e|%g", -42, 3.14159265358979, "Zen2", 0xBEEFu,
                                 -9000000000ll, 'P', -12345.678e-3, 0.0001);
    const char* got = reinterpret_cast<const char*>(buf);
    if ((int32_t)r != wr || std::strcmp(got, want)) { std::snprintf(d, n, "got \"%.60s\" want \"%.60s\"", got, want); return false; }
    std::snprintf(d, n, "\"%.70s\"", got);
    return true;
}

bool t_strtod(char* d, size_t n) {
    const char* in[] = {"3.14159265358979323846", "-0.000001", "1e308", "2.2250738585072014e-308", "123456789.987654321", "0x1.8p3"};
    for (const char* s : in) {
        clear_v();
        const uint64_t a[] = {gstr(s), 0};
        call_guest(g.strtod_, a, 2);
        const double got = guest_ctx()->v[0].df[0], want = std::strtod(s, nullptr);
        if (std::memcmp(&got, &want, 8)) { std::snprintf(d, n, "strtod(%s) = %.17g, want %.17g", s, got, want); return false; }
    }
    const char* li[] = {"-123456", "0x7fffffff", "  42abc", "-9223372036854775807"};
    for (const char* s : li) {
        clear_v();
        const uint64_t a[] = {gstr(s), 0, 0};
        const int64_t got = (int64_t)call_guest(g.strtol_, a, 3), want = std::strtol(s, nullptr, 0);
        if (got != want) { std::snprintf(d, n, "strtol(%s) = %lld, want %lld", s, (long long)got, (long long)want); return false; }
    }
    std::snprintf(d, n, "6 doubles bit-exact, 4 integers");
    return true;
}

bool t_libm(char* d, size_t n) {
    const float xs[] = {0.0f, 0.5f, 1.0f, 2.0f, 3.14159265f, 10.0f, 1e-3f, 123.456f, -2.5f};
    float worst = 0;
    for (float x : xs) {
        if (x >= 0) {
            clear_v();
            guest_ctx()->v[0].f[0] = x;
            call_guest(g.sqrtf_, nullptr, 0);
            const float got = guest_ctx()->v[0].f[0], want = std::sqrt(x);
            if (std::memcmp(&got, &want, 4)) { std::snprintf(d, n, "sqrtf(%g) = %.9g want %.9g", x, got, want); return false; }
        }
        for (int k = 0; k < 2; k++) {
            clear_v();
            guest_ctx()->v[0].f[0] = x;
            call_guest(k ? g.cosf_ : g.sinf_, nullptr, 0);
            const float got = guest_ctx()->v[0].f[0];
            const double want = k ? std::cos((double)x) : std::sin((double)x);
            const float ulp = std::nextafter((float)want, 1e30f) - (float)want;
            const float err = std::fabs(got - (float)want) / (ulp > 0 ? ulp : 1e-30f);
            if (err > worst) worst = err;
            if (err > 2.0f) { std::snprintf(d, n, "%s(%g) = %.9g want %.9g", k ? "cosf" : "sinf", x, got, want); return false; }
        }
    }
    std::snprintf(d, n, "sqrtf exact, sinf/cosf within %.1f ulp", worst);
    return true;
}

uint32_t g_cmp_calls;
void host_cmp(Ctx* c) {   // int (*)(const void*, const void*) called by the guest's qsort through call_indirect
    g_cmp_calls++;
    int32_t a, b;
    std::memcpy(&a, reinterpret_cast<void*>(c->x[0]), 4);
    std::memcpy(&b, reinterpret_cast<void*>(c->x[1]), 4);
    c->x[0] = (uint32_t)((a > b) - (a < b));
}

bool t_qsort(char* d, size_t n) {
    constexpr int kN = 4096;
    const uint64_t arr = arena_alloc(kN * 4, 64);
    int32_t* p = reinterpret_cast<int32_t*>(arr);
    uint32_t seed = 777;
    int64_t sum = 0;
    for (int i = 0; i < kN; i++) { p[i] = (int32_t)(lcg(seed) % 100000) - 50000; sum += p[i]; }
    g_cmp_calls = 0;
    clear_v();
    const uint64_t a[] = {arr, kN, 4, hle_register("selftest_int_compare", host_cmp)};
    call_guest(g.qsort_, a, 4);
    int64_t sum2 = 0;
    for (int i = 0; i < kN; i++) {
        sum2 += p[i];
        if (i && p[i - 1] > p[i]) { std::snprintf(d, n, "not sorted at %d", i); return false; }
    }
    if (sum != sum2) { std::snprintf(d, n, "elements lost"); return false; }
    std::snprintf(d, n, "%d ints, %u host comparator calls", kN, g_cmp_calls);
    return true;
}

bool t_sha256(char* d, size_t n) {
    const uint64_t out = arena_alloc(64, 16);
    struct { const char* msg; const char* want; } kv[] = {
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}};
    char got[65];
    for (auto& v : kv) {
        clear_v();
        const uint64_t a[] = {out, 32, gstr(v.msg), std::strlen(v.msg)};
        call_guest(g.sha_, a, 4);
        hex(reinterpret_cast<uint8_t*>(out), 32, got);
        if (std::strcmp(got, v.want)) { std::snprintf(d, n, "SHA-256(\"%.8s\") = %.16s...", v.msg, got); return false; }
    }
    // 16 MiB pattern: guest vs host reference, and their speed
    const uint64_t len = 16ull << 20;
    const uint64_t big = arena_alloc(len, 64);
    uint32_t seed = 99;
    for (uint64_t i = 0; i < len; i += 4) { uint32_t v = lcg(seed); std::memcpy(reinterpret_cast<void*>(big + i), &v, 4); }
    uint8_t ref[32];
    uint64_t t0 = plat::now_ns();
    sha256(reinterpret_cast<uint8_t*>(big), len, ref);
    uint64_t t1 = plat::now_ns();
    clear_v();
    const uint64_t a[] = {out, 32, big, len};
    call_guest(g.sha_, a, 4);
    uint64_t t2 = plat::now_ns();
    if (std::memcmp(ref, reinterpret_cast<void*>(out), 32)) { std::snprintf(d, n, "16 MiB digest differs"); return false; }
    g.r->sha_mb_s_host = 16.0 / ((double)(t1 - t0) / 1e9);
    g.r->sha_mb_s_guest = 16.0 / ((double)(t2 - t1) / 1e9);
    std::snprintf(d, n, "3 vectors + 16 MiB; guest %.0f MB/s, host ref %.0f MB/s", g.r->sha_mb_s_guest, g.r->sha_mb_s_host);
    return true;
}

bool t_hmac(char* d, size_t n) {
    const char* key = "Jefe";
    const char* msg = "what do ya want for nothing?";
    const char* want = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";   // RFC 4231 case 2
    uint8_t ref[32];
    char hx[65];
    hmac_sha256(reinterpret_cast<const uint8_t*>(key), 4, reinterpret_cast<const uint8_t*>(msg), std::strlen(msg), ref);
    hex(ref, 32, hx);
    if (std::strcmp(hx, want)) { std::snprintf(d, n, "host reference wrong"); return false; }
    const uint64_t out = arena_alloc(64, 16), k = gstr(key), m = gstr(msg);
    const uint64_t fns[] = {g.hmac_, g.hmac_mac_};
    for (uint64_t f : fns) {
        if (!f) continue;
        for (int order = 0; order < 2; order++) {   // (data, key) as the SDK documents it; the other order as a check
            std::memset(reinterpret_cast<void*>(out), 0, 64);
            clear_v();
            const uint64_t a0[] = {out, 32, m, std::strlen(msg), k, 4};
            const uint64_t a1[] = {out, 32, k, 4, m, std::strlen(msg)};
            call_guest(f, order ? a1 : a0, 6);
            if (!std::memcmp(reinterpret_cast<void*>(out), ref, 32)) {
                std::snprintf(d, n, "RFC 4231 case 2 (%s)", order ? "key first" : "data first");
                return true;
            }
        }
    }
    hex(reinterpret_cast<uint8_t*>(out), 32, hx);
    std::snprintf(d, n, "mismatch: %.24s...", hx);
    return false;
}

struct Case {
    const char* name;
    bool (*fn)(char*, size_t);
    uint64_t* need;
};

struct GuardArg {
    bool (*fn)(char*, size_t);
    char* detail;
    bool ok;
};
void guarded(void* p) {
    GuardArg* a = static_cast<GuardArg*>(p);
    a->ok = a->fn(a->detail, 96);
}

}  // namespace

void run_selftest(SelfTestReport* r, const char* exefs_dir) {
    std::memset(r, 0, sizeof *r);
    g = Env{};
    g.r = r;
    char cpu[96];
    plat::cpu_description(cpu, sizeof cpu);
    line(r, "CPU: %s", cpu);
    const plat::MemInfo mi = plat::mem_info();
    if (mi.direct_total || mi.flexible_available)
        line(r, "memory: direct %llu MiB, flexible free %llu MiB, page %llu KiB", (unsigned long long)(mi.direct_total >> 20),
             (unsigned long long)(mi.flexible_available >> 20), (unsigned long long)(mi.page_size >> 10));
    if (!guest_env_init()) { line(r, "FAIL guest arena: cannot map %#llx", (unsigned long long)kArenaBase); r->failed++; return; }

    struct { const NxModule* d; const char* file; } mods[] = {{&nxmod_rtld, "rtld"}, {&nxmod_sdk, "sdk"}};
    for (auto& m : mods) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/%s", exefs_dir, m.file);
        if (!load_module(m.d, path)) { line(r, "FAIL load %s from %s", m.file, path); r->failed++; return; }
        const LoadedModule* lm = module_at(module_count() - 1);
        line(r, "load %-4s %.16s %u relocs, %d fns", m.file, lm->build_id, lm->relocs, m.d->nfns);
    }
    link_modules();

    g.strlen_ = find_symbol("strlen");
    g.memcpy_ = find_symbol("memcpy");
    g.memset_ = find_symbol("memset");
    g.memcmp_ = find_symbol("memcmp");
    g.strcmp_ = find_symbol("strcmp");
    g.snprintf_ = find_symbol("snprintf");
    g.strtod_ = find_symbol("strtod");
    g.strtol_ = find_symbol("strtol");
    g.sqrtf_ = find_symbol("sqrtf");
    g.sinf_ = find_symbol("sinf");
    g.cosf_ = find_symbol("cosf");
    g.qsort_ = find_symbol("qsort");
    g.sha_ = find_symbol("_ZN2nn6crypto18GenerateSha256HashEPvmPKvm");
    g.hmac_ = find_symbol("_ZN2nn6crypto18GenerateHmacSha256EPvmPKvmS3_m");
    g.hmac_mac_ = find_symbol("_ZN2nn6crypto21GenerateHmacSha256MacEPvmPKvmS3_m");

    const Case cases[] = {
        {"strlen", t_strlen, &g.strlen_},     {"memcpy", t_memcpy, &g.memcpy_},     {"memset", t_memset, &g.memset_},
        {"strcmp", t_compare, &g.strcmp_},    {"snprintf", t_snprintf, &g.snprintf_}, {"strtod", t_strtod, &g.strtod_},
        {"libm", t_libm, &g.sqrtf_},          {"qsort", t_qsort, &g.qsort_},        {"sha256", t_sha256, &g.sha_},
        {"hmac", t_hmac, &g.hmac_},
    };
    for (const Case& c : cases) {
        if (!*c.need) { line(r, "SKIP %-8s symbol not found", c.name); r->skipped++; continue; }
        char detail[96] = {0}, why[160] = {0};
        clear_trap();
        GuardArg a{c.fn, detail, false};
        const uint64_t t0 = plat::now_ns();
        const bool ran = plat::run_guarded(guarded, &a, why, sizeof why);
        const double ms = (double)(plat::now_ns() - t0) / 1e6;
        if (ran && a.ok) {
            line(r, "PASS %-8s %s (%.1f ms)", c.name, detail, ms);
            r->passed++;
        } else {
            line(r, "FAIL %-8s %s%s%s", c.name, ran ? detail : why, last_trap() ? " / " : "", last_trap() ? last_trap() : "");
            if (!ran) log("[selftest] last guest function entered: f_%llx", (unsigned long long)guest_ctx()->fn);
            r->failed++;
        }
    }
    if (svc_calls()) line(r, "note: %u SVCs answered 'not implemented'", svc_calls());
    line(r, "CPU self-test: %d passed, %d failed, %d skipped", r->passed, r->failed, r->skipped);
}

void run_layout_probe(SelfTestReport* r) {
    // nxrt's guest layout (third_party/pokopia-recomp/runtime/include/nxrt/memory.h): can this platform reserve it at the
    // same fixed addresses? Each region is reserved (no backing), then released; the heap also gets a 64 MiB commit.
    struct { const char* name; uint64_t base, size; bool commit; } regions[] = {
        {"main image", 0x7100000000ull, 0x10000000ull, false}, {"hle thunks", 0x7400000000ull, 0x1000000ull, false},
        {"heap 8G", 0x7800000000ull, 0x200000000ull, true},   {"alias 8G", 0x7A00000000ull, 0x200000000ull, false},
        {"stacks 4G", 0x7C00000000ull, 0x100000000ull, false},
    };
    int ok = 0;
    for (auto& g2 : regions) {
        char how[160] = {0};
        bool res = plat::vm_reserve(g2.base, g2.size, how, sizeof how);
        bool com = true;
        char how2[160] = {0};
        if (res && g2.commit) com = plat::vm_commit(g2.base, 0x4000000ull, plat::kReadWrite, how2, sizeof how2);
        if (res && g2.commit && com) std::memset(reinterpret_cast<void*>(g2.base), 0x5A, 0x4000000ull);
        if (res) plat::vm_release(g2.base, g2.size);
        line(r, "%s %-10s %#llx +%#llx %s%s%s", res && com ? "OK  " : "FAIL", g2.name, (unsigned long long)g2.base,
             (unsigned long long)g2.size, how, g2.commit ? " commit:" : "", g2.commit ? (com ? "ok" : how2) : "");
        ok += res && com;
    }
    line(r, "layout probe: %d of 5 regions", ok);
}

}  // namespace pkp
