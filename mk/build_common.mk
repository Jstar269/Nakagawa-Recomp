# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

# Keep compiler-discovered transitive header dependencies beside each object.
DEPFLAGS = -MMD -MP -MF $(@:.o=.d) -MT $@

# Resolve the already-supported SDL3_ttf dependency (the font selftest, the player and the
# pause overlay's text). Linux CI installs sdl3-ttf in the SDL3_ttf job; Windows uses the
# discovered UCRT64 SDK. It is resolved here, before mk/smoke_gates.mk, because the runtime
# profile hashes it: the UI and overlay objects compile with or without NK_UI_WITH_SDL_TTF.
ifeq ($(OS),Windows_NT)
SDL3_TTF_AVAILABLE := $(shell "$(PYTHON)" -c "from pathlib import Path; import sys; root=Path(r'$(SDL3_DIR)'); sys.exit(0 if (root/'include/SDL3_ttf/SDL_ttf.h').is_file() and (root/'lib/libSDL3_ttf.dll.a').is_file() and (root/'bin/SDL3_ttf.dll').is_file() else 1)" && echo yes)
SDL3_TTF_CFLAGS := $(SDL3_INC_FLAGS)
SDL3_TTF_LIBS := $(SDL3_LDFLAGS) -lSDL3_ttf -lSDL3
else
SDL3_TTF_AVAILABLE := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl3-ttf && echo yes)
SDL3_TTF_CFLAGS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --cflags sdl3-ttf)
SDL3_TTF_LIBS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --libs sdl3-ttf)
endif
