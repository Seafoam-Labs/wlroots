#ifndef TYPES_CAPTURE_OUTPUT_H
#define TYPES_CAPTURE_OUTPUT_H

#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <drm_fourcc.h>
#include "types/screencopy_sdr.h"

// description is the source encoding for output sources; NULL for others.
bool capture_source_set_constraints(struct wlr_ext_image_capture_source_v1 *source,
	struct wlr_swapchain *swapchain, struct wlr_renderer *renderer, bool sdr_fallback,
	const struct wlr_output_image_description *description);

bool capture_copy_output(struct wlr_ext_image_copy_capture_frame_v1 *frame,
	struct wlr_buffer *src, struct wlr_renderer *renderer,
	const struct wlr_output_image_description *description);

// known_sdr is established by the completed scene render, never buffer format.
bool capture_copy_scene(struct wlr_ext_image_copy_capture_frame_v1 *frame,
	struct wlr_buffer *src, struct wlr_renderer *renderer, bool known_sdr);

static inline bool capture_can_convert_sdr(const struct wlr_output_image_description *desc) {
	return desc == NULL ||
		(desc->transfer_function == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ &&
		 (desc->primaries == WLR_COLOR_NAMED_PRIMARIES_SRGB ||
		  desc->primaries == WLR_COLOR_NAMED_PRIMARIES_BT2020)) ||
		(desc->transfer_function == WLR_COLOR_TRANSFER_FUNCTION_GAMMA22 &&
		 desc->primaries == WLR_COLOR_NAMED_PRIMARIES_SRGB);
}

#define CAPTURE_SDR_DESCRIPTION ((struct wlr_output_image_description) { \
	.primaries = WLR_COLOR_NAMED_PRIMARIES_SRGB, \
	.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22, \
})

static inline bool capture_is_8bit(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return true;
	default:
		return false;
	}
}

static inline bool capture_is_pq(const struct wlr_output_image_description *desc) {
	return desc && desc->transfer_function == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ &&
		capture_can_convert_sdr(desc);
}

// 8-bit cannot carry PQ usefully. Such destinations of a 10-bit PQ source
// receive SDR (sRGB primaries, gamma 2.2) for both SHM and DMA-BUF.
static inline bool capture_sdr_target(const struct wlr_output_image_description *desc,
		uint32_t native, uint32_t format) {
	return capture_is_pq(desc) && screencopy_is_10bit(native) && capture_is_8bit(format);
}

// Encoding a frame of this format receives under the source's current
// constraints. Returns false when the encoding is not known in advance.
bool capture_source_format_encoding(struct wlr_ext_image_capture_source_v1 *source,
	uint32_t format, struct wlr_output_image_description *out);
bool capture_output_source_format_encoding(struct wlr_ext_image_capture_source_v1 *source,
	uint32_t format, struct wlr_output_image_description *out);
bool capture_scene_source_format_encoding(struct wlr_ext_image_capture_source_v1 *source,
	uint32_t format, struct wlr_output_image_description *out);

void capture_color_send(struct wlr_ext_image_copy_capture_frame_v1 *frame,
	const struct wlr_output_image_description *description);

#endif
