#ifndef WLR_TYPES_WLR_FIFO_V1_H
#define WLR_TYPES_WLR_FIFO_V1_H
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#define WLR_AQUEOUS_FIFO_VERSION 1
struct wlr_fifo_manager_v1;
struct wlr_surface;

struct wlr_fifo_manager_v1 *wlr_fifo_manager_v1_create(struct wl_display *display);
struct wl_global *wlr_fifo_manager_v1_get_global(struct wlr_fifo_manager_v1 *manager);
/* Whether this output must submit a non-tearing frame even without damage. */
bool wlr_fifo_manager_v1_output_pending(struct wlr_fifo_manager_v1 *manager,
    struct wlr_output *output);
/* Call before building a client scene frame, then finish after the commit result.
 * Never prepare mirror/capture-only frames. Finish precedes replay of synchronous
 * presentation events. A failed build/commit cannot satisfy a barrier. */
void wlr_fifo_manager_v1_prepare(struct wlr_fifo_manager_v1 *manager,
    struct wlr_output *output);
void wlr_fifo_manager_v1_finish(struct wlr_fifo_manager_v1 *manager,
    struct wlr_output *output, bool committed);
/* Returns true when a rejected submission needs the compositor retry path. */
bool wlr_fifo_manager_v1_present(struct wlr_fifo_manager_v1 *manager,
    struct wlr_output *output, const struct wlr_output_event_present *event);
/* Defer visibility reevaluation, safe inside scene/commit/destruction listeners. */
void wlr_fifo_v1_surface_changed(struct wlr_surface *surface);
#endif
