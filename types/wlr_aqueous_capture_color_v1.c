#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include "types/capture_output.h"
#include "aqueous-capture-color-v1-protocol.h"

struct capture_color_info {
	struct wl_resource *resource;
	struct wl_listener frame_destroy;
	bool sent;
};

struct capture_color_manager {
	struct wl_global *global;
	struct wl_listener display_destroy;
};

static void info_unavailable(struct capture_color_info *info) {
	if (!info->sent) {
		aqueous_capture_color_info_v1_send_unavailable(info->resource);
		info->sent = true;
	}
}

static void info_frame_destroy(struct wl_listener *listener, void *data) {
	struct capture_color_info *info = wl_container_of(listener, info, frame_destroy);
	info_unavailable(info);
	wl_list_remove(&info->frame_destroy.link);
	wl_list_init(&info->frame_destroy.link);
}

static void info_destroy(struct wl_resource *resource) {
	struct capture_color_info *info = wl_resource_get_user_data(resource);
	wl_list_remove(&info->frame_destroy.link);
	free(info);
}

static void resource_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct aqueous_capture_color_info_v1_interface info_impl = {
	.destroy = resource_destroy,
};

static void get_frame_info(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *frame_resource) {
	struct wlr_ext_image_copy_capture_frame_v1 *frame =
		wlr_ext_image_copy_capture_frame_v1_from_resource(frame_resource);
	if (!frame || frame->capturing) {
		wl_resource_post_error(resource, AQUEOUS_CAPTURE_COLOR_MANAGER_V1_ERROR_INVALID_FRAME,
			"color info must be requested before capture");
		return;
	}
	if (wl_signal_get(&frame->events.destroy, info_frame_destroy)) {
		wl_resource_post_error(resource, AQUEOUS_CAPTURE_COLOR_MANAGER_V1_ERROR_DUPLICATE,
			"frame already has color info");
		return;
	}
	struct capture_color_info *info = calloc(1, sizeof(*info));
	if (!info) {
		wl_resource_post_no_memory(resource);
		return;
	}
	info->resource = wl_resource_create(client, &aqueous_capture_color_info_v1_interface, 1, id);
	if (!info->resource) {
		free(info);
		wl_resource_post_no_memory(resource);
		return;
	}
	wl_resource_set_implementation(info->resource, &info_impl, info, info_destroy);
	info->frame_destroy.notify = info_frame_destroy;
	wl_signal_add(&frame->events.destroy, &info->frame_destroy);
}

static const struct aqueous_capture_color_manager_v1_interface manager_impl = {
	.destroy = resource_destroy,
	.get_frame_info = get_frame_info,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&aqueous_capture_color_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void manager_destroy(struct wl_listener *listener, void *data) {
	struct capture_color_manager *manager = wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wl_global *wlr_aqueous_capture_color_manager_v1_create(struct wl_display *display) {
	struct capture_color_manager *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}
	manager->global = wl_global_create(display, &aqueous_capture_color_manager_v1_interface,
		1, manager, manager_bind);
	if (!manager->global) {
		free(manager);
		return NULL;
	}
	manager->display_destroy.notify = manager_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);
	return manager->global;
}

static uint32_t scaled(double value, double scale) {
	if (!isfinite(value) || value <= 0) {
		return 0;
	}
	return (uint32_t)llround(fmin(value * scale, UINT32_MAX));
}

void capture_color_send(struct wlr_ext_image_copy_capture_frame_v1 *frame,
		const struct wlr_output_image_description *desc) {
	struct wl_listener *listener = wl_signal_get(&frame->events.destroy, info_frame_destroy);
	if (!listener) {
		return;
	}
	struct capture_color_info *info = wl_container_of(listener, info, frame_destroy);
	if (info->sent) {
		return;
	}
	if (!desc) {
		info_unavailable(info);
		return;
	}
	uint32_t primaries, tf;
	switch (desc->primaries) {
	case WLR_COLOR_NAMED_PRIMARIES_SRGB: primaries = AQUEOUS_CAPTURE_COLOR_INFO_V1_PRIMARIES_SRGB; break;
	case WLR_COLOR_NAMED_PRIMARIES_BT2020: primaries = AQUEOUS_CAPTURE_COLOR_INFO_V1_PRIMARIES_BT2020; break;
	default: info_unavailable(info); return;
	}
	switch (desc->transfer_function) {
	case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22: tf = AQUEOUS_CAPTURE_COLOR_INFO_V1_TRANSFER_FUNCTION_GAMMA22; break;
	case WLR_COLOR_TRANSFER_FUNCTION_SRGB: tf = AQUEOUS_CAPTURE_COLOR_INFO_V1_TRANSFER_FUNCTION_SRGB; break;
	case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ: tf = AQUEOUS_CAPTURE_COLOR_INFO_V1_TRANSFER_FUNCTION_PQ; break;
	case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR: tf = AQUEOUS_CAPTURE_COLOR_INFO_V1_TRANSFER_FUNCTION_LINEAR; break;
	case WLR_COLOR_TRANSFER_FUNCTION_BT1886: tf = AQUEOUS_CAPTURE_COLOR_INFO_V1_TRANSFER_FUNCTION_BT1886; break;
	default: info_unavailable(info); return;
	}
	bool pq = desc->transfer_function == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
	double reference = pq ? 203.0 : 80.0;
	double white = pq && desc->sdr_white_level > 0 ? desc->sdr_white_level : reference;
	aqueous_capture_color_info_v1_send_encoding(info->resource, primaries, tf,
		scaled(reference, 10000), scaled(white, 10000));
	const struct wlr_color_primaries *p = &desc->mastering_display_primaries;
	if (desc->mastering_luminance.max > 0 && p->red.y > 0 &&
			p->green.y > 0 && p->blue.y > 0 && p->white.y > 0) {
		aqueous_capture_color_info_v1_send_mastering_display(info->resource,
			scaled(p->red.x, 1000000), scaled(p->red.y, 1000000),
			scaled(p->green.x, 1000000), scaled(p->green.y, 1000000),
			scaled(p->blue.x, 1000000), scaled(p->blue.y, 1000000),
			scaled(p->white.x, 1000000), scaled(p->white.y, 1000000),
			scaled(desc->mastering_luminance.min, 10000), scaled(desc->mastering_luminance.max, 10000));
	}
	if (desc->max_cll > 0 || desc->max_fall > 0) {
		aqueous_capture_color_info_v1_send_content_light(info->resource,
			scaled(desc->max_cll, 10000), scaled(desc->max_fall, 10000));
	}
	aqueous_capture_color_info_v1_send_done(info->resource);
	info->sent = true;
}
