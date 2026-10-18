// SPDX-License-Identifier: MIT
#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/vulkan.h>
#include "render/allocator/shm.h"
#include "render/vulkan.h"

struct copy_allocator {
	struct wlr_allocator base;
	struct wlr_allocator *destination;
};

struct copy_buffer {
	struct wlr_addon addon;
};

static void copy_buffer_destroy(struct wlr_addon *addon) {
	struct copy_buffer *buffer = wl_container_of(addon, buffer, addon);
	wlr_addon_finish(addon);
	free(buffer);
}

static const struct wlr_addon_interface copy_buffer_impl = {
	.name = "vulkan_presentation_copy",
	.destroy = copy_buffer_destroy,
};

bool vulkan_is_copy_buffer(struct wlr_buffer *buffer) {
	return wlr_addon_find(&buffer->addons, NULL, &copy_buffer_impl) != NULL;
}

static struct wlr_buffer *copy_allocate(struct wlr_allocator *base,
		int width, int height, const struct wlr_drm_format *format) {
	struct copy_allocator *alloc = wl_container_of(base, alloc, base);
	if (width <= 0 || height <= 0 || width > INT32_MAX / 4 ||
			(uint64_t)width * height > INT32_MAX / 4 ||
			(format->format != DRM_FORMAT_XRGB8888 && format->format != DRM_FORMAT_ARGB8888)) {
		return NULL;
	}
	struct copy_buffer *tag = calloc(1, sizeof(*tag));
	if (!tag) return NULL;
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(alloc->destination, width, height, format);
	if (!buffer) {
		free(tag);
		return NULL;
	}
	wlr_addon_init(&tag->addon, &buffer->addons, NULL, &copy_buffer_impl);
	return buffer;
}

static void copy_destroy(struct wlr_allocator *base) {
	struct copy_allocator *alloc = wl_container_of(base, alloc, base);
	wlr_allocator_destroy(alloc->destination);
	free(alloc);
}

static const struct wlr_allocator_interface copy_impl = {
	.create_buffer = copy_allocate,
	.destroy = copy_destroy,
};

// Takes ownership on success only. The destination must provide writable pixels.
struct wlr_allocator *wlr_vk_copy_allocator_create(struct wlr_allocator *destination) {
	if (!(destination->buffer_caps & WLR_BUFFER_CAP_DATA_PTR)) return NULL;
	struct copy_allocator *alloc = calloc(1, sizeof(*alloc));
	if (!alloc) return NULL;
	alloc->destination = destination;
	wlr_allocator_init(&alloc->base, &copy_impl, destination->buffer_caps);
	return &alloc->base;
}

const struct wlr_drm_format_set *wlr_vk_renderer_get_copy_formats(struct wlr_renderer *base) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(base);
	if (renderer->copy_formats.len) return &renderer->copy_formats;
	VkFormatProperties props;
	vkGetPhysicalDeviceFormatProperties(renderer->dev->phdev, VK_FORMAT_B8G8R8A8_UNORM, &props);
	VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
		VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	if ((props.optimalTilingFeatures & required) != required) return NULL;
	vkGetPhysicalDeviceFormatProperties(renderer->dev->phdev, VK_FORMAT_B8G8R8A8_SRGB, &props);
	if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return NULL;
	if (!wlr_drm_format_set_add(&renderer->copy_formats, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR) ||
			!wlr_drm_format_set_add(&renderer->copy_formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR)) {
		wlr_drm_format_set_finish(&renderer->copy_formats);
		return NULL;
	}
	return &renderer->copy_formats;
}

bool wlr_vk_renderer_requires_copy(struct wlr_renderer *renderer) {
    struct wlr_vk_device *dev = vulkan_get_renderer(renderer)->dev;
	return !dev->dmabuf || dev->drm_fd < 0;
}
