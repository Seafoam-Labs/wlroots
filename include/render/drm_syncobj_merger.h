#ifndef WLR_RENDER_DRM_SYNCOBJ_MERGER_H
#define WLR_RENDER_DRM_SYNCOBJ_MERGER_H

#include <wayland-server-core.h>

/**
 * Accumulate timeline points, to have a destination timeline point be
 * signalled when all inputs are
 */
struct wlr_drm_syncobj_merger {
	int n_ref;
	struct wlr_drm_syncobj_timeline *dst_timeline;
	uint64_t dst_point;
	int sync_fd;
	unsigned recovery_count; // diagnostic context for synchronous callers
	bool failed; // an input dependency could not be retained or proven complete
};

/**
 * Create a new merger.
 *
 * The given timeline point will be signalled when all input points are
 * signalled and the merger is destroyed.
 */
struct wlr_drm_syncobj_merger *wlr_drm_syncobj_merger_create(
	struct wlr_drm_syncobj_timeline *dst_timeline, uint64_t dst_point);

struct wlr_drm_syncobj_merger *wlr_drm_syncobj_merger_ref(
	struct wlr_drm_syncobj_merger *merger);

/**
 * Target timeline point is materialized when all inputs are, and the merger is
 * destroyed. Publication failures use a bounded completion wait; an unresolved
 * dependency or failed final signal is fatal, never an already-signalled fence.
 */
void wlr_drm_syncobj_merger_unref(struct wlr_drm_syncobj_merger *merger);
/**
 * Add a new timeline point to wait for.
 *
 * If the point is not materialized, the supplied event loop is used to schedule
 * a wait.
 * On failure the merger is poisoned and must not publish a release point.
 */
bool wlr_drm_syncobj_merger_add(struct wlr_drm_syncobj_merger *merger,
	struct wlr_drm_syncobj_timeline *dst_timeline, uint64_t dst_point,
	struct wl_event_loop *loop);

#endif
