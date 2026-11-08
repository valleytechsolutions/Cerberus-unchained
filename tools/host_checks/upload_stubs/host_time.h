#pragma once
#include <time.h>
struct tm *test_gmtime_r(const time_t *,struct tm *);
#define gmtime_r test_gmtime_r
