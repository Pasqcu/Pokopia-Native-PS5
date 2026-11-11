// SPDX-License-Identifier: GPL-3.0-or-later
// Folder listing for the RomFS builder (services/fs_romfs.cpp through the scan transform, src/fs_romfs_scan.inc).
// The PS5 title cannot list /app0 folders with opendir (EPERM inside the title's sandbox; std::filesystem's iterator is built
// on it), while open, stat and the getdents system call work there (measured on the console): the listing tries opendir /
// readdir, then on the PS5 getdents on an open descriptor, logs which one worked, and after an EPERM goes to getdents directly.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/log.h"

#if defined(__PROSPERO__)
extern "C" int getdents(int fd, char* buf, int nbytes);   // libkernel (the FreeBSD system call)
#endif

namespace {

bool special(const char* n) { return !n[0] || !std::strcmp(n, ".") || !std::strcmp(n, ".."); }

// opendir / readdir. False with *err when the folder cannot be opened or a read fails.
bool list_readdir(const char* path, std::vector<std::string>& names, int* err) {
    errno = 0;
    DIR* d = ::opendir(path);
    if (!d) {
        *err = errno;
        NXLOG_W("port", "opendir('%s') failed, errno %d", path, *err);
        return false;
    }
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent* e = ::readdir(d);
        if (!e) {
            if (errno) {
                *err = errno;
                NXLOG_W("port", "readdir('%s') failed after %zu entries, errno %d", path, names.size(), *err);
                ok = false;
            }
            break;
        }
        if (!special(e->d_name)) names.emplace_back(e->d_name);
    }
    ::closedir(d);
    return ok;
}

#if defined(__PROSPERO__)
// getdents on an open descriptor: FreeBSD dirent records (d_reclen steps to the next one, d_namlen bytes of name).
bool list_getdents(const char* path, std::vector<std::string>& names, int* err) {
    const int fd = ::open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        *err = errno;
        NXLOG_W("port", "open('%s', O_DIRECTORY) failed, errno %d", path, *err);
        return false;
    }
    alignas(8) static thread_local char buf[0x10000];
    bool ok = true;
    for (;;) {
        const int n = getdents(fd, buf, (int)sizeof buf);
        if (n == 0) break;
        if (n < 0) {
            *err = errno;
            NXLOG_W("port", "getdents('%s') failed, errno %d", path, *err);
            ok = false;
            break;
        }
        for (int off = 0; off < n;) {
            const auto* e = reinterpret_cast<const struct dirent*>(buf + off);
            if (e->d_reclen == 0) break;
            std::string name(e->d_name, e->d_namlen);
            if (e->d_fileno != 0 && !special(name.c_str())) names.push_back(std::move(name));
            off += e->d_reclen;
        }
    }
    ::close(fd);
    return ok;
}
#endif

}  // namespace

// type: 1 file (size set), 2 folder, 0 anything else. 0 on success, else -1 with *err (errno).
extern "C" int pkp_port_read_dir(const char* path, void (*on_entry)(void* ctx, const char* name, int type, uint64_t size),
                                 void* ctx, int* err) {
    static bool s_said_readdir = false, s_said_getdents = false;
    static std::atomic<bool> s_readdir_refused{false};   // the PS5 title: opendir answers EPERM in /app0, every time
    std::vector<std::string> names;
    int e1 = 0;
    bool ok = !s_readdir_refused.load() && list_readdir(path, names, &e1);
    if (ok && !s_said_readdir) {
        s_said_readdir = true;
        NXLOG_I("port", "folders are listed with opendir / readdir");
    }
#if defined(__PROSPERO__)
    if (!ok) {
        std::vector<std::string> alt;
        int e2 = 0;
        if (list_getdents(path, alt, &e2)) {
            names.swap(alt);
            ok = true;
            if (e1 == EPERM) s_readdir_refused.store(true);
            if (!s_said_getdents) {
                s_said_getdents = true;
                NXLOG_I("port", "folders are listed with getdents (opendir / readdir: errno %d)", e1);
            }
        } else if (!names.empty()) {   // readdir delivered entries and then reported an error: take them (logged above)
            ok = true;
        } else {
            e1 = e2 ? e2 : e1;
        }
    }
#endif
    if (!ok) {
        *err = e1;
        return -1;
    }
    for (const std::string& n : names) {
        const std::string full = std::string(path) + "/" + n;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) {
            *err = errno;
            NXLOG_W("port", "stat('%s') failed, errno %d", full.c_str(), *err);
            return -1;
        }
        const int type = S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 0;
        on_entry(ctx, n.c_str(), type, type == 1 ? (uint64_t)st.st_size : 0);
    }
    return 0;
}
