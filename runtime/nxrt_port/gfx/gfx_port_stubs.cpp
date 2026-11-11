// SPDX-License-Identifier: GPL-3.0-or-later
// What the Windows-only files of nxrt's gfx answer on this port (runtime/nxrt_port/gfx.cmake leaves them out):
//   extras/temporal_dlss.cpp   NVIDIA DLSS / DLAA through Streamline (Windows DLLs)
//   extras/temporal_ffx.cpp    AMD FSR 3.1 through the FidelityFX API DLL
//   extras/win_display.cpp     the Windows HDR / monitor luminance probe (CCD, DXGI)
// Temporal upscalers report "not available" (the F1 page shows it and the game keeps its own picture); the spatial ones
// (FSR 1, NIS) are shaders and stay. The HDR probe answers "unknown", so HDR output follows the configuration only.
#include "extras/temporal.h"
#include "extras/win_display.h"

namespace nxrt::extras {

bool temporal_ffx_available() { return false; }
TemporalBackend* temporal_ffx(void*) { return nullptr; }
bool temporal_dlss_available() { return false; }
bool temporal_dlss_started() { return false; }
PFN_vkGetInstanceProcAddr streamline_preinit() { return nullptr; }
TemporalBackend* temporal_dlss(void*, bool) { return nullptr; }

bool query_display_hdr(void*, DisplayHdrInfo* out) {
    if (!out) return false;
    *out = DisplayHdrInfo{};
    const char note[] = "no display HDR query on this platform";
    for (size_t i = 0; i < sizeof note && i < sizeof out->how; i++) out->how[i] = note[i];
    return false;
}

}  // namespace nxrt::extras
