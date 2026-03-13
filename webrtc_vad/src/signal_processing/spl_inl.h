#ifndef WEBRTC_SPL_SPL_INL_H_
#define WEBRTC_SPL_SPL_INL_H_

/*
 * Inline helper functions for the signal processing library.
 * Bit manipulation, normalization, and saturation primitives
 * used by the VAD filterbank and downsampler.
 */

#include <stdint.h>

/* ── Count leading zeros ─────────────────────────────────────── */

static inline int WebRtcSpl_CountLeadingZeros32(uint32_t n) {
    if (n == 0) return 32;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(n);
#else
    int count = 0;
    if ((n & 0xFFFF0000) == 0) { count += 16; n <<= 16; }
    if ((n & 0xFF000000) == 0) { count += 8;  n <<= 8;  }
    if ((n & 0xF0000000) == 0) { count += 4;  n <<= 4;  }
    if ((n & 0xC0000000) == 0) { count += 2;  n <<= 2;  }
    if ((n & 0x80000000) == 0) { count += 1; }
    return count;
#endif
}

static inline int WebRtcSpl_CountLeadingZeros64(uint64_t n) {
    if (n == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(n);
#else
    int count = 0;
    if ((n >> 32) == 0) { count += 32; n <<= 32; }
    return count + WebRtcSpl_CountLeadingZeros32((uint32_t)(n >> 32));
#endif
}

/* ── Bit size / normalization ────────────────────────────────── */

/* Number of bits needed to represent the value */
static inline int16_t WebRtcSpl_GetSizeInBits(uint32_t n) {
    return (int16_t)(32 - WebRtcSpl_CountLeadingZeros32(n));
}

/*
 * Number of left shifts needed to normalize a signed 32-bit value
 * into the range [0x40000000, 0x7FFFFFFF] (or the negative mirror).
 * Used in fixed-point arithmetic to maximize precision before division.
 */
static inline int16_t WebRtcSpl_NormW32(int32_t a) {
    if (a == 0) return 0;
    uint32_t v = (uint32_t)(a < 0 ? ~a : a);
    return (int16_t)(WebRtcSpl_CountLeadingZeros32(v) - 1);
}

static inline int16_t WebRtcSpl_NormU32(uint32_t a) {
    if (a == 0) return 0;
    return (int16_t)(WebRtcSpl_CountLeadingZeros32(a));
}

/*
 * Norm for 16-bit: shifts needed to normalize a signed 16-bit value.
 * Promotes to 32-bit and adjusts.
 */
static inline int16_t WebRtcSpl_NormW16(int16_t a) {
    int32_t a32 = (int32_t)a;
    if (a32 == 0) return 0;
    uint32_t v = (uint32_t)(a32 < 0 ? ~a32 : a32);
    return (int16_t)(WebRtcSpl_CountLeadingZeros32(v) - 17);
}

/* ── Saturation arithmetic ───────────────────────────────────── */

/*
 * Saturate a 32-bit value to 16-bit range [-32768, 32767].
 *
 * This is the most critical helper in the signal processing library.
 * Every FIR filter output, every accumulator result goes through this
 * to prevent wrap-around distortion. The VAD doesn't care about audio
 * quality per se, but wrapped values would corrupt the filterbank
 * energy estimates and cause false VAD decisions.
 */
static inline int16_t WebRtcSpl_SatW32ToW16(int32_t value) {
    if (value > 32767)  return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

/*
 * Add with 32-bit saturation.
 * Prevents signed overflow which is undefined behavior in C.
 */
static inline int32_t WebRtcSpl_AddSatW32(int32_t a, int32_t b) {
    int64_t sum = (int64_t)a + (int64_t)b;
    if (sum > INT32_MAX)  return INT32_MAX;
    if (sum < INT32_MIN)  return INT32_MIN;
    return (int32_t)sum;
}

static inline int16_t WebRtcSpl_AddSatW16(int16_t a, int16_t b) {
    int32_t sum = (int32_t)a + (int32_t)b;
    if (sum > 32767)  return 32767;
    if (sum < -32768) return -32768;
    return (int16_t)sum;
}

static inline int16_t WebRtcSpl_SubSatW16(int16_t a, int16_t b) {
    int32_t diff = (int32_t)a - (int32_t)b;
    if (diff > 32767)  return 32767;
    if (diff < -32768) return -32768;
    return (int16_t)diff;
}

static inline int32_t WebRtcSpl_SubSatW32(int32_t a, int32_t b) {
    int64_t diff = (int64_t)a - (int64_t)b;
    if (diff > INT32_MAX)  return INT32_MAX;
    if (diff < INT32_MIN)  return INT32_MIN;
    return (int32_t)diff;
}

#endif /* WEBRTC_SPL_SPL_INL_H_ */
