// SPDX-License-Identifier: GPL-3.0-or-later
// RGBA8 -> single-surface BC7 DX10 DDS (DXGI_FORMAT_BC7_UNORM = 98, no mipmaps): the format of the PS5 shell's
// sce_sys/pic0.dds and pic1.dds. A small mode-6 encoder (one subset, RGBA 7.7.7.7 endpoints + p-bits, 4-bit indices):
// endpoints from the block's principal axis, two least-squares refinements. Good for smooth artwork; written from the
// BC7 format description (Microsoft "BC7 Format" / Khronos Data Format Specification).
//
//   bc7_dds <in.rgba> <width> <height> <out.dds>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr int kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bits {
    uint64_t lo = 0, hi = 0;
    int pos = 0;
    void put(uint32_t v, int n) {
        for (int i = 0; i < n; i++, pos++) {
            const uint64_t b = (v >> i) & 1;
            if (pos < 64) lo |= b << pos; else hi |= b << (pos - 64);
        }
    }
};

inline int interp(int e0, int e1, int w) { return ((64 - w) * e0 + w * e1 + 32) >> 6; }

// quantize an 8-bit endpoint value pair (RGBA) to 7 bits + shared p-bit, best p by error
void quant(const float e[4], int q[4], int& p) {
    float best = 1e30f;
    // opaque pixels need A = 255 exactly: only p = 1 gives (127 << 1) | 1
    for (int pb = e[3] > 254.0f ? 1 : 0; pb < 2; pb++) {
        float err = 0;
        int t[4];
        for (int ch = 0; ch < 4; ch++) {
            int v = (int)std::lround((e[ch] - pb) / 2.0f);
            v = v < 0 ? 0 : v > 127 ? 127 : v;
            t[ch] = v;
            const float d = (float)((v << 1) | pb) - e[ch];
            err += d * d;
        }
        if (err < best) { best = err; p = pb; std::memcpy(q, t, sizeof t); }
    }
}

void encode_block(const uint8_t (&px)[16][4], uint8_t out[16]) {
    float mean[4] = {0, 0, 0, 0};
    for (auto& p : px) for (int ch = 0; ch < 4; ch++) mean[ch] += p[ch] / 16.0f;
    // principal axis by power iteration on the covariance (RGB; alpha is constant for opaque art but kept general)
    float cov[4][4] = {};
    for (auto& p : px)
        for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) cov[a][b] += (p[a] - mean[a]) * (p[b] - mean[b]);
    float axis[4] = {1, 1, 1, 0};
    for (int it = 0; it < 8; it++) {
        float n[4] = {0, 0, 0, 0};
        for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) n[a] += cov[a][b] * axis[b];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3]);
        if (len < 1e-6f) break;
        for (int a = 0; a < 4; a++) axis[a] = n[a] / len;
    }
    float tmin = 1e30f, tmax = -1e30f;
    for (auto& p : px) {
        float t = 0;
        for (int ch = 0; ch < 4; ch++) t += (p[ch] - mean[ch]) * axis[ch];
        tmin = t < tmin ? t : tmin;
        tmax = t > tmax ? t : tmax;
    }
    float e0[4], e1[4];
    for (int ch = 0; ch < 4; ch++) { e0[ch] = mean[ch] + axis[ch] * tmin; e1[ch] = mean[ch] + axis[ch] * tmax; }
    int q0[4], q1[4], p0 = 0, p1 = 0, idx[16];
    for (int pass = 0; pass < 3; pass++) {
        for (int ch = 0; ch < 4; ch++) {
            e0[ch] = e0[ch] < 0 ? 0 : e0[ch] > 255 ? 255 : e0[ch];
            e1[ch] = e1[ch] < 0 ? 0 : e1[ch] > 255 ? 255 : e1[ch];
        }
        quant(e0, q0, p0);
        quant(e1, q1, p1);
        int E0[4], E1[4];
        for (int ch = 0; ch < 4; ch++) { E0[ch] = (q0[ch] << 1) | p0; E1[ch] = (q1[ch] << 1) | p1; }
        for (int i = 0; i < 16; i++) {   // best palette entry per pixel
            int best = 0;
            long berr = 1L << 40;
            for (int k = 0; k < 16; k++) {
                long err = 0;
                for (int ch = 0; ch < 4; ch++) { const int d = interp(E0[ch], E1[ch], kW4[k]) - px[i][ch]; err += (long)d * d; }
                if (err < berr) { berr = err; best = k; }
            }
            idx[i] = best;
        }
        if (pass == 2) break;
        // least-squares endpoints for the chosen weights
        float aa = 0, ab = 0, bb = 0, ax[4] = {0, 0, 0, 0}, bx[4] = {0, 0, 0, 0};
        for (int i = 0; i < 16; i++) {
            const float w = kW4[idx[i]] / 64.0f, a = 1 - w;
            aa += a * a; ab += a * w; bb += w * w;
            for (int ch = 0; ch < 4; ch++) { ax[ch] += a * px[i][ch]; bx[ch] += w * px[i][ch]; }
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6f) break;
        for (int ch = 0; ch < 4; ch++) {
            e0[ch] = (ax[ch] * bb - bx[ch] * ab) / det;
            e1[ch] = (bx[ch] * aa - ax[ch] * ab) / det;
        }
    }
    // the anchor (pixel 0) index must have its top bit clear: swap the endpoints and invert the indices if not
    if (idx[0] & 8) {
        for (int ch = 0; ch < 4; ch++) { const int t = q0[ch]; q0[ch] = q1[ch]; q1[ch] = t; }
        const int t = p0; p0 = p1; p1 = t;
        for (int& i : idx) i = 15 - i;
    }
    Bits b;
    b.put(1u << 6, 7);   // mode 6
    for (int ch = 0; ch < 4; ch++) { b.put((uint32_t)q0[ch], 7); b.put((uint32_t)q1[ch], 7); }
    b.put((uint32_t)p0, 1);
    b.put((uint32_t)p1, 1);
    b.put((uint32_t)idx[0], 3);
    for (int i = 1; i < 16; i++) b.put((uint32_t)idx[i], 4);
    std::memcpy(out, &b.lo, 8);
    std::memcpy(out + 8, &b.hi, 8);
}

void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) { std::fprintf(stderr, "usage: bc7_dds <in.rgba> <width> <height> <out.dds>\n"); return 2; }
    const int w = std::atoi(argv[2]), h = std::atoi(argv[3]);
    if (w % 4 || h % 4) { std::fprintf(stderr, "width and height must be multiples of 4\n"); return 2; }
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    FILE* f = std::fopen(argv[1], "rb");
    if (!f || std::fread(rgba.data(), 1, rgba.size(), f) != rgba.size()) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    std::fclose(f);
    std::vector<uint8_t> dds;
    dds.insert(dds.end(), {'D', 'D', 'S', ' '});
    put32(dds, 124);                      // DDS_HEADER.dwSize
    put32(dds, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);   // CAPS HEIGHT WIDTH PIXELFORMAT MIPMAPCOUNT LINEARSIZE
    put32(dds, (uint32_t)h);
    put32(dds, (uint32_t)w);
    put32(dds, (uint32_t)(w / 4) * (uint32_t)(h / 4) * 16);    // linear size of the top level
    put32(dds, 0);                        // depth
    put32(dds, 1);                        // one mip level: no mipmaps
    for (int i = 0; i < 11; i++) put32(dds, 0);
    put32(dds, 32);                       // DDS_PIXELFORMAT.dwSize
    put32(dds, 0x4);                      // DDPF_FOURCC
    dds.insert(dds.end(), {'D', 'X', '1', '0'});
    for (int i = 0; i < 5; i++) put32(dds, 0);
    put32(dds, 0x1000);                   // DDSCAPS_TEXTURE
    for (int i = 0; i < 4; i++) put32(dds, 0);
    put32(dds, 98);                       // DDS_HEADER_DXT10: DXGI_FORMAT_BC7_UNORM
    put32(dds, 3);                        // TEXTURE2D
    put32(dds, 0);
    put32(dds, 1);                        // array size
    put32(dds, 0);
    const size_t header = dds.size();
    dds.resize(header + (size_t)(w / 4) * (h / 4) * 16);
    uint8_t* blocks = dds.data() + header;
    for (int by = 0; by < h / 4; by++)
        for (int bx = 0; bx < w / 4; bx++) {
            uint8_t px[16][4];
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    const uint8_t* s = &rgba[(((size_t)(by * 4 + y)) * w + bx * 4 + x) * 4];
                    std::memcpy(px[y * 4 + x], s, 4);
                    px[y * 4 + x][3] = 255;
                }
            encode_block(px, blocks + ((size_t)by * (w / 4) + bx) * 16);
        }
    f = std::fopen(argv[4], "wb");
    if (!f || std::fwrite(dds.data(), 1, dds.size(), f) != dds.size()) { std::fprintf(stderr, "cannot write %s\n", argv[4]); return 1; }
    std::fclose(f);
    std::printf("%s: %dx%d BC7, %zu bytes\n", argv[4], w, h, dds.size());
    return 0;
}
