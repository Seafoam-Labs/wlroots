#ifndef TYPES_WLR_FIFO_V1_H
#define TYPES_WLR_FIFO_V1_H
#include <stdbool.h>
struct wlr_surface;
struct wlr_surface_state;
bool fifo_v1_state_ready(struct wlr_surface *surface, struct wlr_surface_state *state);
void surface_flush_cached(struct wlr_surface *surface);
#endif
