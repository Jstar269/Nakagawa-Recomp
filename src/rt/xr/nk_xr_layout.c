// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Pure layout maths for the VR module; see nk_xr_layout.h.

#include "xr/nk_xr_layout.h"

#include <math.h>
#include <stddef.h>

int nk_xr_quad_extent(float width_m, float *w, float *h) {
    if (w == NULL || h == NULL || !isfinite(width_m) || !(width_m > 0.0f)) {
        return -1;
    }
    *w = width_m;
    *h = (width_m * NK_XR_PSP_HEIGHT) / NK_XR_PSP_WIDTH;
    return 0;
}

int nk_xr_quad_pose(float distance_m, float height_m, float pos[3], float quat[4]) {
    if (pos == NULL || quat == NULL || !isfinite(distance_m) || !(distance_m > 0.0f) || !isfinite(height_m)) {
        return -1;
    }
    pos[0] = 0.0f;
    pos[1] = height_m;
    pos[2] = -distance_m;
    quat[0] = 0.0f;
    quat[1] = 0.0f;
    quat[2] = 0.0f;
    quat[3] = 1.0f;
    return 0;
}

float nk_xr_eye_clip_x(float x_clip, float w_clip, float separation, float convergence, int eye) {
    if (!isfinite(x_clip) || !isfinite(w_clip) || !isfinite(separation) || !isfinite(convergence)) {
        return NAN;
    }
    float sign = eye < 0 ? -1.0f : (eye > 0 ? 1.0f : 0.0f);
    return x_clip + sign * separation * (w_clip - convergence);
}
