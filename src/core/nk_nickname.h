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
 * 32 is the project's setting limit for what a user enters. PPSSPP caps its Nickname editor
 * at 32 as well, which is the precedent (PPSSPP 3bc60270eae3, UI/Settings/NetworkingPage.cpp:102,
 * a maxLen of 32 that Common/UI/View.cpp:1527 enforces in UTF-8 bytes).
 *
 * The PSP's nickname field is 128 bytes: 127 characters and the NUL (PSPSDK pspnet_adhocctl.h
 * `char nickname[128]`, and the 0x80-byte sceUtilityGetSystemParamString read measured on a
 * PSP-3000, docs/HARDWARE_ORACLE.md PSP-HLE-SYSPARAM-001). That read writes the string and its
 * NUL only: it does not fill or zero the rest of the 128 bytes. A game that reads the nickname
 * into a buffer too small for it and its NUL gets 0x80110102 and nothing written
 * (src/rt/hle_utility.c, as measured), so a short nickname makes that far less likely.
 *
 * The limit of the PSP's own System Settings entry is unmeasured: no public source gives it, and
 * pspautotests (70e72391, tests/utility/systemparam) measures only buffer sizes. The PSP stores
 * the nickname as UTF-8, so 32 bytes hold about 10 Japanese characters. */
#define NK_NICKNAME_MAX_BYTES 32u

/* True when `nickname` is 1 to NK_NICKNAME_MAX_BYTES bytes of well-formed UTF-8 (no overlong
 * form, no surrogate, nothing past U+10FFFF) with no control character: C0 (U+0000 to U+001F),
 * DEL (U+007F) or C1 (U+0080 to U+009F), and with at least one character that is not a space
 * (U+0020): a nickname made only of spaces shows as no name at all, so it is refused. Spaces
 * around or inside a name are kept; the player's editor trims the ones around it on Done. */
static inline bool nk_nickname_valid(const char *nickname) {
    const unsigned char *s = (const unsigned char *)nickname;
    size_t i = 0u;
    bool named = false; /* a character other than U+0020 was seen */
    if (s == NULL || s[0] == 0u) return false;
    while (s[i] != 0u) {
        unsigned char lead = s[i];
        unsigned long cp;
        size_t extra;
        unsigned long min;
        if (lead < 0x80u) {
            if (lead < 0x20u || lead == 0x7Fu) return false;
            if (lead != 0x20u) named = true;
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
        named = true;
        i += 1u + extra;
        if (i > NK_NICKNAME_MAX_BYTES) return false;
    }
    return named;
}

#endif /* NK_NICKNAME_H */
