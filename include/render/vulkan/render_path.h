#ifndef RENDER_VULKAN_RENDER_PATH_H
#define RENDER_VULKAN_RENDER_PATH_H
#include <stdbool.h>
#include <stdint.h>
#include <wlr/render/color.h>

enum wlr_vk_render_path {
    WLR_VK_PATH_LINEAR,
    WLR_VK_PATH_SRGB,
    WLR_VK_PATH_BACKDROP,
    WLR_VK_PATH_COLOR_TRANSFORM,
};

static inline enum wlr_vk_render_path vulkan_choose_render_path(
        uint32_t inverse_eotf, bool backdrop, bool srgb_attachment) {
    if (backdrop) return WLR_VK_PATH_BACKDROP;
    if (inverse_eotf == WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR) return WLR_VK_PATH_LINEAR;
    if (inverse_eotf == WLR_COLOR_TRANSFER_FUNCTION_SRGB && srgb_attachment) return WLR_VK_PATH_SRGB;
    // Gamma 2.2, PQ, BT.1886 and LUTs cannot be replaced with sRGB encoding.
    return WLR_VK_PATH_COLOR_TRANSFORM;
}

static inline bool vulkan_blend_can_retire(unsigned unused_passes,
        uint64_t last_use, uint64_t completed) {
    return unused_passes >= 8 && last_use <= completed;
}
#endif
