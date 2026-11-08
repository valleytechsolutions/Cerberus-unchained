#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
typedef unsigned TickType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
void test_enter(void);
void test_exit(void);
#define portENTER_CRITICAL(lock) ((void)(lock),test_enter())
#define portEXIT_CRITICAL(lock) ((void)(lock),test_exit())
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
