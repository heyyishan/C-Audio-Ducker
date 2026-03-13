#ifndef SHIM_RTC_BASE_SYSTEM_ARCH_H_
#define SHIM_RTC_BASE_SYSTEM_ARCH_H_

/*
 * Shim for webrtc/rtc_base/system/arch.h
 *
 * The real header detects CPU architecture and defines
 * WEBRTC_ARCH_X86_64, WEBRTC_ARCH_ARM, etc.
 * complex_fft.c uses it to select optimized code paths.
 * We detect the architecture using standard compiler macros.
 */

#if defined(__x86_64__) || defined(_M_X64)
#define WEBRTC_ARCH_X86_64
#define WEBRTC_ARCH_X86_FAMILY
#define WEBRTC_ARCH_64_BITS
#elif defined(__i386__) || defined(_M_IX86)
#define WEBRTC_ARCH_X86
#define WEBRTC_ARCH_X86_FAMILY
#define WEBRTC_ARCH_32_BITS
#elif defined(__aarch64__)
#define WEBRTC_ARCH_ARM64
#define WEBRTC_ARCH_ARM_FAMILY
#define WEBRTC_ARCH_64_BITS
#elif defined(__arm__)
#define WEBRTC_ARCH_ARM
#define WEBRTC_ARCH_ARM_FAMILY
#define WEBRTC_ARCH_32_BITS
#endif

#endif /* SHIM_RTC_BASE_SYSTEM_ARCH_H_ */
