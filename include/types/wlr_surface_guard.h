#ifndef TYPES_WLR_SURFACE_GUARD_H
#define TYPES_WLR_SURFACE_GUARD_H
#include <stdbool.h>
#include <wlr/types/wlr_compositor.h>
/* Stack-owned lifetime guard for callbacks which may destroy the surface. */
struct surface_guard {
	struct wlr_addon addon;
	bool alive;
};
static void surface_guard_destroy(struct wlr_addon *addon) {
	struct surface_guard *guard = wl_container_of(addon, guard, addon);
	guard->alive = false;
	wlr_addon_finish(addon);
}
static const struct wlr_addon_interface surface_guard_impl = {
	.name = "surface_commit_guard", .destroy = surface_guard_destroy,
};
static inline void surface_guard_init(struct surface_guard *guard, struct wlr_surface *surface) {
	guard->alive = true;
	wlr_addon_init(&guard->addon, &surface->addons, guard, &surface_guard_impl);
}
#endif
