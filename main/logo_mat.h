#pragma once

#include <stdint.h>

// Boot logo, RGB565. The data lives in logo_mat.c and is only declared here:
// a "static const" array in a header gives every translation unit that
// includes it its own 13 KB copy.
#define LOGO_MAT_W 139
#define LOGO_MAT_H  50

extern const uint16_t img_logo_mat[];
