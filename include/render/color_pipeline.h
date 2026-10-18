#ifndef RENDER_COLOR_PIPELINE_H
#define RENDER_COLOR_PIPELINE_H

#include "render/color.h"
#include <wlr/types/wlr_buffer.h>

/* Immutable, buffer-scoped recipe. Keeps the public output/state ABI intact. */
#define COLOR_PIPELINE_MAX_OPS 32
enum color_pipeline_op_type { CP_CURVE, CP_MATRIX, CP_MULTIPLIER, CP_LUT, CP_LUT3D };
struct color_pipeline_op {
	enum color_pipeline_op_type type;
	const char *curve;
	double matrix[12];
	double multiplier;
	struct wlr_color_transform *transform;
};
struct color_pipeline_recipe {
	size_t len;
	struct color_pipeline_op ops[COLOR_PIPELINE_MAX_OPS];
};

bool color_pipeline_add_decode(
	struct color_pipeline_recipe *recipe, enum wlr_color_transfer_function tf);
bool color_pipeline_add_matrix(struct color_pipeline_recipe *recipe, const float matrix[9]);
bool color_pipeline_add_transform(
	struct color_pipeline_recipe *recipe, struct wlr_color_transform *transform);
uint64_t color_pipeline_fingerprint(const struct color_pipeline_recipe *recipe);
bool color_pipeline_fixed(double value, uint64_t *result);
/* The wrapper owns references to source and recipe transforms. Drop after use. */
struct wlr_buffer *color_pipeline_wrap(
	struct wlr_buffer *source, const struct color_pipeline_recipe *recipe);
const struct color_pipeline_recipe *color_pipeline_get(struct wlr_buffer *buffer);

#endif
