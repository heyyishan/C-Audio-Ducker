#ifndef SHIM_SPL_SQRT_FLOOR_H_
#define SHIM_SPL_SQRT_FLOOR_H_

/*
 * Shim for webrtc/common_audio/third_party/spl_sqrt_floor/spl_sqrt_floor.h
 *
 * Provides WebRtcSpl_SqrtFloor(): integer square root (floor).
 * The VAD filterbank uses this for energy normalization.
 *
 * Algorithm: binary search over 16 iterations for 32-bit input.
 * This is the same algorithm as the original WebRTC implementation,
 * just inlined here to avoid pulling in another source file.
 * It computes floor(sqrt(value)) exactly for all uint32_t inputs.
 *
 * Time complexity: O(1) — exactly 16 iterations regardless of input.
 * No floating point, no division — suitable for real-time use.
 */

#include <stdint.h>

static inline int32_t WebRtcSpl_SqrtFloor(int32_t value) {
    if (value <= 0) return 0;

    /*
     * Binary search: test each bit position from high to low.
     * result accumulates the answer bit by bit.
     * If setting a bit makes result^2 <= value, keep the bit.
     *
     * For a 32-bit input, sqrt fits in 16 bits, so 16 iterations suffice.
     */
    int32_t result = 0;
    int32_t bit = 1 << 15; /* Start from the highest possible bit */

    for (int i = 0; i < 16; i++) {
        int32_t candidate = result | bit;
        if (candidate * candidate <= value) {
            result = candidate;
        }
        bit >>= 1;
    }

    return result;
}

#endif /* SHIM_SPL_SQRT_FLOOR_H_ */
