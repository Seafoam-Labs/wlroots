#include <assert.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/util/log.h>
#include "text-input-unstable-v3-protocol.h"

/* Private extension preserves the public layout consumed by zig-wlroots. */
struct text_input {
	struct wlr_text_input_v3 base;
	uint32_t pending_actions, current_actions;
	bool panel_visible;
	bool has_preedit;
	size_t preedit_len;
};

static struct text_input *private_text_input(struct wlr_text_input_v3 *base) {
	struct text_input *input = wl_container_of(base, input, base);
	return input;
}

uint32_t wlr_text_input_v3_get_available_actions(struct wlr_text_input_v3 *base) {
	return private_text_input(base)->current_actions;
}

bool wlr_text_input_v3_get_panel_requested(struct wlr_text_input_v3 *base) {
	return private_text_input(base)->panel_visible;
}

void wlr_text_input_v3_send_action(struct wlr_text_input_v3 *base,
		uint32_t action, uint32_t serial) {
	if (wl_resource_get_version(base->resource) >= 2 && base->current_enabled &&
			base->focused_surface && action == ZWP_TEXT_INPUT_V3_ACTION_SUBMIT &&
			(private_text_input(base)->current_actions & (1u << action))) {
		zwp_text_input_v3_send_action(base->resource, action, serial);
	}
}

void wlr_text_input_v3_send_language(struct wlr_text_input_v3 *base, const char *language) {
	if (wl_resource_get_version(base->resource) >= 2) {
		zwp_text_input_v3_send_language(base->resource, language);
	}
}

void wlr_text_input_v3_send_preedit_hint(struct wlr_text_input_v3 *base,
		uint32_t start, uint32_t end, uint32_t hint) {
	struct text_input *input = private_text_input(base);
	if (wl_resource_get_version(base->resource) >= 2 && input->has_preedit &&
			start <= end && end <= input->preedit_len &&
			hint >= ZWP_TEXT_INPUT_V3_PREEDIT_HINT_WHOLE &&
			hint <= ZWP_TEXT_INPUT_V3_PREEDIT_HINT_COMPOSE_ERROR) {
		zwp_text_input_v3_send_preedit_hint(base->resource, start, end, hint);
	}
}

static void text_input_clear_focused_surface(struct wlr_text_input_v3 *text_input) {
	wl_list_remove(&text_input->surface_destroy.link);
	wl_list_init(&text_input->surface_destroy.link);
	text_input->focused_surface = NULL;
	struct text_input *input = private_text_input(text_input);
	input->pending_actions = input->current_actions = 0;
	input->panel_visible = input->has_preedit = false;
}

static const struct zwp_text_input_v3_interface text_input_impl;

static struct wlr_text_input_v3 *text_input_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_text_input_v3_interface,
		&text_input_impl));
	return wl_resource_get_user_data(resource);
}

void wlr_text_input_v3_send_enter(struct wlr_text_input_v3 *text_input,
		struct wlr_surface *surface) {
	assert(wl_resource_get_client(text_input->resource)
		== wl_resource_get_client(surface->resource));
	assert(text_input->focused_surface == NULL);
	struct text_input *input = private_text_input(text_input);
	input->pending_actions = input->current_actions = 0;
	input->panel_visible = false;
	text_input->focused_surface = surface;
	wl_signal_add(&text_input->focused_surface->events.destroy,
		&text_input->surface_destroy);
	zwp_text_input_v3_send_enter(text_input->resource,
		text_input->focused_surface->resource);
}

void wlr_text_input_v3_send_leave(struct wlr_text_input_v3 *text_input) {
	assert(text_input->focused_surface != NULL);
	zwp_text_input_v3_send_leave(text_input->resource,
		text_input->focused_surface->resource);
	text_input_clear_focused_surface(text_input);
}

void wlr_text_input_v3_send_preedit_string(struct wlr_text_input_v3 *text_input,
		const char *text, int32_t cursor_begin, int32_t cursor_end) {
	zwp_text_input_v3_send_preedit_string(text_input->resource, text,
		cursor_begin, cursor_end);
	struct text_input *input = private_text_input(text_input);
	input->has_preedit = text != NULL;
	input->preedit_len = text ? strlen(text) : 0;
	if (text_input->current.content_type.hint & ZWP_TEXT_INPUT_V3_CONTENT_HINT_PREEDIT_SHOWN) {
		wlr_text_input_v3_send_preedit_hint(text_input, 0, input->preedit_len,
			ZWP_TEXT_INPUT_V3_PREEDIT_HINT_WHOLE);
	}
}

void wlr_text_input_v3_send_commit_string(struct wlr_text_input_v3 *text_input,
		const char *text) {
	zwp_text_input_v3_send_commit_string(text_input->resource, text);
}

void wlr_text_input_v3_send_delete_surrounding_text(
		struct wlr_text_input_v3 *text_input, uint32_t before_length,
		uint32_t after_length) {
	zwp_text_input_v3_send_delete_surrounding_text(text_input->resource,
		before_length, after_length);
}

void wlr_text_input_v3_send_done(struct wlr_text_input_v3 *text_input) {
	zwp_text_input_v3_send_done(text_input->resource,
		text_input->current_serial);
	private_text_input(text_input)->has_preedit = false;
}

static void wlr_text_input_destroy(struct wlr_text_input_v3 *text_input) {
	wl_signal_emit_mutable(&text_input->events.destroy, NULL);

	assert(wl_list_empty(&text_input->events.enable.listener_list));
	assert(wl_list_empty(&text_input->events.commit.listener_list));
	assert(wl_list_empty(&text_input->events.disable.listener_list));
	assert(wl_list_empty(&text_input->events.destroy.listener_list));

	text_input_clear_focused_surface(text_input);
	wl_list_remove(&text_input->seat_destroy.link);
	// remove from manager.text_inputs
	wl_list_remove(&text_input->link);
	free(text_input->current.surrounding.text);
	free(text_input->pending.surrounding.text);
	free(text_input);
}

static void text_input_resource_destroy(struct wl_resource *resource) {
	struct wlr_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}
	wlr_text_input_destroy(text_input);
}

static void text_input_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void text_input_enable(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}
	struct wlr_text_input_v3_state defaults = {0};
	struct text_input *input = private_text_input(text_input);
	input->pending_actions = 0;
	input->has_preedit = false;
	input->panel_visible = false;
	free(text_input->pending.surrounding.text);
	text_input->pending = defaults;
	text_input->pending_enabled = true;
}

static void text_input_disable(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}
	text_input->pending_enabled = false;
	private_text_input(text_input)->pending_actions = 0;
}

static void text_input_set_surrounding_text(struct wl_client *client,
		struct wl_resource *resource, const char *text, int32_t cursor,
		int32_t anchor) {
	struct wlr_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}
	free(text_input->pending.surrounding.text);
	text_input->pending.surrounding.text = strdup(text);
	if (!text_input->pending.surrounding.text) {
		wl_client_post_no_memory(client);
	}
	text_input->pending.features |= WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT;
	text_input->pending.surrounding.cursor = cursor;
	text_input->pending.surrounding.anchor = anchor;
}

static void text_input_set_text_change_cause(struct wl_client *client,
		struct wl_resource *resource, uint32_t cause) {
	struct wlr_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}
	text_input->pending.text_change_cause = cause;
}

static void text_input_set_content_type(struct wl_client *client,
		struct wl_resource *resource, uint32_t hint, uint32_t purpose) {
	struct wlr_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}
	text_input->pending.features |= WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE;
	text_input->pending.content_type.hint = hint;
	text_input->pending.content_type.purpose = purpose;
}

static void text_input_set_cursor_rectangle(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y, int32_t width,
		int32_t height) {
	struct wlr_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}
	text_input->pending.features |= WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE;
	text_input->pending.cursor_rectangle.x = x;
	text_input->pending.cursor_rectangle.y = y;
	text_input->pending.cursor_rectangle.width = width;
	text_input->pending.cursor_rectangle.height = height;
}

static void text_input_commit(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}
	free(text_input->current.surrounding.text);
	text_input->current = text_input->pending;
	if (text_input->pending.surrounding.text) {
		text_input->current.surrounding.text =
			strdup(text_input->pending.surrounding.text);
		if (text_input->current.surrounding.text == NULL) {
			wl_client_post_no_memory(client);
			return;
		}
	}

	bool old_enabled = text_input->current_enabled;
	text_input->current_enabled = text_input->pending_enabled;
	text_input->current_serial++;
	struct text_input *input = private_text_input(text_input);
	input->current_actions = text_input->current_enabled ? input->pending_actions : 0;
	if (!text_input->current_enabled) {
		input->panel_visible = false;
		input->has_preedit = false;
	}

	if (text_input->focused_surface == NULL) {
		wlr_log(WLR_DEBUG, "Text input commit received without focus");
	}

	if (!old_enabled && text_input->current_enabled) {
		text_input->active_features	= text_input->current.features;
		wl_signal_emit_mutable(&text_input->events.enable, NULL);
	} else if (old_enabled && !text_input->current_enabled) {
		text_input->active_features	= 0;
		wl_signal_emit_mutable(&text_input->events.disable, NULL);
	} else { // including never enabled
		wl_signal_emit_mutable(&text_input->events.commit, NULL);
	}
}

static void text_input_set_available_actions(struct wl_client *client,
		struct wl_resource *resource, struct wl_array *actions) {
	struct wlr_text_input_v3 *base = text_input_from_resource(resource);
	if (!base) {
		return;
	}
	uint32_t mask = 0;
	if (actions->size % sizeof(uint32_t) != 0) {
		goto invalid;
	}
	uint32_t *action;
	wl_array_for_each(action, actions) {
		if (*action != ZWP_TEXT_INPUT_V3_ACTION_SUBMIT || (mask & (1u << *action))) {
			goto invalid;
		}
		mask |= 1u << *action;
	}
	private_text_input(base)->pending_actions = mask;
	return;
invalid:
	wl_resource_post_error(resource, ZWP_TEXT_INPUT_V3_ERROR_INVALID_ACTION,
		"invalid or duplicate available action");
}

/* Panel visibility is a hint, not an enable/disable request. The compositor's
 * existing input-method-v2 backend has no panel visibility request; remember
 * the focused hint for providers which can consume it without changing focus. */
static void text_input_show_panel(struct wl_client *client, struct wl_resource *resource) {
	struct wlr_text_input_v3 *base = text_input_from_resource(resource);
	if (base && base->current_enabled && base->focused_surface) {
		private_text_input(base)->panel_visible = true;
	}
}

static void text_input_hide_panel(struct wl_client *client, struct wl_resource *resource) {
	struct wlr_text_input_v3 *base = text_input_from_resource(resource);
	if (base && base->focused_surface) {
		private_text_input(base)->panel_visible = false;
	}
}

static const struct zwp_text_input_v3_interface text_input_impl = {
	.destroy = text_input_destroy,
	.enable = text_input_enable,
	.disable = text_input_disable,
	.set_surrounding_text = text_input_set_surrounding_text,
	.set_text_change_cause = text_input_set_text_change_cause,
	.set_content_type = text_input_set_content_type,
	.set_cursor_rectangle = text_input_set_cursor_rectangle,
	.commit = text_input_commit,
	.set_available_actions = text_input_set_available_actions,
	.show_input_panel = text_input_show_panel,
	.hide_input_panel = text_input_hide_panel,
};

static const struct zwp_text_input_manager_v3_interface text_input_manager_impl;

static struct wlr_text_input_manager_v3 *text_input_manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwp_text_input_manager_v3_interface, &text_input_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void text_input_manager_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void text_input_handle_seat_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_text_input_v3 *text_input = wl_container_of(listener, text_input,
		seat_destroy);
	struct wl_resource *resource = text_input->resource;
	wlr_text_input_destroy(text_input);
	wl_resource_set_user_data(resource, NULL);
}

static void text_input_handle_focused_surface_destroy(
		struct wl_listener *listener, void *data) {
	struct wlr_text_input_v3 *text_input = wl_container_of(listener, text_input,
		surface_destroy);
	text_input_clear_focused_surface(text_input);
}

static void text_input_manager_get_text_input(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *seat) {
	int version = wl_resource_get_version(resource);
	struct wl_resource *text_input_resource = wl_resource_create(client,
		&zwp_text_input_v3_interface, version, id);
	if (text_input_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(text_input_resource, &text_input_impl,
		NULL, text_input_resource_destroy);

	struct wlr_seat_client *seat_client = wlr_seat_client_from_resource(seat);
	if (seat_client == NULL) {
		return;
	}

	struct wlr_text_input_v3 *text_input = calloc(1, sizeof(struct text_input));
	if (text_input == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_signal_init(&text_input->events.enable);
	wl_signal_init(&text_input->events.commit);
	wl_signal_init(&text_input->events.disable);
	wl_signal_init(&text_input->events.destroy);

	text_input->resource = text_input_resource;
	wl_resource_set_user_data(text_input_resource, text_input);

	struct wlr_seat *wlr_seat = seat_client->seat;
	text_input->seat = wlr_seat;
	wl_signal_add(&seat_client->events.destroy,
		&text_input->seat_destroy);
	text_input->seat_destroy.notify =
		text_input_handle_seat_destroy;
	text_input->surface_destroy.notify =
		text_input_handle_focused_surface_destroy;
	wl_list_init(&text_input->surface_destroy.link);

	struct wlr_text_input_manager_v3 *manager =
		text_input_manager_from_resource(resource);
	wl_list_insert(&manager->text_inputs, &text_input->link);

	wl_signal_emit_mutable(&manager->events.new_text_input, text_input);
}

static const struct zwp_text_input_manager_v3_interface
		text_input_manager_impl = {
	.destroy = text_input_manager_destroy,
	.get_text_input = text_input_manager_get_text_input,
};

static void text_input_manager_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_text_input_manager_v3 *manager = data;

	struct wl_resource *resource = wl_resource_create(wl_client,
		&zwp_text_input_manager_v3_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &text_input_manager_impl,
		manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_text_input_manager_v3 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_signal_emit_mutable(&manager->events.destroy, NULL);

	assert(wl_list_empty(&manager->events.new_text_input.listener_list));
	assert(wl_list_empty(&manager->events.destroy.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_text_input_manager_v3 *wlr_text_input_manager_v3_create(
		struct wl_display *display) {
	struct wlr_text_input_manager_v3 *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}

	wl_list_init(&manager->text_inputs);

	wl_signal_init(&manager->events.new_text_input);
	wl_signal_init(&manager->events.destroy);

	manager->global = wl_global_create(display,
		&zwp_text_input_manager_v3_interface, 2, manager,
		text_input_manager_bind);
	if (!manager->global) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
