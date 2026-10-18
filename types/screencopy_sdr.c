#include <float.h>
#include <pthread.h>
#include <string.h>
#include "types/screencopy_sdr.h"

_Static_assert(sizeof(float) == sizeof(uint32_t) && FLT_RADIX == 2 &&
	FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128, "capture gamma requires binary32");

static struct screencopy_sdr_gamma gamma_table;
static pthread_once_t gamma_once = PTHREAD_ONCE_INIT;

static float float_from_bits(uint32_t bits) {
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static void gamma_init(void) {
	// Positive binary32 bit patterns are ordered by value. Find the first
	// input producing each code using the original encoder, including its
	// rounded float exponent, powf, multiply and halfway-away rounding.
	// Analytic inverse-gamma thresholds alone differ at rounding boundaries.
	for (unsigned code = 1; code < 256; code++) {
		uint32_t lo = 0, hi = 0x3f800000; // [0, 1]
		while (lo < hi) {
			uint32_t mid = lo + (hi - lo) / 2;
			float value = float_from_bits(mid);
			long encoded = lroundf(powf(value, 1.0f / 2.2f) * 255.0f);
			if (encoded < (long)code) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		gamma_table.threshold[code] = float_from_bits(lo);
	}
	unsigned code = 0;
	for (unsigned bucket = 0; bucket < 4096; bucket++) {
		float lower = (float)bucket / 4096.0f;
		while (code < 255 && lower >= gamma_table.threshold[code + 1]) {
			code++;
		}
		gamma_table.coarse[bucket] = code;
	}
}

const struct screencopy_sdr_gamma *screencopy_sdr_gamma_get(void) {
	pthread_once(&gamma_once, gamma_init);
	return &gamma_table;
}
