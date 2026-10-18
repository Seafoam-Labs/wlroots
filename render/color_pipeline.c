#include "render/color_pipeline.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/color.h>
#include <wlr/render/dmabuf.h>

static struct color_pipeline_op *append(
	struct color_pipeline_recipe *r, enum color_pipeline_op_type type) {
	if (r->len == COLOR_PIPELINE_MAX_OPS) {
		return NULL;
	}
	struct color_pipeline_op *op = &r->ops[r->len++];
	*op = (struct color_pipeline_op){.type = type};
	return op;
}

bool color_pipeline_fixed(double value, uint64_t *result) {
	/* S31.32 sign-magnitude, NOT two's complement. Avoid undefined casts. */
	if (!isfinite(value) || fabs(value) >= 2147483648.0) {
		return false;
	}
	long double scaled = roundl(fabsl((long double)value) * 4294967296.0L);
	if (scaled > (long double)INT64_MAX) {
		return false;
	}
	*result = (uint64_t)scaled;
	if (value < 0 && *result != 0) {
		*result |= UINT64_C(1) << 63;
	}
	return true;
}

static bool add_multiplier(struct color_pipeline_recipe *r, double value) {
	struct color_pipeline_op *op = append(r, CP_MULTIPLIER);
	if (!op) {
		return false;
	}
	op->multiplier = value;
	return true;
}

static bool add_curve(
	struct color_pipeline_recipe *r, enum wlr_color_transfer_function tf, bool inverse) {
	const char *name;
	switch (tf) {
	case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		return true;
	case WLR_COLOR_TRANSFER_FUNCTION_SRGB:
		name = inverse ? "sRGB Inverse EOTF" : "sRGB EOTF";
		break;
	case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22:
		name = inverse ? "Gamma 2.2 Inverse" : "Gamma 2.2";
		break;
	case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		name = inverse ? "PQ 125 Inverse EOTF" : "PQ 125 EOTF";
		break;
	default:
		return false;
	}
	if (tf == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ && inverse && !add_multiplier(r, 125.0)) {
		return false;
	}
	struct color_pipeline_op *op = append(r, CP_CURVE);
	if (!op) {
		return false;
	}
	op->curve = name;
	return tf != WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ || inverse || add_multiplier(r, 1.0 / 125.0);
}

bool color_pipeline_add_decode(
	struct color_pipeline_recipe *r, enum wlr_color_transfer_function tf) {
	return add_curve(r, tf, false);
}

bool color_pipeline_add_matrix(struct color_pipeline_recipe *r, const float m[9]) {
	struct color_pipeline_op *op = append(r, CP_MATRIX);
	if (!op) {
		return false;
	}
	for (size_t row = 0; row < 3; row++) {
		for (size_t col = 0; col < 3; col++) {
			if (!isfinite(m[row * 3 + col])) {
				return false;
			}
			op->matrix[row * 4 + col] = m[row * 3 + col];
		}
	}
	return true;
}

bool color_pipeline_add_transform(struct color_pipeline_recipe *r, struct wlr_color_transform *tr) {
	if (!tr) {
		return true;
	}
	switch (tr->type) {
	case COLOR_TRANSFORM_INVERSE_EOTF:
		return add_curve(r, wlr_color_transform_inverse_eotf_from_base(tr)->tf, true);
	case COLOR_TRANSFORM_MATRIX: {
		struct wlr_color_transform_matrix *m = wl_container_of(tr, m, base);
		return color_pipeline_add_matrix(r, m->matrix);
	}
	case COLOR_TRANSFORM_LUT_3X1D:
	case COLOR_TRANSFORM_LCMS2: {
		struct color_pipeline_op *op =
			append(r, tr->type == COLOR_TRANSFORM_LUT_3X1D ? CP_LUT : CP_LUT3D);
		if (!op) {
			return false;
		}
		op->transform = tr;
		return true;
	}
	case COLOR_TRANSFORM_PIPELINE: {
		struct wlr_color_transform_pipeline *p = wl_container_of(tr, p, base);
		for (size_t i = 0; i < p->len; i++) {
			if (!color_pipeline_add_transform(r, p->transforms[i])) {
				return false;
			}
		}
		return true;
	}
	}
	return false;
}

/* Combine adjacent linear operations; order is left multiplication. This also
 * folds PQ-125 normalization into a matrix on devices lacking a multiplier. */
static void normalize(struct color_pipeline_recipe *r) {
	for (size_t i = 0; i + 1 < r->len;) {
		struct color_pipeline_op *a = &r->ops[i], *b = &r->ops[i + 1];
		if ((a->type != CP_MATRIX && a->type != CP_MULTIPLIER) ||
			(b->type != CP_MATRIX && b->type != CP_MULTIPLIER)) {
			i++;
			continue;
		}
		double am[12] = {0}, bm[12] = {0};
		if (a->type == CP_MATRIX) {
			memcpy(am, a->matrix, sizeof(am));
		} else {
			am[0] = am[5] = am[10] = a->multiplier;
		}
		if (b->type == CP_MATRIX) {
			memcpy(bm, b->matrix, sizeof(bm));
		} else {
			bm[0] = bm[5] = bm[10] = b->multiplier;
		}
		*a = (struct color_pipeline_op){.type = CP_MATRIX};
		for (size_t row = 0; row < 3; row++) {
			for (size_t col = 0; col < 4; col++) {
				for (size_t k = 0; k < 3; k++) {
					a->matrix[row * 4 + col] += bm[row * 4 + k] * am[k * 4 + col];
				}
			}
			a->matrix[row * 4 + 3] += bm[row * 4 + 3];
		}
		memmove(b, b + 1, (r->len - i - 2) * sizeof(*b));
		r->len--;
	}
}

/* Cache only rejection attempts, never skip the final atomic test. All recipe
 * storage is initialized and immutable; pointer identity covers opaque LUTs. */
uint64_t color_pipeline_fingerprint(const struct color_pipeline_recipe *recipe) {
	uint64_t hash = UINT64_C(14695981039346656037);
	for (size_t i = 0; i < recipe->len; i++) {
		const struct color_pipeline_op *op = &recipe->ops[i];
		hash = (hash ^ op->type) * UINT64_C(1099511628211);
		const unsigned char *data = (const unsigned char *)op->matrix;
		for (size_t j = 0; j < sizeof(op->matrix); j++)
			hash = (hash ^ data[j]) * UINT64_C(1099511628211);
		data = (const unsigned char *)&op->multiplier;
		for (size_t j = 0; j < sizeof(op->multiplier); j++)
			hash = (hash ^ data[j]) * UINT64_C(1099511628211);
		for (const char *c = op->curve; c && *c; c++)
			hash = (hash ^ (unsigned char)*c) * UINT64_C(1099511628211);
		hash = (hash ^ (uintptr_t)op->transform) * UINT64_C(1099511628211);
	}
	return hash;
}

struct pipeline_buffer {
	struct wlr_buffer base;
	struct wlr_buffer *source;
	struct color_pipeline_recipe recipe;
};

static void destroy(struct wlr_buffer *buffer) {
	struct pipeline_buffer *b = wl_container_of(buffer, b, base);
	for (size_t i = 0; i < b->recipe.len; i++) {
		wlr_color_transform_unref(b->recipe.ops[i].transform);
	}
	wlr_buffer_unlock(b->source);
	free(b);
}

static bool get_dmabuf(struct wlr_buffer *buffer, struct wlr_dmabuf_attributes *attrs) {
	struct pipeline_buffer *b = wl_container_of(buffer, b, base);
	return wlr_buffer_get_dmabuf(b->source, attrs);
}

static const struct wlr_buffer_impl impl = {
	.destroy = destroy,
	.get_dmabuf = get_dmabuf,
};

struct wlr_buffer *color_pipeline_wrap(
	struct wlr_buffer *source, const struct color_pipeline_recipe *recipe) {
	if (!source || !recipe->len || recipe->len > COLOR_PIPELINE_MAX_OPS) {
		return NULL;
	}
	struct pipeline_buffer *b = calloc(1, sizeof(*b));
	if (!b) {
		return NULL;
	}
	b->source = wlr_buffer_lock(source);
	b->recipe = *recipe;
	normalize(&b->recipe);
	for (size_t i = 0; i < b->recipe.len; i++) {
		if (b->recipe.ops[i].transform) {
			wlr_color_transform_ref(b->recipe.ops[i].transform);
		}
	}
	wlr_buffer_init(&b->base, &impl, source->width, source->height);
	return &b->base;
}

const struct color_pipeline_recipe *color_pipeline_get(struct wlr_buffer *buffer) {
	if (!buffer || buffer->impl != &impl) {
		return NULL;
	}
	struct pipeline_buffer *b = wl_container_of(buffer, b, base);
	return &b->recipe;
}

bool wlr_buffer_has_color_pipeline(struct wlr_buffer *buffer) {
	return color_pipeline_get(buffer) != NULL;
}
