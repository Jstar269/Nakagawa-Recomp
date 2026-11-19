/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_DISPLAY_SIZE_H
#define NK_DISPLAY_SIZE_H

/* The size of the PSP's screen in pixels, defined once for the window and presenter
 * (src/rt/gui.c, src/rt/gpu_sdl3vk/sdl3vk.c), the VR quad (src/rt/xr/nk_xr_layout.h) and the
 * player's hero art (src/ui/nk_ui_theme.h). The float forms are the same numbers for aspect
 * arithmetic. The header holds only these constants, so a file outside src/core includes it
 * by relative path and needs no include directory for it. */
#define NK_PSP_DISPLAY_WIDTH 480
#define NK_PSP_DISPLAY_HEIGHT 272
#define NK_PSP_DISPLAY_WIDTH_F 480.0f
#define NK_PSP_DISPLAY_HEIGHT_F 272.0f

#endif
