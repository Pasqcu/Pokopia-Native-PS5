// SPDX-License-Identifier: GPL-3.0-or-later
// The shader cache as one file (scripts/pack_shader_cache.py: spirv.pack next to, or inside, the cache folder). nxrt's
// nvn_shaders.cpp reads every cache file through read_file(path), which the port's transform sends here first: a path
// "<dir>/<name>" whose <dir> holds spirv.pack (or whose <dir>.pack exists) is answered from the pack; anything else returns
// false and is read from the file system as before. Entries are LZ4 blocks (decoded here) or stored.
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/log.h"

namespace {

struct Entry {
    uint64_t off;
    uint32_t stored, size, flags;
};
struct Pack {
    int fd = -1;
    std::map<std::string, Entry> index;
};

std::mutex g_mu;
std::map<std::string, std::unique_ptr<Pack>> g_packs;   // by folder; null: the folder has no pack

bool read_at(int fd, uint64_t off, void* dst, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    while (n) {
        const ssize_t r = ::pread(fd, p, n, static_cast<off_t>(off));
        if (r <= 0) return false;
        p += r;
        off += static_cast<uint64_t>(r);
        n -= static_cast<size_t>(r);
    }
    return true;
}

// LZ4 block format (lz4_Block_format.md of github.com/lz4/lz4): exactly dst_size bytes, false on a malformed stream.
bool lz4_decode(const uint8_t* src, size_t n, uint8_t* dst, size_t dst_size) {
    size_t ip = 0, op = 0;
    while (ip < n) {
        const uint8_t token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= n) return false;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (ip + lit > n || op + lit > dst_size) return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip >= n) break;   // the last sequence has literals only
        if (ip + 2 > n) return false;
        const size_t offset = src[ip] | (static_cast<size_t>(src[ip + 1]) << 8);
        ip += 2;
        if (offset == 0 || offset > op) return false;
        size_t len = (token & 15) + 4;
        if ((token & 15) == 15) {
            uint8_t b;
            do {
                if (ip >= n) return false;
                b = src[ip++];
                len += b;
            } while (b == 255);
        }
        if (op + len > dst_size) return false;
        for (size_t i = 0; i < len; i++) dst[op + i] = dst[op - offset + i];   // overlapping copies are the point
        op += len;
    }
    return op == dst_size;
}

Pack* pack_for(const std::string& dir) {
    auto it = g_packs.find(dir);
    if (it != g_packs.end()) return it->second.get();
    std::unique_ptr<Pack> p;
    for (const std::string& file : {dir + "/spirv.pack", dir + ".pack"}) {
        const int fd = ::open(file.c_str(), O_RDONLY);
        if (fd < 0) continue;
        uint8_t h[24];
        uint32_t count = 0;
        uint64_t index_off = 0;
        if (!read_at(fd, 0, h, sizeof h) || std::memcmp(h, "PKPSPK01", 8) != 0) {
            NXLOG_W("port", "%s is not a shader pack", file.c_str());
            ::close(fd);
            continue;
        }
        std::memcpy(&count, h + 8, 4);
        std::memcpy(&index_off, h + 16, 8);
        std::vector<uint8_t> idx(static_cast<size_t>(count) * 64);
        if (!read_at(fd, index_off, idx.data(), idx.size())) {
            NXLOG_W("port", "%s: the index cannot be read", file.c_str());
            ::close(fd);
            continue;
        }
        p = std::make_unique<Pack>();
        p->fd = fd;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* e = idx.data() + static_cast<size_t>(i) * 64;
            Entry en{};
            std::memcpy(&en.off, e + 40, 8);
            std::memcpy(&en.stored, e + 48, 4);
            std::memcpy(&en.size, e + 52, 4);
            std::memcpy(&en.flags, e + 56, 4);
            p->index.emplace(std::string(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 40)), en);
        }
        NXLOG_I("port", "shader pack %s: %u files", file.c_str(), count);
        break;
    }
    Pack* raw = p.get();
    g_packs.emplace(dir, std::move(p));
    return raw;
}

}  // namespace

extern "C" bool pkp_shader_pack_read(const char* path, void (*sink)(void* ctx, const char* data, size_t n), void* ctx) {
    const std::string s(path ? path : "");
    const size_t slash = s.find_last_of('/');
    if (slash == std::string::npos) return false;
    const std::string dir = s.substr(0, slash), name = s.substr(slash + 1);
    Entry e{};
    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Pack* p = pack_for(dir);
        if (!p) return false;
        auto it = p->index.find(name);
        if (it == p->index.end()) return false;
        e = it->second;
        fd = p->fd;
    }
    std::string raw(e.stored, '\0');
    if (!read_at(fd, e.off, raw.data(), raw.size())) return false;
    if (!(e.flags & 1)) {
        sink(ctx, raw.data(), raw.size());
        return true;
    }
    std::string out(e.size, '\0');
    if (!lz4_decode(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), reinterpret_cast<uint8_t*>(out.data()), out.size())) {
        NXLOG_W("port", "shader pack: %s is corrupt", s.c_str());
        return false;
    }
    sink(ctx, out.data(), out.size());
    return true;
}
