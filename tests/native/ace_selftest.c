/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_selftest.c - Ace Mode's CPU-only cells (ace.h): the launch line fails closed (C1), the
 * 30 Hz detector (C4), the speed readout (C5), the per-run report, and the off run's silence.
 *
 * Built from ace.c and this file alone, with nothing else of the runtime: a reference from
 * ace.c to any runtime function fails the link, which is invariant S1's link test for ace.c.
 */
#include "ace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;

#define CHECK(cond, ...) do { \
        if (!(cond)) { s_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                       fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } while (0)

static const char *const k_good =
    "v1;tier=enhanced;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=off";
/* Forty characters, to build lines longer than ACE_SPEC_MAX. */
#define ACE_TEST_X40 "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

/* ---- C1: the launch line ------------------------------------------------------------------- */

static void cell_parse(void) {
    AceConfig c;
    char why[64];
    CHECK(ace_parse(k_good, &c, why, sizeof(why)) == 1, "the canonical line parses (%s)", why);
    CHECK(c.on == 1 && c.af == 16 && c.text == 1 && c.readout == 0 && c.notice == 1 &&
          c.wide == 0 && c.smooth == 0 && c.sas == ACE_SAS_OFF, "fields of the canonical line");
    CHECK(ace_parse("v1;tess=off;sas=off;smooth=off;wide=off;notice=off;readout=on;text=off;af=0", &c, why,
                    sizeof(why)) == 1 && c.on && c.af == 0 && c.readout == 1,
          "keys in any order");
    CHECK(ace_parse("off", &c, why, sizeof(why)) == 1 && c.on == 0, "off is valid and off");
    /* #2055: tier names the run (Enhanced or Ace); a line without it is an Enhanced run. */
    CHECK(ace_parse("v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=off", &c, why,
                    sizeof(why)) == 1 && c.on && c.tier == ACE_TIER_ENHANCED,
          "a line without tier is an Enhanced run");
    CHECK(ace_parse("v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=off;tier=ace",
                    &c, why, sizeof(why)) == 1 && c.on && c.tier == ACE_TIER_ACE,
          "tier=ace names an Ace run");
    /* cull (contract 11.3.6): optional, a missing key and cull=off are both off, cull=on needs an Ace
       line, and a line without it formats as before. */
    {
        static const char base[] = "v1;af=16;text=on;readout=off;notice=on;wide=fill;smooth=off;sas=off;tess=off";
        char spec[ACE_SPEC_MAX], out[ACE_SPEC_MAX];
        CHECK(ace_parse(base, &c, why, sizeof(why)) == 1 && c.cull == 0, "a missing cull is off");
        snprintf(spec, sizeof(spec), "%s;tier=ace;cull=off", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 1 && c.cull == 0 && c.tier == ACE_TIER_ACE,
              "cull=off is off");
        snprintf(spec, sizeof(spec), "%s;tier=ace;cull=on", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 1 && c.on && c.cull == 1, "cull=on in an Ace line");
        CHECK(ace_format(&c, out, sizeof(out)) && strcmp(out + strlen(out) - 8, ";cull=on") == 0 &&
              strstr(out, "tier=ace;") == out + 3, "an Ace line with cull=on formats with the key last (%s)", out);
        c.cull = 0;
        CHECK(ace_format(&c, out, sizeof(out)) && strstr(out, "cull") == NULL, "off writes no cull key (%s)", out);
        snprintf(spec, sizeof(spec), "%s;cull=on", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 0 && strcmp(why, "cull needs tier=ace") == 0 &&
              c.on == 0 && c.cull == 0, "cull=on without tier=ace is refused (%s)", why);
        snprintf(spec, sizeof(spec), "%s;tier=ace;cull=fill", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 0 && strcmp(why, "bad value") == 0, "cull=fill is a bad value");
        snprintf(spec, sizeof(spec), "%s;tier=ace;cull=on;cull=off", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 0 && strcmp(why, "duplicate key") == 0, "a duplicate cull");
    }
    /* stick (contract 11.3.11): optional, a missing key is off, its value is the game's dead zone
       (1..126) in canonical decimal, it needs an Ace line, and a line without it formats as before. */
    {
        static const char base[] = "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=off";
        char spec[ACE_SPEC_MAX], out[ACE_SPEC_MAX];
        CHECK(ace_parse(base, &c, why, sizeof(why)) == 1 && c.stick == 0, "a missing stick is off");
        snprintf(spec, sizeof(spec), "%s;tier=ace;stick=64", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 1 && c.on && c.stick == 64 && c.tier == ACE_TIER_ACE,
              "stick=64 in an Ace line (%s)", why);
        CHECK(ace_format(&c, out, sizeof(out)) && strcmp(out + strlen(out) - 9, ";stick=64") == 0,
              "an Ace line with stick=64 formats with the key last (%s)", out);
        {
            AceConfig back;
            CHECK(ace_parse(out, &back, why, sizeof(why)) == 1 && back.stick == 64 &&
                  memcmp(&back, &c, sizeof(c)) == 0, "the stick line round-trips (%s)", out);
        }
        c.cull = 1;
        CHECK(ace_format(&c, out, sizeof(out)) && strcmp(out + strlen(out) - 17, ";stick=64;cull=on") == 0,
              "cull follows stick (%s)", out);
        c.cull = 0;
        c.stick = 0;
        CHECK(ace_format(&c, out, sizeof(out)) && strstr(out, "stick") == NULL, "off writes no stick key (%s)", out);
        snprintf(spec, sizeof(spec), "%s;tier=ace;stick=1", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 1 && c.stick == 1, "stick=1 is the smallest");
        snprintf(spec, sizeof(spec), "%s;tier=ace;stick=126", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 1 && c.stick == 126, "stick=126 is the largest");
        snprintf(spec, sizeof(spec), "%s;stick=64", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 0 && strcmp(why, "stick needs tier=ace") == 0 &&
              c.on == 0 && c.stick == 0, "stick=64 without tier=ace is refused (%s)", why);
        snprintf(spec, sizeof(spec), "%s;tier=enhanced;stick=64", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 0 && strcmp(why, "stick needs tier=ace") == 0,
              "stick=64 in an Enhanced line is refused (%s)", why);
        static const char *const bad[] = {"0", "127", "255", "-1", "+64", "064", "6x", "x", "on", "off",
                                          "64.0", " 64", "1000"};
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            snprintf(spec, sizeof(spec), "%s;tier=ace;stick=%s", base, bad[i]);
            CHECK(ace_parse(spec, &c, why, sizeof(why)) == 0 && strcmp(why, "bad value") == 0 && c.stick == 0,
                  "stick=%s is a bad value (%s)", bad[i], why);
        }
        snprintf(spec, sizeof(spec), "%s;tier=ace;stick=64;stick=32", base);
        CHECK(ace_parse(spec, &c, why, sizeof(why)) == 0 && strcmp(why, "duplicate key") == 0, "a duplicate stick");
    }
    /* Every key but tier is required, whatever its place: leaving out any one of them refuses
       the line, leaving out tier does not (src/rt/ace.c names the required keys one by one). */
    {
        static const char *const fields[] = { "tier=ace", "af=16", "text=on", "readout=off",
                                              "notice=on", "wide=off", "smooth=off",
                                              "sas=off", "tess=off" };
        enum { FIELDS = (int)(sizeof(fields) / sizeof(fields[0])) };
        for (int skip = 0; skip < FIELDS; skip++) {
            char spec[ACE_SPEC_MAX] = "v1";
            for (int i = 0; i < FIELDS; i++) {
                if (i == skip) continue;
                strncat(spec, ";", sizeof(spec) - strlen(spec) - 1u);
                strncat(spec, fields[i], sizeof(spec) - strlen(spec) - 1u);
            }
            int ok = ace_parse(spec, &c, why, sizeof(why));
            if (skip == 0) {
                CHECK(ok == 1 && c.on && c.tier == ACE_TIER_ENHANCED,
                      "a line without tier parses as Enhanced (%s)", why);
            } else {
                CHECK(ok == 0 && strcmp(why, "missing key") == 0,
                      "a line without %s is refused as a missing key ('%s')", fields[skip], why);
            }
        }
    }

    static const struct { const char *spec; const char *why; } refused[] = {
        { NULL, "unset" },
        { "", "empty field" },
        { "on", "unknown version" },
        { "v2;af=16;text=on;readout=off;notice=on;wide=off;smooth=off", "unknown version" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;boost=on", "unknown key" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;af=8", "duplicate key" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off", "missing key" },
        { "v1;af=3;text=on;readout=off;notice=on;wide=off;smooth=off", "bad value" },
        { "v1;af=16;text=yes;readout=off;notice=on;wide=off;smooth=off", "bad value" },
        { "v1;af=16;text=On;readout=off;notice=on;wide=off;smooth=off", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;", "empty field" },
        { "v1;;af=16;text=on;readout=off;notice=on;wide=off;smooth=off", "empty field" },
        { "v1;af;text=on;readout=off;notice=on;wide=off;smooth=off", "field is not key=value" },
        { "v1;af=;text=on;readout=off;notice=on;wide=off;smooth=off", "field is not key=value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=Fill;smooth=off;sas=off;tess=off", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=stretch;smooth=off;sas=off;tess=off", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=match", "this smooth mode is not part of this build" },
        { "v1;tier=boost;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=off", "bad value" },
        { "v1;tier=ace;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=off;tier=ace",
          "duplicate key" },
        { "v1;tier=ace;af=16;text=on;readout=off;notice=on;smooth=off;sas=off;tess=off", "missing key" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;" ACE_TEST_X40
          ACE_TEST_X40 ACE_TEST_X40 ACE_TEST_X40, "line too long" },
        /* sas (#2054): required like every key, off or sinc, and nothing else. */
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off", "missing key" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=on", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=Sinc", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=linear", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;sas=sinc", "duplicate key" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=", "field is not key=value" },
        /* tess (#2053): required like every key, off or the factor 2, 3 or 4, and nothing else. */
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off", "missing key" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=1", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=5", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=on", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=22", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=-2", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=off;tess=2", "duplicate key" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=", "field is not key=value" },
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        memset(&c, 0x5A, sizeof(c));
        int ok = ace_parse(refused[i].spec, &c, why, sizeof(why));
        CHECK(ok == 0, "refused[%zu] parsed", i);
        CHECK(c.on == 0 && c.af == 0 && c.text == 0 && c.notice == 0, "refused[%zu] left fields set", i);
        CHECK(strcmp(why, refused[i].why) == 0, "refused[%zu]: reason '%s', expected '%s'", i, why,
              refused[i].why);
    }

    /* format and parse agree */
    char line[ACE_SPEC_MAX];
    AceConfig a = { 1, 4, 0, 1, 0, 0, 0, ACE_TIER_ENHANCED, ACE_SAS_OFF, 0, 0, 0 }, b;
    CHECK(ace_format(&a, line, sizeof(line)) && ace_parse(line, &b, NULL, 0) &&
          memcmp(&a, &b, sizeof(a)) == 0, "format round trip (%s)", line);
    CHECK(ace_format(&a, line, 8) == 0 && line[0] == '\0', "format refuses a short buffer");
    a.tier = ACE_TIER_ACE;
    CHECK(ace_format(&a, line, sizeof(line)) && strncmp(line, "v1;tier=ace;", 12) == 0 &&
          ace_parse(line, &b, NULL, 0) && memcmp(&a, &b, sizeof(a)) == 0,
          "an Ace line round trips (%s)", line);
    a.tier = ACE_TIER_ENHANCED;
    a.sas = ACE_SAS_SINC;
    CHECK(ace_format(&a, line, sizeof(line)) &&
          strcmp(line, "v1;tier=enhanced;af=4;text=off;readout=on;notice=off;wide=off;smooth=off;"
                       "sas=sinc;tess=off") == 0 &&
          ace_parse(line, &b, NULL, 0) && b.sas == ACE_SAS_SINC && memcmp(&a, &b, sizeof(a)) == 0,
          "sas=sinc round trip (%s)", line);
    CHECK(ace_parse("v1;sas=sinc;tess=off;af=0;text=off;readout=off;notice=off;wide=off;smooth=off", &b, why,
                    sizeof(why)) == 1 && b.on && b.sas == ACE_SAS_SINC && b.af == 0,
          "sas=sinc alone is a valid line (%s)", why);
    /* tess=2..4 (#2053): each factor round trips, and a line carrying it is canonical. */
    a.sas = ACE_SAS_OFF;
    for (int f = 2; f <= 4; f++) {
        char want[ACE_SPEC_MAX];
        a.tess = f;
        snprintf(want, sizeof(want), "v1;tier=enhanced;af=4;text=off;readout=on;notice=off;wide=off;"
                                     "smooth=off;sas=off;tess=%d", f);
        CHECK(ace_format(&a, line, sizeof(line)) && strcmp(line, want) == 0 &&
              ace_parse(line, &b, NULL, 0) && b.tess == f && memcmp(&a, &b, sizeof(a)) == 0,
              "tess=%d round trip (%s)", f, line);
    }
    a.tess = 0;
    a.on = 0;
    CHECK(ace_format(&a, line, sizeof(line)) && strcmp(line, "off") == 0, "off formats as off");
}

/* ---- the run's switch and its silence ------------------------------------------------------- */

static void cell_switch(void) {
    AceCounters n;
    CHECK(ace_init_spec(NULL, NULL) == 0 && g_ace_on == 0, "unset is off");
    CHECK(ace_init_spec("off", NULL) == 0 && g_ace_on == 0, "off is off");
    CHECK(strcmp(ace_spec_text(), "off") == 0, "off run's line");
    /* A3's counter part: with Ace off the feed does nothing. */
    ace_note_target_draw(0x44000u);
    ace_note_host_wait(1000000u);
    ace_note_skipped_slots(3u);
    CHECK(ace_flip(0x44000u, 0x22000u, 1u, 1000u) == 0, "an off flip reports nothing");
    ace_counters(&n);
    CHECK(!n.draws && !n.flips && !n.new_frames && !n.host_wait_ns && !n.skipped_slots && !n.windows,
          "off counters stay zero");
    char words[128];
    CHECK(ace_feature_words(words, sizeof(words)) == 0 && words[0] == '\0', "no words with Ace off");
    ace_feature_unavailable(ACE_FEATURE_AF, "test");
    CHECK(ace_effective()->af == 0 && ace_requested()->on == 0, "off ignores capability calls");

    CHECK(ace_init_spec("v1;af=16;text=on;readout=on;notice=on;wide=off;smooth=off;bad", NULL) == 0 &&
          g_ace_on == 0, "a refused line is off");
    CHECK(ace_init_spec(k_good, NULL) == 1 && g_ace_on == 1, "the canonical line is on");
    CHECK(strcmp(ace_spec_text(), k_good) == 0, "the run's line is canonical");
    CHECK(strcmp(ace_tier_name(), "Enhanced") == 0, "an Enhanced run is called Enhanced");
    CHECK(ace_init_spec("v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;"
                        "tess=off;tier=ace", NULL) == 1 &&
          strcmp(ace_tier_name(), "Ace") == 0 &&
          strcmp(ace_spec_text(),
                 "v1;tier=ace;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=off") == 0,
          "an Ace run is called Ace, and its line is canonical");
    CHECK(ace_init_spec(k_good, NULL) == 1 && strcmp(ace_tier_name(), "Enhanced") == 0,
          "the next Enhanced run is called Enhanced again");
    CHECK(ace_feature_words(words, sizeof(words)) > 0 &&
          strcmp(words, "16x filtering \xC2\xB7 crisp keyboard") == 0, "feature words: '%s'", words);
    ace_set_af_effective(8, NULL);
    CHECK(ace_effective()->af == 8 && ace_requested()->af == 16, "the device clamps filtering");
    ace_feature_unavailable(ACE_FEATURE_TEXT, "no window");
    ace_feature_unavailable(ACE_FEATURE_TEXT, "named once");
    CHECK(ace_effective()->text == 0 && ace_effective()->notice == 1, "one feature off, the rest on");
    ace_set_af_effective(0, "no samplerAnisotropy");
    CHECK(ace_effective()->af == 0, "no anisotropy turns filtering off");
    ace_set_readout(1);
    CHECK(ace_effective()->readout == 1, "the pause menu turns the readout on");
    ace_set_readout(0);
    CHECK(ace_effective()->readout == 0, "and off");
    ace_feature_unavailable(ACE_FEATURE_READOUT, "no window");
    ace_set_readout(1);
    CHECK(ace_effective()->readout == 0, "a readout this host cannot draw stays off");

    /* wide=fill (#1881): part of this build, named in the words and the stamp's line; off with
       its feature (the presenter turns it off while smooth motion is on, gui.c). */
    CHECK(ace_init_spec("v1;af=0;text=off;readout=off;notice=off;wide=fill;smooth=off;sas=off;tess=off", NULL) == 1 &&
          ace_effective()->wide == ACE_WIDE_FILL, "wide=fill turns Ace on with Fill");
    CHECK(strcmp(ace_spec_text(), "v1;tier=enhanced;af=0;text=off;readout=off;notice=off;wide=fill;"
                                  "smooth=off;sas=off;tess=off") == 0,
          "the Fill run's line keeps wide=fill: '%s'", ace_spec_text());
    CHECK(ace_feature_words(words, sizeof(words)) > 0 && strcmp(words, "ultrawide") == 0,
          "wide feature words: '%s'", words);
    ace_feature_unavailable(ACE_FEATURE_WIDE, "test");
    CHECK(ace_effective()->wide == ACE_WIDE_OFF && ace_requested()->wide == ACE_WIDE_FILL,
          "an unavailable Fill is off for the run");

    /* sas=sinc: named in the words, the stamp's line and the report; off with its feature. */
    CHECK(ace_init_spec("v1;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=sinc;tess=off", NULL) == 1 &&
          ace_effective()->sas == ACE_SAS_SINC, "sas=sinc turns Ace on");
    CHECK(ace_feature_words(words, sizeof(words)) > 0 && strcmp(words, "clean pitched sound") == 0,
          "sas feature words: '%s'", words);
    char json[ACE_REPORT_MAX];
    CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "\"sas\": \"sinc\"") != NULL,
          "the report names sas=sinc:\n%s", json);
    ace_feature_unavailable(ACE_FEATURE_SAS, "test");
    CHECK(ace_effective()->sas == ACE_SAS_OFF && ace_requested()->sas == ACE_SAS_SINC,
          "an unavailable sas is off for the run");
    CHECK(ace_init_spec(k_good, NULL) == 1 && ace_effective()->sas == ACE_SAS_OFF, "sas=off is off");
    /* cull: the report counts the matrices the hook left alone once its table was full, and a run with
     * any is partial (cull unavailable), never a complete one (contract 11.3.6). */
    CHECK(ace_init_spec("v1;af=0;text=off;readout=off;notice=off;wide=fill;smooth=off;sas=off;tess=off;tier=ace;"
                        "cull=on", NULL) == 1 && ace_effective()->cull == 1, "cull=on with tier=ace turns cull on");
    ace_set_cull_untracked(7);
    CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "\"cull\": true") != NULL &&
          strstr(json, "\"cull_untracked\": 7") != NULL, "the report counts untracked matrices:\n%s", json);
    ace_feature_unavailable(ACE_FEATURE_CULL, "partial: test");
    CHECK(ace_effective()->cull == 0 && ace_requested()->cull == 1 &&
          ace_report_json(json, sizeof(json)) > 0 && strstr(json, "\"cull\": false") != NULL &&
          strstr(json, "partial: test") != NULL, "a partial cull run is reported unavailable:\n%s", json);

    /* tess=4 (#2053): named in the words, the stamp's line and the report; off with its feature;
       off in the canonical line and in an authentic run. */
    CHECK(ace_init_spec("v1;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;tess=4", NULL) == 1 &&
          ace_effective()->tess == 4, "tess=4 turns Ace on");
    CHECK(ace_feature_words(words, sizeof(words)) > 0 && strcmp(words, "4x patches") == 0,
          "tess feature words: '%s'", words);
    CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "\"tess\": \"4\"") != NULL,
          "the report names tess=4:\n%s", json);
    ace_feature_unavailable(ACE_FEATURE_TESS, "test");
    CHECK(ace_effective()->tess == 0 && ace_requested()->tess == 4,
          "an unavailable tess is off for the run");
    CHECK(ace_init_spec(k_good, NULL) == 1 && ace_effective()->tess == 0, "tess=off is off");
    CHECK(ace_init_spec("off", NULL) == 0 && ace_effective()->tess == 0, "an authentic run has no tess");
}

/* ---- stick dead-zone remap (contract 11.3.11) ------------------------------------------------ */

/* The title's centring of one stick byte (its own coordinates, -127..127). */
static int game_x(uint8_t b) { return (int)b - (b > 128 ? 1 : 0) - 127; }
static int iabs(int v) { return v < 0 ? -v : v; }

/* x / y = hx / hy within each axis's rounding to nearest: x = hx * t / m + ex with |ex| <= 1/2,
   and the same for y, so |x * hy - y * hx| = |ex * hy - ey * hx| <= (|hx| + |hy|) / 2. */
static int stick_direction_kept(int hx, int hy, int x, int y) {
    const long cross = (long)x * hy - (long)y * hx;
    return 2L * (cross < 0 ? -cross : cross) <= (long)(iabs(hx) + iabs(hy));
}

/* The game's magnitude for a host push of m counts along +x (byte 128 + m), y at rest. */
static int remap_x(int m, int g, int d) {
    uint8_t lx = (uint8_t)(128 + m), ly = 128u;
    ace_stick_remap(&lx, &ly, g, d);
    return iabs(game_x(lx));
}

static void cell_stick_curve(void) {
    /* Rest is untouched, for any G and D. */
    for (int d = 0; d <= 63; d++) {
        uint8_t lx = 128u, ly = 128u;
        ace_stick_remap(&lx, &ly, 64, d);
        CHECK(lx == 128u && ly == 128u, "rest stays 128,128 with D=%d", d);
    }
    /* Full throw and the keyboard's edges are unchanged. */
    {
        static const uint8_t edges[][2] = {{255, 128}, {0, 128}, {128, 255}, {128, 0},
                                           {255, 255}, {0, 0}, {255, 0}, {0, 255}};
        for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
            for (int d = 0; d <= 63; d += 30) {
                uint8_t lx = edges[i][0], ly = edges[i][1];
                ace_stick_remap(&lx, &ly, 64, d);
                CHECK(lx == edges[i][0] && ly == edges[i][1], "edge %u,%u with D=%d became %u,%u",
                      edges[i][0], edges[i][1], d, lx, ly);
            }
        }
    }
    /* G=64, D=30: a push just past D is the game's dead-zone edge (scale 0), the scale rises
       monotonically, and it is (m - D) / (127 - D) within one count. */
    {
        const int g = 64, d = 30;
        CHECK(remap_x(d + 1, g, d) == 64, "a push just past D gives 64, got %d", remap_x(d + 1, g, d));
        CHECK(remap_x(127, g, d) == 127, "full throw gives 127");
        int prev = -1;
        for (int m = 1; m <= 127; m++) {
            const int out = remap_x(m, g, d);
            CHECK(out >= prev, "the curve falls at m=%d (%d after %d)", m, out, prev);
            prev = out;
            CHECK((out >= 64) == (m > d), "m=%d: game magnitude %d is %s the game's dead zone", m, out,
                  m > d ? "inside" : "outside");
            if (m > d) {
                /* (out - 64) / 63 against (m - D) / (127 - D), in counts of 63. */
                const int want_x97 = 63 * (m - d);           /* x (127 - D) = 97 */
                const int got_x97 = (out - 64) * (127 - d);
                CHECK(iabs(got_x97 - want_x97) <= (127 - d), "m=%d: game scale %d/63, want %d/97", m,
                      out - 64, m - d);
            }
        }
    }
    /* Direction is kept, every quadrant inverts the game's centring exactly, and the game sees
       a push past its dead zone exactly when the host's push is past D. */
    {
        const int g = 64;
        static const int ds[] = {0, 30, 63, 100};
        for (size_t k = 0; k < sizeof(ds) / sizeof(ds[0]); k++) {
            const int d = ds[k];
            for (int hx = -127; hx <= 127; hx += 3) {
                for (int hy = -127; hy <= 127; hy += 5) {
                    const int m = iabs(hx) > iabs(hy) ? iabs(hx) : iabs(hy);
                    if (m == 0) continue;
                    uint8_t lx = (uint8_t)(128 + hx), ly = (uint8_t)(128 + hy);
                    ace_stick_remap(&lx, &ly, g, d);
                    const int x = hx == 0 ? 0 : game_x(lx), y = hy == 0 ? 0 : game_x(ly);
                    const int mx = iabs(x) > iabs(y) ? iabs(x) : iabs(y);
                    CHECK((mx >= 64) == (m > d), "D=%d %d,%d: game magnitude %d", d, hx, hy, mx);
                    CHECK((hx < 0 ? x <= 0 : x >= 0) && (hy < 0 ? y <= 0 : y >= 0),
                          "D=%d %d,%d: a sign flipped (%d,%d)", d, hx, hy, x, y);
                    CHECK(stick_direction_kept(hx, hy, x, y), "D=%d %d,%d: direction %d,%d", d, hx, hy, x, y);
                    /* A remapped axis goes back through the exact inverse of the centring. */
                    if (hx != 0) CHECK(lx == (uint8_t)(x >= 1 ? x + 128 : x + 127), "lx inverse %d", x);
                    if (hy != 0) CHECK(ly == (uint8_t)(y >= 1 ? y + 128 : y + 127), "ly inverse %d", y);
                    if (hx == 0) CHECK(lx == 128u, "an axis at rest keeps its byte");
                }
            }
        }
    }
    /* The negative side: byte 0 is -127 (unchanged), and a push of -(D+1) is the game's -64. */
    {
        uint8_t lx = (uint8_t)(128 - 31), ly = 128u;
        ace_stick_remap(&lx, &ly, 64, 30);
        CHECK(game_x(lx) == -64 && lx == 63u, "a push of -31 past D=30 is the game's -64 (byte %u)", lx);
        lx = 1u;                           /* h = -127 */
        ace_stick_remap(&lx, &ly, 64, 30);
        CHECK(lx == 0u, "h=-127 is the game's -127 (byte %u)", lx);
    }
    /* D clamps: below 0 or 127 and above reads as 0; G out of range does nothing. */
    CHECK(remap_x(1, 64, 0) == 64 && remap_x(1, 64, -5) == 64 && remap_x(1, 64, 127) == 64,
          "D=0, D<0 and D>=127 start the game's live range at the first count");
    CHECK(remap_x(63, 64, 63) == 63 && remap_x(64, 64, 63) == 64, "D=G-1: the first live count is G");
    {
        uint8_t lx = 150u, ly = 100u;
        ace_stick_remap(&lx, &ly, 0, 30);
        ace_stick_remap(&lx, &ly, 127, 30);
        CHECK(lx == 150u && ly == 100u, "a dead zone outside 1..126 leaves the sample as it is");
    }
    /* The host's dead zone in centred counts: the default profile's 7849 is 30. */
    CHECK(ace_stick_host_counts(7849, 7849) == 30, "7849 is 30 counts");
    CHECK(ace_stick_host_counts(0, 7849) == 30 && ace_stick_host_counts(7849, 0) == 30, "the larger axis");
    CHECK(ace_stick_host_counts(0, 0) == 0 && ace_stick_host_counts(-1, -9) == 0, "none is 0");
    CHECK(ace_stick_host_counts(32767, 0) == 127, "a full dead zone is 127 (which the remap reads as 0)");
}

/* A host dead zone at or above the game's (a large user dead zone, or a title with a small one)
   is used as it is: up to D the game reads below G, past D at G or more, the curve is monotonic
   and has no gap wider than its own slope at D. */
static void cell_stick_host_dead_zone_at_or_above_g(void) {
    static const int cases[][2] = {{10, 30}, {64, 100}, {64, 64}};
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        const int g = cases[k][0], d = cases[k][1];
        int prev = -1;
        for (int m = 1; m <= 127; m++) {   /* m = 0 is rest, byte 128, which the title reads as +1 */
            const int out = remap_x(m, g, d);
            CHECK(out >= prev, "G=%d D=%d: the curve falls at m=%d (%d after %d)", g, d, m, out, prev);
            CHECK((out >= g) == (m > d), "G=%d D=%d m=%d: game magnitude %d is %s the game's dead zone",
                  g, d, m, out, m > d ? "inside" : "outside");
            prev = out;
        }
        /* Continuity at the edge: D is the game's G - 1, D + 1 its G plus at most one slope step. */
        const int step = (127 - g + (127 - d) - 1) / (127 - d);     /* ceil((127 - G) / (127 - D)) */
        CHECK(remap_x(d, g, d) == g - 1, "G=%d D=%d: m=D gives G-1, got %d", g, d, remap_x(d, g, d));
        CHECK(remap_x(d + 1, g, d) >= g && remap_x(d + 1, g, d) <= g + step,
              "G=%d D=%d: m=D+1 gives %d, want %d..%d", g, d, remap_x(d + 1, g, d), g, g + step);
        CHECK(remap_x(127, g, d) == 127, "G=%d D=%d: full throw stays 127", g, d);
    }
    /* The review's two cases, by value. */
    CHECK(remap_x(30, 10, 30) == 9 && remap_x(31, 10, 30) == 11, "G=10 D=30: 30 -> 9, 31 -> 11");
    CHECK(remap_x(100, 64, 100) == 63 && remap_x(101, 64, 100) == 66, "G=64 D=100: 100 -> 63, 101 -> 66");
    /* The run's remap keeps such a D as it is, for the report too. */
    {
        char json[ACE_REPORT_MAX];
        uint8_t lx = (uint8_t)(128 + 101), ly = 128u;
        CHECK(ace_init_spec("v1;tier=ace;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;"
                            "tess=off;stick=64", NULL) == 1, "stick=64 for a large host dead zone");
        ace_stick_sample(&lx, &ly, 100);
        CHECK(game_x(lx) == 66 && ly == 128u, "a live push of 101 past D=100 is the game's 66 (%u)", lx);
        CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "\"host_dead_zone\": 100") != NULL,
              "the report keeps D=100:\n%s", json);
        (void)ace_init_spec("off", NULL);
    }
}

/* A round host stick (a gamepad's circular gate): points on rays at every 15 degrees, radius 0 to
   127. The direction is kept at every point, and the game's magnitude max(|x|, |y|), which sets
   its movement scale, never falls as the radius grows. A Chebyshev remap pushes the circle
   outward: the host's full diagonal (90, 90) reaches the game as (102, 102), a scale of 38/63.
   Each axis also depends on the other: y held at 40 reads 70 with x at 40 and 40 with x at 127. */
/* cos(15 * step degrees) in thousandths, from a table: the test links no maths library. */
static int stick_cos15(int step) {
    static const int q[7] = {1000, 966, 866, 707, 500, 259, 0};   /* 0..90 degrees */
    step = ((step % 24) + 24) % 24;
    if (step <= 6) return q[step];
    if (step <= 12) return -q[12 - step];
    if (step <= 18) return -q[step - 12];
    return q[24 - step];
}
static int stick_round_milli(int v) { return v >= 0 ? (v + 500) / 1000 : -((-v + 500) / 1000); }

static void cell_stick_round_host(void) {
    static const int ds[] = {0, 30, 100};
    for (size_t k = 0; k < sizeof(ds) / sizeof(ds[0]); k++) {
        const int g = 64, d = ds[k];
        for (int step = 0; step < 24; step++) {
            const int deg = step * 15;
            const int c = stick_cos15(step), sn = stick_cos15(step + 18);   /* sin = cos(deg - 90) */
            int prev = -1;
            for (int r = 0; r <= 127; r++) {
                const int hx = stick_round_milli(r * c), hy = stick_round_milli(r * sn);
                uint8_t lx = (uint8_t)(128 + hx), ly = (uint8_t)(128 + hy);
                ace_stick_remap(&lx, &ly, g, d);
                const int x = hx == 0 ? 0 : game_x(lx), y = hy == 0 ? 0 : game_x(ly);
                const int mx = iabs(x) > iabs(y) ? iabs(x) : iabs(y);
                CHECK(stick_direction_kept(hx, hy, x, y), "D=%d %d deg r=%d: %d,%d became %d,%d", d, deg, r,
                      hx, hy, x, y);
                CHECK(mx >= prev, "D=%d %d deg: the game's magnitude falls at r=%d (%d after %d)", d, deg, r,
                      mx, prev);
                prev = mx;
            }
        }
    }
    {
        uint8_t lx = (uint8_t)(128 + 90), ly = (uint8_t)(128 + 90);
        ace_stick_remap(&lx, &ly, 64, 30);
        CHECK(game_x(lx) == 102 && game_x(ly) == 102, "the host diagonal 90,90 is the game's 102,102 (%d,%d)",
              game_x(lx), game_x(ly));
        lx = (uint8_t)(128 + 40); ly = (uint8_t)(128 + 40);
        ace_stick_remap(&lx, &ly, 64, 30);
        CHECK(game_x(ly) == 70, "y=40 with x=40 is the game's 70 (%d)", game_x(ly));
        lx = 255u; ly = (uint8_t)(128 + 40);
        ace_stick_remap(&lx, &ly, 64, 30);
        CHECK(game_x(ly) == 40, "y=40 with x=127 is the game's 40 (%d)", game_x(ly));
    }
}

/* The run's remap: off unless the line asked for it; it counts, confirms and reports. */
static void cell_stick_run(void) {
    char json[ACE_REPORT_MAX];
    uint8_t lx = 160u, ly = 128u;
    CHECK(ace_init_spec("off", NULL) == 0, "an authentic run");
    ace_stick_sample(&lx, &ly, 30);
    CHECK(lx == 160u && ly == 128u, "an authentic run leaves the stick as it is");
    CHECK(ace_init_spec(k_good, NULL) == 1 && ace_effective()->stick == 0, "an Enhanced run has no stick");
    ace_stick_sample(&lx, &ly, 30);
    CHECK(lx == 160u && ly == 128u, "a run without stick leaves the stick as it is");
    CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "\"stick\"") == NULL,
          "a run without stick reports none:\n%s", json);

    CHECK(ace_init_spec("v1;tier=ace;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;tess=off;"
                        "stick=64", NULL) == 1 && ace_effective()->stick == 64, "stick=64 turns the remap on");
    CHECK(strstr(ace_spec_text(), ";stick=64") != NULL, "the run's line carries the key (%s)", ace_spec_text());
    lx = 128u; ly = 128u;
    ace_stick_sample(&lx, &ly, 30);
    CHECK(lx == 128u && ly == 128u, "rest is not remapped");
    CHECK(ace_report_json(json, sizeof(json)) > 0 &&
          strstr(json, "\"stick\": {\"counts\": 64, \"host_dead_zone\": 30, \"samples_remapped\": 0}") != NULL,
          "the report names the remap before any sample:\n%s", json);
    lx = (uint8_t)(128 + 31);
    ace_stick_sample(&lx, &ly, 30);
    CHECK(game_x(lx) == 64 && ly == 128u, "a live push just past the host's dead zone is the game's 64 (%u)", lx);
    lx = 255u;
    ace_stick_sample(&lx, &ly, 30);
    CHECK(lx == 255u, "full throw stays full throw");
    CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "\"samples_remapped\": 2") != NULL,
          "the report counts remapped samples:\n%s", json);
    CHECK(ace_feature_words(json, sizeof(json)) > 0 && strcmp(json, "stick remap") == 0, "stick words: '%s'", json);
    ace_finish();
    CHECK(ace_effective()->stick == 64 && ace_report_json(json, sizeof(json)) > 0 &&
          strstr(json, "\"unavailable\": {}") != NULL, "a run that remapped is confirmed:\n%s", json);

    /* A run whose stick never left rest reports the remap unavailable at its end. */
    CHECK(ace_init_spec("v1;tier=ace;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;tess=off;"
                        "stick=64", NULL) == 1, "stick=64 again");
    lx = 128u; ly = 128u;
    ace_stick_sample(&lx, &ly, 30);
    ace_finish();
    CHECK(ace_effective()->stick == 0 && ace_requested()->stick == 64 && ace_report_json(json, sizeof(json)) > 0 &&
          strstr(json, "\"stick\": \"did not run") != NULL, "an unused remap is unavailable:\n%s", json);
    (void)ace_init_spec("off", NULL);
}

/* ---- C4: the 30 Hz detector ----------------------------------------------------------------- */

/* Feeds new frames at the given intervals (cycled) for `vblanks` VBLANKs; returns the class. */
static AceRate det_run(AceDetector *d, uint32_t *vc, const uint8_t *gaps, size_t ngaps,
                       uint32_t vblanks) {
    uint32_t end = *vc + vblanks;
    size_t i = 0;
    while (*vc < end) {
        *vc += gaps[i % ngaps];
        (void)ace_det_flip(d, *vc, 1);
        i++;
    }
    return d->stable;
}

static void cell_detector(void) {
    AceDetector d;
    uint32_t vc = 1000;
    static const uint8_t k1[] = {1}, k2[] = {2}, k3[] = {3}, k4[] = {4};
    struct { const uint8_t *g; size_t n; AceRate want; } rates[] = {
        { k1, 1, ACE_RATE_K1 }, { k2, 1, ACE_RATE_K2 }, { k3, 1, ACE_RATE_K3 }, { k4, 1, ACE_RATE_K4 },
    };
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        ace_det_reset(&d);
        vc = 1000;
        CHECK(det_run(&d, &vc, rates[r].g, rates[r].n, 400) == rates[r].want,
              "steady %s reads %s", ace_rate_name(rates[r].want), ace_rate_name(d.stable));
    }

    /* Entry timing: no class before the window is full and 1.0 s has agreed. */
    ace_det_reset(&d);
    vc = 1000;
    uint32_t first_stable = 0;
    for (int i = 0; i < 200 && !first_stable; i++) {
        vc += 2;
        (void)ace_det_flip(&d, vc, 1);
        if (d.stable == ACE_RATE_K2) first_stable = vc;
    }
    /* The window fills after 33 frames (vcount 1066); the class then needs 60 VBLANKs. */
    CHECK(first_stable >= 1066u + ACE_DET_ENTER_VBLANKS && first_stable <= 1066u + ACE_DET_ENTER_VBLANKS + 2u,
          "30 Hz entered at vcount %u", first_stable);

    /* A dropped frame now and then (28 of 32 equal 2, the rest 3) stays 30 Hz. */
    {
        static const uint8_t drops[] = {2,2,2,2,2,2,2,3};
        ace_det_reset(&d);
        vc = 1000;
        CHECK(det_run(&d, &vc, drops, sizeof(drops), 600) == ACE_RATE_K2, "30 Hz with drops: %s",
              ace_rate_name(d.stable));
    }
    /* Two flips in one slot (0) and then a gap of 2: a jittery 60 Hz game, not 30. */
    {
        static const uint8_t jitter[] = {1,1,1,1,1,1,1,0,2,1,1,1,1,1,1,1};
        ace_det_reset(&d);
        vc = 1000;
        CHECK(det_run(&d, &vc, jitter, sizeof(jitter), 600) == ACE_RATE_K1,
              "two flips per slot read 60 Hz: %s", ace_rate_name(d.stable));
    }
    /* 1, 2, 1, 2: no one rate. */
    {
        static const uint8_t alt[] = {1,2};
        ace_det_reset(&d);
        vc = 1000;
        CHECK(det_run(&d, &vc, alt, sizeof(alt), 600) == ACE_RATE_VARIABLE, "1,2 alternating: %s",
              ace_rate_name(d.stable));
    }
    /* Exit timing: 30 Hz, then 60 Hz. The class is left 0.5 s into the disagreement and 60 Hz
     * entered once it has agreed for 1.0 s. */
    {
        ace_det_reset(&d);
        vc = 1000;
        (void)det_run(&d, &vc, k2, 1, 400);
        uint32_t change = vc, left = 0, entered = 0;
        for (int i = 0; i < 400 && !entered; i++) {
            vc += 1;
            (void)ace_det_flip(&d, vc, 1);
            if (!left && d.stable != ACE_RATE_K2) left = vc;
            if (d.stable == ACE_RATE_K1) entered = vc;
        }
        CHECK(left && left - change <= 32u + ACE_DET_LEAVE_VBLANKS + 2u, "30 Hz left after %u VBLANKs",
              left - change);
        CHECK(entered && d.ever_stable && d.last_rate == ACE_RATE_K1, "60 Hz entered after %u VBLANKs",
              entered - change);
    }
    /* Hitches the class rule absorbs (two short intervals, one long one) never move it. */
    {
        static const uint8_t hitch[] = {1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,6,2,2,2,2,2,2,2,2,2,2,2,
                                        2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};
        ace_det_reset(&d);
        vc = 1000;
        (void)det_run(&d, &vc, k2, 1, 400);
        int changed = 0;
        for (size_t i = 0; i < sizeof(hitch); i++) {
            vc += hitch[i];
            changed |= ace_det_flip(&d, vc, 1);
        }
        CHECK(!changed && d.stable == ACE_RATE_K2, "hitches keep 30 Hz: %s", ace_rate_name(d.stable));
    }
    /* STATIC: 30 VBLANKs with no new frame, then frames at the old rate return to it at once. */
    {
        ace_det_reset(&d);
        vc = 1000;
        (void)det_run(&d, &vc, k2, 1, 400);
        uint32_t last = vc;
        for (int i = 1; i <= 40; i++) (void)ace_det_flip(&d, last + (uint32_t)i, 0);
        CHECK(d.stable == ACE_RATE_STATIC && d.last_rate == ACE_RATE_K2, "static keeps the class: %s",
              ace_rate_name(d.stable));
        vc = last + 40;
        vc += 2;
        (void)ace_det_flip(&d, vc, 1);
        CHECK(d.stable == ACE_RATE_K2, "frames at the kept rate return to it: %s", ace_rate_name(d.stable));
    }
}

/* C4's feed: a flip is new when its target was drawn since its own previous flip, and two
 * flips of the same slot are both seen (the feed sits before the presenter's slot rule). */
static void cell_feed(void) {
    AceReadoutView v;
    (void)ace_init_spec(k_good, NULL);
    uint64_t ns = 0;
    /* Single buffer, 30 Hz: drawn every other VBLANK, flipped every VBLANK. */
    for (uint32_t vc = 1; vc <= 600; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        ace_note_host_wait(8000000u);
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
    }
    CHECK(ace_rate_stable() == ACE_RATE_K2, "single-buffered 30 Hz: %s", ace_rate_name(ace_rate_stable()));
    CHECK(ace_readout_view(&v) && strcmp(v.text, "100%  30 FPS") == 0, "readout '%s'", v.text);
    /* Double buffer, 60 Hz with every other flip landing in the slot of the one before. */
    (void)ace_init_spec(k_good, NULL);
    ns = 0;
    uint32_t vc = 1;
    for (int i = 0; i < 600; i++) {
        uint32_t fb = (i & 1) ? 0x88000u : 0x0u;
        ace_note_target_draw(fb);
        if (i % 16 == 7) { /* two flips share a slot, the next slot has none */ }
        else vc++;
        ns += 16683333u;
        (void)ace_flip(fb, 0x22000u, vc, ns);
    }
    CHECK(ace_rate_stable() == ACE_RATE_K1, "double-buffered 60 Hz with shared slots: %s",
          ace_rate_name(ace_rate_stable()));
    /* A latched flip of a target whose draws are not submitted yet is still new: the count is
     * taken when the draw is recorded, not when it is submitted. */
    (void)ace_init_spec(k_good, NULL);
    CHECK(ace_flip(0x44000u, 0x22000u, 1u, 0u) == 1, "a first flip is new");
    CHECK(ace_flip(0x44000u, 0x22000u, 2u, 0u) == 0, "an undrawn re-flip is not new");
    ace_note_target_draw(0x44000u);
    CHECK(ace_flip(0x44000u, 0x22000u, 3u, 0u) == 1, "a recorded draw makes the next flip new");
    ace_note_vram_write(0x50000u, 64u);
    CHECK(ace_flip(0x44000u, 0x22000u, 4u, 0u) == 1, "a CPU write inside the buffer makes it new");
    ace_note_vram_write(0x66000u, 64u);
    CHECK(ace_flip(0x44000u, 0x22000u, 5u, 0u) == 0, "a CPU write past the buffer does not");
    AceCounters n;
    ace_counters(&n);
    CHECK(n.flips == 5 && n.new_frames == 3 && n.draws == 2, "counters %llu %llu %llu",
          (unsigned long long)n.flips, (unsigned long long)n.new_frames, (unsigned long long)n.draws);
}

/* ---- C5: the speed readout ------------------------------------------------------------------ */

static AceWindow window(double fps, double idle, uint32_t skipped, AceRate stable, int ever, AceRate last) {
    AceWindow w;
    memset(&w, 0, sizeof(w));
    w.wall_ns = 1000000000ull;
    w.new_frames = (uint32_t)(fps + 0.5);
    w.idle_ns = (uint64_t)(idle * 1e9);
    w.skipped_slots = skipped;
    w.stable = stable;
    w.ever_stable = ever;
    w.last_rate = last;
    return w;
}

/* Smooth motion is never off without saying so: with it on, the readout ends with the pictures
 * a second that reached the screen, or SMOOTH OFF in a window it added none in. */
static void cell_readout_smooth(void) {
    AceReadoutState s;
    AceReadoutView v;
    AceWindow w;

    ace_readout_reset(&s);
    w = window(30, 0.5, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    w.smooth = 1;
    w.betweens = 30;
    w.shown = 60;
    ace_readout_judge(&s, &w, &v);
    CHECK(v.smooth_on && v.shown_fps > 59.9 && strcmp(v.text, "100%  30 FPS  SMOOTH 60") == 0,
          "smooth engaged: '%s'", v.text);
    /* A 60 FPS scene: nothing to add, and the readout says it added nothing. */
    w = window(60, 0.4, 0, ACE_RATE_K1, 1, ACE_RATE_K1);
    w.smooth = 1;
    ace_readout_judge(&s, &w, &v);
    CHECK(v.smooth_on && strcmp(v.text, "100%  60 FPS  SMOOTH OFF") == 0, "60 FPS scene: '%s'", v.text);
    /* A PC that cannot keep up: uneven frames get no in-between. */
    ace_readout_reset(&s);
    w = window(26, 0.0, 4, ACE_RATE_VARIABLE, 1, ACE_RATE_K2);
    w.smooth = 1;
    ace_readout_judge(&s, &w, &v);
    CHECK(strcmp(v.text, "\xE2\x96\xB2 86%  26 FPS  PC LIMITED  SMOOTH OFF") == 0,
          "PC limited: '%s'", v.text);
    /* Part of a second engaged: the count is what reached the screen. */
    ace_readout_reset(&s);
    w = window(30, 0.5, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    w.smooth = 1;
    w.betweens = 12;
    w.shown = 42;
    ace_readout_judge(&s, &w, &v);
    CHECK(strcmp(v.text, "100%  30 FPS  SMOOTH 42") == 0, "part engaged: '%s'", v.text);
    /* Flips whose present was dropped are not pictures on the screen: 30 new frames and 20
     * in-betweens, but only 37 pictures. */
    w = window(30, 0.5, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    w.smooth = 1;
    w.betweens = 20;
    w.shown = 37;
    ace_readout_judge(&s, &w, &v);
    CHECK(v.shown_fps > 36.9 && v.shown_fps < 37.1 && strcmp(v.text, "100%  30 FPS  SMOOTH 37") == 0,
          "pictures that reached the screen: '%s'", v.text);
    /* Smooth motion off: the text is as before. */
    w = window(30, 0.5, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.smooth_on && strcmp(v.text, "100%  30 FPS") == 0, "smooth off: '%s'", v.text);

    /* The feed: a 30 Hz run with smooth motion asked for counts the pictures that reached the
     * screen: each in-between and held frame the presenter accepted. */
    (void)ace_init_spec("v1;tier=enhanced;af=16;text=on;readout=on;notice=on;wide=off;smooth=x2;sas=off;tess=off",
                        NULL);
    uint64_t ns = 0;
    for (uint32_t vc = 1; vc <= 600; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        ace_note_host_wait(8000000u);
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
        if (ace_smooth_take() == ACE_SHOW_BETWEEN) {
            ace_smooth_shown_between(ns);
            ace_smooth_note_present(ACE_PRESENT_BETWEEN, ns);
        } else {
            ace_smooth_note_present(ACE_PRESENT_REAL, ns);   /* the held frame, re-flipped */
        }
    }
    CHECK(ace_readout_view(&v) && strcmp(v.text, "100%  30 FPS  SMOOTH 60") == 0,
          "fed 30 Hz with smooth motion: '%s'", v.text);
    /* The same, but the presenter drops the present of every fourth VBLANK (an in-between on
     * its flip, or a held frame): the readout counts what reached the screen, not every flip. */
    (void)ace_init_spec("v1;tier=enhanced;af=16;text=on;readout=on;notice=on;wide=off;smooth=x2;sas=off;tess=off",
                        NULL);
    ns = 0;
    for (uint32_t vc = 1; vc <= 600; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        ace_note_host_wait(8000000u);
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
        int between = ace_smooth_take() == ACE_SHOW_BETWEEN;
        if (between) ace_smooth_shown_between(ns);
        if (vc % 4 == 0) continue;                               /* dropped: never noted */
        ace_smooth_note_present(between ? ACE_PRESENT_BETWEEN : ACE_PRESENT_REAL, ns);
    }
    CHECK(ace_readout_view(&v) && v.shown_fps > 40.0 && v.shown_fps < 50.0 &&
              strcmp(v.text, "100%  30 FPS  SMOOTH 45") == 0,
          "fed 30 Hz, some presents dropped: '%s'", v.text);
    /* The same with no in-between shown (no GPU for it): SMOOTH OFF. */
    (void)ace_init_spec("v1;tier=enhanced;af=16;text=on;readout=on;notice=on;wide=off;smooth=x2;sas=off;tess=off",
                        NULL);
    ns = 0;
    for (uint32_t vc = 1; vc <= 600; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        ace_note_host_wait(8000000u);
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
        (void)ace_smooth_take();
        ace_smooth_note_present(ACE_PRESENT_REAL, ns);
    }
    CHECK(ace_readout_view(&v) && strcmp(v.text, "100%  30 FPS  SMOOTH OFF") == 0,
          "fed 30 Hz, no in-between shown: '%s'", v.text);
}

static void cell_readout(void) {
    AceReadoutState s;
    AceReadoutView v;
    AceWindow w;

    ace_readout_reset(&s);
    w = window(30, 0.5, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.pc_limited && v.speed_pct == 100 && strcmp(v.text, "100%  30 FPS") == 0, "30 on time: '%s'", v.text);

    /* A game that slows down on its own while the PC idles is at full speed, as on a PSP. */
    ace_readout_reset(&s);
    w = window(20, 0.6, 0, ACE_RATE_VARIABLE, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.pc_limited && strcmp(v.text, "100%  20 FPS") == 0, "slow game, idle PC: '%s'", v.text);

    /* A steady slowdown keeps T: the detector moves to VARIABLE and then to 20 Hz, the target
     * stays 30. */
    ace_readout_reset(&s);
    w = window(26, 0.0, 4, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(v.pc_limited && v.speed_pct == 86 &&
          strcmp(v.text, "\xE2\x96\xB2 86%  26 FPS  PC LIMITED") == 0, "slowdown begins: '%s'", v.text);
    w = window(20, 0.0, 10, ACE_RATE_K3, 1, ACE_RATE_K3);
    ace_readout_judge(&s, &w, &v);
    CHECK(v.pc_limited && v.speed_pct == 66 && v.target_fps > 29.9 && v.target_fps < 30.0,
          "steady slowdown keeps T: '%s' T=%.2f", v.text, v.target_fps);
    /* A busy PC with no skipped slot is limited when frames come slower than the class allows. */
    ace_readout_reset(&s);
    w = window(27, 0.01, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(v.pc_limited && v.speed_pct == 90, "busy PC, no skip: '%s'", v.text);
    /* ...and not when they come on time. */
    ace_readout_reset(&s);
    w = window(30, 0.01, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.pc_limited, "busy PC on time: '%s'", v.text);

    /* 60 FPS menus, 30 FPS play: one skipped slot in play is judged against 30, not 60. */
    ace_readout_reset(&s);
    w = window(60, 0.4, 0, ACE_RATE_K1, 1, ACE_RATE_K1);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.pc_limited && strcmp(v.text, "100%  60 FPS") == 0, "menus at 60: '%s'", v.text);
    w = window(30, 0.4, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    w = window(29, 0.3, 1, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(v.pc_limited && v.speed_pct == 96 && v.target_fps < 30.0,
          "one skipped slot in play: '%s' T=%.2f", v.text, v.target_fps);

    /* Before a class has been stable once, no percentage. */
    ace_readout_reset(&s);
    w = window(27, 0.5, 0, ACE_RATE_NONE, 0, ACE_RATE_NONE);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.have_target && strcmp(v.text, "\xE2\x80\x94 27 FPS") == 0, "no class yet: '%s'", v.text);
    w = window(20, 0.0, 3, ACE_RATE_NONE, 0, ACE_RATE_NONE);
    ace_readout_judge(&s, &w, &v);
    CHECK(v.pc_limited && !v.have_target &&
          strcmp(v.text, "\xE2\x96\xB2 \xE2\x80\x94  20 FPS  PC LIMITED") == 0, "limited, no class: '%s'", v.text);
}

/* ---- the per-run report --------------------------------------------------------------------- */

static void cell_report(const char *dir) {
    char json[ACE_REPORT_MAX];
    (void)ace_init_spec(k_good, NULL);
    ace_set_af_effective(8, NULL);
    ace_feature_unavailable(ACE_FEATURE_TEXT, "no \"window\" \\ here");
    uint64_t ns = 0;
    for (uint32_t vc = 1; vc <= 300; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
    }
    size_t len = ace_report_json(json, sizeof(json));
    CHECK(len > 0 && len < ACE_REPORT_MAX, "report length %zu", len);
    CHECK(strstr(json, "\"schema\": 1") && strstr(json, "\"af\": 8") &&
          strstr(json, "\"af_device_max\": 8") && strstr(json, "\"dominant\": \"30 Hz\"") &&
          strstr(json, "\"text\": \"no window  here\""), "report content:\n%s", json);
    CHECK(ace_report_json(json, 64) == 0 && json[0] == '\0', "a short buffer refuses the report");

    /* ace_finish writes it through a temporary file and replaces an older one. */
    char path[512];
    snprintf(path, sizeof(path), "%s%sace_selftest_report.json", dir ? dir : "", dir ? "/" : "");
    FILE *old = fopen(path, "wb");
    if (old) { fputs("stale", old); fclose(old); }
    (void)ace_init_spec(k_good, path);
    for (uint32_t vc = 1; vc <= 100; vc++) { ace_note_target_draw(0x0u); (void)ace_flip(0x0u, 0x22000u, vc, vc * 16683333ull); }
    /* Filtering ran; the in-game UI never started (an offscreen run, say). */
    ace_confirm(ACE_FEATURE_AF);
    ace_note_af_failure();
    ace_finish();
    CHECK(ace_effective()->af == 16 && ace_effective()->text == 0 && ace_effective()->notice == 0,
          "at exit, features that never ran are off and named");
    FILE *f = fopen(path, "rb");
    char back[ACE_REPORT_MAX] = {0};
    size_t got = f ? fread(back, 1, sizeof(back) - 1, f) : 0;
    if (f) fclose(f);
    CHECK(got > 0 && strncmp(back, "{\n  \"schema\": 1", 15) == 0, "report file replaced (%zu bytes)", got);
    CHECK(strstr(back, "\"text\": \"did not run") != NULL && strstr(back, "\"af\": 16") != NULL &&
          strstr(back, "\"af_sampler_failures\": 1") != NULL,
          "the report names what did not run and counts sampler failures:\n%s", back);
    (void)remove(path);
    /* An off run writes nothing. */
    (void)ace_init_spec("off", path);
    ace_finish();
    f = fopen(path, "rb");
    CHECK(f == NULL, "an off run wrote a report");
    if (f) fclose(f);

    /* Smooth motion's inputs: what the HUD rule and the depth gate did is in the report. */
    (void)ace_init_spec("v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=x2;sas=off;tess=off", NULL);
    CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "smooth_aux") == NULL,
          "a smooth run reported inputs before the renderer counted any");
    AceSmoothAuxStats st;
    memset(&st, 0, sizeof(st));
    st.pairs = 40; st.pairs_mask = 30; st.pairs_depth = 20; st.depth_refused = 1;
    st.boxes_kept = 500; st.boxes_merged = 450; st.boxes_large = 7; st.boxes_target = 3;
    st.boxes_outside = 2; st.boxes_full = 11;
    st.frames_mask = 41; st.frames_overflow = 2; st.frames_depth = 30; st.frames_depth_unwritten = 9;
    st.frames_depth_none = 4; st.published_unshown = 5;
    ace_smooth_aux_report(&st);
    len = ace_report_json(json, sizeof(json));
    CHECK(len > 0 && strstr(json, "\"smooth_aux\": {\"pairs\": 40, \"pairs_mask\": 30, \"pairs_depth\": 20, "
                                  "\"depth_refused\": 1,") &&
          strstr(json, "\"boxes\": {\"kept\": 500, \"merged\": 450, \"large\": 7, \"target\": 3, "
                       "\"outside\": 2, \"full\": 11}") &&
          strstr(json, "\"frames\": {\"mask\": 41, \"overflow\": 2, \"depth\": 30, \"depth_unwritten\": 9, "
                       "\"depth_none\": 4, \"unshown\": 5}}"),
          "the report lacks smooth motion's input counts:\n%s", json);
    /* A run without smooth motion reports none, whatever the renderer said. */
    (void)ace_init_spec(k_good, NULL);
    ace_smooth_aux_report(&st);
    CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "smooth_aux") == NULL,
          "a run without smooth motion reported its inputs");
}

/* A 30 Hz title's frames: the flip shows the in-between, the next VBLANK the frame. */
static void cell_pace(void) {
    AceConfig c;
    char why[64];
    AcePace p;
    CHECK(ace_parse("v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=x2;sas=off;tess=off", &c, why, sizeof(why)) == 1 &&
          c.smooth == ACE_SMOOTH_X2, "smooth=x2 is part of this build");
    ace_pace_reset(&p);
    /* Off: every present is today's and nothing is ever held. */
    ace_pace_flip(&p, 1, ACE_RATE_K2, 0, 100u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_REAL && !ace_pace_vblank(&p, 11u), "smooth off holds nothing");
    /* 30 Hz: flip at 10, in-between shown; nothing in its own VBLANK; the frame at 11. */
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 100u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_BETWEEN, "a new 30 Hz frame is interpolated");
    CHECK(ace_pace_take(&p) == ACE_SHOW_REAL, "the verdict is taken once");
    ace_pace_shown_between(&p, 10u, 0u);
    CHECK(!ace_pace_vblank(&p, 10u), "the held frame waits for the next VBLANK");
    CHECK(ace_pace_vblank(&p, 11u), "the held frame shows on the next VBLANK");
    CHECK(!ace_pace_vblank(&p, 12u), "a held frame shows once");
    /* A present that was dropped holds nothing. */
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 200u);
    CHECK(!ace_pace_vblank(&p, 13u), "no in-between shown, nothing held");
    /* A flip replaces a frame still held: a stale frame is never shown after a newer one. */
    ace_pace_shown_between(&p, 14u, 0u);
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 300u);
    CHECK(!ace_pace_vblank(&p, 15u), "a later flip replaces the held frame");
    (void)ace_pace_take(&p);
    /* Every other class shows its frames as they come and keeps a copy. */
    static const AceRate others[] = { ACE_RATE_NONE, ACE_RATE_K1, ACE_RATE_K3, ACE_RATE_K4,
                                      ACE_RATE_STATIC, ACE_RATE_VARIABLE };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        ace_pace_flip(&p, 1, others[i], 1, 400u);
        CHECK(ace_pace_take(&p) == ACE_SHOW_KEEP, "class %d is not interpolated", (int)others[i]);
    }
    /* A second flip of an unchanged frame is no new frame: there is nothing to make. */
    ace_pace_flip(&p, 0, ACE_RATE_K2, 1, 500u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_REAL, "an unchanged frame is not interpolated");
    CHECK(ace_pace_flip_kind(&p) == ACE_PRESENT_REAL, "nothing was held: a real present");
    /* A title that flips each frame twice: the second flip shows the held frame, and its
     * VBLANK has nothing left to show. */
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 600u);
    (void)ace_pace_take(&p);
    ace_pace_shown_between(&p, 20u, 601u);
    ace_pace_flip(&p, 0, ACE_RATE_K2, 1, 617u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_REAL && ace_pace_flip_kind(&p) == ACE_PRESENT_HELD,
          "a re-flip of the held frame is its held present");
    CHECK(!ace_pace_vblank(&p, 21u), "a re-flipped frame is not shown again");
    CHECK(p.held_ns == 601u, "the added delay runs from the in-between's present");
    /* An in-between is made only of consecutive frames. The frame flipped at 700 never reached
     * a present (the presenter's slot rule dropped it): the pass still holds the one before, so
     * the next frame is shown as it is and kept, never blended with a frame two steps back. */
    ace_pace_reset(&p);
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 650u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_BETWEEN, "consecutive: an in-between");
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 667u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_BETWEEN && p.unpaired == 0u, "consecutive again");
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 700u);          /* its present is dropped */
    ace_pace_flip(&p, 0, ACE_RATE_K2, 1, 717u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_REAL, "the re-flip after the drop shows the real frame");
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 733u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_KEEP && p.unpaired == 1u,
          "after a dropped frame, no in-between: the real frame is shown and kept");
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 767u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_BETWEEN && p.unpaired == 1u, "the next pair is consecutive");
    /* The report's numbers. */
    AcePaceStats st;
    memset(&st, 0, sizeof(st));
    ace_pace_stats_add(&st, ACE_PRESENT_BETWEEN, 1000000000ull);
    ace_pace_stats_add(&st, ACE_PRESENT_HELD, 1016683000ull);
    ace_pace_stats_add(&st, ACE_PRESENT_BETWEEN, 1033366000ull);
    ace_pace_stats_add(&st, ACE_PRESENT_REAL, 2033366000ull);
    CHECK(st.kind[ACE_PRESENT_BETWEEN] == 2u && st.kind[ACE_PRESENT_HELD] == 1u &&
          st.kind[ACE_PRESENT_REAL] == 1u, "presents counted by kind");
    CHECK(st.interval_count == 3u && st.interval[16] == 2u && st.interval[ACE_PACE_BINS - 1u] == 1u,
          "intervals fall in 1 ms bins, the last open-ended");
}

int main(int argc, char **argv) {
    cell_parse();
    cell_stick_curve();
    cell_stick_host_dead_zone_at_or_above_g();
    cell_stick_round_host();
    cell_stick_run();
    cell_pace();
    cell_switch();
    cell_detector();
    cell_feed();
    cell_readout();
    cell_readout_smooth();
    cell_report(argc > 1 ? argv[1] : NULL);
    if (s_fail) {
        fprintf(stderr, "ace-selftest: %d check(s) FAILED\n", s_fail);
        return 1;
    }
    printf("ace-selftest: PASS (parse, stick, switch, detector, feed, readout, report)\n");
    return 0;
}
