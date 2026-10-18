#ifndef TYPES_WLR_COMMIT_TIMING_V1_H
#define TYPES_WLR_COMMIT_TIMING_V1_H
#include <stdbool.h>
#include "types/wlr_surface_guard.h"
#define COMMIT_TIMING_IMMEDIATE_CAP 32
struct commit_timing_immediate {
	unsigned count;
	struct {
		struct surface_guard guard;
		struct wlr_surface *surface;
		uint32_t seq;
		bool released;
	} children[COMMIT_TIMING_IMMEDIATE_CAP];
};
bool commit_timing_prepare_immediate(struct wlr_surface *surface, struct commit_timing_immediate *frame);
void commit_timing_finish_immediate(struct commit_timing_immediate *frame);
struct wlr_surface;
struct wlr_surface_state;
bool commit_timing_capture(struct wlr_surface *surface);
bool commit_timing_ready(struct wlr_surface *surface, struct wlr_surface_state *state);
void commit_timing_apply(struct wlr_surface *surface);
void commit_timing_changed(struct wlr_surface *surface);
void commit_timing_detach(struct wlr_surface *surface);
bool commit_timing_managed(struct wlr_surface *surface);
#endif
