#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_SPIRAM 4U
void *heap_caps_malloc(size_t n,unsigned caps);
void *heap_caps_calloc(size_t n,size_t size,unsigned caps);
void heap_caps_free(void *p);
