#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/util/log.h>
#include "ext-image-capture-source-v1-protocol.h"
#include "render/wlr_renderer.h"
#include "types/capture_output.h"
#include "types/screencopy_sdr.h"

static void source_handle_destroy(struct wl_client *client,
		struct wl_resource *source_resource) {
	wl_resource_destroy(source_resource);
}

static const struct ext_image_capture_source_v1_interface source_impl = {
	.destroy = source_handle_destroy,
};

struct wlr_ext_image_capture_source_v1 *wlr_ext_image_capture_source_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_image_capture_source_v1_interface, &source_impl));
	return wl_resource_get_user_data(resource);
}

static void source_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

void wlr_ext_image_capture_source_v1_init(struct wlr_ext_image_capture_source_v1 *source,
		const struct wlr_ext_image_capture_source_v1_interface *impl) {
	*source = (struct wlr_ext_image_capture_source_v1){
		.impl = impl,
	};
	wl_list_init(&source->resources);
	wl_signal_init(&source->events.destroy);
	wl_signal_init(&source->events.constraints_update);
	wl_signal_init(&source->events.frame);
}

void wlr_ext_image_capture_source_v1_finish(struct wlr_ext_image_capture_source_v1 *source) {
	wl_signal_emit_mutable(&source->events.destroy, NULL);

	assert(wl_list_empty(&source->events.destroy.listener_list));
	assert(wl_list_empty(&source->events.constraints_update.listener_list));
	assert(wl_list_empty(&source->events.frame.listener_list));

	struct wl_resource *resource, *resource_tmp;
	wl_resource_for_each_safe(resource, resource_tmp, &source->resources) {
		wl_resource_set_user_data(resource, NULL);
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}

	free(source->shm_formats);
	wlr_drm_format_set_finish(&source->dmabuf_formats);
}

bool wlr_ext_image_capture_source_v1_create_resource(struct wlr_ext_image_capture_source_v1 *source,
		struct wl_client *client, uint32_t new_id) {
	struct wl_resource *resource = wl_resource_create(client,
		&ext_image_capture_source_v1_interface, 1, new_id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return false;
	}
	wl_resource_set_implementation(resource, &source_impl, source,
		source_handle_resource_destroy);
	if (source != NULL) {
		wl_list_insert(&source->resources, wl_resource_get_link(resource));
	} else {
		wl_list_init(wl_resource_get_link(resource));
	}
	return true;
}

static uint32_t get_swapchain_shm_format(struct wlr_swapchain *swapchain,
		struct wlr_renderer *renderer) {
	struct wlr_buffer *buffer = wlr_swapchain_acquire(swapchain);
	if (buffer == NULL) {
		return DRM_FORMAT_INVALID;
	}

	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, buffer);
	wlr_buffer_unlock(buffer);
	if (texture == NULL) {
		return DRM_FORMAT_INVALID;
	}

	uint32_t format = wlr_texture_preferred_read_format(texture);
	wlr_texture_destroy(texture);

	return format;
}

static bool add_drm_format(struct wlr_drm_format_set *set, const struct wlr_drm_format *fmt) {
	for (size_t i = 0; i < fmt->len; i++) {
		if (!wlr_drm_format_set_add(set, fmt->format, fmt->modifiers[i])) {
			return false;
		}
	}
	return true;
}

bool capture_source_set_constraints(struct wlr_ext_image_capture_source_v1 *source,
		struct wlr_swapchain *swapchain, struct wlr_renderer *renderer, bool sdr_fallback) {
	// Prepare the complete replacement before notifying any sessions. In
	// particular, an unavailable read format must clear old SHM constraints.
	uint32_t native = get_swapchain_shm_format(swapchain, renderer);
	size_t len = native == DRM_FORMAT_INVALID ? 0 :
		(sdr_fallback && screencopy_is_10bit(native) ? 2 : 1);
	uint32_t *formats = len ? calloc(len, sizeof(*formats)) : NULL;
	struct wlr_drm_format_set dmabuf = {0};
	dev_t device = 0;
	if (len && !formats) {
		return false;
	}
	if (len) {
		formats[0] = native;
		if (len == 2) {
			formats[1] = DRM_FORMAT_XRGB8888;
		}
	}
	int drm_fd = wlr_renderer_get_drm_fd(renderer);
	if (swapchain->allocator &&
			(swapchain->allocator->buffer_caps & WLR_BUFFER_CAP_DMABUF) && drm_fd >= 0) {
		struct stat st;
		if (fstat(drm_fd, &st) != 0) {
			goto error;
		}
		device = st.st_rdev;
		if (!add_drm_format(&dmabuf, &swapchain->format)) {
			goto error;
		}
		const struct wlr_drm_format_set *render_formats = wlr_renderer_get_render_formats(renderer);
		assert(render_formats);
		uint32_t fallbacks[] = {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};
		for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
			const struct wlr_drm_format *fmt = wlr_drm_format_set_get(render_formats, fallbacks[i]);
			if (fmt && swapchain->format.format != fmt->format && !add_drm_format(&dmabuf, fmt)) {
				goto error;
			}
		}
	}
	free(source->shm_formats);
	wlr_drm_format_set_finish(&source->dmabuf_formats);
	source->shm_formats = formats;
	source->shm_formats_len = len;
	source->dmabuf_formats = dmabuf;
	source->dmabuf_device = device;
	source->width = swapchain->width;
	source->height = swapchain->height;
	wl_signal_emit_mutable(&source->events.constraints_update, NULL);
	return true;
error:
	free(formats);
	wlr_drm_format_set_finish(&dmabuf);
	return false;
}

bool wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(struct wlr_ext_image_capture_source_v1 *source,
		struct wlr_swapchain *swapchain, struct wlr_renderer *renderer) {
	// Scene and cursor sources keep their own color and copy semantics.
	return capture_source_set_constraints(source, swapchain, renderer, false);
}

void wlr_ext_image_capture_source_v1_cursor_init(struct wlr_ext_image_capture_source_v1_cursor *source_cursor,
		const struct wlr_ext_image_capture_source_v1_interface *impl) {
	*source_cursor = (struct wlr_ext_image_capture_source_v1_cursor){0};
	wlr_ext_image_capture_source_v1_init(&source_cursor->base, impl);
	wl_signal_init(&source_cursor->events.update);
}

void wlr_ext_image_capture_source_v1_cursor_finish(struct wlr_ext_image_capture_source_v1_cursor *source_cursor) {
	wlr_ext_image_capture_source_v1_finish(&source_cursor->base);
	assert(wl_list_empty(&source_cursor->events.update.listener_list));
}
