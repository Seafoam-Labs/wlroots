#ifndef TYPES_POINTER_ENTER_SERIAL_H
#define TYPES_POINTER_ENTER_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Exact, bounded history. Unlike the general event ringset, never accept an
// evicted serial or a serial from another kind of event.
struct pointer_enter_serials {
	uint32_t values[64];
	size_t count, next;
};

static inline void pointer_enter_serials_record(struct pointer_enter_serials *set,
		uint32_t serial) {
	if (set->count > 0 && set->values[(set->next + 63) % 64] == serial) {
		return;
	}
	set->values[set->next] = serial;
	set->next = (set->next + 1) % 64;
	if (set->count < 64) {
		set->count++;
	}
}

static inline bool pointer_enter_serials_validate(
		const struct pointer_enter_serials *set, uint32_t serial, uint32_t current) {
	if ((uint32_t)(current - serial) >= UINT32_C(0x80000000)) {
		return false;
	}
	for (size_t i = 0; i < set->count; i++) {
		if (set->values[i] == serial) {
			return true;
		}
	}
	return false;
}

#endif
