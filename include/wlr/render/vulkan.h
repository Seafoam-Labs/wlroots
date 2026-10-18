/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_VULKAN_H
#define WLR_RENDER_VULKAN_H

#include <vulkan/vulkan_core.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>

struct wlr_allocator;
struct wlr_drm_format_set;
bool wlr_vk_renderer_requires_copy(struct wlr_renderer *renderer);
struct wlr_allocator *wlr_vk_copy_allocator_create(struct wlr_allocator *destination);
const struct wlr_drm_format_set *wlr_vk_renderer_get_copy_formats(struct wlr_renderer *renderer);

#define WLR_AQUEOUS_RENDER_HOOK_VERSION 10

#define WLR_AQUEOUS_VULKAN_SELECTION_VERSION 1
struct wlr_backend;
struct wlr_vk_renderer_candidates;
/**
 * Snapshot usable Vulkan physical devices in preferred hardware, other hardware,
 * then CPU order. Explicit WLR_RENDER_DRM_DEVICE and FORCE_SOFTWARE restrictions
 * are honored. No renderer or logical device is created until next().
 */
struct wlr_vk_renderer_candidates *wlr_vk_renderer_candidates_create(
	struct wlr_backend *backend, bool allow_software);
/** Try remaining candidates until one renderer is created, or return NULL. */
struct wlr_renderer *wlr_vk_renderer_candidates_next(
	struct wlr_vk_renderer_candidates *candidates);
/** Returned renderers remain valid after destroying the candidates. */
void wlr_vk_renderer_candidates_destroy(struct wlr_vk_renderer_candidates *candidates);
/** Recreate this exact physical device for recovery without migrating clients. */
struct wlr_renderer *wlr_vk_renderer_recreate(struct wlr_renderer *renderer);

struct wlr_vk_image_attribs {
	VkImage image;
	VkImageLayout layout;
	VkFormat format;
};

struct wlr_vk_render_pass_attribs {
	VkCommandBuffer command_buffer;
	VkRenderPass render_pass;
	VkExtent2D extent;
	uint32_t subpass;
	bool has_signal_timeline;
};

struct wlr_vk_render_texture_attribs {
	struct wlr_vk_render_pass_attribs render_pass;
	VkPipelineLayout pipeline_layout;
	VkDescriptorSetLayout descriptor_set_layout;
	VkDescriptorSet descriptor_set;
	float projection[4][4];
	float uv_offset[2];
	float uv_size[2];
	float color_matrix[4][4];
	float alpha;
	float luminance_multiplier;
	uint32_t texture_transform;
};

typedef bool (*wlr_vk_render_texture_hook_func_t)(
	const struct wlr_vk_render_texture_attribs *attribs, void *data);

struct wlr_vk_render_offscreen_attribs {
	VkCommandBuffer command_buffer;
	VkImageView source_image_view;
	VkFormat source_format;
	VkExtent2D extent;
};

typedef bool (*wlr_vk_render_offscreen_func_t)(
	const struct wlr_vk_render_offscreen_attribs *attribs, void *data);

typedef void (*wlr_vk_render_completion_func_t)(void *data);

struct wlr_renderer *wlr_vk_renderer_create_with_drm_fd(int drm_fd);

VkInstance wlr_vk_renderer_get_instance(struct wlr_renderer *renderer);
VkPhysicalDevice wlr_vk_renderer_get_physical_device(struct wlr_renderer *renderer);
VkDevice wlr_vk_renderer_get_device(struct wlr_renderer *renderer);
uint32_t wlr_vk_renderer_get_queue_family(struct wlr_renderer *renderer);

bool wlr_renderer_is_vk(struct wlr_renderer *wlr_renderer);
bool wlr_texture_is_vk(struct wlr_texture *texture);

void wlr_vk_texture_get_image_attribs(struct wlr_texture *texture,
	struct wlr_vk_image_attribs *attribs);
bool wlr_vk_texture_has_alpha(struct wlr_texture *texture);

/**
 * Get the active Vulkan command buffer and compatible render pass.
 *
 * This is only valid while a scene render hook is running. Retrieving the
 * attributes invalidates wlroots' cached pipeline binding so callers may bind
 * their own pipeline without corrupting later wlroots draws.
 */
bool wlr_vk_render_pass_get_attribs(struct wlr_render_pass *render_pass,
	struct wlr_vk_render_pass_attribs *attribs);

/**
 * Install a callback for the next texture added to this Vulkan render pass.
 *
 * The callback receives wlroots' prepared texture descriptor, pipeline layout,
 * transforms, color data, and active render-pass attributes. Returning true
 * replaces wlroots' texture draw. Returning false uses the stock draw.
 */
bool wlr_vk_render_pass_set_texture_hook(struct wlr_render_pass *render_pass,
	wlr_vk_render_texture_hook_func_t hook, void *data);

/**
 * Force the renderer to use its linear intermediate attachment.
 */
bool wlr_vk_renderer_enable_offscreen(struct wlr_renderer *renderer);

/**
 * Temporarily suspend the active pass and expose its completed linear color
 * attachment to a callback. The callback may only read the source image view.
 */
bool wlr_vk_render_pass_run_offscreen(struct wlr_render_pass *render_pass,
	wlr_vk_render_offscreen_func_t callback, void *data);

/**
 * Run a callback after the active pass' command buffer has completed.
 *
 * The callback is also run if recording or submission fails, making it safe
 * to use for releasing resources retained by a custom draw.
 */
bool wlr_vk_render_pass_add_completion(struct wlr_render_pass *render_pass,
	wlr_vk_render_completion_func_t callback, void *data);

#endif
