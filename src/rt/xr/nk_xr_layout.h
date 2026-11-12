// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// nk_xr_layout: pure maths for the VR module. No state, no OpenXR types.
//
// Coordinates follow OpenXR: metres, +X right, +Y up, -Z forward from the viewer,
// quaternions as (x, y, z, w).

#ifndef NK_XR_LAYOUT_H
#define NK_XR_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

// PSP picture size, the aspect of the big-screen quad (480:272).
#define NK_XR_PSP_WIDTH 480.0f
#define NK_XR_PSP_HEIGHT 272.0f

// Size of the quad that shows the PSP picture: `*w` is `width_m`, `*h` follows
// the 480:272 aspect. Returns 0, or -1 (outputs untouched) for a NULL output or a
// width that is not a positive finite number.
int nk_xr_quad_extent(float width_m, float *w, float *h);

// Pose of the quad in the viewer's reference space: straight ahead (-Z) at
// `distance_m`, centre `height_m` above the origin, identity orientation (facing
// the viewer). Returns 0, or -1 (outputs untouched) for a NULL output or a
// distance that is not a positive finite number or a height that is not finite.
int nk_xr_quad_pose(float distance_m, float height_m, float pos[3], float quat[4]);

// Clip-space stereo rule for the generic stereo tier: x' = x + eye * s * (w - c).
// `separation` is s, `convergence` is c (the clip w that stays at screen depth),
// `eye` is -1 for the left eye and +1 for the right eye (any negative value is
// left, any positive value is right, and 0 is mono: x unchanged). A vertex with
// w == c is unchanged. Any non-finite input gives NaN (no result is invented). The y, z and w coordinates are not touched by the rule.
float nk_xr_eye_clip_x(float x_clip, float w_clip, float separation, float convergence, int eye);

#ifdef __cplusplus
}
#endif

#endif
