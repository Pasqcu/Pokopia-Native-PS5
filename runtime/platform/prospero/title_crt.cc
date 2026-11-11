// SPDX-License-Identifier: GPL-3.0-or-later
// Start-up of the PS5 graphics title (stage 3), in place of the boilerplate's tooling/native/app_crt.cpp: the same sequence
// (libc's _init_env, the constructors, main, exit), plus what the title needs before anything else runs:
//  - the IEEE floating-point state (the console starts a title with flush-to-zero and denormals-are-zero; RADV and the
//    game's lifted AArch64 code expect IEEE denormals), as PS5_Vulkan's start-up does;
//  - a boot trace, /app0/logs/boot_trace.txt, written with write(2) before each constructor and at each step of main
//    (pkp_boot_trace), so a title that ends before the runtime's log exists still says how far it got; the same lines go
//    to klog. Addresses are run-time ones: the first line gives _start's, which with build/ppsa/llvm-pie.elf gives the base;
//  - a fault handler for that early part (signal, address, PC), until the runtime installs its own.
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>

using Destructor = void (*)();
using Initializer = void (*)();

extern "C" {
void _init_env(void* process_parameters);
int atexit(Destructor callback);
[[noreturn]] void exit(int status);
int application_main(int argc, char** argv, char** envp) __asm__("main");
void ps5_fp_ieee(void);                                   // ps5platform/fp.h
int sceKernelDebugOutText(int channel, const char* text);  // libkernel

extern Initializer __preinit_array_start[] __attribute__((weak));
extern Initializer __preinit_array_end[] __attribute__((weak));
extern Initializer __init_array_start[] __attribute__((weak));
extern Initializer __init_array_end[] __attribute__((weak));
extern Initializer __fini_array_start[] __attribute__((weak));
extern Initializer __fini_array_end[] __attribute__((weak));
}

namespace {

int g_trace = -1;

char* put_str(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}

char* put_hex(char* p, uint64_t v) {
    *p++ = '0';
    *p++ = 'x';
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

char* put_dec(char* p, uint64_t v) {
    char tmp[20];
    int n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

void emit(const char* line, size_t n) {
    if (g_trace >= 0) (void)!write(g_trace, line, n);
    char k[256];
    const size_t m = n < sizeof k - 1 ? n : sizeof k - 1;
    memcpy(k, line, m);
    k[m] = '\0';
    sceKernelDebugOutText(0, k);
}

void early_fault(int sig, siginfo_t* si, void* uc) {
    char line[200];
    char* p = put_str(line, "[pokopia] FAULT signal ");
    p = put_dec(p, static_cast<uint64_t>(sig));
    p = put_str(p, " at ");
    p = put_hex(p, reinterpret_cast<uint64_t>(si ? si->si_addr : nullptr));
    p = put_str(p, " pc ");
    p = put_hex(p, uc ? static_cast<uint64_t>(static_cast<ucontext_t*>(uc)->uc_mcontext.mc_rip) : 0);
    *p++ = '\n';
    emit(line, static_cast<size_t>(p - line));
    signal(sig, SIG_DFL);
    raise(sig);
}

void run_traced(const char* what, Initializer* first, Initializer* last) {
    if (first == nullptr || last == nullptr) return;
    const uint64_t count = static_cast<uint64_t>(last - first);
    for (uint64_t i = 0; first != last; ++i) {
        char line[160];
        char* p = put_str(line, "[pokopia] ");
        p = put_str(p, what);
        *p++ = ' ';
        p = put_dec(p, i + 1);
        *p++ = '/';
        p = put_dec(p, count);
        *p++ = ' ';
        p = put_hex(p, reinterpret_cast<uint64_t>(*first));
        *p++ = '\n';
        emit(line, static_cast<size_t>(p - line));
        (*first++)();
    }
}

void run_reverse(Initializer* first, Initializer* last) {
    if (first == nullptr || last == nullptr) return;
    while (last != first) (*--last)();
}

}  // namespace

extern "C" void pkp_boot_trace(const char* step) {
    char line[256];
    char* p = put_str(line, "[pokopia] ");
    const size_t room = sizeof line - static_cast<size_t>(p - line) - 1;
    const size_t n = step ? strnlen(step, room) : 0;
    memcpy(p, step, n);
    p += n;
    *p++ = '\n';
    emit(line, static_cast<size_t>(p - line));
}

extern "C" __attribute__((weak)) void catchReturnFromMain(int status) { (void)status; }

extern "C" void _init() {
    run_traced("preinit", __preinit_array_start, __preinit_array_end);
    run_traced("constructor", __init_array_start, __init_array_end);
}

extern "C" void _fini() { run_reverse(__fini_array_start, __fini_array_end); }

extern "C" [[noreturn]] __attribute__((visibility("default"))) void _start(void* process_parameters, Destructor loader_teardown) {
    const int argc = *static_cast<const int*>(process_parameters);
    auto* parameters = static_cast<std::uint8_t*>(process_parameters);
    auto** argv = reinterpret_cast<char**>(parameters + sizeof(std::uint64_t));

    _init_env(process_parameters);
    ps5_fp_ieee();
    g_trace = open("/app0/logs/boot_trace.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    {
        char line[96];
        char* p = put_str(line, "[pokopia] start: libc ready, IEEE floating point; _start at ");
        p = put_hex(p, reinterpret_cast<uint64_t>(&_start));
        *p++ = '\n';
        emit(line, static_cast<size_t>(p - line));
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = early_fault;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
    if (loader_teardown != nullptr) (void)atexit(loader_teardown);
    (void)atexit(_fini);
    _init();
    pkp_boot_trace("constructors done: main");
    const int status = application_main(argc, argv, nullptr);
    catchReturnFromMain(status);
    exit(status);
}
