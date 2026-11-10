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
    AceConfig a = { 1, 4, 0, 1, 0, 0, 0, ACE_TIER_ENHANCED, ACE_SAS_OFF, 0, 0 }, b;
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

/* Smooth motion is never off without saying so: with it on, the readout ends with SMOOTH n, or
 * SMOOTH OFF in a window it showed no in-between in. n is what moved: the real pictures that
 * reached the screen, plus each in-between by its measured motion weight. */
static void cell_readout_smooth(void) {
    AceReadoutState s;
    AceReadoutView v;
    AceWindow w;

    ace_readout_reset(&s);
    w = window(30, 0.5, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    w.smooth = 1;
    w.betweens = 30;
    w.shown = 60;
    w.motion_milli = 30000;
    ace_readout_judge(&s, &w, &v);
    CHECK(v.smooth_on && v.shown_fps > 59.9 && v.motion_fps > 59.9 &&
              strcmp(v.text, "100%  30 FPS  SMOOTH 60") == 0,
          "smooth engaged: '%s'", v.text);
    /* Sixty presents a second whose in-betweens all repeat the old frame: 30 pictures moved. */
    w.motion_milli = 0;
    ace_readout_judge(&s, &w, &v);
    CHECK(v.shown_fps > 59.9 && v.motion_fps > 29.9 && v.motion_fps < 30.1 &&
              strcmp(v.text, "100%  30 FPS  SMOOTH 30") == 0,
          "in-betweens that repeat the old frame: '%s'", v.text);
    /* In-betweens that carry a sixteenth of what changes add two pictures, not thirty. */
    w.motion_milli = 30u * 64u;
    ace_readout_judge(&s, &w, &v);
    CHECK(strcmp(v.text, "100%  30 FPS  SMOOTH 32") == 0, "in-betweens that move little: '%s'", v.text);
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
    w.motion_milli = 12000;
    ace_readout_judge(&s, &w, &v);
    CHECK(strcmp(v.text, "100%  30 FPS  SMOOTH 42") == 0, "part engaged: '%s'", v.text);
    /* Flips whose present was dropped are not pictures on the screen: 30 new frames and 20
     * in-betweens, but only 37 pictures. */
    w = window(30, 0.5, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    w.smooth = 1;
    w.betweens = 20;
    w.shown = 37;
    w.motion_milli = 20000;
    ace_readout_judge(&s, &w, &v);
    CHECK(v.shown_fps > 36.9 && v.shown_fps < 37.1 && strcmp(v.text, "100%  30 FPS  SMOOTH 37") == 0,
          "pictures that reached the screen: '%s'", v.text);
    /* Smooth motion off: the text is as before. */
    w = window(30, 0.5, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.smooth_on && strcmp(v.text, "100%  30 FPS") == 0, "smooth off: '%s'", v.text);

    /* The feed: a 30 Hz run with smooth motion asked for counts the pictures that reached the
     * screen, each in-between and held frame the presenter accepted, and weighs each in-between
     * by its measurement. Here every in-between shows the changing content half-way. */
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
            ace_smooth_note_motion(4000u, 1900u, 1900u, 90000u, 90000u, 0u);
        } else {
            ace_smooth_note_present(ACE_PRESENT_REAL, ns);   /* the held frame, re-flipped */
        }
    }
    CHECK(ace_readout_view(&v) && strcmp(v.text, "100%  30 FPS  SMOOTH 60") == 0,
          "fed 30 Hz with smooth motion: '%s'", v.text);
    /* The same presents, but every in-between repeats the old frame over what changes: the
     * readout says 30, though sixty pictures a second reached the screen. */
    (void)ace_init_spec("v1;tier=enhanced;af=16;text=on;readout=on;notice=on;wide=off;smooth=x2;sas=off;tess=off",
                        NULL);
    ns = 0;
    for (uint32_t vc = 1; vc <= 600; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        ace_note_host_wait(8000000u);
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
        if (ace_smooth_take() == ACE_SHOW_BETWEEN) {
            ace_smooth_shown_between(ns);
            ace_smooth_note_present(ACE_PRESENT_BETWEEN, ns);
            ace_smooth_note_motion(4000u, 4000u, 0u, 0u, 180000u, 0u);
        } else {
            ace_smooth_note_present(ACE_PRESENT_REAL, ns);
        }
    }
    CHECK(ace_readout_view(&v) && v.shown_fps > 59.0 && strcmp(v.text, "100%  30 FPS  SMOOTH 30") == 0,
          "fed 30 Hz, in-betweens that repeat the old frame: '%s' (%.1f shown)", v.text, v.shown_fps);
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
        if (between) ace_smooth_note_motion(4000u, 1900u, 1900u, 90000u, 90000u, 0u);
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

/* Fast forward (ENHANCEMENT_CONTRACT.md section 10.15): a second of wall time at 3x holds three
 * seconds of frames, and a host that cannot keep up skips slots. Neither reaches the readout or
 * the report: the readout hides while it runs and returns after a full second at normal speed.
 *
 * MUTANT: drop the s_ff_running branch in ace_flip -- windows are judged during fast forward, so
 *   the readout shows and the window count moves.
 * MUTANT: ace_note_skipped_slots counts during fast forward -- the skipped-slot counter moves.
 * Both together also make the windows PC LIMITED, and pc_limited_seconds is not 0. */
static void cell_fast_forward(void) {
    AceReadoutView v;
    AceCounters c0, c1;
    char json[ACE_REPORT_MAX];
    (void)ace_init_spec(k_good, NULL);
    uint64_t ns = 0;
    uint32_t vc = 1;
    for (; vc <= 300; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        ace_note_host_wait(8000000u);
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
    }
    CHECK(ace_readout_view(&v) && strcmp(v.text, "100%  30 FPS") == 0, "before fast forward '%s'", v.text);
    ace_counters(&c0);
    ace_note_fast_forward(1);
    CHECK(!ace_readout_view(&v), "the readout hides while fast forward runs");
    for (uint32_t end = vc + 900; vc < end; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u / 3u;
        if (vc % 10 == 0) ace_note_skipped_slots(1u);
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
    }
    ace_counters(&c1);
    CHECK(!ace_readout_view(&v), "five seconds of fast forward judge no window");
    CHECK(c1.windows == c0.windows && c1.skipped_slots == c0.skipped_slots,
          "fast forward adds no window (%llu to %llu) and no skipped slot (%llu to %llu)",
          (unsigned long long)c0.windows, (unsigned long long)c1.windows,
          (unsigned long long)c0.skipped_slots, (unsigned long long)c1.skipped_slots);
    ace_note_fast_forward(0);
    for (uint32_t end = vc + 30; vc < end; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
    }
    CHECK(!ace_readout_view(&v), "half a second after fast forward the readout is still hidden");
    for (uint32_t end = vc + 60; vc < end; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        ace_note_host_wait(8000000u);
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
    }
    CHECK(ace_readout_view(&v) && strcmp(v.text, "100%  30 FPS") == 0,
          "a full second at normal speed brings the readout back: '%s'", v.text);
    CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "\"pc_limited_seconds\": 0\n"),
          "fast forward adds no PC LIMITED second to the report:\n%s", json);
}

/* Smooth motion under fast forward (ENHANCEMENT_CONTRACT 10.6, 10.15): no in-between is made
 * while it runs, every new frame is shown and kept, and the first new frame after it gets its
 * in-between again, made of the frame kept last. */
static void cell_fast_forward_smooth(void) {
    AcePaceStats st0, st1;
    (void)ace_init_spec("v1;tier=enhanced;af=16;text=on;readout=on;notice=on;wide=off;smooth=x2;sas=off;tess=off",
                        NULL);
    uint64_t ns = 0;
    uint32_t vc = 1, betweens = 0, keeps = 0, reals = 0;
    for (; vc <= 300; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
        AceShow show = ace_smooth_take();
        if (show == ACE_SHOW_BETWEEN) {
            ace_smooth_shown_between(ns);
            ace_smooth_note_present(ACE_PRESENT_BETWEEN, ns);
            betweens++;
        } else {
            ace_smooth_note_present(ACE_PRESENT_REAL, ns);
        }
    }
    CHECK(betweens > 100u, "30 Hz at normal speed makes in-betweens (%u)", betweens);
    ace_pace_stats(&st0);
    ace_note_fast_forward(1);
    betweens = 0;
    for (uint32_t end = vc + 900; vc < end; vc++) {
        int new_frame = vc % 2 == 0;
        if (new_frame) ace_note_target_draw(0x0u);
        ns += 16683333u / 3u;
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
        AceShow show = ace_smooth_take();
        if (show == ACE_SHOW_BETWEEN) betweens++;
        else if (show == ACE_SHOW_KEEP) keeps++;
        else if (new_frame) reals++;
        ace_smooth_note_present(ACE_PRESENT_REAL, ns);
    }
    ace_pace_stats(&st1);
    CHECK(betweens == 0u && st1.kind[ACE_PRESENT_BETWEEN] == st0.kind[ACE_PRESENT_BETWEEN],
          "fast forward makes no in-between (%u taken, %llu to %llu shown)", betweens,
          (unsigned long long)st0.kind[ACE_PRESENT_BETWEEN], (unsigned long long)st1.kind[ACE_PRESENT_BETWEEN]);
    CHECK(keeps == 450u && reals == 0u, "fast forward shows and keeps every new frame (%u kept, %u not)", keeps,
          reals);
    ace_note_fast_forward(0);
    if (vc % 2 != 0) {
        ns += 16683333u;
        (void)ace_flip(0x0u, 0x22000u, vc++, ns);
        (void)ace_smooth_take();
    }
    ace_note_target_draw(0x0u);
    ns += 16683333u;
    (void)ace_flip(0x0u, 0x22000u, vc++, ns);
    CHECK(ace_smooth_take() == ACE_SHOW_BETWEEN,
          "the first new frame after fast forward gets its in-between from the frame kept last");
}

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

/* The report's worst case fits ACE_REPORT_MAX: every block present, the spec and every reason at
 * their longest, and every number as wide as one can be. A count is a uint64_t, at most 20 digits;
 * the widest other number is a rate's seconds, a uint64_t VBLANK count over 59.94 with one decimal,
 * also 20 characters. So each run of digits and points in the report is counted as 20 wide, and
 * each rate class the run has no seconds for as present. */
static void cell_report_worst(void) {
    char json[ACE_REPORT_MAX];
    char reason[128];
    memset(reason, 'r', sizeof(reason));
    reason[sizeof(reason) - 1] = '\0';
    CHECK(ace_init_spec("v1;tier=ace;af=16;text=on;readout=on;notice=on;wide=fill;smooth=x2;sas=sinc;tess=off;cull=on",
                        NULL), "the longest line was refused");
    ace_set_af_effective(8, NULL);
    ace_note_af_failure();
    ace_set_cull_untracked(1ul);
    for (int f = 0; f < ACE_FEATURE_COUNT; f++) ace_feature_unavailable((AceFeature)f, reason);
    for (uint32_t vc = 1; vc <= 200; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        (void)ace_flip(0x0u, 0x22000u, vc, vc * 16683333ull);
    }
    ace_smooth_note_present(ACE_PRESENT_BETWEEN, 1000000000ull);
    ace_smooth_note_present(ACE_PRESENT_REAL, 1016683000ull);
    ace_smooth_note_motion(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    ace_smooth_note_motion_unread();
    AceSmoothAuxStats st;
    memset(&st, 0xFF, sizeof(st));
    ace_smooth_aux_report(&st);
    size_t len = ace_report_json(json, sizeof(json));
    CHECK(len > 0 && strstr(json, "\"cull_untracked\"") && strstr(json, "\"af_device_max\"") &&
              strstr(json, "\"af_sampler_failures\"") && strstr(json, "\"presents\"") &&
              strstr(json, "\"added_delay_ms\"") && strstr(json, "\"smooth_motion\"") &&
              strstr(json, "\"smooth_aux\"") && strstr(json, "\"present_interval_ms\""),
          "a report with every block:\n%s", json);
    if (!len) return;
    size_t worst = len;
    for (size_t i = 0; i < len;) {
        size_t run = 0;
        while (i + run < len && ((json[i + run] >= '0' && json[i + run] <= '9') || json[i + run] == '.')) run++;
        if (run && run < 20u) worst += 20u - run;
        i += run ? run : 1u;
    }
    for (int r = ACE_RATE_K1; r < ACE_RATE_COUNT; r++) {
        char key[64];
        snprintf(key, sizeof(key), "\"%s\": ", ace_rate_name((AceRate)r));
        if (!strstr(json, key)) worst += strlen(", ") + strlen(key) + 20u;
    }
    const char *spec = strstr(json, "\"spec\": \"");
    const char *spec_end = spec ? strchr(spec + 9, '"') : NULL;
    CHECK(spec_end != NULL, "the report has no spec");
    if (spec_end) worst += (ACE_SPEC_MAX - 1u) - (size_t)(spec_end - (spec + 9));
    /* A feature named unavailable reads "off" in the effective block, shorter than its value on. */
    worst += (strlen("fill") - strlen("off")) + (strlen("match") - strlen("off")) + (strlen("sinc") - strlen("off"));
    CHECK(worst < ACE_REPORT_MAX, "the worst-case report needs %zu bytes; ACE_REPORT_MAX is %u",
          worst + 1u, ACE_REPORT_MAX);
    printf("ace-selftest: the worst-case report needs %zu of %u bytes\n", worst + 1u, ACE_REPORT_MAX);
}

/* What an in-between moved: the weight of a measurement, when a measurement may be read, and the
 * report's totals. */
static void cell_motion(void) {
    /* The weight: 0 for a repeat of either frame or when nothing differs, 1000 at half-way, and
     * the same from either side. */
    CHECK(ace_motion_weight_milli(0u, 0u) == 0u, "nothing differs: %u", ace_motion_weight_milli(0u, 0u));
    CHECK(ace_motion_weight_milli(0u, 5000u) == 0u, "a repeat of the old frame");
    CHECK(ace_motion_weight_milli(5000u, 0u) == 0u, "the new frame shown early");
    CHECK(ace_motion_weight_milli(5000u, 5000u) == 1000u, "half-way");
    CHECK(ace_motion_weight_milli(1000u, 3000u) == 500u && ace_motion_weight_milli(3000u, 1000u) == 500u,
          "a quarter of the way, from either side: %u %u", ace_motion_weight_milli(1000u, 3000u),
          ace_motion_weight_milli(3000u, 1000u));
    CHECK(ace_motion_weight_milli(32u, 968u) == 64u, "phase 0.032: %u", ace_motion_weight_milli(32u, 968u));
    CHECK(ace_motion_weight_milli(0xFFFFFFFFu, 0xFFFFFFFFu) == 1000u, "the largest sums do not overflow");

    /* A measurement is due once the presenter has seen its submission finished, in the order
     * made. Submissions finish in order, so the count of the newest one seen finished says it for
     * every one before. */
    AceMotionQueue q;
    ace_motion_queue_reset(&q);
    CHECK(ace_motion_queue_due(&q, 100u) == 0u, "an empty queue had something due");
    CHECK(ace_motion_queue_made(&q, 1u, 5u) == 1, "the queue refused its first serial");
    CHECK(ace_motion_queue_due(&q, 4u) == 0u, "due before its submission finished");
    /* Two presents in the same submission's place: one recorded but never submitted, so the
     * next submission carries the same serial number. Both wait for it. */
    CHECK(ace_motion_queue_made(&q, 2u, 7u) == 1 && ace_motion_queue_made(&q, 3u, 7u) == 1,
          "the queue refused serials 2 and 3");
    CHECK(ace_motion_queue_due(&q, 6u) == 1u, "serial 1 was not due once its submission finished");
    CHECK(ace_motion_queue_due(&q, 6u) == 0u, "serial 2 was due before its submission finished");
    /* Slots used out of turn (a failed present keeps its slot) change nothing: only the count
     * of finished submissions decides. */
    CHECK(ace_motion_queue_due(&q, 9u) == 2u && ace_motion_queue_due(&q, 9u) == 3u &&
              ace_motion_queue_due(&q, 9u) == 0u,
          "serials 2 and 3 were not due, in order, once a later submission finished");
    /* A queue nobody reads fills, then drops and counts: it never overwrites a pending serial. */
    ace_motion_queue_reset(&q);
    for (uint64_t serial = 1; serial <= ACE_MOTION_PENDING + 2u; serial++) {
        int kept = ace_motion_queue_made(&q, serial, serial);
        CHECK(kept == (serial <= ACE_MOTION_PENDING), "serial %llu kept=%d", (unsigned long long)serial, kept);
    }
    CHECK(q.dropped == 2u && ace_motion_queue_due(&q, 1u) == 1u && ace_motion_queue_due(&q, 1u) == 0u,
          "a full queue dropped %llu", (unsigned long long)q.dropped);

    /* The report: the totals, the pooled phase and the mean weight; nothing before a measurement
     * and nothing in a run without smooth motion. */
    char json[ACE_REPORT_MAX];
    AceMotionStats m;
    (void)ace_init_spec("v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=x2;sas=off;tess=off", NULL);
    CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "smooth_motion") == NULL,
          "a smooth run reported motion before any in-between was measured");
    ace_smooth_note_motion(1000u, 900u, 10u, 300u, 9700u, 4u);      /* phase 0.03: weight 60 */
    ace_smooth_note_motion(3000u, 100u, 1400u, 5000u, 5000u, 6u);   /* half-way: weight 1000 */
    ace_smooth_note_motion_unread();
    ace_motion_stats(&m);
    CHECK(m.measured == 2u && m.unread == 1u && m.moving == 4000u && m.eq_old == 1000u && m.eq_new == 1410u &&
              m.sum_old == 5300u && m.sum_new == 14700u && m.novel == 10u && m.weight_milli == 1060u,
          "motion totals: measured %llu weight %llu", (unsigned long long)m.measured,
          (unsigned long long)m.weight_milli);
    CHECK(ace_report_json(json, sizeof(json)) > 0 &&
              strstr(json, "\"smooth_motion\": {\"measured\": 2, \"unread\": 1, \"moving_px\": 4000, "
                           "\"old_px\": 1000, \"new_px\": 1410, \"novel_px\": 10, \"phase\": 0.265, "
                           "\"weight\": 0.530},"),
          "the report lacks what the in-betweens moved:\n%s", json);
    (void)ace_init_spec(k_good, NULL);
    ace_smooth_note_motion(1000u, 900u, 10u, 300u, 9700u, 4u);
    CHECK(ace_report_json(json, sizeof(json)) > 0 && strstr(json, "smooth_motion") == NULL,
          "a run without smooth motion reported motion");
    /* Off, the feed reads nothing. */
    (void)ace_init_spec(NULL, NULL);
    ace_smooth_note_motion(1000u, 900u, 10u, 300u, 9700u, 4u);
    ace_smooth_note_motion_unread();
    ace_motion_stats(&m);
    CHECK(m.measured == 0u && m.unread == 0u, "an off run counted a measurement");
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
    CHECK(ace_pace_vblank(&p, 11u), "the held frame stays due until a present shows it");
    CHECK(ace_pace_held_shown(&p) == 0u && !p.held, "the present that shows it ends the hold");
    CHECK(!ace_pace_vblank(&p, 12u), "a held frame shows once");
    /* A present that was dropped holds nothing. */
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 200u);
    CHECK(!ace_pace_vblank(&p, 13u), "no in-between shown, nothing held");
    /* A flip replaces a frame still held: a stale frame is never shown after a newer one. */
    ace_pace_shown_between(&p, 14u, 0u);
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 300u);
    CHECK(!ace_pace_vblank(&p, 15u) && p.superseded == 1u, "a later flip replaces the held frame");
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
    CHECK(ace_pace_held_shown(&p) == 601u, "the added delay runs from the in-between's present");
    CHECK(!ace_pace_vblank(&p, 21u), "a re-flipped frame is not shown again");

    /* A held frame whose present finds the display busy is owed, not lost. It stays due, and the
     * next new flip's present shows it in place of an in-between and holds the new frame. */
    ace_pace_reset(&p);
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 800u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_BETWEEN, "the frame before the busy present is interpolated");
    ace_pace_shown_between(&p, 30u, 801u);
    CHECK(ace_pace_vblank(&p, 31u), "its held frame is due");
    ace_pace_held_missed(&p, 1);
    CHECK(p.held && p.owed && p.busy == 1u && p.superseded == 0u, "a busy display leaves the frame owed");
    CHECK(ace_pace_vblank(&p, 32u), "an owed frame is due again at the next VBLANK");
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 833u);
    CHECK(p.held && p.superseded == 0u, "a new flip does not replace an owed frame");
    CHECK(ace_pace_take(&p) == ACE_SHOW_OWED, "the next flip's present shows the owed frame");
    ace_pace_shown_owed(&p, 32u, 834u);
    CHECK(ace_pace_held_shown(&p) == 801u, "the owed frame's delay runs from its own in-between");
    CHECK(p.held && !p.owed && !ace_pace_vblank(&p, 32u) && ace_pace_vblank(&p, 33u),
          "the new frame is held for the VBLANK after its flip");
    CHECK(ace_pace_held_shown(&p) == 834u && !p.held, "the new frame's held present ends the hold");
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 867u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_BETWEEN && p.unpaired == 0u, "in-betweens resume with the next frame");
    /* A title that flips each frame twice: the second flip is the held present. Busy, the
     * frame stays held and owed; taken, the hold ends. */
    ace_pace_shown_between(&p, 34u, 868u);
    ace_pace_flip(&p, 0, ACE_RATE_K2, 1, 884u);
    CHECK(ace_pace_flip_kind(&p) == ACE_PRESENT_HELD && p.held, "a re-flip leaves the frame held until shown");
    ace_pace_held_missed(&p, 1);
    CHECK(p.held && p.owed && p.busy == 2u, "a busy re-flip leaves the frame owed");
    /* Owed, but the frame after it never reached a show pass either (its present was busy too):
     * the pass holds a frame two steps back, so the next frame is shown as it is and kept, and
     * the owed frame is counted as never shown. */
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 900u);          /* verdict OWED, never taken */
    CHECK(p.held && p.owed, "a flip whose present is dropped leaves the owed frame owed");
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 933u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_KEEP && p.superseded == 1u && !p.held && !p.owed && p.unpaired == 0u,
          "an owed frame two steps back is superseded, not shown late");
    /* A present that failed, or a closed window, gives the frame up, and it is counted. */
    ace_pace_flip(&p, 1, ACE_RATE_K2, 1, 967u);
    CHECK(ace_pace_take(&p) == ACE_SHOW_BETWEEN, "consecutive again");
    ace_pace_shown_between(&p, 40u, 968u);
    ace_pace_held_missed(&p, 0);
    CHECK(!p.held && !p.owed && p.superseded == 2u && !ace_pace_vblank(&p, 41u),
          "a failed held present gives the frame up");
    /* Nothing held: a miss changes nothing. */
    ace_pace_held_missed(&p, 1);
    CHECK(!p.held && !p.owed && p.busy == 2u, "a miss with nothing held counted something");
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
    cell_pace();
    cell_switch();
    cell_detector();
    cell_feed();
    cell_readout();
    cell_readout_smooth();
    cell_motion();
    cell_fast_forward();
    cell_fast_forward_smooth();
    cell_report(argc > 1 ? argv[1] : NULL);
    cell_report_worst();
    if (s_fail) {
        fprintf(stderr, "ace-selftest: %d check(s) FAILED\n", s_fail);
        return 1;
    }
    printf("ace-selftest: PASS (parse, switch, detector, feed, readout, fast forward, report)\n");
    return 0;
}
