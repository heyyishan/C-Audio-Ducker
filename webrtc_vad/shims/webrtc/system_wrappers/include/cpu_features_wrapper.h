#ifndef SHIM_CPU_FEATURES_WRAPPER_H_
#define SHIM_CPU_FEATURES_WRAPPER_H_

/*
 * Shim for webrtc/system_wrappers/include/cpu_features_wrapper.h
 *
 * The real header provides WebRtc_GetCPUFeaturesARM() which detects
 * NEON support at runtime. spl_init.c uses it to select optimized
 * DSP routines for ARM platforms.
 *
 * On x86_64 this is never called — the function pointer table in
 * spl_init.c just uses the generic C implementations.
 *
 * We provide a stub that returns 0 (no features detected), which
 * forces the generic code path on ARM too. This is correct and safe,
 * just slightly slower on ARM (irrelevant for VAD at 16 kHz).
 */

#include <stdint.h>

/* CPU feature flags from the real header */
#define kCPUFeatureARMv7    (1 << 0)
#define kCPUFeatureVFPv3    (1 << 1)
#define kCPUFeatureNEON     (1 << 2)
#define kCPUFeatureLDREXSTREX (1 << 3)

static inline uint64_t WebRtc_GetCPUFeaturesARM(void) {
    return 0; /* No ARM features — use generic C fallbacks */
}

/* x86 variant some files check for */
static inline int WebRtc_GetCPUInfo(int feature) {
    (void)feature;
    return 0;
}

#endif /* SHIM_CPU_FEATURES_WRAPPER_H_ */
