#ifndef BACKEND_DRM_COLOR_PIPELINE_H
#define BACKEND_DRM_COLOR_PIPELINE_H

#include <xf86drmMode.h>
struct wlr_drm_backend;
struct wlr_drm_plane;
struct wlr_drm_connector_state;
struct wlr_buffer;

void drm_color_pipeline_init(struct wlr_drm_backend *drm);
bool drm_color_pipeline_discover(struct wlr_drm_backend *drm, struct wlr_drm_plane *plane);
bool drm_color_pipeline_supported(struct wlr_drm_backend *drm);
void drm_color_pipeline_finish(struct wlr_drm_plane *plane);
bool drm_color_pipeline_reset(struct wlr_drm_connector_state *state, drmModeAtomicReq *req);
bool drm_color_pipeline_add(struct wlr_drm_connector_state *state, drmModeAtomicReq *req,
	struct wlr_drm_plane *plane, struct wlr_buffer *buffer);
void drm_color_pipeline_state_finish(struct wlr_drm_connector_state *state, bool committed);

#endif
