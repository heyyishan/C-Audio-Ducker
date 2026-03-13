#ifndef SHIM_RTC_BASE_SANITIZER_H_
#define SHIM_RTC_BASE_SANITIZER_H_

/*
 * Shim for webrtc/rtc_base/sanitizer.h
 *
 * Replaces MSan (MemorySanitizer) annotation functions with no-ops.
 * These exist in Chromium to suppress false positives when MSan
 * can't track initialization through assembly or complex loops.
 * Completely irrelevant for standalone builds.
 */

#include <stddef.h>

/* Marks memory as initialized (no-op outside MSan) */
static inline void rtc_MsanUnpoison(const volatile void *ptr, size_t size) {
    (void)ptr;
    (void)size;
}

/* Asserts memory is initialized (no-op outside MSan) */
static inline void rtc_MsanCheckInitialized(const volatile void *ptr,
                                              size_t elem_size,
                                              size_t count) {
    (void)ptr;
    (void)elem_size;
    (void)count;
}

#ifndef RTC_NO_SANITIZE
#define RTC_NO_SANITIZE(x)
#endif

#endif /* SHIM_RTC_BASE_SANITIZER_H_ */
