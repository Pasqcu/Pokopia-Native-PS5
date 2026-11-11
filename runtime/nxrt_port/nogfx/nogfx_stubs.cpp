// SPDX-License-Identifier: GPL-3.0-or-later
// What the graphics shell (gfx.cmake, nxrt_gfx) provides, for builds without it (the PS5 title until PS5_Vulkan, a host
// boot built with PKP_NXRT_GFX=OFF): the HLE services call these whether a renderer exists or not.
#include "nxrt/extras.h"

namespace nxrt {
namespace gfx {
void wait_screenshot_writes() {}   // no renderer: nothing pending
}
namespace extras {
void hdr_note_layer_luminance(bool, unsigned, unsigned) {}   // no HDR output to configure
}
}  // namespace nxrt
