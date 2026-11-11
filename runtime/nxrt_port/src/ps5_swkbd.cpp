// SPDX-License-Identifier: GPL-3.0-or-later
// The software keyboard's prompt on the PS5 (src/services/am.cpp asks swkbdui::win32_prompt when the game opens the
// keyboard applet): the system's on-screen keyboard, libSceImeDialog. The dialog is the system's: it is drawn over the
// game's picture and has the controller while it is open; the calling thread (am.cpp's, made for the prompt) waits for it.
// A cancel is a cancel (the game asks again). When the dialog cannot be opened the answer is the text the game proposed,
// else PKP_SWKBD_TEXT (/app0/env.txt), else "Player": a game that insists on a name is not left waiting.
// [applet] swkbd_text (NXRT_APPLET_SWKBD_TEXT) answers without any prompt, as on the PC.
#if defined(__PROSPERO__)
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/services_api.h"

extern "C" {
struct PkpImeDialogParam {   // SceImeDialogParam
    int32_t user_id;
    int32_t type;
    uint64_t supported_languages;
    int32_t enter_label;
    int32_t input_method;
    void* filter;
    uint32_t option;
    uint32_t max_text_length;
    char16_t* input_text_buffer;   // max_text_length + 1 UTF-16 units: the initial text in, the answer out
    float pos_x, pos_y;            // in a 1920x1080 picture
    int32_t horizontal_alignment;
    int32_t vertical_alignment;
    const char16_t* placeholder;
    const char16_t* title;
    int8_t reserved[16];
};
struct PkpImeDialogResult {   // SceImeDialogResult
    int32_t end_status;
    int32_t reserved[12];
};
// The module is not one a title starts with: its imports are null when the title starts (a call to address 0, the first
// time the game asked for a name), and sceKernelDlsym answers ESRCH for a system module's functions. So the module is
// loaded here first, and the imports are used only when that load has bound them: an import that is still null is a
// fact to test (through bound(): a compiler takes a function's address for non-null), not a call to make. Ordinary
// imports, not weak ones: the link (scripts/make_ppsa.sh) names a system library's stub for the symbols a title needs.
int32_t sceImeDialogInit(const PkpImeDialogParam* param, const void* extended);
int32_t sceImeDialogGetStatus(void);
int32_t sceImeDialogGetResult(PkpImeDialogResult* result);
int32_t sceImeDialogTerm(void);
__attribute__((weak)) const char* sceKernelGetFsSandboxRandomWord(void);
__attribute__((weak)) int32_t sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, void* option, int32_t* result);
__attribute__((weak)) int32_t sceKernelDlsym(int32_t handle, const char* symbol, void** address);
}
static_assert(sizeof(PkpImeDialogParam) == 96, "SceImeDialogParam layout");

namespace nxrt::services::swkbdui {
namespace {
constexpr const char* kTag = "swkbd";
constexpr int32_t kUserEveryone = 254;      // SCE_USER_SERVICE_USER_ID_EVERYONE: what sceImeDialogParamInit sets
constexpr uint32_t kOptionPassword = 4;     // SCE_IME_OPTION_PASSWORD
constexpr int32_t kAlignCenter = 1;         // SCE_IME_HALIGN_CENTER, SCE_IME_VALIGN_CENTER
constexpr int32_t kStatusNone = 0, kStatusFinished = 2;   // SCE_IME_DIALOG_STATUS_*
constexpr int32_t kEndOk = 0;               // SCE_IME_DIALOG_END_STATUS_OK (1: the player cancelled, 2: aborted)
constexpr uint32_t kDefaultMaxLength = 32, kLongestText = 2048;   // the dialog's own limit

std::u16string to_utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu); n = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu); n = 3; }
        else if ((c >> 3) == 30 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
            n = 4;
        }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string to_utf8(const char16_t* s) {
    std::string out;
    for (size_t i = 0; s[i]; ++i) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

struct ImeDialog {
    int32_t (*init)(const PkpImeDialogParam*, const void*) = nullptr;
    int32_t (*status)() = nullptr;
    int32_t (*result)(PkpImeDialogResult*) = nullptr;
    int32_t (*term)() = nullptr;
    bool ok() const { return init && status && result && term; }
};

// A system module's handle: loaded now, or the one it has when it is loaded already. By its path in the title's sandbox,
// then by the system's path, then by its name alone; every answer is in the log (what a firmware refuses shows there).
int32_t module_handle(const char* file) {
    std::vector<std::string> paths;
    const char* word = sceKernelGetFsSandboxRandomWord ? sceKernelGetFsSandboxRandomWord() : nullptr;
    if (word && *word) paths.push_back(std::string("/") + word + "/common/lib/" + file);
    paths.push_back(std::string("/system/common/lib/") + file);
    paths.push_back(file);
    for (const std::string& p : paths) {
        const int32_t h = sceKernelLoadStartModule(p.c_str(), 0, nullptr, 0, nullptr, nullptr);
        NXLOG_I(kTag, "system keyboard: sceKernelLoadStartModule(%s) = %#x", p.c_str(), static_cast<unsigned>(h));
        if (h >= 0) return h;
    }
    return -1;
}

// The title's imports as they are bound at this moment. Its own function, not inlined: the addresses are read when it is
// called (a compiler takes an import's address for a constant, and would keep the one it read before the module load).
template <typename F>
F* bound(F* f) {   // the address as the loader left it, not as the compiler knows it
    __asm__ volatile("" : "+r"(f));
    return f;
}
__attribute__((noinline)) void bind_imports(ImeDialog& api, const char* when) {
    api.init = bound(sceImeDialogInit);
    api.status = bound(sceImeDialogGetStatus);
    api.result = bound(sceImeDialogGetResult);
    api.term = bound(sceImeDialogTerm);
    NXLOG_I(kTag, "system keyboard: imports %s: init %d status %d result %d term %d", when, api.init ? 1 : 0, api.status ? 1 : 0, api.result ? 1 : 0, api.term ? 1 : 0);
}

// libSceImeDialog, loaded once, then its functions: the title's own imports when loading the module has bound them
// (read after the load: they are null before it), else a lookup by name. What each way gave is in the log.
const ImeDialog& ime_dialog() {
    static ImeDialog api;
    static std::once_flag once;
    std::call_once(once, [] {
        if (!sceKernelLoadStartModule) {
            NXLOG_W(kTag, "system keyboard: libkernel has no sceKernelLoadStartModule here");
            return;
        }
        bind_imports(api, "before the load");
        const int32_t handle = module_handle("libSceImeDialog.sprx");
        if (handle < 0) {
            api = ImeDialog{};
            return;
        }
        bind_imports(api, "after the load");
        if (api.ok() || !sceKernelDlsym) return;
        const auto find = [handle](const char* name) -> void* {
            void* a = nullptr;
            const int32_t r = sceKernelDlsym(handle, name, &a);
            if (r != 0 || !a) NXLOG_W(kTag, "system keyboard: sceKernelDlsym(%s) = %#x", name, static_cast<unsigned>(r));
            return r == 0 ? a : nullptr;
        };
        api.init = reinterpret_cast<int32_t (*)(const PkpImeDialogParam*, const void*)>(find("sceImeDialogInit"));
        api.status = reinterpret_cast<int32_t (*)()>(find("sceImeDialogGetStatus"));
        api.result = reinterpret_cast<int32_t (*)(PkpImeDialogResult*)>(find("sceImeDialogGetResult"));
        api.term = reinterpret_cast<int32_t (*)()>(find("sceImeDialogTerm"));
    });
    return api;
}

// The answer without a dialog: what the game proposed, else the title's setting, else a name.
bool answer_without_dialog(const Request& r, std::string* text, const char* why, int32_t code) {
    const char* e = std::getenv("PKP_SWKBD_TEXT");
    *text = !r.initial.empty() ? r.initial : (e && *e ? e : "Player");
    NXLOG_W(kTag, "%s (%#x): the keyboard answers \"%s\"", why, static_cast<unsigned>(code), text->c_str());
    return true;
}
}  // namespace

bool win32_prompt(const Request& r, std::string* text) {
    if (!text) return false;
    const uint32_t max_len = r.max_len ? (r.max_len < kLongestText ? r.max_len : kLongestText) : kDefaultMaxLength;
    const std::u16string title = to_utf16(!r.header.empty() ? r.header : r.title);
    const std::u16string placeholder = to_utf16(!r.guide.empty() ? r.guide : r.sub);
    std::u16string initial = to_utf16(r.initial);
    if (initial.size() > max_len) initial.resize(max_len);
    std::vector<char16_t> buffer(max_len + 1, u'\0');
    std::memcpy(buffer.data(), initial.data(), initial.size() * sizeof(char16_t));

    PkpImeDialogParam p;
    std::memset(&p, 0, sizeof p);
    p.user_id = kUserEveryone;
    p.option = r.password ? kOptionPassword : 0;
    p.max_text_length = max_len;
    p.input_text_buffer = buffer.data();
    p.pos_x = 960.0f;
    p.pos_y = 540.0f;
    p.horizontal_alignment = kAlignCenter;
    p.vertical_alignment = kAlignCenter;
    p.placeholder = placeholder.empty() ? nullptr : placeholder.c_str();
    p.title = title.empty() ? nullptr : title.c_str();
    NXLOG_I(kTag, "system keyboard: \"%s\", up to %u characters (at least %u), proposed \"%s\"", r.header.c_str(), max_len, r.min_len, r.initial.c_str());
    const ImeDialog& ime = ime_dialog();
    if (!ime.ok()) return answer_without_dialog(r, text, "libSceImeDialog is not available", 0);
    const int32_t rc = ime.init(&p, nullptr);
    if (rc < 0) return answer_without_dialog(r, text, "sceImeDialogInit failed", rc);
    for (;;) {
        const int32_t st = ime.status();
        if (st == kStatusFinished) break;
        if (st == kStatusNone || st < 0) {   // gone without a result
            ime.term();
            return answer_without_dialog(r, text, "the system keyboard closed without an answer", st);
        }
        ::usleep(50 * 1000);
    }
    PkpImeDialogResult res;
    std::memset(&res, 0, sizeof res);
    const int32_t gr = ime.result(&res);
    ime.term();
    if (gr < 0) return answer_without_dialog(r, text, "sceImeDialogGetResult failed", gr);
    if (res.end_status != kEndOk) {
        NXLOG_I(kTag, "system keyboard: cancelled (end status %d)", res.end_status);
        return false;
    }
    buffer[max_len] = u'\0';
    *text = to_utf8(buffer.data());
    NXLOG_I(kTag, "system keyboard: \"%s\"", text->c_str());
    return true;
}

}  // namespace nxrt::services::swkbdui
#endif
