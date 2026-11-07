#pragma once
#include <time.h>
/* Win32's timeval has a 32-bit seconds member. Model IDF's 64-bit clock
 * without replacing or conflicting with the host networking headers. */
#define timeval cerberus_test_timeval
#define gettimeofday cerberus_test_gettimeofday
#define settimeofday cerberus_test_settimeofday
typedef long suseconds_t;
struct timeval { time_t tv_sec; suseconds_t tv_usec; };
int gettimeofday(struct timeval *tv, void *zone);
int settimeofday(const struct timeval *tv, const void *zone);
