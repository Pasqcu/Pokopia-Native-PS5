// SPDX-License-Identifier: GPL-3.0-or-later
// libc functions whose only definition in the SDK's stubs is in a module that gives a title nothing: the import links, and at
// run time it is null, so a call jumps to address 0 (libkernel_sys's readlink, libScePosixForWebKit's mkstemp; PS5_Vulkan's
// tools/radv-link.sh lists them). Later pins of the SDK fork's platform layer have ps5_ versions of some; scripts/make_ppsa.sh
// binds a libc name here (--defsym, kept local) only when the platform layer it links has none.
//
// Who imports them in the graphics title: SDL (SDL_GetBasePath: readlink; its process API: fork, vfork, setsid), libc++'s
// std::filesystem (read_symlink, canonical: readlink; current_path: pathconf, getcwd), RADV (ac_gpu_info.c: readlink;
// aco_print_asm.cpp: mkstemp), Dear ImGui (fork, for its "open in shell" helper). getcwd is exported, and faults inside
// the system's libc when a title calls it (docs/PORT_NXRT.md, "Sandbox del titolo PS5"): nxrt's play mode asks
// std::filesystem::current_path for its prompt icons.
#if defined(PKP_PS5_PLATFORM_LAYER)
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {

// No caller needs a symbolic link in a title's file systems: "not a link", and each caller takes its fallback.
ssize_t pkp_libc_readlink(const char*, char*, size_t) {
    errno = EINVAL;
    return -1;
}

// FreeBSD's limits for the title's file systems.
long pkp_libc_pathconf(const char*, int name) {
    switch (name) {
    case _PC_PATH_MAX: return PATH_MAX;
    case _PC_NAME_MAX: return NAME_MAX;
    case _PC_LINK_MAX: return 32767;
    case _PC_PIPE_BUF: return PIPE_BUF;
    case _PC_NO_TRUNC: return 1;
    default: errno = EINVAL; return -1;
    }
}

// mkstemps is bound to the platform layer's
int pkp_libc_mkstemp(char* path_template) { return mkstemps(path_template, 0); }

// The port's working folder is the title's (GetFullPathNameA resolves relative paths under /app0, win32_files.cpp).
char* pkp_libc_getcwd(char* buf, size_t size) {
    static const char kFolder[] = "/app0";
    if (!buf) {
        if (size == 0) size = sizeof kFolder;
        buf = static_cast<char*>(malloc(size));
        if (!buf) {
            errno = ENOMEM;
            return nullptr;
        }
    }
    if (size < sizeof kFolder) {
        errno = ERANGE;
        return nullptr;
    }
    memcpy(buf, kFolder, sizeof kFolder);
    return buf;
}

// A title runs one process.
pid_t pkp_libc_fork(void) {
    errno = ENOSYS;
    return -1;
}
pid_t pkp_libc_vfork(void) {
    errno = ENOSYS;
    return -1;
}
pid_t pkp_libc_setsid(void) {
    errno = EPERM;
    return -1;
}

}  // extern "C"
#endif
