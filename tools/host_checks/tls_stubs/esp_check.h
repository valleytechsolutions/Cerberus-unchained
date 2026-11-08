#pragma once
#include <inttypes.h>
#include <stdlib.h>
#include "mbedtls/private/ecdsa.h"
#define likely(value) (value)
#define unlikely(value) (value)
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
