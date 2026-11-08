#ifndef TYPES_WLR_SCENE_H
#define TYPES_WLR_SCENE_H

#include <wlr/types/wlr_scene.h>

struct wlr_scene *scene_node_get_root(struct wlr_scene_node *node);

void scene_node_get_size(struct wlr_scene_node *node, int *width, int *height);

void scene_surface_set_clip(struct wlr_scene_surface *surface, struct wlr_box *clip);

struct wlr_drm_syncobj_merger *scene_snapshot_release(struct wlr_scene_buffer *buffer);
struct wlr_output *scene_surface_preview_output(struct wlr_surface *surface);

#endif
