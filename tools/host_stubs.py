# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors

# Semantic name overrides for known HST functions. Imported by codegen.py so
# the same table is available to analysis tools (nidseq.py, analyze.py)
# without depending on the translator.
#
# Key  : guest address (int)
# Value: (human-readable name (str), return_value (int))
#
# The table is empty and must stay that way unless a function's whole PSP-visible
# effect is its return value: an entry replaces the guest body with "set r2 and
# return", so it is a forced return. A no-op that returns success while skipping the
# routine's writes is the fake success AGENTS.md Section 6 forbids; a missing
# behaviour is fixed in the runtime or the translation, never hidden here.

# Retired under #363 (2026-10-27): the last five entries (0x0001c010, 0x0001c0fc,
# 0x0001c104, 0x0001c560, 0x0001c604) were the title's archive open/close/seek/read path.
# With the UMD block device readable in sectors, each now translates as written.
HST_SIMPLE_STUBS: dict = {}
