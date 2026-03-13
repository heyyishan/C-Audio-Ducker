#ifndef SHIM_RTC_BASE_CHECKS_H_
#define SHIM_RTC_BASE_CHECKS_H_

/*
 * Shim for webrtc/rtc_base/checks.h
 *
 * The real header provides RTC_DCHECK — a debug assertion macro.
 * The VAD code uses it for parameter validation (e.g., checking
 * that pointers aren't NULL and lengths are positive). In a
 * standalone build we replace it with a standard C assert.
 * In release builds (-DNDEBUG), assert() compiles to nothing,
 * matching the behavior of RTC_DCHECK in Chromium release builds.
 */

#include <assert.h>

#define RTC_DCHECK(condition)       assert(condition)
#define RTC_DCHECK_GT(a, b)         assert((a) > (b))
#define RTC_DCHECK_LT(a, b)         assert((a) < (b))
#define RTC_DCHECK_GE(a, b)         assert((a) >= (b))
#define RTC_DCHECK_LE(a, b)         assert((a) <= (b))
#define RTC_DCHECK_EQ(a, b)         assert((a) == (b))
#define RTC_DCHECK_NE(a, b)         assert((a) != (b))

#endif /* SHIM_RTC_BASE_CHECKS_H_ */
