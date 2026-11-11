// SPDX-License-Identifier: GPL-3.0-or-later
// Emulated TLS runtime for the PS5 title. The SDK compiles with -femulated-tls, so every thread_local becomes a call to
// __emutls_get_address(control); compiler-rt provides it on other systems, the public PS5 SDK does not. This is the same
// ABI (LLVM compiler-rt emutls.c), written for this project on pthread keys:
//   control = { size, align, index (0 = not assigned yet), initial value or nullptr }
// Each thread keeps an array of object pointers, indexed by the control's index, freed when the thread exits.
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

struct EmutlsControl {
    size_t size;
    size_t align;
    union {
        uintptr_t index;
        void* address;
    } object;
    void* value;
};

struct Array {
    size_t n;
    void* obj[1];   // n entries
};

pthread_key_t g_key;
pthread_once_t g_once = PTHREAD_ONCE_INIT;
std::atomic<uintptr_t> g_next{1};
pthread_mutex_t g_assign = PTHREAD_MUTEX_INITIALIZER;

void destroy(void* p) {
    Array* a = static_cast<Array*>(p);
    for (size_t i = 0; i < a->n; i++) std::free(a->obj[i]);
    std::free(a);
}

void init_key() { pthread_key_create(&g_key, destroy); }

uintptr_t index_of(EmutlsControl* c) {
    uintptr_t i = __atomic_load_n(&c->object.index, __ATOMIC_ACQUIRE);
    if (i) return i;
    pthread_mutex_lock(&g_assign);
    i = c->object.index;
    if (!i) {
        i = g_next.fetch_add(1);
        __atomic_store_n(&c->object.index, i, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_assign);
    return i;
}

void* make_object(const EmutlsControl* c) {
    const size_t align = c->align < sizeof(void*) ? sizeof(void*) : c->align;
    void* p = nullptr;
    if (posix_memalign(&p, align, c->size ? c->size : 1) != 0) std::abort();
    if (c->value) std::memcpy(p, c->value, c->size);
    else std::memset(p, 0, c->size);
    return p;
}

}  // namespace

extern "C" void* __emutls_get_address(void* control) {
    auto* c = static_cast<EmutlsControl*>(control);
    pthread_once(&g_once, init_key);
    const uintptr_t i = index_of(c);
    Array* a = static_cast<Array*>(pthread_getspecific(g_key));
    if (!a || i > a->n) {
        const size_t old_n = a ? a->n : 0, n = i + 16;
        Array* b = static_cast<Array*>(std::calloc(1, sizeof(Array) + sizeof(void*) * n));
        if (!b) std::abort();
        b->n = n;
        if (a) { std::memcpy(b->obj, a->obj, sizeof(void*) * old_n); std::free(a); }
        a = b;
        pthread_setspecific(g_key, a);
    }
    void*& slot = a->obj[i - 1];
    if (!slot) slot = make_object(c);
    return slot;
}

#if !defined(PKP_PS5_PLATFORM_LAYER)   // the SDK fork's platform layer has its own (ps5___cxa_thread_atexit_impl)
// Destructors of thread_local objects (libc++abi's __cxa_thread_atexit forwards here; the title's libc has none):
// a per-thread list, run in reverse order of registration when the thread exits.
namespace {
struct AtExit {
    void (*dtor)(void*);
    void* obj;
    AtExit* next;
};
pthread_key_t g_atexit_key;
pthread_once_t g_atexit_once = PTHREAD_ONCE_INIT;
void run_atexit(void* p) {
    for (AtExit* e = static_cast<AtExit*>(p); e;) {
        AtExit* next = e->next;
        e->dtor(e->obj);
        std::free(e);
        e = next;
    }
}
void init_atexit_key() { pthread_key_create(&g_atexit_key, run_atexit); }
}  // namespace

extern "C" int __cxa_thread_atexit_impl(void (*dtor)(void*), void* obj, void* dso_symbol) {
    (void)dso_symbol;
    pthread_once(&g_atexit_once, init_atexit_key);
    auto* e = static_cast<AtExit*>(std::malloc(sizeof(AtExit)));
    if (!e) return -1;
    e->dtor = dtor;
    e->obj = obj;
    e->next = static_cast<AtExit*>(pthread_getspecific(g_atexit_key));
    pthread_setspecific(g_atexit_key, e);
    return 0;
}
#endif  // !PKP_PS5_PLATFORM_LAYER
