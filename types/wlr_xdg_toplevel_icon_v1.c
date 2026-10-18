#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include <wayland-server-protocol.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_shm.h>
#include "render/pixel_format.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>
#include <wlr/util/log.h>

#include "xdg-toplevel-icon-v1-protocol.h"

#define MANAGER_VERSION 1

static const struct xdg_toplevel_icon_v1_interface icon_impl;

static struct wlr_xdg_toplevel_icon_v1 *icon_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_toplevel_icon_v1_interface, &icon_impl));
	return wl_resource_get_user_data(resource);
}

// Limits include unassigned icons and snapshots still held by scene consumers.
#define ICON_BYTES (16u * 1024 * 1024)
#define CLIENT_BYTES (64u * 1024 * 1024)
#define TOTAL_BYTES (256u * 1024 * 1024)
#define ICON_COUNT 256
#define TOTAL_ICONS 4096
#define ICON_VARIANTS 16

struct icon_account {
    struct wl_client *client;
    struct wl_listener destroy;
    struct wl_list link;
    size_t refs, bytes, icons;
};
static struct wl_list accounts = { &accounts, &accounts };
static size_t total_bytes, total_icons;

struct icon_private {
    struct wlr_xdg_toplevel_icon_v1 base;
    struct wl_resource *resource;
    struct icon_account *account;
    size_t bytes, variants;
};
struct icon_pixels {
    struct wlr_buffer base;
    struct icon_account *account;
    size_t size, stride;
    uint32_t format;
    unsigned char data[];
};
struct icon_variant {
    struct wlr_xdg_toplevel_icon_v1_buffer base;
    struct wl_listener destroy;
    struct icon_private *icon;
};

static void account_unref(struct icon_account *account) {
    if (--account->refs != 0) {
        return;
    }
    if (account->client != NULL) {
        wl_list_remove(&account->destroy.link);
    }
    wl_list_remove(&account->link);
    free(account);
}
static void account_client_destroy(struct wl_listener *listener, void *data) {
    struct icon_account *account = wl_container_of(listener, account, destroy);
    wl_list_remove(&account->destroy.link);
    account->client = NULL;
    account_unref(account);
}
static struct icon_account *account_get(struct wl_client *client) {
    struct icon_account *account;
    wl_list_for_each(account, &accounts, link) {
        if (account->client == client) {
            return account;
        }
    }
    account = calloc(1, sizeof(*account));
    if (account == NULL) {
        return NULL;
    }
    account->client = client;
    account->refs = 1;
    account->destroy.notify = account_client_destroy;
    wl_client_add_destroy_listener(client, &account->destroy);
    wl_list_insert(&accounts, &account->link);
    return account;
}
static void pixels_destroy(struct wlr_buffer *buffer) {
    struct icon_pixels *pixels = wl_container_of(buffer, pixels, base);
    pixels->account->bytes -= pixels->size;
    total_bytes -= pixels->size;
    account_unref(pixels->account);
    wlr_buffer_finish(buffer);
    free(pixels);
}
static bool pixels_begin(struct wlr_buffer *buffer, uint32_t flags,
        void **data, uint32_t *format, size_t *stride) {
    struct icon_pixels *pixels = wl_container_of(buffer, pixels, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
        return false;
    }
    *data = pixels->data;
    *format = pixels->format;
    *stride = pixels->stride;
    return true;
}
static void pixels_end(struct wlr_buffer *buffer) {
}
static const struct wlr_buffer_impl pixels_impl = {
    .destroy = pixels_destroy,
    .begin_data_ptr_access = pixels_begin,
    .end_data_ptr_access = pixels_end,
};
static void variant_destroy(struct icon_variant *variant) {
    wl_list_remove(&variant->destroy.link);
    wl_list_remove(&variant->base.link);
    struct icon_pixels *pixels = wl_container_of(variant->base.buffer, pixels, base);
    variant->icon->bytes -= pixels->size;
    variant->icon->variants--;
    wlr_buffer_unlock(variant->base.buffer);
    free(variant);
}
static void variant_resource_destroy(struct wl_listener *listener, void *data) {
    struct icon_variant *variant = wl_container_of(listener, variant, destroy);
    wl_list_remove(&variant->destroy.link);
    wl_list_init(&variant->destroy.link);
    wl_resource_post_error(variant->icon->resource,
        XDG_TOPLEVEL_ICON_V1_ERROR_NO_BUFFER, "icon buffer destroyed before icon");
}
static void icon_destroy(struct wlr_xdg_toplevel_icon_v1 *icon) {
    struct icon_private *priv = wl_container_of(icon, priv, base);
    struct icon_variant *variant, *tmp;
    wl_list_for_each_safe(variant, tmp, &icon->buffers, base.link) {
        variant_destroy(variant);
    }
    priv->account->icons--;
    total_icons--;
    account_unref(priv->account);
    free(icon->name);
    free(priv);
}

static void icon_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void icon_handle_set_name(struct wl_client *client, struct wl_resource *resource,
		const char *name) {
	struct wlr_xdg_toplevel_icon_v1 *icon = icon_from_resource(resource);
	if (icon->immutable) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
			"the icon has already been assigned to a toplevel and must not be changed");
		return;
	}

	if (strlen(name) > 1024) {
        wl_resource_post_no_memory(resource);
        return;
    }
    char *dup = strdup(name);
	if (dup == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	free(icon->name);
	icon->name = dup;
}

static void icon_handle_add_buffer(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *buffer_resource, int32_t scale) {
	struct wlr_xdg_toplevel_icon_v1 *icon = icon_from_resource(resource);
	if (icon->immutable) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
			"the icon has already been assigned to a toplevel and must not be changed");
		return;
	}

    struct icon_private *priv = wl_container_of(icon, priv, base);
    // Borrow the native SHM buffer without locking it: this protocol does not
    // use wl_buffer.release. The request and copy run on the display thread.
    struct wlr_buffer *buffer = wlr_shm_buffer_try_from_resource(buffer_resource);
    if (buffer == NULL || buffer->width != buffer->height) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER,
            "icon requires a square wl_shm buffer");
        return;
    }
    struct wlr_shm_attributes attrs;
    if (!wlr_buffer_get_shm(buffer, &attrs)) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER,
            "icon requires wl_shm");
        return;
    }
    const struct wlr_pixel_format_info *info = drm_get_pixel_format_info(attrs.format);
    int32_t row = info ? pixel_format_info_min_stride(info, buffer->width) : 0;
    if (row <= 0 || buffer->height <= 0 || (size_t)row > ICON_BYTES / buffer->height) {
        wl_resource_post_no_memory(resource);
        return;
    }
    size_t owned_stride = ((size_t)row + 3) & ~(size_t)3;
    if (owned_stride > ICON_BYTES / buffer->height) {
        wl_resource_post_no_memory(resource);
        return;
    }
    size_t size = owned_stride * buffer->height;
    struct icon_variant *old = NULL, *variant;
    wl_list_for_each(variant, &icon->buffers, base.link) {
        if (variant->base.buffer->width == buffer->width && variant->base.scale == scale) {
            old = variant;
            break;
        }
    }
    size_t old_size = 0;
    if (old != NULL) {
        struct icon_pixels *pixels = wl_container_of(old->base.buffer, pixels, base);
        old_size = pixels->size;
    }
    if ((old == NULL && priv->variants >= ICON_VARIANTS) ||
            size > ICON_BYTES - (priv->bytes - old_size) ||
            size > CLIENT_BYTES - priv->account->bytes || size > TOTAL_BYTES - total_bytes) {
        wl_resource_post_no_memory(resource);
        return;
    }
    variant = calloc(1, sizeof(*variant));
    struct icon_pixels *pixels = malloc(sizeof(*pixels) + size);
    if (variant == NULL || pixels == NULL) {
        free(variant);
        free(pixels);
        wl_resource_post_no_memory(resource);
        return;
    }
    void *data;
    uint32_t format;
    size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
            &data, &format, &stride)) {
        free(variant);
        free(pixels);
        wl_resource_post_no_memory(resource);
        return;
    }
    memset(pixels->data, 0, size);
    for (int y = 0; y < buffer->height; y++) {
        memcpy(pixels->data + y * owned_stride, (char *)data + y * stride, row);
    }
    wlr_buffer_end_data_ptr_access(buffer);
    wlr_buffer_init(&pixels->base, &pixels_impl, buffer->width, buffer->height);
    pixels->account = priv->account;
    pixels->account->refs++;
    pixels->account->bytes += size;
    total_bytes += size;
    pixels->size = size;
    pixels->stride = owned_stride;
    pixels->format = format;
    variant->base.buffer = wlr_buffer_lock(&pixels->base);
    wlr_buffer_drop(&pixels->base);
    variant->base.scale = scale;
    variant->icon = priv;
    variant->destroy.notify = variant_resource_destroy;
    wl_resource_add_destroy_listener(buffer_resource, &variant->destroy);
    if (old != NULL) {
        variant_destroy(old);
    }
    priv->bytes += size;
    priv->variants++;
    wl_list_insert(&icon->buffers, &variant->base.link);
}

static const struct xdg_toplevel_icon_v1_interface icon_impl = {
	.destroy = icon_handle_destroy,
	.set_name = icon_handle_set_name,
	.add_buffer = icon_handle_add_buffer,
};

static void icon_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_xdg_toplevel_icon_v1 *icon = icon_from_resource(resource);
    struct icon_private *priv = wl_container_of(icon, priv, base);
    priv->resource = NULL;
    struct icon_variant *variant;
    wl_list_for_each(variant, &icon->buffers, base.link) {
        wl_list_remove(&variant->destroy.link);
        wl_list_init(&variant->destroy.link);
    }
    wlr_xdg_toplevel_icon_v1_unref(icon);
}

struct wlr_xdg_toplevel_icon_v1 *wlr_xdg_toplevel_icon_v1_ref(
		struct wlr_xdg_toplevel_icon_v1 *icon) {
	++icon->n_refs;
	return icon;
}

void wlr_xdg_toplevel_icon_v1_unref(struct wlr_xdg_toplevel_icon_v1 *icon) {
	if (icon == NULL) {
		return;
	}

	assert(icon->n_refs > 0);
	--icon->n_refs;
	if (icon->n_refs == 0) {
		icon_destroy(icon);
	};
}

static const struct xdg_toplevel_icon_manager_v1_interface manager_impl;

static struct wlr_xdg_toplevel_icon_manager_v1 *manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_toplevel_icon_manager_v1_interface,
		&manager_impl));
	return wl_resource_get_user_data(resource);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void manager_handle_create_icon(struct wl_client *client, struct wl_resource *resource,
		uint32_t id) {
	struct icon_account *account = account_get(client);
    if (account == NULL || account->icons >= ICON_COUNT || total_icons >= TOTAL_ICONS) {
        wl_client_post_no_memory(client);
        return;
    }
    struct icon_private *priv = calloc(1, sizeof(*priv));
    struct wlr_xdg_toplevel_icon_v1 *icon = priv ? &priv->base : NULL;
    if (icon == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct wl_resource *icon_resource = wl_resource_create(client,
		&xdg_toplevel_icon_v1_interface, wl_resource_get_version(resource), id);
	if (icon_resource == NULL) {
		wl_client_post_no_memory(client);
		free(icon);
		return;
	}

	wl_list_init(&icon->buffers);
    icon->n_refs = 1;
    priv->resource = icon_resource;
    priv->account = account;
    account->refs++;
    account->icons++;
    total_icons++;

	wl_resource_set_implementation(icon_resource, &icon_impl, icon, icon_handle_resource_destroy);
}

static void manager_handle_set_icon(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *toplevel_resource, struct wl_resource *icon_resource) {
	struct wlr_xdg_toplevel_icon_manager_v1 *manager = manager_from_resource(resource);
	struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);

	struct wlr_xdg_toplevel_icon_v1 *icon = NULL;
	if (icon_resource != NULL) {
		icon = icon_from_resource(icon_resource);
		icon->immutable = true;

		if (icon->name == NULL && wl_list_empty(&icon->buffers)) {
			// Same as supplying null icon
			icon = NULL;
		}
	}

	struct wlr_xdg_toplevel_icon_manager_v1_set_icon_event event = {
		.toplevel = toplevel,
		.icon = icon,
	};

	wl_signal_emit_mutable(&manager->events.set_icon, &event);
}

static const struct xdg_toplevel_icon_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.create_icon = manager_handle_create_icon,
	.set_icon = manager_handle_set_icon,
};

static void manager_send_sizes(struct wlr_xdg_toplevel_icon_manager_v1 *manager,
		struct wl_resource *resource) {
	for (size_t i = 0; i < manager->n_sizes; i++) {
		xdg_toplevel_icon_manager_v1_send_icon_size(resource, manager->sizes[i]);
	}
	xdg_toplevel_icon_manager_v1_send_done(resource);
}

static void manager_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wlr_xdg_toplevel_icon_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&xdg_toplevel_icon_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, manager_handle_resource_destroy);

	wl_list_insert(&manager->resources, wl_resource_get_link(resource));

	manager_send_sizes(manager, resource);
}

static void manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_icon_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);

	wl_signal_emit_mutable(&manager->events.destroy, NULL);

	assert(wl_list_empty(&manager->events.set_icon.listener_list));
	assert(wl_list_empty(&manager->events.destroy.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);

	wl_list_remove(&manager->resources);

	free(manager->sizes);
	free(manager);
}

void wlr_xdg_toplevel_icon_manager_v1_set_sizes(struct wlr_xdg_toplevel_icon_manager_v1 *manager,
		int *sizes, size_t n_sizes) {
	if (n_sizes != manager->n_sizes) {
		int *dup_sizes = NULL;
		if (n_sizes > 0) {
			dup_sizes = calloc(n_sizes, sizeof(*dup_sizes));
			if (dup_sizes == NULL) {
				wlr_log(WLR_ERROR, "Allocation failed");
				return;
			}
		}

		free(manager->sizes);
		manager->sizes = dup_sizes;
		manager->n_sizes = n_sizes;
	}

	for (size_t i = 0; i < n_sizes; i++) {
		manager->sizes[i] = sizes[i];
	}

	struct wl_resource *resource;
	wl_resource_for_each(resource, &manager->resources) {
		manager_send_sizes(manager, resource);
	}
}

struct wlr_xdg_toplevel_icon_manager_v1 *wlr_xdg_toplevel_icon_manager_v1_create(
			struct wl_display *display, uint32_t version) {
	assert(version <= MANAGER_VERSION);

	struct wlr_xdg_toplevel_icon_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display, &xdg_toplevel_icon_manager_v1_interface,
		version, manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	wl_signal_init(&manager->events.set_icon);
	wl_signal_init(&manager->events.destroy);

	wl_list_init(&manager->resources);

	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
