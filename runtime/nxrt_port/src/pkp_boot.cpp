// SPDX-License-Identifier: GPL-3.0-or-later
// Boot steps shared by the host boot and the PS5 title (include/pkp_boot.h).
#include "pkp_boot.h"

#include <atomic>

#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"

extern "C" const NxModule nxmod_main, nxmod_sdk, nxmod_subsdk0, nxmod_rtld;
extern "C" const uint32_t nxmod_main_abi, nxmod_sdk_abi, nxmod_subsdk0_abi, nxmod_rtld_abi;

namespace pkp::boot {
namespace {

struct GameModule {
    const char* file;
    const NxModule* nx;
    const uint32_t* abi;
    uint64_t base;
};
// rtld first: the process entry point is its offset 0
const GameModule kModules[] = {
    {"rtld", &nxmod_rtld, &nxmod_rtld_abi, 0x7700000000ull},
    {"main", &nxmod_main, &nxmod_main_abi, 0x7100000000ull},
    {"sdk", &nxmod_sdk, &nxmod_sdk_abi, 0x7500000000ull},
    {"subsdk0", &nxmod_subsdk0, &nxmod_subsdk0_abi, 0x7600000000ull},
};

std::atomic<const char*> g_stage{"starting"};
std::atomic<bool> g_nvn{false};

// nvnBootstrapLoader(const char* name): the game's way into NVN (pkp_main.cpp: "The game reaches NVN only through
// nvnBootstrapLoader"). The sdk's own definition boots the console's GPU driver (NvOsDrvOpen on /dev/nvhost-*), which
// does not exist here.
void nvn_gate(::Ctx* c) {
    const char* name = reinterpret_cast<const char*>(c->x[0]);
    g_nvn.store(true);
    g_stage.store("the game reached NVN: graphics back end comes with stage M6");
    NXLOG_I("boot", "the game reached NVN: nvnBootstrapLoader(\"%s\") from %s. No graphics back end yet (stage M6): this game "
            "thread stops here, the game's other threads keep running", name ? name : "", nxrt::module_describe(c->x[30]).c_str());
    nxrt::log_flush();
    for (;;) nxrt::kernel::sleep_ns(1000000000ll);
}

}  // namespace

bool load_modules(const std::string& exefs_dir, std::string* err, bool nvn_front_end) {
    g_stage.store("loading modules");
    for (const GameModule& g : kModules) {
        nxrt::LoadOptions lo;
        lo.name = g.file;
        lo.base = g.base;
        std::string e;
        const std::string path = exefs_dir + "/" + g.file;
        nxrt::Module* m = nxrt::module_load_nso_file(path.c_str(), lo, &e);
        if (!m) {
            if (err) *err = "cannot load " + path + ": " + e;
            g_stage.store("FAIL: module load (see log)");
            return false;
        }
        if (m->base() != g.nx->base) {
            if (err) *err = std::string("module ") + g.file + " is mapped at a base different from the one it was lifted for";
            g_stage.store("FAIL: module base");
            return false;
        }
        const nxrt::RegResult r = nxrt::nxmodule_register(m, g.nx, *g.abi);
        if (!r.ok()) {
            if (err) *err = std::string("lifted module ") + g.file + " refused: " + r.message;
            g_stage.store("FAIL: module registration");
            return false;
        }
    }
    // The lifted PLT stub of main calls host_import() for the symbol, and the sdk's definition is also replaced in the call table
    // for an indirect call that reaches it by address (as pkp_main.cpp does for the NVN front end).
    nxrt::HleFn nvn = nvn_front_end ? nxrt::hle_lookup("nvnBootstrapLoader") : nullptr;   // nxrt::nvn::install() registered it
    if (!nvn) {
        nxrt::hle_register("nvnBootstrapLoader", nvn_gate);
        nvn = nvn_gate;
    }
    nxrt::module_override_symbol("nvnBootstrapLoader");
    if (const uint64_t def = nxrt::nxmodule_find_export("nvnBootstrapLoader")) nxrt::fn_register(def, nvn);
    // [boot] link = rtld, pokopia-recomp's default: the host maps and protects the images and fills the Horizon memory map that
    // svcQueryMemory reports; the game's own (lifted) rtld then finds the modules and relocates them, as on the console
    nxrt::module_protect_all();
    g_stage.store("modules loaded");
    return true;
}

uint64_t run_entry() {
    const uint32_t h = nxrt::kernel::main_thread_adopt(1u << 20, "main");
    if (!h) {
        NXLOG_E("boot", "cannot create the main thread");
        g_stage.store("FAIL: main thread");
        return ~0ull;
    }
    ::Ctx* ctx = nxrt::kernel::current_ctx();
    ctx->x[0] = 0;
    ctx->x[1] = h;
    ctx->x[30] = 0;
    g_stage.store("game running (rtld entered)");
    NXLOG_I("boot", "entering rtld at 0x%llx (x1 = main thread handle 0x%x)", (unsigned long long)kModules[0].base, h);
    nxrt::log_flush();
    nxrt::dispatch_call(ctx, kModules[0].base);
    g_stage.store("the game's entry function returned");
    NXLOG_I("boot", "the entry function returned (x0 = 0x%llx)", (unsigned long long)ctx->x[0]);
    nxrt::log_flush();
    return ctx->x[0];
}

const char* stage() { return g_stage.load(); }
bool nvn_reached() { return g_nvn.load(); }

}  // namespace pkp::boot
