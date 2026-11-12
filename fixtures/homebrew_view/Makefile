# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
#
# The homebrew folder probe (main.c, #2143), built into an EBOOT.PBP with PSPDEV:
#   make -f fixtures/homebrew_view/Makefile -C <out> VPATH=<this folder> EBOOT.PBP
# Project-authored, adapted from fixtures/pspdev_phase5/Makefile.

TARGET = homebrew_view
OBJS = main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspuser -lpspkernel

PSP_EBOOT_TITLE = Nakagawa Homebrew Folder Probe
PSP_EBOOT_ICON = NULL

BUILD_PRX = 1
PSP_FW_VERSION = 500

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
