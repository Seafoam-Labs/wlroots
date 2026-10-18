#ifndef WLR_TYPES_OUTPUT_PRESENTATION_H
#define WLR_TYPES_OUTPUT_PRESENTATION_H

#include <stdbool.h>
struct wlr_output;
struct wlr_output_state;

#define WLR_AQUEOUS_PRESENTATION_VERSION 1

/** Experimental CPU-copy presentation. Disabled unless explicitly enabled. */
bool wlr_output_allow_presentation_copy(struct wlr_output *output, bool allow);
/** Select copy for a failed composed presentation. Does not commit an output. */
bool wlr_output_try_presentation_copy(struct wlr_output *output);
bool wlr_output_uses_presentation_copy(struct wlr_output *output);
/** True only after a copied frame has passed a real output commit. */
bool wlr_output_presentation_copy_committed(struct wlr_output *output);
/** Reset on renderer replacement. Releases this policy's optimization locks. */
void wlr_output_reset_presentation(struct wlr_output *output);

#endif
