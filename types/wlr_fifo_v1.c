/* FIFO state belongs to the surface, not the lifetime of the protocol resource. */
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/util/log.h>
#include <wlr/backend/drm.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fifo_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include "fifo-v1-protocol.h"
#include "types/wlr_fifo_v1.h"
#include "types/wlr_scene.h"

struct wlr_fifo_manager_v1 {
    struct wl_global *global;
    struct wl_event_loop *loop;
    struct wl_listener display_destroy;
    struct wl_list surfaces;
    struct wl_list hidden;
    struct wl_event_source *timer;
    bool timer_dispatching, timer_failed;
    int64_t armed_deadline;
};
struct fifo_state { bool set, wait; };
struct fifo_surface {
    struct wlr_addon addon;
    struct wlr_surface_synced synced;
    struct fifo_state pending, current;
    struct wl_resource *resource;
    struct wlr_fifo_manager_v1 *manager;
    struct wl_list link;
    struct wl_event_source *idle;
    struct wlr_output *output;
    struct wl_listener output_destroy, output_commit;
    uint64_t generation, staged, submitted;
    uint32_t submitted_seq;
    bool barrier;
    struct wl_list hidden_link;
    uint64_t hidden_generation;
    int64_t hidden_deadline;
};
static const struct wlr_addon_interface addon_impl;
static const struct wp_fifo_v1_interface fifo_impl;

static struct fifo_surface *get_surface(struct wlr_surface *surface) {
    struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL, &addon_impl);
    if (!addon) return NULL;
    struct fifo_surface *fifo = wl_container_of(addon, fifo, addon);
    return fifo;
}
static bool synchronized(struct wlr_surface *surface) {
    struct wlr_subsurface *sub;
    while ((sub = wlr_subsurface_try_from_wlr_surface(surface))) {
        if (sub->synchronized) return true;
        surface = sub->parent;
    }
    return false;
}
/* Match scene/surface.c's frame-pacing output, including suspended visibility. */
static struct wlr_output *pacing_output(struct wlr_surface *surface) {
    struct wlr_output *preview = scene_surface_preview_output(surface);
    if (preview != NULL && preview->enabled) return preview;
    struct wlr_output *output = NULL;
    struct wlr_surface_output *entered;
    wl_list_for_each(entered, &surface->current_outputs, link) {
        if (!entered->suspended && entered->output->enabled &&
                (!output || entered->output->refresh > output->refresh)) {
            output = entered->output;
        }
    }
    return output;
}
static void schedule(struct fifo_surface *fifo);
static void arm_timer(struct wlr_fifo_manager_v1 *manager);
static int64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
static void remove_hidden(struct fifo_surface *fifo) {
    if (!fifo->hidden_generation) return;
    wl_list_remove(&fifo->hidden_link);
    wl_list_init(&fifo->hidden_link);
    fifo->hidden_generation = 0;
    arm_timer(fifo->manager);
}
static void arm_timer(struct wlr_fifo_manager_v1 *manager) {
    if (manager->timer_dispatching || manager->timer_failed) return;
    int64_t deadline = 0;
    struct fifo_surface *fifo;
    wl_list_for_each(fifo, &manager->hidden, hidden_link) {
        if (!deadline || fifo->hidden_deadline < deadline) deadline = fifo->hidden_deadline;
    }
    if (deadline == manager->armed_deadline) return;
    // Wayland uses milliseconds; round up and never use 0 for an overdue timer
    // (0 disarms it). Recheck the monotonic deadline in the callback.
    int64_t remaining = deadline ? deadline - now_ns() : 0;
    int delay = deadline ? (remaining > 0 ? (remaining + 999999) / 1000000 : 1) : 0;
    if (wl_event_source_timer_update(manager->timer, delay) == 0) {
        manager->armed_deadline = deadline;
        return;
    }
    // Fail open only for hidden FIFO, without flushing from a readiness check
    // or list mutation. Idles perform the flush and preserve independent locks.
    wlr_log(WLR_ERROR, "Hidden FIFO timer failed; disabling hidden FIFO pacing");
    manager->timer_failed = true;
    wl_list_for_each(fifo, &manager->hidden, hidden_link) schedule(fifo);
}
static void pace_hidden(struct fifo_surface *fifo) {
    if (!fifo->barrier || fifo->manager->timer_failed) return;
    if (fifo->hidden_generation == fifo->generation) return;
    remove_hidden(fifo);
    fifo->hidden_generation = fifo->generation;
    fifo->hidden_deadline = now_ns() + 25000000; // 40 Hz, with no catch-up bursts.
    wl_list_insert(fifo->manager->hidden.prev, &fifo->hidden_link);
    arm_timer(fifo->manager);
}
static void disconnect_output(struct fifo_surface *fifo) {
    if (!fifo->output) return;
    wl_list_remove(&fifo->output_destroy.link);
    wl_list_remove(&fifo->output_commit.link);
    fifo->output = NULL;
    fifo->staged = fifo->submitted = 0;
}
static void output_destroy(struct wl_listener *listener, void *data) {
    struct fifo_surface *fifo = wl_container_of(listener, fifo, output_destroy);
    disconnect_output(fifo);
    schedule(fifo);
}
static void output_commit(struct wl_listener *listener, void *data) {
    struct fifo_surface *fifo = wl_container_of(listener, fifo, output_commit);
    struct wlr_output_event_commit *event = data;
    if (event->state->committed & (WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE)) {
        fifo->submitted = 0;
        schedule(fifo);
    }
}
static void update_output(struct fifo_surface *fifo) {
    struct wlr_output *output = pacing_output(fifo->synced.surface);
    if (output) remove_hidden(fifo);
    if (output == fifo->output) return;
    disconnect_output(fifo);
    fifo->output = output;
    if (!output) return;
    fifo->output_destroy.notify = output_destroy;
    fifo->output_commit.notify = output_commit;
    wl_signal_add(&output->events.destroy, &fifo->output_destroy);
    wl_signal_add(&output->events.commit, &fifo->output_commit);
}
static void clear_barrier(struct fifo_surface *fifo) {
    remove_hidden(fifo);
    fifo->barrier = false;
    fifo->staged = fifo->submitted = 0;
}
static void idle(void *data) {
    struct fifo_surface *fifo = data;
    fifo->idle = NULL;
    update_output(fifo);
    if (synchronized(fifo->synced.surface) ||
            (!fifo->output && fifo->manager->timer_failed)) {
        clear_barrier(fifo);
        // Flushing can invoke compositor code and destroy this addon.
        surface_flush_cached(fifo->synced.surface);
        return;
    }
    if (!fifo->output) {
        pace_hidden(fifo);
        return;
    }
    if (fifo->barrier && !fifo->submitted) wlr_output_schedule_frame(fifo->output);
}
static int hidden_timer(void *data) {
    struct wlr_fifo_manager_v1 *manager = data;
    manager->armed_deadline = 0;
    manager->timer_dispatching = true;
    // A fixed cutoff prevents callbacks which take a long time from releasing
    // newly installed barriers in this same dispatch.
    int64_t now = now_ns();
    for (;;) {
        struct fifo_surface *fifo, *ready = NULL;
        wl_list_for_each(fifo, &manager->hidden, hidden_link) {
            if (fifo->hidden_deadline <= now) { ready = fifo; break; }
        }
        if (!ready) break;
        uint64_t generation = ready->hidden_generation;
        remove_hidden(ready);
        update_output(ready);
        if (synchronized(ready->synced.surface) ||
                (!ready->output && ready->barrier && ready->generation == generation)) {
            clear_barrier(ready);
            // Flushing may destroy any surface or install a new generation.
            surface_flush_cached(ready->synced.surface);
        } else {
            schedule(ready);
        }
    }
    manager->timer_dispatching = false;
    arm_timer(manager);
    return 0;
}
static void schedule(struct fifo_surface *fifo) {
    if (!fifo->idle) {
        fifo->idle = wl_event_loop_add_idle(fifo->manager->loop, idle, fifo);
        if (!fifo->idle) wl_resource_post_no_memory(fifo->synced.surface->resource);
    }
}
void wlr_fifo_v1_surface_changed(struct wlr_surface *surface) {
    struct fifo_surface *fifo = get_surface(surface);
    if (fifo) schedule(fifo);
}
bool fifo_v1_state_ready(struct wlr_surface *surface, struct wlr_surface_state *state) {
    struct fifo_surface *fifo = get_surface(surface);
    if (!fifo) return true;
    const struct fifo_state *next = wlr_surface_synced_get_state(&fifo->synced, state);
    if (!next->wait || !fifo->barrier) return true;
    update_output(fifo);
    if (!fifo->output && !synchronized(surface)) pace_hidden(fifo);
    if (synchronized(surface) || (!fifo->output && fifo->manager->timer_failed)) {
        clear_barrier(fifo);
        return true;
    }
    schedule(fifo);
    return false;
}
static void move_state(void *dst, void *src) {
    *(struct fifo_state *)dst = *(struct fifo_state *)src;
    *(struct fifo_state *)src = (struct fifo_state){0};
}
static void apply(struct wlr_surface_synced *synced) {
    struct fifo_surface *fifo = wl_container_of(synced, fifo, synced);
    if (fifo->current.set) {
        remove_hidden(fifo);
        fifo->barrier = true;
        // Zero is reserved for "not submitted"; independent of the u32 surface seq.
        if (++fifo->generation == 0) ++fifo->generation;
        fifo->staged = fifo->submitted = 0;
        update_output(fifo);
        if (!fifo->output && !synchronized(fifo->synced.surface)) pace_hidden(fifo);
        schedule(fifo);
    }
}
static const struct wlr_surface_synced_impl synced_impl = {
    .state_size = sizeof(struct fifo_state), .move_state = move_state, .commit = apply,
};
static void destroy_addon(struct wlr_addon *addon) {
    struct fifo_surface *fifo = wl_container_of(addon, fifo, addon);
    remove_hidden(fifo);
    if (fifo->idle) wl_event_source_remove(fifo->idle);
    disconnect_output(fifo);
    if (fifo->resource) wl_resource_set_user_data(fifo->resource, NULL);
    wlr_surface_synced_finish(&fifo->synced);
    wlr_addon_finish(&fifo->addon);
    wl_list_remove(&fifo->link);
    free(fifo);
}
static const struct wlr_addon_interface addon_impl = {
    .name = "wlr_fifo_v1", .destroy = destroy_addon,
};
static void destroy_resource(struct wl_resource *resource) {
    struct fifo_surface *fifo = wl_resource_get_user_data(resource);
    if (fifo) fifo->resource = NULL;
}
static struct fifo_surface *from_resource(struct wl_resource *resource) {
    struct fifo_surface *fifo = wl_resource_get_user_data(resource);
    if (!fifo) wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
        "FIFO surface has been destroyed");
    return fifo;
}
static void set_barrier(struct wl_client *client, struct wl_resource *resource) {
    struct fifo_surface *fifo = from_resource(resource);
    if (fifo) fifo->pending.set = true;
}
static void wait_barrier(struct wl_client *client, struct wl_resource *resource) {
    struct fifo_surface *fifo = from_resource(resource);
    if (fifo) fifo->pending.wait = true;
}
static void destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}
static const struct wp_fifo_v1_interface fifo_impl = {
    .destroy = destroy, .set_barrier = set_barrier, .wait_barrier = wait_barrier,
};
static void get_fifo(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *surface_resource) {
    struct wlr_fifo_manager_v1 *manager = wl_resource_get_user_data(resource);
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
    struct fifo_surface *fifo = get_surface(surface);
    if (fifo && fifo->resource) {
        wl_resource_post_error(resource, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
            "FIFO object already exists for surface");
        return;
    }
    if (!fifo) {
        fifo = calloc(1, sizeof(*fifo));
        if (!fifo) goto oom;
        fifo->manager = manager;
        wl_list_init(&fifo->hidden_link);
        if (!wlr_surface_synced_init(&fifo->synced, surface, &synced_impl,
                &fifo->pending, &fifo->current)) {
            free(fifo);
            goto oom;
        }
        wlr_addon_init(&fifo->addon, &surface->addons, NULL, &addon_impl);
        wl_list_insert(&manager->surfaces, &fifo->link);
    }
    fifo->resource = wl_resource_create(client, &wp_fifo_v1_interface, 1, id);
    if (!fifo->resource) goto oom;
    wl_resource_set_implementation(fifo->resource, &fifo_impl, fifo, destroy_resource);
    return;
oom:
    wl_client_post_no_memory(client);
}
static const struct wp_fifo_manager_v1_interface manager_impl = {
    .destroy = destroy, .get_fifo = get_fifo,
};
static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &wp_fifo_manager_v1_interface, version, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}
static void display_destroy(struct wl_listener *listener, void *data) {
    struct wlr_fifo_manager_v1 *manager = wl_container_of(listener, manager, display_destroy);
    manager->timer_dispatching = true;
    wl_event_source_remove(manager->timer);
    struct fifo_surface *fifo, *tmp;
    wl_list_for_each_safe(fifo, tmp, &manager->surfaces, link) destroy_addon(&fifo->addon);
    wl_list_remove(&manager->display_destroy.link);
    wl_global_destroy(manager->global);
    free(manager);
}
struct wlr_fifo_manager_v1 *wlr_fifo_manager_v1_create(struct wl_display *display) {
    struct wlr_fifo_manager_v1 *manager = calloc(1, sizeof(*manager));
    if (!manager) return NULL;
    manager->loop = wl_display_get_event_loop(display);
    wl_list_init(&manager->surfaces);
    wl_list_init(&manager->hidden);
    manager->timer = wl_event_loop_add_timer(manager->loop, hidden_timer, manager);
    if (!manager->timer) { free(manager); return NULL; }
    manager->global = wl_global_create(display, &wp_fifo_manager_v1_interface, 1,
        manager, bind_manager);
    if (!manager->global) {
        wl_event_source_remove(manager->timer);
        free(manager);
        return NULL;
    }
    manager->display_destroy.notify = display_destroy;
    wl_display_add_destroy_listener(display, &manager->display_destroy);
    return manager;
}
struct wl_global *wlr_fifo_manager_v1_get_global(struct wlr_fifo_manager_v1 *manager) {
    return manager->global;
}
bool wlr_fifo_manager_v1_output_pending(struct wlr_fifo_manager_v1 *manager,
        struct wlr_output *output) {
    struct fifo_surface *fifo;
    wl_list_for_each(fifo, &manager->surfaces, link) {
        if (fifo->barrier && !fifo->submitted && pacing_output(fifo->synced.surface) == output) return true;
    }
    return false;
}
void wlr_fifo_manager_v1_prepare(struct wlr_fifo_manager_v1 *manager, struct wlr_output *output) {
    struct fifo_surface *fifo;
    wl_list_for_each(fifo, &manager->surfaces, link) {
        update_output(fifo);
        if (fifo->output == output) fifo->staged = fifo->barrier && !fifo->submitted ? fifo->generation : 0;
    }
}
void wlr_fifo_manager_v1_finish(struct wlr_fifo_manager_v1 *manager,
        struct wlr_output *output, bool committed) {
    struct fifo_surface *fifo;
    wl_list_for_each(fifo, &manager->surfaces, link) {
        if (fifo->output != output) continue;
        if (committed && fifo->staged && fifo->staged == fifo->generation) {
            fifo->submitted = fifo->staged;
            fifo->submitted_seq = output->commit_seq;
        }
        fifo->staged = 0;
    }
}
bool wlr_fifo_manager_v1_present(struct wlr_fifo_manager_v1 *manager,
        struct wlr_output *output, const struct wlr_output_event_present *event) {
    bool retry = false;
    // Aqueous submits non-tearing frames while a barrier is active. DRM must
    // confirm VSYNC; headless/nested backends may provide synthetic feedback.
    bool latched = event->presented &&
        (!wlr_output_is_drm(output) || (event->flags & WLR_OUTPUT_PRESENT_VSYNC));
    for (;;) {
        struct fifo_surface *fifo, *ready = NULL;
        wl_list_for_each(fifo, &manager->surfaces, link) {
            if (fifo->output == output && pacing_output(fifo->synced.surface) == output && fifo->submitted &&
                    fifo->submitted == fifo->generation && fifo->submitted_seq == event->commit_seq) {
                ready = fifo;
                break;
            }
        }
        if (!ready) return retry;
        if (!latched) {
            ready->submitted = 0;
            retry = true;
            continue;
        }
        clear_barrier(ready);
        // Restart iteration: commit listeners can remove other surfaces as well.
        surface_flush_cached(ready->synced.surface);
    }
}
