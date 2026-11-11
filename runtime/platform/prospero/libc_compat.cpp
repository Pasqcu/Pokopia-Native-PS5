// SPDX-License-Identifier: GPL-3.0-or-later
// C library pieces the SDK's static libc++ / libunwind need and the title's libc (libSceLibcInternal + the clean-room
// libc.prx) does not export: FreeBSD's xlocale "_l" variants (answered in the C locale: a title has one locale), the
// rune table the libc++ ctype facet reads, message catalogues (none), dladdr (no dynamic modules), __assert.
#include <dlfcn.h>
#include <locale.h>
#include <nl_types.h>
#include <runetype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <xlocale.h>

#include <cctype>

namespace {

// The C locale's rune table: ASCII classes, identity maps above 0x7F.
_RuneLocale g_c_runes;
bool g_runes_ready;

void build_runes() {
    memcpy(g_c_runes.__magic, "RuneMagi", 8);
    memcpy(g_c_runes.__encoding, "NONE", 5);
    g_c_runes.__invalid_rune = 0xFFFD;
    for (int c = 0; c < _CACHED_RUNES; c++) {
        unsigned long t = 0;
        if (c < 0x80) {
            const bool upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
            const bool xdigit = digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            const bool space = c == ' ' || (c >= '\t' && c <= '\r'), blank = c == ' ' || c == '\t';
            const bool cntrl = c < 0x20 || c == 0x7F, print = c >= 0x20 && c < 0x7F, graph = c > 0x20 && c < 0x7F;
            const bool alpha = upper || lower, punct = graph && !alpha && !digit;
            if (alpha) t |= _CTYPE_A;
            if (cntrl) t |= _CTYPE_C;
            if (digit) t |= _CTYPE_D | _CTYPE_N | (unsigned long)(c - '0');
            if (graph) t |= _CTYPE_G;
            if (lower) t |= _CTYPE_L;
            if (punct) t |= _CTYPE_P;
            if (space) t |= _CTYPE_S;
            if (upper) t |= _CTYPE_U;
            if (xdigit) t |= _CTYPE_X;
            if (blank) t |= _CTYPE_B;
            if (print) t |= _CTYPE_R | (1ul << _CTYPE_SWS);   // printable: screen width 1
            if (xdigit && !digit) t |= (unsigned long)((c | 0x20) - 'a' + 10);
        }
        g_c_runes.__runetype[c] = t;
        g_c_runes.__maplower[c] = c >= 'A' && c <= 'Z' ? c + 32 : c;
        g_c_runes.__mapupper[c] = c >= 'a' && c <= 'z' ? c - 32 : c;
    }
    g_runes_ready = true;
}

}  // namespace

extern "C" {

_RuneLocale* __runes_for_locale(locale_t, int* mb_sb_limit) {
    if (!g_runes_ready) build_runes();
    if (mb_sb_limit) *mb_sb_limit = 256;
    return &g_c_runes;
}

int ___mb_cur_max_l(locale_t) { return 1; }
int freelocale(locale_t) { return 0; }

double strtod_l(const char* s, char** e, locale_t) { return strtod(s, e); }
float strtof_l(const char* s, char** e, locale_t) { return strtof(s, e); }
long double strtold_l(const char* s, char** e, locale_t) { return strtold(s, e); }
long long strtoll_l(const char* s, char** e, int b, locale_t) { return strtoll(s, e, b); }
unsigned long long strtoull_l(const char* s, char** e, int b, locale_t) { return strtoull(s, e, b); }

int snprintf_l(char* out, size_t n, locale_t, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return r;
}
int asprintf_l(char** out, locale_t, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vasprintf(out, fmt, ap);
    va_end(ap);
    return r;
}
int sscanf_l(const char* s, locale_t, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
size_t strftime_l(char* out, size_t n, const char* fmt, const struct tm* tm, locale_t) { return strftime(out, n, fmt, tm); }

size_t mbrlen_l(const char* s, size_t n, mbstate_t* ps, locale_t) { return mbrlen(s, n, ps); }
size_t mbrtowc_l(wchar_t* pwc, const char* s, size_t n, mbstate_t* ps, locale_t) { return mbrtowc(pwc, s, n, ps); }
int mbtowc_l(wchar_t* pwc, const char* s, size_t n, locale_t) { return mbtowc(pwc, s, n); }
size_t wcrtomb_l(char* s, wchar_t wc, mbstate_t* ps, locale_t) { return wcrtomb(s, wc, ps); }
size_t mbsrtowcs_l(wchar_t* dst, const char** src, size_t len, mbstate_t* ps, locale_t) { return mbsrtowcs(dst, src, len, ps); }
size_t mbsnrtowcs_l(wchar_t* dst, const char** src, size_t nms, size_t len, mbstate_t* ps, locale_t) {
    return mbsnrtowcs(dst, src, nms, len, ps);
}
size_t wcsnrtombs_l(char* dst, const wchar_t** src, size_t nwc, size_t len, mbstate_t* ps, locale_t) {
    return wcsnrtombs(dst, src, nwc, len, ps);
}

nl_catd catopen(const char*, int) { return (nl_catd)-1; }
char* catgets(nl_catd, int, int, const char* s) { return const_cast<char*>(s); }
int catclose(nl_catd) { return 0; }

int dladdr(const void*, Dl_info*) { return 0; }   // one statically linked executable: no module information

void __assert(const char* func, const char* file, int line, const char* expr) {
    fprintf(stderr, "assertion failed: %s, %s:%d (%s)\n", expr ? expr : "?", file ? file : "?", line, func ? func : "?");
    abort();
}

}  // extern "C"

// Not exported by the title's libc at all: built on mbrtowc / wcrtomb, which are (POSIX.1-2008 semantics).
extern "C" size_t mbsnrtowcs(wchar_t* dst, const char** src, size_t nms, size_t len, mbstate_t* ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    const char* s = *src;
    size_t count = 0;
    while (nms > 0 && (!dst || count < len)) {
        wchar_t wc;
        const size_t r = mbrtowc(&wc, s, nms, ps);
        if (r == (size_t)-1) { if (dst) *src = s; return (size_t)-1; }   // invalid sequence (errno EILSEQ)
        if (r == (size_t)-2) { s += nms; nms = 0; break; }                // incomplete: consumed, state kept
        if (r == 0) {                                                     // the terminating null
            if (dst) { dst[count] = L'\0'; *src = nullptr; }
            return count;
        }
        if (dst) dst[count] = wc;
        s += r;
        nms -= r;
        count++;
    }
    if (dst) *src = s;
    return count;
}

extern "C" size_t wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, mbstate_t* ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    const wchar_t* s = *src;
    size_t count = 0;
    char buf[16];
    while (nwc > 0) {
        const wchar_t wc = *s;
        const size_t r = wcrtomb(buf, wc, ps);
        if (r == (size_t)-1) { if (dst) *src = s; return (size_t)-1; }
        if (dst && count + r > len) break;
        if (wc == L'\0') {
            if (dst) { dst[count] = '\0'; *src = nullptr; }
            return count;
        }
        if (dst) memcpy(dst + count, buf, r);
        count += r;
        s++;
        nwc--;
    }
    if (dst) *src = s;
    return count;
}
