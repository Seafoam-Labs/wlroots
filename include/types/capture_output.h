#ifndef TYPES_CAPTURE_OUTPUT_H
#define TYPES_CAPTURE_OUTPUT_H

#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>

bool capture_source_set_constraints(struct wlr_ext_image_capture_source_v1 *source,
	struct wlr_swapchain *swapchain, struct wlr_renderer *renderer, bool sdr_fallback);

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

void capture_color_send(struct wlr_ext_image_copy_capture_frame_v1 *frame,
	const struct wlr_output_image_description *description);

#endif
