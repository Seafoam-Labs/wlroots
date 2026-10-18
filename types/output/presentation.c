// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/config.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_presentation.h>
#include <wlr/util/log.h>
#include "types/wlr_output.h"

#if WLR_HAS_VULKAN_RENDERER
#include <wlr/render/vulkan.h>
// Allocator owns a re-opened DRM FD, never the backend's FD.
struct wlr_allocator *allocator_autocreate_cpu(struct wlr_backend *backend);
bool vulkan_is_copy_buffer(struct wlr_buffer *buffer);

struct output_presentation {
	struct wlr_addon addon;
	struct wlr_output *output;
	struct wlr_allocator *original;
	struct wlr_allocator *copy;
	bool allowed;
	bool attempted;
	bool committed;
	struct wl_listener commit;
};

static void presentation_destroy(struct wlr_addon *addon) {
	struct output_presentation *p = wl_container_of(addon, p, addon);
	wl_list_remove(&p->commit.link);
	wlr_addon_finish(addon);
	wlr_allocator_destroy(p->copy);
	free(p);
}

static const struct wlr_addon_interface presentation_impl = {
	.name = "vulkan_output_presentation",
	.destroy = presentation_destroy,
};

static struct output_presentation *get_presentation(struct wlr_output *output) {
	struct wlr_addon *addon = wlr_addon_find(&output->addons, NULL, &presentation_impl);
	if (!addon) return NULL;
	struct output_presentation *p = wl_container_of(addon, p, addon);
	return p;
}
#endif

#if WLR_HAS_VULKAN_RENDERER
static void presentation_handle_commit(struct wl_listener *listener, void *data) {
	struct output_presentation *p = wl_container_of(listener, p, commit);
	struct wlr_output_event_commit *event = data;
	if (!p->copy || !event->state->buffer || !vulkan_is_copy_buffer(event->state->buffer)) return;
	if (!p->committed) {
		wlr_log(WLR_INFO, "Output '%s': Vulkan CPU-copy frame committed", p->output->name);
		p->committed = true;
	}
}
#endif

bool wlr_output_allow_presentation_copy(struct wlr_output *output, bool allow) {
#if WLR_HAS_VULKAN_RENDERER
	struct output_presentation *p = get_presentation(output);
	if (!p) {
		if (!allow) return true;
		if (!output->renderer || !wlr_renderer_is_vk(output->renderer)) return false;
		p = calloc(1, sizeof(*p));
		if (!p) return false;
		p->output = output;
		p->original = output->allocator;
		p->commit.notify = presentation_handle_commit;
		wl_signal_add(&output->events.commit, &p->commit);
		wlr_addon_init(&p->addon, &output->addons, NULL, &presentation_impl);
	}
	if (!allow && p->copy) return false; // reset after draining committed buffers
	p->allowed = allow;
	return true;
#else
	return !allow;
#endif
}

bool wlr_output_try_presentation_copy(struct wlr_output *output) {
#if WLR_HAS_VULKAN_RENDERER
	struct output_presentation *p = get_presentation(output);
	if (!p || !p->allowed || p->attempted) return false;
	p->attempted = true;
	if (!wlr_vk_renderer_get_copy_formats(output->renderer)) return false;
	struct wlr_allocator *destination = allocator_autocreate_cpu(output->backend);
	if (!destination) return false;
	p->copy = wlr_vk_copy_allocator_create(destination);
	if (!p->copy) {
		wlr_allocator_destroy(destination);
		return false;
	}
	output->allocator = p->copy;
	wlr_output_lock_attach_render(output, true);
	wlr_output_lock_software_cursors(output, true);
	wlr_log(WLR_INFO, "Output '%s': trying synchronous Vulkan CPU-copy presentation (SDR)", output->name);
	return true;
#else
	return false;
#endif
}

bool wlr_output_uses_presentation_copy(struct wlr_output *output) {
#if WLR_HAS_VULKAN_RENDERER
	struct output_presentation *p = get_presentation(output);
	return p && p->copy;
#else
	return false;
#endif
}

bool wlr_output_presentation_copy_committed(struct wlr_output *output) {
#if WLR_HAS_VULKAN_RENDERER
	struct output_presentation *p = get_presentation(output);
	return p && p->committed;
#else
	return false;
#endif
}

void wlr_output_reset_presentation(struct wlr_output *output) {
#if WLR_HAS_VULKAN_RENDERER
	struct output_presentation *p = get_presentation(output);
	if (!p) return;
	if (p->copy) {
		output->allocator = p->original;
		wlr_output_lock_attach_render(output, false);
		wlr_output_lock_software_cursors(output, false);
	}
	presentation_destroy(&p->addon);
#endif
}
