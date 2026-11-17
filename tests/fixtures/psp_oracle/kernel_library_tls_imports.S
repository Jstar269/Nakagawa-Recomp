/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Complete Kernel_Library import block for the safe-scalar case (CASE_ID 141,
   probe_kernel_semantics.c), the one probe that calls sceKernelGetTlsAddr.

   PSPSDK ships no stub for sceKernelGetTlsAddr. IMPORT_START emits a global
   __stub_module_Kernel_Library, which stops the linker from pulling the SDK's
   header member for that library; an SDK Kernel_Library function stub that is
   still pulled would then land in a second, non-adjacent stub run that
   psp-fixup-imports cannot describe ("stubs out of order", which fails the
   build). So this block also declares the two Kernel_Library calls the PSPSDK
   libc glue references in every probe, sceKernelLockLwMutex and
   sceKernelUnlockLwMutex, and the Makefile's import gate pins the library to
   exactly these three.

   The version word 0x00010000 is the one the SDK's own Kernel_Library block
   carries (a user-mode library, linked by direct jump, not by syscall). The
   two lightweight-mutex NIDs were read from the PSPSDK libpspuser.a
   `.rodata.sceNid` members; all three match src/rt/nid_names.h, and
   sceKernelGetTlsAddr's is the first four bytes of the SHA-1 of its name. */

#include "pspimport.s"

    IMPORT_START "Kernel_Library", 0x00010000

    IMPORT_FUNC  "Kernel_Library", 0xBEA46419, sceKernelLockLwMutex
    IMPORT_FUNC  "Kernel_Library", 0x15B6446B, sceKernelUnlockLwMutex
    IMPORT_FUNC  "Kernel_Library", 0xFA835CDE, sceKernelGetTlsAddr
