/*
 * Minimal min/max operations for the signal processing library.
 *
 * spl_init.c sets up function pointers to these. On ARM with NEON,
 * WebRTC would swap in optimized versions; on x86 these C fallbacks
 * are used directly. The VAD filterbank calls MaxAbsValueW16 to find
 * peak amplitude for scaling decisions.
 *
 * Performance: O(n) linear scan. At 320 samples per frame (20 ms),
 * this is ~320 comparisons — trivially fast, well within L1 cache.
 */

#include <stdint.h>
#include <stdlib.h>  /* abs() */
#include "signal_processing_library.h"

/*
 * Find the maximum absolute value in a 16-bit vector.
 * Used by the filterbank to detect clipping / compute headroom.
 * Returns 0 for zero-length input (defensive).
 */
int16_t WebRtcSpl_MaxAbsValueW16C(const int16_t *vector, size_t length) {
    if (vector == NULL || length == 0) return 0;

    int16_t max_val = 0;
    for (size_t i = 0; i < length; i++) {
        int16_t abs_val = (int16_t)abs((int)vector[i]);
        /* Handle the INT16_MIN corner case: abs(-32768) overflows
         * int16_t, but we just saturate to 32767. */
        if (vector[i] == -32768) abs_val = 32767;
        if (abs_val > max_val) max_val = abs_val;
    }
    return max_val;
}

/*
 * Find the maximum absolute value in a 32-bit vector.
 */
int32_t WebRtcSpl_MaxAbsValueW32C(const int32_t *vector, size_t length) {
    if (vector == NULL || length == 0) return 0;

    int32_t max_val = 0;
    for (size_t i = 0; i < length; i++) {
        int32_t abs_val = (vector[i] >= 0) ? vector[i] : -vector[i];
        if (vector[i] == INT32_MIN) abs_val = INT32_MAX;
        if (abs_val > max_val) max_val = abs_val;
    }
    return max_val;
}

/*
 * Find the maximum value in a 16-bit vector.
 */
int16_t WebRtcSpl_MaxValueW16C(const int16_t *vector, size_t length) {
    if (vector == NULL || length == 0) return (int16_t)(-32768);

    int16_t max_val = vector[0];
    for (size_t i = 1; i < length; i++) {
        if (vector[i] > max_val) max_val = vector[i];
    }
    return max_val;
}

/*
 * Find the maximum value in a 32-bit vector.
 */
int32_t WebRtcSpl_MaxValueW32C(const int32_t *vector, size_t length) {
    if (vector == NULL || length == 0) return INT32_MIN;

    int32_t max_val = vector[0];
    for (size_t i = 1; i < length; i++) {
        if (vector[i] > max_val) max_val = vector[i];
    }
    return max_val;
}

/*
 * Find the minimum value in a 16-bit vector.
 */
int16_t WebRtcSpl_MinValueW16C(const int16_t *vector, size_t length) {
    if (vector == NULL || length == 0) return 32767;

    int16_t min_val = vector[0];
    for (size_t i = 1; i < length; i++) {
        if (vector[i] < min_val) min_val = vector[i];
    }
    return min_val;
}

/*
 * Find the minimum value in a 32-bit vector.
 */
int32_t WebRtcSpl_MinValueW32C(const int32_t *vector, size_t length) {
    if (vector == NULL || length == 0) return INT32_MAX;

    int32_t min_val = vector[0];
    for (size_t i = 1; i < length; i++) {
        if (vector[i] < min_val) min_val = vector[i];
    }
    return min_val;
}
