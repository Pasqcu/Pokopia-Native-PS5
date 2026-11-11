// SPDX-License-Identifier: GPL-3.0-or-later
// Inserted into nxrt's gfx/platform.cpp by the PS5 transform (runtime/nxrt_port/gfx.cmake), in place of the start of its
// Vulkan section, whose SDL functions are renamed *_sdl and stay unused. On the PS5, SDL runs its dummy video driver (events,
// virtual joysticks, surfaces for screenshots); the picture goes to the TV through RADV's VK_KHR_display on VideoOut
// (PS5_Vulkan / PS5_Mesa: Mesa's wsi_common_videoout.c): one display, its modes, a display-plane surface.
#include <volk.h>

#include <vector>

namespace {
uint32_t g_ps5_width = 3840, g_ps5_height = 2160;   // the display mode's size once the surface exists
}

void pixel_size(int* w, int* h) {   // the swapchain's size: the display plane's
    if (w) *w = static_cast<int>(g_ps5_width);
    if (h) *h = static_cast<int>(g_ps5_height);
}

const char* const* vulkan_instance_extensions(uint32_t* count) {
    static const char* const kExtensions[] = {"VK_KHR_surface", "VK_KHR_display"};
    if (count) *count = 2;
    return kExtensions;
}

bool create_vulkan_surface(void* vk_instance, uint64_t* surface_out) {
    if (!vk_instance || !surface_out) return false;
    const VkInstance instance = static_cast<VkInstance>(vk_instance);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (!n) {
        NXLOG_E(kTag, "VideoOut: no Vulkan device");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance, &n, devices.data());
    const VkPhysicalDevice pd = devices[0];
    VkDisplayPropertiesKHR display{};
    uint32_t displays = 1;
    const VkResult rd = vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &displays, &display);
    if ((rd != VK_SUCCESS && rd != VK_INCOMPLETE) || displays == 0) {
        NXLOG_E(kTag, "VideoOut: no display (%d)", static_cast<int>(rd));
        return false;
    }
    uint32_t mc = 0;
    vkGetDisplayModePropertiesKHR(pd, display.display, &mc, nullptr);
    std::vector<VkDisplayModePropertiesKHR> modes(mc);
    if (mc) vkGetDisplayModePropertiesKHR(pd, display.display, &mc, modes.data());
    if (!mc) {
        NXLOG_E(kTag, "VideoOut: the display has no modes");
        return false;
    }
    // The first mode is the display's own (3840x2160 on a 4K TV); the game renders at its size and the present pass scales.
    const VkDisplayModePropertiesKHR& mode = modes[0];
    VkDisplaySurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
    sci.displayMode = mode.displayMode;
    sci.planeIndex = 0;
    sci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    sci.imageExtent = mode.parameters.visibleRegion;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult rs = vkCreateDisplayPlaneSurfaceKHR(instance, &sci, nullptr, &surface);
    if (rs != VK_SUCCESS) {
        NXLOG_E(kTag, "VideoOut: vkCreateDisplayPlaneSurfaceKHR failed (%d)", static_cast<int>(rs));
        return false;
    }
    g_ps5_width = sci.imageExtent.width;
    g_ps5_height = sci.imageExtent.height;
    NXLOG_I(kTag, "VideoOut: %s, %u mode(s), using %ux%u at %.3f Hz", display.displayName ? display.displayName : "display", mc,
            g_ps5_width, g_ps5_height, mode.parameters.refreshRate / 1000.0);
    *surface_out = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(surface));
    return true;
}

void destroy_vulkan_surface(void* vk_instance, uint64_t surface) {
    if (!vk_instance || !surface) return;
    vkDestroySurfaceKHR(static_cast<VkInstance>(vk_instance), reinterpret_cast<VkSurfaceKHR>(static_cast<uintptr_t>(surface)), nullptr);
}

