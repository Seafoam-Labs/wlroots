#ifndef WLR_TYPES_WLR_COMMIT_TIMING_V1_H
#define WLR_TYPES_WLR_COMMIT_TIMING_V1_H
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif
#include <wayland-server-core.h>
#define WLR_AQUEOUS_COMMIT_TIMING_VERSION 1
struct wlr_commit_timing_manager_v1;
/* Uses CLOCK_MONOTONIC, as advertised by the pinned presentation-time manager.
 * Surface state is applied from the event loop, before any scene construction. */
struct wlr_commit_timing_manager_v1 *wlr_commit_timing_manager_v1_create(struct wl_display *display);
struct wl_global *wlr_commit_timing_manager_v1_get_global(struct wlr_commit_timing_manager_v1 *manager);
#endif
