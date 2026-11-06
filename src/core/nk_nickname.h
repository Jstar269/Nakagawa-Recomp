/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_NICKNAME_H
#define NK_NICKNAME_H

/* The console nickname (#2197): the player's Nickname setting, and the value the runtime serves
 * as the registry's /CONFIG/SYSTEM/owner_name, which sceUtilityGetSystemParamString(1) reads. The
 * user chooses it; the default is the neutral NK_NICKNAME_DEFAULT, and it is never derived from the
 * operating system's or the PC's user name. One rule for the player and the runtime. It is
 * header-only, so neither needs another source in its link lists. */

#include <stdbool.h>
#include <stddef.h>

#define NK_NICKNAME_DEFAULT "Player"

/* At most 32 bytes before the NUL, counted in UTF-8 bytes, not characters.
 *
 * The PSP's 128-byte nickname fields hold more (PSPSDK pspnet_adhocctl.h `char nickname[128]`,
 * and the measured 0x80-byte sceUtilityGetSystemParamString read, docs/HARDWARE_ORACLE.md
 * PSP-HLE-SYSPARAM-001). The cap on what a user enters is lower: 32 bytes, the limit PPSSPP has
 * shipped for its Nickname editor (PPSSPP 3bc60270eae3, UI/Settings/NetworkingPage.cpp:102, a
 * maxLen of 32 that Common/UI/View.cpp:1527 enforces in UTF-8 bytes). A game that reads the
 * nickname into a buffer too small for it and its NUL gets 0x80110102 and nothing written
 * (src/rt/hle_utility.c, as measured), so a short nickname makes that far less likely.
 *
 * The limit of the PSP's own System Settings entry is UNMEASURED: no public source gives it, and
 * pspautotests (70e72391, tests/utility/systemparam) measures only buffer sizes. A measurement on
 * a PSP is still wanted. The PSP stores the nickname as UTF-8, so 32 bytes hold about 10
 * Japanese characters. */
#define NK_NICKNAME_MAX_BYTES 32u

/* True when `nickname` is 1 to NK_NICKNAME_MAX_BYTES bytes of well-formed UTF-8 (no overlong
 * form, no surrogate, nothing past U+10FFFF) with no control character: C0 (U+0000 to U+001F),
 * DEL (U+007F) or C1 (U+0080 to U+009F). */
static inline bool nk_nickname_valid(const char *nickname) {
    const unsigned char *s = (const unsigned char *)nickname;
    size_t i = 0u;
    if (s == NULL || s[0] == 0u) return false;
    while (s[i] != 0u) {
        unsigned char lead = s[i];
        unsigned long cp;
        size_t extra;
        unsigned long min;
        if (lead < 0x80u) {
            if (lead < 0x20u || lead == 0x7Fu) return false;
            i++;
            if (i > NK_NICKNAME_MAX_BYTES) return false;
            continue;
        }
        if (lead >= 0xC2u && lead <= 0xDFu) {
            extra = 1u;
            min = 0x80u;
            cp = lead & 0x1Fu;
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            extra = 2u;
            min = 0x800u;
            cp = lead & 0x0Fu;
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            extra = 3u;
            min = 0x10000u;
            cp = lead & 0x07u;
        } else {
            return false; /* a continuation byte, an overlong C0/C1 lead, or F5 and above */
        }
        for (size_t k = 1u; k <= extra; k++) {
            if ((s[i + k] & 0xC0u) != 0x80u) return false; /* also stops at the NUL */
            cp = (cp << 6) | (s[i + k] & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
        if (cp <= 0x9Fu) return false; /* C1 controls */
        i += 1u + extra;
        if (i > NK_NICKNAME_MAX_BYTES) return false;
    }
    return true;
}

#endif /* NK_NICKNAME_H */
