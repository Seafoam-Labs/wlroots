#ifndef TYPES_CAPTURE_SHM_H
#define TYPES_CAPTURE_SHM_H

#include <stdlib.h>
#include <wlr/render/wlr_texture.h>
#include "types/screencopy_sdr.h"

// The caller owns texture/data access. Native readback never passes through
// the 8-bit conversion, including when the output uses PQ.
static inline bool capture_shm_read(struct wlr_texture *texture,
		void *data, uint32_t format, size_t stride, const struct wlr_box *box,
		bool allow_sdr, const struct wlr_output_image_description *description) {
	uint32_t native = wlr_texture_preferred_read_format(texture);
	// A legacy frame may have been advertised before HDR was toggled. Only
	// native and XRGB8888 are supported here; other old 8-bit layouts must
	// retry instead of receiving a renderer blit of undecoded PQ samples.
	if (allow_sdr && screencopy_is_10bit(native) &&
			format != native && format != DRM_FORMAT_XRGB8888) {
		return false;
	}
	if (!allow_sdr || !screencopy_is_10bit(native) || format != DRM_FORMAT_XRGB8888) {
		return wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options) {
			.data = data, .format = format, .stride = stride, .src_box = *box,
		});
	}
	if (box->width <= 0 || box->height <= 0 ||
			(size_t)box->width > SIZE_MAX / 4 / (size_t)box->height) {
		return false;
	}
	size_t native_stride = (size_t)box->width * 4;
	uint8_t *pixels = malloc(native_stride * (size_t)box->height);
	if (!pixels) {
		return false;
	}
	bool ok = wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options) {
		.data = pixels, .format = native, .stride = native_stride, .src_box = *box,
	});
	if (ok) {
		struct screencopy_sdr_conversion conv;
		screencopy_sdr_conversion_init(&conv, description);
		screencopy_sdr_convert(&conv, native, pixels, native_stride,
			data, stride, box->width, box->height);
	}
	free(pixels);
	return ok;
}

#endif
