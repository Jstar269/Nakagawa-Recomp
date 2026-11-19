// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * What the isolated-run harnesses share: the snapshot runner (src/rt/snapshot_runner.c,
 * issue #1842), the form sweep built on it (src/rt/form_sweep.c, issue #1844) and the
 * verify harness (src/rt/verify_harness.c, issue #1843). src/rt/harness_stubs.c defines
 * the runtime symbols those programs link in place of the scheduler, HLE, GE and host
 * platform, and the two input helpers declared here.
 *
 * Each harness still defines sr_yield, sr_syscall and sr_raw_syscall itself, because what
 * happens at a yield or at a stop is what tells the harnesses apart.
 */
#ifndef SR_HARNESS_STUBS_H
#define SR_HARNESS_STUBS_H

#include <stdint.h>

/* The whole file in a malloc'd buffer (one byte for an empty file), its length in
 * *len_out. NULL when the file cannot be opened or read in full, or is larger than
 * UINT32_MAX bytes. */
uint8_t *sr_harness_read_image(const char *path, uint32_t *len_out);

/* A whole argument as one hexadecimal number of at most 32 bits (strtoull, base 16).
 * Returns 1 and sets *out, or 0 for anything else. */
int sr_harness_parse_hex32(const char *text, uint32_t *out);

#endif
