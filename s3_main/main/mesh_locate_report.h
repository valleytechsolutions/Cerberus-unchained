#pragma once
#include "mesh_locate.h"

#define MESH_LOCATE_REPORT_MAX_BYTES 16384U

/* A fully validated final Locate projection and exact released acquisition.
 * The origin survey is provenance; this report never changes live targets. */
bool mesh_locate_report_valid(const mesh_locate_t *locate);
/* NULL/0 measures CSV bytes excluding NUL. Capacity includes that NUL. Invalid
 * input/size leaves output unchanged. Aliases reject before changing length;
 * other failures set length0. No allocation, filesystem, radio or global buffer. */
bool mesh_locate_report_encode(const mesh_locate_t *locate,
    char *output, size_t capacity, size_t *length);
