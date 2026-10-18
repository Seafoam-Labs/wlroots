#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif
#ifndef WLR_TYPES_XDG_TOPLEVEL_DRAG_V1_H
#define WLR_TYPES_XDG_TOPLEVEL_DRAG_V1_H
#include <wayland-server-core.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_xdg_shell.h>

struct wlr_xdg_toplevel_drag_v1 {
	struct wl_resource *resource;
	struct wlr_data_source *source;
	struct wlr_xdg_toplevel *toplevel;
	int32_t x_offset, y_offset;
	bool active, ended;
	struct {
		struct wl_signal change; // NULL; attachment, map, start or end
		struct wl_signal destroy; // NULL
	} events;
	struct {
		struct wl_listener source_destroy;
		struct wl_resource *manager_resource;
		struct wl_listener manager_destroy;
		struct wl_listener toplevel_destroy, unmap, map;
		struct wl_listener drag_destroy, drag_drop;
	} WLR_PRIVATE;
};

struct wl_global *wlr_xdg_toplevel_drag_manager_v1_create(struct wl_display *display);
struct wlr_xdg_toplevel_drag_v1 *wlr_xdg_toplevel_drag_v1_try_from_data_source(
	struct wlr_data_source *source);
#endif
