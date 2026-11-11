// SPDX-License-Identifier: GPL-3.0-or-later
// Win32 file and text-conversion calls of nxrt's file-system services (fs_volume.cpp, fs_romfs.cpp, common.cpp) on POSIX.
// Wide strings: wchar_t is UTF-32 on these platforms (UTF-16 on Windows); paths become UTF-8.
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pkp/platform.h"
#include "windows.h"

HANDLE shim_file_new(int fd);   // win32_shim.cpp: file handles are shim objects (CloseHandle closes the descriptor)
int shim_file_fd(HANDLE h);

namespace {

struct FileRef {
    int fd;
};
// the descriptor behind a file handle, or nullptr for anything else
const FileRef* file_of(HANDLE h) {
    static thread_local FileRef r;
    r.fd = shim_file_fd(h);
    return r.fd >= 0 ? &r : nullptr;
}

std::string utf8(const wchar_t* w) {
    std::string out;
    for (; w && *w; w++) {
        const uint32_t c = (uint32_t)*w;
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

DWORD win_error(int e) {
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EEXIST: return ERROR_FILE_EXISTS;
    case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    default: return ERROR_INVALID_PARAMETER;
    }
}

HANDLE open_file(const std::string& path, DWORD access, DWORD disposition) {
    int flags = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default: break;
    }
    const int fd = open(path.c_str(), flags, 0644);
    if (fd < 0) { SetLastError(win_error(errno)); return INVALID_HANDLE_VALUE; }
    SetLastError(0);
    return shim_file_new(fd);
}

FILETIME filetime(const timespec& ts) {   // 100 ns intervals since 1601-01-01
    const uint64_t t = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100ull;
    return FILETIME{(DWORD)t, (DWORD)(t >> 32)};
}

}  // namespace

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD, void*, DWORD disposition, DWORD, HANDLE) {
    return open_file(utf8(name), access, disposition);
}
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD, void*, DWORD disposition, DWORD, HANDLE) {
    return open_file(name ? name : "", access, disposition);
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    const FileRef* f = file_of(h);
    if (got) *got = 0;
    if (!f) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    pkp::plat::prefault(buf, n);   // lazily backed guest memory: the kernel answers EFAULT instead of faulting
    ssize_t r = ov ? pread(f->fd, buf, n, (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset)) : read(f->fd, buf, n);
    if (r < 0) { SetLastError(win_error(errno)); return FALSE; }
    if (got) *got = (DWORD)r;
    if (ov && r == 0 && n) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }   // positioned read at the end: Win32's answer
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov) {
    const FileRef* f = file_of(h);
    if (put) *put = 0;
    if (!f) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    pkp::plat::prefault(buf, n);
    ssize_t r = ov ? pwrite(f->fd, buf, n, (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset)) : write(f->fd, buf, n);
    if (r < 0) { SetLastError(win_error(errno)); return FALSE; }
    if (put) *put = (DWORD)r;
    return TRUE;
}

BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* pos, DWORD method) {
    const FileRef* f = file_of(h);
    if (!f) return FALSE;
    const off_t r = lseek(f->fd, (off_t)dist.QuadPart, method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) { SetLastError(win_error(errno)); return FALSE; }
    if (pos) pos->QuadPart = (LONGLONG)r;
    return TRUE;
}

BOOL SetEndOfFile(HANDLE h) {
    const FileRef* f = file_of(h);
    return f && ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0;
}

BOOL FlushFileBuffers(HANDLE h) {
    const FileRef* f = file_of(h);
    return f && fsync(f->fd) == 0;
}

BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size) {
    const FileRef* f = file_of(h);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) return FALSE;
    size->QuadPart = (LONGLONG)st.st_size;
    return TRUE;
}

BOOL GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS, LPVOID info) {
    struct stat st;
    if (stat(utf8(name).c_str(), &st) != 0) { SetLastError(win_error(errno)); return FALSE; }
    auto* d = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(info);
    std::memset(d, 0, sizeof *d);
    d->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
#if defined(__APPLE__)
    d->ftLastWriteTime = filetime(st.st_mtimespec);
    d->ftLastAccessTime = filetime(st.st_atimespec);
    d->ftCreationTime = filetime(st.st_birthtimespec);
#else
    d->ftLastWriteTime = filetime(st.st_mtim);
    d->ftLastAccessTime = filetime(st.st_atim);
    d->ftCreationTime = filetime(st.st_ctim);
#endif
    return TRUE;
}

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD) {   // rename() replaces atomically: REPLACE_EXISTING and WRITE_THROUGH hold
    if (rename(utf8(from).c_str(), utf8(to).c_str()) != 0) { SetLastError(win_error(errno)); return FALSE; }
    return TRUE;
}

BOOL CreateDirectoryW(LPCWSTR path, void*) { return CreateDirectoryA(utf8(path).c_str(), nullptr); }
BOOL CreateDirectoryA(LPCSTR path, void*) {
    if (mkdir(path, 0755) != 0) { SetLastError(errno == EEXIST ? ERROR_ALREADY_EXISTS : win_error(errno)); return FALSE; }
    return TRUE;
}
BOOL DeleteFileW(LPCWSTR path) { return unlink(utf8(path).c_str()) == 0; }

int MultiByteToWideChar(UINT, DWORD, LPCSTR src, int n, LPWSTR dst, int cap) {
    const size_t len = n < 0 ? std::strlen(src) + 1 : (size_t)n;
    int out = 0;
    for (size_t i = 0; i < len;) {
        const unsigned char c = (unsigned char)src[i];
        uint32_t cp;
        int k;
        if (c < 0x80) { cp = c; k = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; k = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; k = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; k = 4; }
        else { cp = 0xFFFD; k = 1; }
        for (int j = 1; j < k; j++) cp = (cp << 6) | ((unsigned char)src[i + (size_t)j] & 0x3F);
        i += (size_t)k;
        if (dst && cap) { if (out >= cap) return 0; dst[out] = (wchar_t)cp; }
        out++;
    }
    return out;
}

int WideCharToMultiByte(UINT, DWORD, LPCWSTR src, int n, LPSTR dst, int cap, LPCSTR, BOOL* used) {
    if (used) *used = FALSE;
    std::wstring w = n < 0 ? std::wstring(src) : std::wstring(src, (size_t)n);
    std::string s = utf8(w.c_str());
    if (n < 0) s.push_back('\0');
    else if (w.size() != std::wcslen(w.c_str())) {   // embedded zeros: keep them
        s.clear();
        for (wchar_t c : w) { wchar_t one[2] = {c, 0}; s += c ? utf8(one) : std::string(1, '\0'); }
    }
    if (dst && cap) {
        if ((int)s.size() > cap) return 0;
        std::memcpy(dst, s.data(), s.size());
    }
    return (int)s.size();
}

DWORD GetFullPathNameA(LPCSTR name, DWORD n, LPSTR out, LPSTR* file_part) {
    std::string p = name ? name : "";
    if (p.empty() || p[0] != '/') {
#if defined(__PROSPERO__)
        p = "/app0/" + p;   // a title has no meaningful working directory: relative paths are the title folder's
#else
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd)) p = std::string(cwd) + "/" + p;
#endif
    }
    if (p.size() + 1 > n) return (DWORD)p.size() + 1;
    std::memcpy(out, p.c_str(), p.size() + 1);
    if (file_part) { char* s = std::strrchr(out, '/'); *file_part = s ? s + 1 : out; }
    return (DWORD)p.size();
}

DWORD GetPrivateProfileStringA(LPCSTR, LPCSTR, LPCSTR def, LPSTR out, DWORD n, LPCSTR) {
    std::snprintf(out, n, "%s", def ? def : "");
    return (DWORD)std::strlen(out);
}

// The language the system shows: $LANG on a POSIX host (it_IT.UTF-8 -> Italian); the PS5 title sets PKP_LANG from the console.
LANGID GetUserDefaultUILanguage() {
    const char* l = std::getenv("PKP_LANG");
    if (!l || !*l) l = std::getenv("LANG");
    if (!l) return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    struct { const char* p; WORD id; } map[] = {
        {"it", MAKELANGID(LANG_ITALIAN, 1)},   {"fr", MAKELANGID(LANG_FRENCH, 1)},   {"de", MAKELANGID(LANG_GERMAN, 1)},
        {"es", MAKELANGID(LANG_SPANISH, SUBLANG_SPANISH_MODERN)}, {"ja", MAKELANGID(LANG_JAPANESE, 1)}, {"ko", MAKELANGID(LANG_KOREAN, 1)},
        {"nl", MAKELANGID(LANG_DUTCH, 1)},     {"pt", MAKELANGID(LANG_PORTUGUESE, 2)}, {"ru", MAKELANGID(LANG_RUSSIAN, 1)},
        {"zh_TW", MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)}, {"zh", MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)},
        {"en_GB", MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_UK)},
    };
    for (auto& m : map)
        if (!std::strncmp(l, m.p, std::strlen(m.p))) return m.id;
    return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
}

HANDLE CreateMutexW(void* sa, BOOL owner, LPCWSTR) { return CreateMutexA(sa, owner, nullptr); }

BOOL QueryThreadCycleTime(HANDLE, ULONG64* cycles) {   // CPU time of the calling thread, in nanoseconds (a monotonic "cycle" count)
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    *cycles = (ULONG64)ts.tv_sec * 1000000000ull + (ULONG64)ts.tv_nsec;
    return TRUE;
}

FILE* _wfopen(const wchar_t* path, const wchar_t* mode) { return std::fopen(utf8(path).c_str(), utf8(mode).c_str()); }

HANDLE CreateFileW(const char* name, DWORD access, DWORD, void*, DWORD disposition, DWORD, HANDLE) {
    return open_file(name ? name : "", access, disposition);
}
BOOL GetFileAttributesExW(const char* name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    struct stat st;
    if (stat(name, &st) != 0) { SetLastError(win_error(errno)); return FALSE; }
    (void)level;
    auto* d = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(info);
    std::memset(d, 0, sizeof *d);
    d->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}
BOOL MoveFileExW(const char* from, const char* to, DWORD) {
    if (rename(from, to) != 0) { SetLastError(win_error(errno)); return FALSE; }
    return TRUE;
}
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) { return MoveFileExW(from, to, flags); }
BOOL CreateDirectoryW(const char* path, void* sa) { return CreateDirectoryA(path, sa); }
BOOL DeleteFileW(const char* path) { return unlink(path) == 0; }
// A live process is answered with the current-process pseudo-handle (WaitForSingleObject: never signalled, as a running
// process); a pid that does not exist is an error, as on Windows (nxrt's save recovery asks whether a lock holder still runs).
HANDLE OpenProcess(DWORD, BOOL, DWORD pid) {
    if (pid == 0 || ::kill((pid_t)pid, 0) != 0) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    return GetCurrentProcess();
}
BOOL GetProcessHandleCount(HANDLE, DWORD* count) { if (count) *count = 0; return TRUE; }
HMODULE LoadLibraryA(LPCSTR) { return nullptr; }
HMODULE LoadLibraryW(const wchar_t*) { return nullptr; }
BOOL FreeLibrary(HMODULE) { return TRUE; }

BOOL GetProcessTimes(HANDLE, FILETIME* create, FILETIME* exit_t, FILETIME* kernel, FILETIME* user) {
    *create = *exit_t = FILETIME{0, 0};
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    const uint64_t t100 = ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) / 100ull;
    *kernel = FILETIME{0, 0};
    *user = FILETIME{(DWORD)t100, (DWORD)(t100 >> 32)};
    return TRUE;
}
DWORD GetGuiResources(HANDLE, DWORD) { return 0; }
FILE* _wfopen(const char* path, const wchar_t* mode) { return std::fopen(path, utf8(mode).c_str()); }
void GetSystemTimePreciseAsFileTime(FILETIME* ft) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *ft = filetime(ts);
}
void GetSystemTimeAsFileTime(FILETIME* ft) { GetSystemTimePreciseAsFileTime(ft); }

BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    const int in = ::open(from, O_RDONLY);
    if (in < 0) { SetLastError(win_error(errno)); return FALSE; }
    const int out = ::open(to, O_WRONLY | O_CREAT | O_TRUNC | (fail_if_exists ? O_EXCL : 0), 0644);
    if (out < 0) { SetLastError(win_error(errno)); ::close(in); return FALSE; }
    char buf[1 << 16];
    bool ok = true;
    for (;;) {
        const ssize_t r = ::read(in, buf, sizeof buf);
        if (r == 0) break;
        if (r < 0 || ::write(out, buf, (size_t)r) != r) { ok = false; break; }
    }
    ::close(in);
    ::close(out);
    if (!ok) SetLastError(ERROR_WRITE_FAULT);
    return ok ? TRUE : FALSE;
}
