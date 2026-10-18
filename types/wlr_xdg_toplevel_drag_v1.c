#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_toplevel_drag_v1.h>
#include "types/wlr_data_device.h"
#include "xdg-toplevel-drag-v1-protocol.h"

struct manager {
	struct wl_global *global;
	struct wl_listener display_destroy;
};

static void handle_manager_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_container_of(listener, drag, manager_destroy);
	wl_list_remove(&drag->manager_destroy.link);
	wl_list_init(&drag->manager_destroy.link);
	drag->manager_resource = NULL;
}

bool xdg_toplevel_drag_check_selection(struct wlr_client_data_source *source,
		struct wl_resource *device) {
	if (!source->toplevel_drag_used) {
		return true;
	}
	struct wlr_xdg_toplevel_drag_v1 *drag =
		wlr_xdg_toplevel_drag_v1_try_from_data_source(&source->source);
	if (drag && drag->manager_resource) {
		wl_resource_post_error(drag->manager_resource,
			XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE,
			"toplevel drag source cannot be used for selection");
	} else {
		// The factory may have been destroyed; report misuse on the live device.
		wl_resource_post_error(device, WL_DATA_DEVICE_ERROR_USED_SOURCE,
			"toplevel drag source cannot be used for selection");
	}
	return false;
}

static void changed(struct wlr_xdg_toplevel_drag_v1 *drag) {
	wl_signal_emit_mutable(&drag->events.change, NULL);
}

static void detach(struct wlr_xdg_toplevel_drag_v1 *drag) {
	if (!drag->toplevel) {
		return;
	}
	wl_list_remove(&drag->toplevel_destroy.link);
	wl_list_remove(&drag->unmap.link);
	wl_list_remove(&drag->map.link);
	drag->toplevel = NULL;
	changed(drag);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_container_of(listener, drag, unmap);
	detach(drag);
}

static void handle_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_container_of(listener, drag, toplevel_destroy);
	detach(drag);
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_container_of(listener, drag, map);
	changed(drag);
}

static void end(struct wlr_xdg_toplevel_drag_v1 *drag) {
	drag->active = false;
	drag->ended = true;
	changed(drag);
}

static void handle_source_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_container_of(listener, drag, source_destroy);
	wl_list_remove(&drag->source_destroy.link);
	wl_list_init(&drag->source_destroy.link);
	drag->source = NULL;
	end(drag);
}

struct wlr_xdg_toplevel_drag_v1 *wlr_xdg_toplevel_drag_v1_try_from_data_source(
		struct wlr_data_source *source) {
	if (!source) {
		return NULL;
	}
	struct wl_listener *listener = wl_signal_get(&source->events.destroy, handle_source_destroy);
	if (!listener) {
		return NULL;
	}
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_container_of(listener, drag, source_destroy);
	return drag;
}

static void handle_drag_drop(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_container_of(listener, drag, drag_drop);
	end(drag);
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_container_of(listener, drag, drag_destroy);
	wl_list_remove(&drag->drag_destroy.link);
	wl_list_init(&drag->drag_destroy.link);
	wl_list_remove(&drag->drag_drop.link);
	wl_list_init(&drag->drag_drop.link);
	end(drag);
}

void xdg_toplevel_drag_start(struct wlr_drag *wlr_drag) {
	struct wlr_xdg_toplevel_drag_v1 *drag =
		wlr_xdg_toplevel_drag_v1_try_from_data_source(wlr_drag->source);
	if (!drag || drag->ended || drag->active) {
		return;
	}
	drag->active = true;
	drag->drag_destroy.notify = handle_drag_destroy;
	wl_signal_add(&wlr_drag->events.destroy, &drag->drag_destroy);
	drag->drag_drop.notify = handle_drag_drop;
	wl_signal_add(&wlr_drag->events.drop, &drag->drag_drop);
	changed(drag);
}

static void destroy_resource(struct wl_resource *resource) {
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_resource_get_user_data(resource);
	wl_signal_emit_mutable(&drag->events.destroy, NULL);
	// Do not emit change after the compositor has removed its listeners.
	if (drag->toplevel) {
		wl_list_remove(&drag->toplevel_destroy.link);
		wl_list_remove(&drag->unmap.link);
		wl_list_remove(&drag->map.link);
	}
	wl_list_remove(&drag->manager_destroy.link);
	wl_list_remove(&drag->source_destroy.link);
	wl_list_remove(&drag->drag_destroy.link);
	wl_list_remove(&drag->drag_drop.link);
	free(drag);
}

static void destroy(struct wl_client *client, struct wl_resource *resource) {
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_resource_get_user_data(resource);
	if (!drag->ended) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG,
			"the data source drag has not ended");
		return;
	}
	wl_resource_destroy(resource);
}

static void attach(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *top_resource, int32_t x, int32_t y) {
	struct wlr_xdg_toplevel_drag_v1 *drag = wl_resource_get_user_data(resource);
	if (drag->toplevel) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED,
			"a toplevel is already attached");
		return;
	}
	struct wlr_xdg_toplevel *top = wlr_xdg_toplevel_from_resource(top_resource);
	if (!top || drag->ended) {
		return;
	}
	drag->toplevel = top;
	drag->x_offset = x;
	drag->y_offset = y;
	drag->toplevel_destroy.notify = handle_toplevel_destroy;
	wl_signal_add(&top->events.destroy, &drag->toplevel_destroy);
	drag->unmap.notify = handle_unmap;
	wl_signal_add(&top->base->surface->events.unmap, &drag->unmap);
	drag->map.notify = handle_map;
	wl_signal_add(&top->base->surface->events.map, &drag->map);
	changed(drag);
}

static const struct xdg_toplevel_drag_v1_interface drag_impl = {
	.destroy = destroy,
	.attach = attach,
};

static void get_drag(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *source_resource) {
	struct wlr_client_data_source *source = client_data_source_from_resource(source_resource);
	if (!source || source->finalized || source->toplevel_drag_used) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE,
			"source already used");
		return;
	}
	struct wlr_xdg_toplevel_drag_v1 *drag = calloc(1, sizeof(*drag));
	if (!drag) {
		wl_resource_post_no_memory(resource);
		return;
	}
	drag->resource = wl_resource_create(client, &xdg_toplevel_drag_v1_interface, 1, id);
	if (!drag->resource) {
		free(drag);
		wl_resource_post_no_memory(resource);
		return;
	}
	wl_resource_set_implementation(drag->resource, &drag_impl, drag, destroy_resource);
	drag->manager_resource = resource;
	drag->manager_destroy.notify = handle_manager_destroy;
	wl_resource_add_destroy_listener(resource, &drag->manager_destroy);
	source->toplevel_drag_used = true;
	drag->source = &source->source;
	wl_signal_init(&drag->events.change);
	wl_signal_init(&drag->events.destroy);
	wl_list_init(&drag->drag_destroy.link);
	wl_list_init(&drag->drag_drop.link);
	drag->source_destroy.notify = handle_source_destroy;
	wl_signal_add(&source->source.events.destroy, &drag->source_destroy);
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct xdg_toplevel_drag_manager_v1_interface manager_impl = {
	.destroy = manager_destroy,
	.get_xdg_toplevel_drag = get_drag,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&xdg_toplevel_drag_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void display_destroy(struct wl_listener *listener, void *data) {
	struct manager *manager = wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wl_global *wlr_xdg_toplevel_drag_manager_v1_create(struct wl_display *display) {
	struct manager *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}
	manager->global = wl_global_create(display, &xdg_toplevel_drag_manager_v1_interface,
		1, manager, bind_manager);
	if (!manager->global) {
		free(manager);
		return NULL;
	}
	manager->display_destroy.notify = display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);
	return manager->global;
}
