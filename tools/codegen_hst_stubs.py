# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-11-01.
# See NOTICE.md for upstream lineage and modification provenance.

"""The hand-written C bodies ``tools/codegen.py`` emits in place of translated code under
``--profile=hst``.

Each entry is the complete text of one generated function: ``HST_CUSTOM_STUBS`` maps a guest
address to it.  The text is data, not logic: ``codegen.py`` looks the address up and appends it
to the chunk unchanged, so editing a body here changes the generated C exactly as editing it in
``codegen.py`` did.  ``tools/compat_overrides.py`` (``CODEGEN_CUSTOM_STUBS``) records why each
address is coupled to the title, and ``tools/test_compat_manifest.py`` checks the two agree.

Retired stubs, kept here so nobody reinstates them: 0x00010738 (_malloc_r), 0x0000f538
(_free_r), 0x000101c4 (_memalign_r) and 0x00013524 (_realloc_r) bridged the title's allocator
to a heap the runtime owned (sr_newlib_malloc and its three siblings) and were retired together
under #363.  The four share one set of chunk headers, bins and top chunk, and the runtime's
block header gave bit 0 another meaning than theirs (PREV_INUSE), so no part of the family
could run as written while another part was bridged; all four now translate as written and
the title's allocator owns its own metadata
(``tools/test_codegen_retail_allocator.py``).  0x000468c8 (main_RunGameLoop scheduler wrapper, which ran the translated body renamed ``f_000468c8_real`` inside a frame loop with a yield per iteration) was retired under #363: the worker thread's own loop yields through its syscalls, and the function translates as written.  0x000011b0 (__register_frame_info bypass), 0x0000260c (exception helper bypass), 0x0000fe3c (_getmodreent), 0x00015ea0 (CSV tokenizer) and 0x000149a8 (strcpy) were retired under #363: the guest's own bodies run as written.  0x000143b0 (a native sprintf, sr_guest_sprintf) was retired under #363 on 2026-11-11: the title's own sprintf and the formatter it calls translate as written.  0x0001034c (heap-statistics walk skip) and
0x001d9eb0 (title backdrop chooser postcondition) were retired under #363 while the
allocator was still bridged (the walk then read bins that nothing wrote; it now reads the bins
the title's allocator maintains), and the chooser never yields 0; both translate as written.  0x0001a5f8 and
0x0001c008 had constant-return stubs until 2026-07-18.  A Ghidra-assisted review showed both
shadow real, fully translatable code: 0x1c008 is ``jr ra; sw a1,0x4028(a0)`` (the stub dropped
the delay-slot store) and 0x1a5f8 is a computed-goto resume point reached through .data pointer
tables.  Both are discovered entries and translate faithfully;
``tools/test_codegen_no_shadow_stubs.py`` keeps them so.
"""

# One body shared by 0x00011090, 0x000110DC; "@ADDR@" is the address in
# lower-case hex, as in the function name.
_MEMCPY_NATIVE = """void f_@ADDR@(CpuState *s) {  /* custom stub: memcpy native */
    uint32_t dest = s->r[4];
    sr_native_memmove(dest, s->r[5], s->r[6], 0x@ADDR@u);
    s->r[2] = dest;
    s->pc = s->r[31];
}"""

# One body shared by 0x000114C0, 0x000114A8; "@ADDR@" is the address in
# lower-case hex, as in the function name.
_MEMSET_NATIVE = """void f_@ADDR@(CpuState *s) {  /* custom stub: sceKernelMemset native */
    uint32_t dest = s->r[4];
    sr_native_memset(dest, (uint8_t)(s->r[5] & 0xFF), s->r[6], 0x@ADDR@u);
    s->r[2] = dest;
    s->pc = s->r[31];
}"""


def _per_address(template, addresses):
    return {a: template.replace("@ADDR@", f"{a:08x}") for a in addresses}


HST_CUSTOM_STUBS = {
    **_per_address(_MEMCPY_NATIVE, (0x00011090, 0x000110DC)),
    **_per_address(_MEMSET_NATIVE, (0x000114C0, 0x000114A8)),
}
