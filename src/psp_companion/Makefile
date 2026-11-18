# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

# Nakagawa Font Copy (fontcopy.c): the PSP app that copies the console's own system fonts to its
# Memory Stick folder for the player's font import (docs/SETUP.md, "System fonts"). It needs the
# PSPSDK from pspdev (docs/PSPDEV_LOCAL_VERIFICATION.md) with psp-config on PATH:
#
#   make -C src/psp_companion
#
# builds build/EBOOT.PBP. Copy it to PSP/GAME/NAKAGAWAFONTS/EBOOT.PBP on the Memory Stick.

BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/fontcopy
OBJS = $(BUILD_DIR)/fontcopy.o

$(shell mkdir -p "$(BUILD_DIR)")

INCDIR = ../../tests/fixtures/psp_oracle
CFLAGS = -O2 -G0 -Wall -Wextra -Werror -std=gnu11
# The thread, callback, exit and file calls come from the user-mode stubs in libpspuser, which
# psp-gcc's specs link last. Naming it (or libpspkernel) here as well links its stubs in two
# places, psp-fixup-imports then reports out-of-order stubs, and FIXUP below fails the build.
LIBS = -lpspdebug -lpspdisplay -lpspge -lpspctrl
BUILD_PRX = 1
PSP_FW_VERSION = 500
PSP_EBOOT = $(BUILD_DIR)/EBOOT.PBP
PSP_EBOOT_SFO = $(BUILD_DIR)/PARAM.SFO
PSP_EBOOT_TITLE = Nakagawa Font Copy
PSP_EBOOT_ICON = NULL
EXTRA_TARGETS = $(PSP_EBOOT)

PSPSDK = $(shell psp-config --pspsdk-path)
ifeq ($(strip $(PSPSDK)),)
$(error PSPDEV/PSPSDK is not available: psp-config --pspsdk-path failed)
endif
include $(PSPSDK)/lib/build.mak

# The link-time import gate (tools/psp_oracle/user_mode_imports.py) runs after
# psp-fixup-imports: user-mode libraries only, no registry call, and exactly these file calls:
# sceIoOpen, sceIoRead, sceIoWrite, sceIoClose, sceIoLseek, sceIoDopen, sceIoDread,
# sceIoDclose and sceIoMkdir, plus sceIoChdir and sceIoGetstat from the PSPSDK C library glue.
# No remove, rename, rmdir, ioctl or devctl call can be linked into the app.
PYTHON ?= python3
IMPORT_GATE = ../../tools/psp_oracle/user_mode_imports.py
IMPORT_GATE_ARGS = --forbid-library sceReg
IMPORT_GATE_ARGS += --exact-library IoFileMgrForUser=0x109F50BC,0x6A638D83,0x42EC03AC,0x810C4BC3,0x27EB27B8,0xB29DDF9C,0xE3EB004C,0xEB092469,0x06A70004,0x55F4717D,0xACE946E8
# As in tests/fixtures/psp_oracle/Makefile: keep psp-fixup-imports' output, and fail closed when it
# reports out-of-order stubs (it exits 0 then), before the import gate runs.
FIXUP = bash -c 'out=$$(psp-fixup-imports "$$1" 2>&1); rc=$$?; test -n "$$out" && echo "$$out"; echo "$$out" | grep -q "stubs out of order" && exit 1; test $$rc -eq 0 || exit $$rc; exec "$(PYTHON)" "$(IMPORT_GATE)" $(IMPORT_GATE_ARGS) "$$1"' --
# The SDK link rule writes the ELF before FIXUP runs: .DELETE_ON_ERROR removes an ELF the gate
# rejected, so a later make cannot take it as up to date and package it.
.DELETE_ON_ERROR:

# Make judges staleness by file times alone, and the SDK's ELF and PARAM.SFO rules depend on no
# setting. The stamp holds the settings the build uses (the import gate's policy among them) and
# the FORCE rule rewrites it only when its content changes, so a changed setting rebuilds the
# object, and with it the ELF that FIXUP gates again, and PARAM.SFO. The gate script is an object
# prerequisite for the same reason. Neither is an ELF prerequisite: the SDK link rule passes $^
# to the linker.
BUILD_CONFIG = CFLAGS=$(CFLAGS) LIBS=$(LIBS) PSP_FW_VERSION=$(PSP_FW_VERSION) BUILD_PRX=$(BUILD_PRX) IMPORT_GATE_ARGS=$(IMPORT_GATE_ARGS) SFOFLAGS=$(SFOFLAGS) PSP_EBOOT_TITLE=$(PSP_EBOOT_TITLE)
BUILD_CONFIG_STAMP = $(BUILD_DIR)/build-config.stamp
EXTRA_CLEAN += $(BUILD_CONFIG_STAMP)
$(OBJS) $(PSP_EBOOT_SFO): $(BUILD_CONFIG_STAMP)
$(OBJS): $(IMPORT_GATE)
$(BUILD_CONFIG_STAMP): FORCE
	@printf '%s\n' '$(subst ','\'',$(BUILD_CONFIG))' > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; \
	else echo "build configuration changed: rebuilding"; mv -f $@.tmp $@; fi
.PHONY: FORCE
FORCE:

$(BUILD_DIR)/fontcopy.o: fontcopy.c fontcopy_config.h ../../tests/fixtures/psp_oracle/font_files.h
	$(CC) $(CFLAGS) -c $< -o $@
