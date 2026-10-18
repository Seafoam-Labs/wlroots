#ifndef TYPES_SCREENCOPY_SDR_H
#define TYPES_SCREENCOPY_SDR_H

#include <math.h>
#include <stdint.h>
#include <drm_fourcc.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_output.h>

struct screencopy_sdr_gamma {
	// First binary32 input which rounds to each output code. Entry zero is 0.
	float threshold[256];
	// Output code at the lower edge of each linear-light bucket. Threshold
	// correction below preserves the original quantization within a bucket.
	uint8_t coarse[4096];
};

// Shared by legacy screencopy and ext-image-copy capture. Initialized once,
// before entering any pixel loop; subsequent reads are immutable.
const struct screencopy_sdr_gamma *screencopy_sdr_gamma_get(void);

static inline bool screencopy_is_10bit(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		return true;
	default:
		return false;
	}
}

static inline uint32_t screencopy_shm_format(uint32_t preferred) {
	// Legacy screencopy clients commonly only understand 8-bit SHM formats.
	return screencopy_is_10bit(preferred) ? DRM_FORMAT_XRGB8888 : preferred;
}

struct screencopy_sdr_conversion {
	bool pq;
	float linear[1024];
	float matrix[9];
	const struct screencopy_sdr_gamma *gamma;
};

static inline void screencopy_sdr_conversion_init(struct screencopy_sdr_conversion *conv,
		const struct wlr_output_image_description *desc) {
	conv->pq = desc &&
		desc->transfer_function == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
	if (!conv->pq) {
		return;
	}
	conv->gamma = screencopy_sdr_gamma_get();

	struct wlr_color_primaries source, destination;
	wlr_color_primaries_from_named(&source, desc->primaries);
	wlr_color_primaries_from_named(&destination, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	wlr_color_primaries_transform_absolute_colorimetric(
		&source, &destination, conv->matrix);

	// Undo the scene's SDR-on-HDR gain. PQ is absolute, with 1.0 = 10000 nits.
	// Use the wlroots PQ reference white when no override was configured.
	double white = desc->sdr_white_level > 0 ? desc->sdr_white_level : 203.0;
	for (size_t i = 0; i < 1024; i++) {
		double p = pow((double)i / 1023.0, 32.0 / 2523.0);
		double linear = pow(fmax(p - 3424.0 / 4096.0, 0.0) /
			(2413.0 / 128.0 - (2392.0 / 128.0) * p), 16384.0 / 2610.0);
		conv->linear[i] = (float)(linear * 10000.0 / white);
	}
}

static inline uint8_t screencopy_sdr_encode(
		const struct screencopy_sdr_gamma *gamma, float value) {
	// Match the compositor's default SDR gamma. Highlights and colors outside
	// the SDR gamut clip; the legacy protocol cannot describe HDR content.
	// The original fmaxf(value, 0) also maps NaN to black.
	if (!(value > 0.0f)) {
		return 0;
	}
	if (value >= 1.0f) {
		return 255;
	}
	// Multiplication by this power of two is exact for inputs in (0, 1).
	unsigned bucket = (unsigned)(value * 4096.0f);
	unsigned code = gamma->coarse[bucket];
	while (code < 255 && value >= gamma->threshold[code + 1]) {
		code++;
	}
	return code;
}

static inline void screencopy_sdr_convert(const struct screencopy_sdr_conversion *conv,
		uint32_t format, const uint8_t *src, size_t src_stride,
		uint8_t *dst, size_t dst_stride, int width, int height) {
	bool bgr = format == DRM_FORMAT_XBGR2101010 || format == DRM_FORMAT_ABGR2101010;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const uint8_t *s = src + (size_t)y * src_stride + (size_t)x * 4;
			uint8_t *d = dst + (size_t)y * dst_stride + (size_t)x * 4;
			uint32_t pixel = (uint32_t)s[0] | (uint32_t)s[1] << 8 |
				(uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
			uint32_t r = (pixel >> (bgr ? 0 : 20)) & 1023;
			uint32_t g = (pixel >> 10) & 1023;
			uint32_t b = (pixel >> (bgr ? 20 : 0)) & 1023;
			if (conv->pq) {
				float rgb[3] = {conv->linear[r], conv->linear[g], conv->linear[b]};
				for (size_t c = 0; c < 3; c++) {
					const float *m = &conv->matrix[c * 3];
					d[2 - c] = screencopy_sdr_encode(conv->gamma,
						m[0] * rgb[0] + m[1] * rgb[1] + m[2] * rgb[2]);
				}
			} else {
				d[2] = (r * 255 + 511) / 1023;
				d[1] = (g * 255 + 511) / 1023;
				d[0] = (b * 255 + 511) / 1023;
			}
			d[3] = 255;
		}
	}
}

#endif
