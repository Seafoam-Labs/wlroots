/* Based on the discovery and atomic programming design in wlroots !5220,
 * head e67d45a31e2df6492a00dc94498774e1dbaaf7e3. See patch provenance. */
#include "backend/drm/color_pipeline.h"
#include "backend/drm/drm.h"
#include "config.h"
#include "render/color_pipeline.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include <xf86drm.h>

#if HAVE_DRM_COLOR_PIPELINE

#define MAX_NODES 32
#define MAX_PIPELINES 128
struct colorop {
	uint32_t id, type;
	uint32_t bypass, curve, data, multiplier;
	uint64_t size;
	bool interpolation_supported;
	drmModePropertyRes *curves;
};
struct color_pipeline {
	uint32_t root;
	size_t len;
	struct colorop nodes[MAX_NODES];
};
struct color_plane {
	uint32_t id;
	uint32_t property;
	size_t len;
	struct color_pipeline *pipelines;
	uint32_t committed_crtc, committed_root, logged_root;
	bool rejection_logged;
};
struct color_state {
	struct color_plane *plane;
	uint32_t root;
	uint32_t blobs[MAX_NODES];
	size_t blobs_len;
};

static bool add(drmModeAtomicReq *req, uint32_t id, uint32_t prop, uint64_t value) {
	return prop && drmModeAtomicAddProperty(req, id, prop, value) >= 0;
}

void drm_color_pipeline_init(struct wlr_drm_backend *drm) {
	const char *mode = getenv("AQUEOUS_DRM_COLOR_PIPELINE");
	if (mode && strcmp(mode, "off") == 0) {
		wlr_log(WLR_DEBUG, "DRM color pipelines compiled in; disabled by startup policy");
		return;
	}
	if (mode && strcmp(mode, "auto") != 0) {
		wlr_log(WLR_ERROR, "Invalid AQUEOUS_DRM_COLOR_PIPELINE (expected off or auto)");
		return;
	}
	if (drm->iface == &legacy_iface || drm->parent) {
		wlr_log(WLR_INFO, "Color pipelines unavailable on legacy/multi-GPU backend");
		return;
	}
	// Check the driver owning this DRM fd before opting into colorop semantics.
	// Both NVIDIA kernel module variants report nvidia-drm; Nouveau also drives
	// NVIDIA hardware. An unrelated GPU must not disable this backend's support.
	drmVersion *version = drmGetVersion(drm->fd);
	if (!version || !version->name) {
		drmFreeVersion(version);
		wlr_log(WLR_INFO, "DRM fd %d: color pipelines disabled; driver identification failed", drm->fd);
		return;
	}
	bool nvidia = strcmp(version->name, "nvidia-drm") == 0 ||
		strcmp(version->name, "nouveau") == 0;
	drmFreeVersion(version);
	if (nvidia) {
		wlr_log(WLR_INFO, "DRM fd %d: color pipelines disabled by NVIDIA compatibility policy", drm->fd);
		return;
	}

	drm->color_pipeline_enabled =
		drmSetClientCap(drm->fd, DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE, 1) == 0;
	wlr_log(WLR_INFO, "DRM fd %d (%s): color pipeline capability %s", drm->fd,
		drm->iface == &atomic_iface ? "atomic" : "libliftoff",
		drm->color_pipeline_enabled ? "enabled (experimental)" : "unavailable");
}

static bool read_node(int fd, uint32_t id, struct colorop *op, uint32_t *next) {
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_COLOROP);
	if (!props) {
		return false;
	}
	bool type_found = false, next_found = false, valid = true;
	op->id = id;
	op->interpolation_supported = true;
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (!p) {
			valid = false;
			break;
		}
		uint64_t value = props->prop_values[i];
		uint32_t kind = drmModeGetPropertyType(p);
		if (strcmp(p->name, "TYPE") == 0) {
			type_found = kind == DRM_MODE_PROP_ENUM && value <= UINT32_MAX;
			op->type = value;
		} else if (strcmp(p->name, "NEXT") == 0) {
			next_found = value <= UINT32_MAX && (p->flags & DRM_MODE_PROP_IMMUTABLE);
			*next = value;
		} else if (strcmp(p->name, "BYPASS") == 0 && kind == DRM_MODE_PROP_RANGE &&
				   p->count_values == 2 && p->values[0] == 0 && p->values[1] == 1) {
			op->bypass = p->prop_id;
		} else if (strcmp(p->name, "CURVE_1D_TYPE") == 0 && kind == DRM_MODE_PROP_ENUM) {
			op->curve = p->prop_id;
			op->curves = p;
			p = NULL;
		} else if (strcmp(p->name, "DATA") == 0 && kind == DRM_MODE_PROP_BLOB) {
			op->data = p->prop_id;
		} else if (strcmp(p->name, "SIZE") == 0 && (p->flags & DRM_MODE_PROP_IMMUTABLE)) {
			op->size = value;
		} else if (strcmp(p->name, "MULTIPLIER") == 0 && kind == DRM_MODE_PROP_RANGE) {
			op->multiplier = p->prop_id;
		} else if (strstr(p->name, "INTERPOLATION") != NULL) {
			/* Only a fixed advertised linear 1D interpolation is qualified. */
			op->interpolation_supported = kind == DRM_MODE_PROP_ENUM && p->count_enums == 1 &&
										  strcmp(p->enums[0].name, "Linear") == 0;
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return valid && type_found && next_found;
}

static void finish_pipeline(struct color_pipeline *p) {
	for (size_t i = 0; i < p->len; i++) {
		drmModeFreeProperty(p->nodes[i].curves);
	}
	*p = (struct color_pipeline){0};
}

bool drm_color_pipeline_discover(struct wlr_drm_backend *drm, struct wlr_drm_plane *plane) {
	if (!drm->color_pipeline_enabled) {
		return true;
	}
	drmModeObjectProperties *props =
		drmModeObjectGetProperties(drm->fd, plane->id, DRM_MODE_OBJECT_PLANE);
	if (!props) {
		return false;
	}
	drmModePropertyRes *roots = NULL;
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(drm->fd, props->props[i]);
		if (!p) {
			drmModeFreeObjectProperties(props);
			return false;
		}
		if (strcmp(p->name, "COLOR_PIPELINE") == 0 &&
			drmModeGetPropertyType(p) == DRM_MODE_PROP_ENUM) {
			roots = p;
			break;
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	if (!roots) {
		return true;
	}
	struct color_plane *cp = calloc(1, sizeof(*cp));
	if (!cp) {
		drmModeFreeProperty(roots);
		return false;
	}
	/* Keep the reset property even if discovery/allocation fails. */
	plane->color_pipeline = cp;
	cp->id = plane->id;
	cp->property = roots->prop_id;
	if (roots->count_enums <= 0 || roots->count_enums > MAX_PIPELINES) {
		goto out;
	}
	cp->pipelines = calloc(roots->count_enums, sizeof(*cp->pipelines));
	if (!cp->pipelines) {
		goto out;
	}
	for (int i = 0; i < roots->count_enums; i++) {
		uint64_t root = roots->enums[i].value;
		if (!root || root > UINT32_MAX) {
			continue;
		}
		struct color_pipeline *p = &cp->pipelines[cp->len];
		p->root = root;
		uint32_t next = root;
		bool valid = true;
		while (next) {
			for (size_t j = 0; j < p->len; j++) {
				if (p->nodes[j].id == next) {
					valid = false;
				}
			}
			if (!valid || p->len == MAX_NODES ||
				!read_node(drm->fd, next, &p->nodes[p->len++], &next)) {
				valid = false;
				break;
			}
		}
		if (valid && p->len) {
			cp->len++;
		} else {
			finish_pipeline(p);
		}
	}
out:
	wlr_log(WLR_INFO, "DRM plane %u: %zu color pipelines discovered", plane->id, cp->len);
	drmModeFreeProperty(roots);
	return true;
}

bool drm_color_pipeline_supported(struct wlr_drm_backend *drm) {
	if (!drm->color_pipeline_enabled)
		return false;
	for (size_t i = 0; i < drm->num_planes; i++) {
		if (drm->planes[i].color_pipeline && drm->planes[i].color_pipeline->len)
			return true;
	}
	return false;
}

void drm_color_pipeline_finish(struct wlr_drm_plane *plane) {
	struct color_plane *cp = plane->color_pipeline;
	if (!cp) {
		return;
	}
	for (size_t i = 0; i < cp->len; i++) {
		finish_pipeline(&cp->pipelines[i]);
	}
	free(cp->pipelines);
	free(cp);
	plane->color_pipeline = NULL;
}

static bool curve_value(const struct colorop *node, const char *name, uint64_t *value) {
	if (!node->curve || !node->curves) {
		return false;
	}
	for (int i = 0; i < node->curves->count_enums; i++) {
		if (strcmp(node->curves->enums[i].name, name) == 0) {
			*value = node->curves->enums[i].value;
			return true;
		}
	}
	return false;
}

static double lut_sample(const uint16_t *values, size_t size, double x) {
	double position = x * (size - 1);
	size_t lo = (size_t)floor(position);
	size_t hi = lo + 1 < size ? lo + 1 : lo;
	return (values[lo] + (values[hi] - (double)values[lo]) * (position - lo)) / UINT16_MAX;
}

static uint32_t lut_resample(const uint16_t *values, size_t source_size, size_t size, size_t i) {
	return (uint32_t)llround(lut_sample(values, source_size, (double)i / (size - 1)) * UINT32_MAX);
}

static bool lut_resample_qualified(const struct wlr_color_transform_lut_3x1d *lut, size_t size) {
	if (lut->dim < 2 || lut->dim > 65536 || size < 2 || size > 65536)
		return false;
	if (lut->dim == size)
		return true;
	for (size_t c = 0; c < 3; c++) {
		const uint16_t *v = lut->lut_3x1d + c * lut->dim;
		for (size_t i = 0; i < lut->dim; i++) {
			double x = (double)i / (lut->dim - 1);
			double position = x * (size - 1);
			size_t lo = (size_t)floor(position), hi = lo + 1 < size ? lo + 1 : lo;
			double a = lut_resample(v, lut->dim, size, lo) / (double)UINT32_MAX;
			double b = lut_resample(v, lut->dim, size, hi) / (double)UINT32_MAX;
			double actual = a + (b - a) * (position - lo);
			if (fabs(actual - v[i] / (double)UINT16_MAX) > 1.0 / UINT16_MAX)
				return false;
		}
	}
	return true;
}

static bool compatible(const struct colorop *n, const struct color_pipeline_op *op) {
	uint64_t ignored;
	switch (op->type) {
	case CP_CURVE:
		return n->type == DRM_COLOROP_1D_CURVE && curve_value(n, op->curve, &ignored);
	case CP_MATRIX:
		if (n->type != DRM_COLOROP_CTM_3X4 || !n->data) {
			return false;
		}
		for (size_t i = 0; i < 12; i++) {
			if (!color_pipeline_fixed(op->matrix[i], &ignored)) {
				return false;
			}
		}
		return true;
	case CP_MULTIPLIER:
		return n->type == DRM_COLOROP_MULTIPLIER && n->multiplier &&
			   color_pipeline_fixed(op->multiplier, &ignored);
	case CP_LUT: {
		struct wlr_color_transform_lut_3x1d *lut =
			color_transform_lut_3x1d_from_base(op->transform);
		return n->type == DRM_COLOROP_1D_LUT && n->data && n->interpolation_supported &&
			   lut_resample_qualified(lut, n->size);
	}
	case CP_LUT3D:
		/* LCMS domains/interpolation are not yet qualified against KMS. */
		return false;
	}
	return false;
}

static bool match(const struct color_pipeline *p, const struct color_pipeline_recipe *r, size_t ni,
	size_t oi, int assignment[MAX_NODES], unsigned *budget) {
	if (!*budget) {
		return false;
	}
	--*budget;
	if (ni == p->len) {
		return oi == r->len;
	}
	if (oi < r->len && compatible(&p->nodes[ni], &r->ops[oi])) {
		assignment[ni] = oi;
		if (match(p, r, ni + 1, oi + 1, assignment, budget)) {
			return true;
		}
	}
	assignment[ni] = -1;
	return p->nodes[ni].bypass && match(p, r, ni + 1, oi, assignment, budget);
}

static bool emit(int fd, drmModeAtomicReq *req, const struct colorop *node,
	const struct color_pipeline_op *op, struct color_state *pending) {
	if (node->bypass && !add(req, node->id, node->bypass, op == NULL)) {
		return false;
	}
	if (!op) {
		return true;
	}
	uint64_t value;
	if (op->type == CP_CURVE) {
		return curve_value(node, op->curve, &value) && add(req, node->id, node->curve, value);
	}
	if (op->type == CP_MULTIPLIER) {
		return color_pipeline_fixed(op->multiplier, &value) &&
			   add(req, node->id, node->multiplier, value);
	}
	uint32_t blob = 0;
	if (op->type == CP_MATRIX) {
		struct drm_color_ctm_3x4 matrix = {0};
		for (size_t i = 0; i < 12; i++) {
			uint64_t entry;
			if (!color_pipeline_fixed(op->matrix[i], &entry)) {
				return false;
			}
			matrix.matrix[i] = entry;
		}
		if (drmModeCreatePropertyBlob(fd, &matrix, sizeof(matrix), &blob) != 0) {
			return false;
		}
	} else if (op->type == CP_LUT) {
		struct wlr_color_transform_lut_3x1d *tr = color_transform_lut_3x1d_from_base(op->transform);
		struct drm_color_lut32 *lut = calloc(node->size, sizeof(*lut));
		if (!lut) {
			return false;
		}
		for (size_t i = 0; i < node->size; i++) {
			lut[i].red = lut_resample(tr->lut_3x1d, tr->dim, node->size, i);
			lut[i].green = lut_resample(tr->lut_3x1d + tr->dim, tr->dim, node->size, i);
			lut[i].blue = lut_resample(tr->lut_3x1d + 2 * tr->dim, tr->dim, node->size, i);
		}
		int ret = drmModeCreatePropertyBlob(fd, lut, node->size * sizeof(*lut), &blob);
		free(lut);
		if (ret != 0) {
			return false;
		}
	} else {
		return false;
	}
	pending->blobs[pending->blobs_len++] = blob;
	return add(req, node->id, node->data, blob);
}

bool drm_color_pipeline_reset(struct wlr_drm_connector_state *s, drmModeAtomicReq *req) {
	struct wlr_drm_backend *drm = s->connector->backend;
	for (size_t i = 0; i < drm->num_planes; i++) {
		struct wlr_drm_plane *p = &drm->planes[i];
		struct color_plane *cp = p->color_pipeline;
		if (cp && cp->committed_crtc == s->connector->crtc->id && cp->committed_root) {
			if (!add(req, p->id, cp->property, 0)) {
				return false;
			}
		}
	}
	return true;
}

bool drm_color_pipeline_add(struct wlr_drm_connector_state *s, drmModeAtomicReq *req,
	struct wlr_drm_plane *plane, struct wlr_buffer *buffer) {
	const struct color_pipeline_recipe *recipe = color_pipeline_get(buffer);
	struct wlr_drm_backend *drm = s->connector->backend;
	struct color_plane *cp = plane ? plane->color_pipeline : NULL;
	if (!recipe) {
		return !cp || add(req, plane->id, cp->property, 0);
	}
	if (!drm->color_pipeline_enabled || !cp || drm_connector_is_cursor_visible(s->connector)) {
		return false;
	}
	for (size_t i = 0; i < cp->len; i++) {
		struct color_pipeline *p = &cp->pipelines[i];
		int assignment[MAX_NODES];
		unsigned budget = 4096;
		if (!match(p, recipe, 0, 0, assignment, &budget)) {
			continue;
		}
		struct color_state *pending = wl_array_add(&s->color_pipeline_state, sizeof(*pending));
		if (!pending) {
			return false;
		}
		*pending = (struct color_state){.plane = cp, .root = p->root};
		if (!add(req, plane->id, cp->property, p->root)) {
			return false;
		}
		for (size_t j = 0; j < p->len; j++) {
			const struct color_pipeline_op *op =
				assignment[j] < 0 ? NULL : &recipe->ops[assignment[j]];
			if (!emit(drm->fd, req, &p->nodes[j], op, pending)) {
				return false;
			}
		}
		return true;
	}
	if (!cp->rejection_logged) {
		wlr_log(WLR_DEBUG, "DRM plane %u: no compatible color pipeline; composing", plane->id);
		cp->rejection_logged = true;
	}
	return false;
}

void drm_color_pipeline_state_finish(struct wlr_drm_connector_state *s, bool committed) {
	struct wlr_drm_backend *drm = s->connector->backend;
	uint32_t crtc = s->connector->crtc->id;
	if (committed) {
		for (size_t i = 0; i < drm->num_planes; i++) {
			struct color_plane *cp = drm->planes[i].color_pipeline;
			if (cp && cp->committed_crtc == crtc) {
				cp->committed_root = 0;
			}
		}
	}
	struct color_state *p;
	wl_array_for_each(p, &s->color_pipeline_state) {
		if (committed) {
			if (p->plane->logged_root != p->root) {
				wlr_log(WLR_DEBUG, "DRM fd %d CRTC %u plane %u: color pipeline %u active", drm->fd,
					crtc, p->plane->id, p->root);
			}
			p->plane->logged_root = p->root;
			p->plane->committed_crtc = crtc;
			p->plane->committed_root = p->root;
		}
		/* KMS retains its own reference after a successful atomic commit. */
		for (size_t i = 0; i < p->blobs_len; i++) {
			drmModeDestroyPropertyBlob(drm->fd, p->blobs[i]);
		}
	}
	wl_array_release(&s->color_pipeline_state);
	wl_array_init(&s->color_pipeline_state);
}

#else
void drm_color_pipeline_init(struct wlr_drm_backend *drm) {
	if (getenv("AQUEOUS_DRM_COLOR_PIPELINE")) {
		wlr_log(WLR_INFO, "DRM color pipelines unavailable in build headers");
	}
}
bool drm_color_pipeline_discover(struct wlr_drm_backend *drm, struct wlr_drm_plane *plane) {
	return true;
}
bool drm_color_pipeline_supported(struct wlr_drm_backend *drm) {
	return false;
}
void drm_color_pipeline_finish(struct wlr_drm_plane *plane) {}
bool drm_color_pipeline_reset(struct wlr_drm_connector_state *s, drmModeAtomicReq *req) {
	return true;
}
bool drm_color_pipeline_add(struct wlr_drm_connector_state *s, drmModeAtomicReq *req,
	struct wlr_drm_plane *plane, struct wlr_buffer *buffer) {
	return color_pipeline_get(buffer) == NULL;
}
void drm_color_pipeline_state_finish(struct wlr_drm_connector_state *s, bool committed) {}
#endif
