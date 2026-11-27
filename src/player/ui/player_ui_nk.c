/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"

#include "player_state.h"
#include "../../rt/ace.h"
#include "cover_art.h"
#include "player_ui_art.h"
#include "player_ui_music.h"
#include "player_ui_snd0.h"
#include "player_ui_sound.h"
#include "iso_reader.h"
#include "nk_adhoc_relay.h"
#include "nk_platform.h"
#include "nk_shared_helpers.h"
#include "nk_portable.h"
#include "nk_ui_input.h"
#include "nk_ui_text.h"
#include "nk_ui_theme.h"
#include "nk_ui_widgets.h"
#include "backend/nk_ui_sdlr.h"

#include <SDL3/SDL.h>
#ifdef NK_PLAYER_UI_REGRESSION_TEST
#include "nk_ui_draw.h" /* the frame's runs, for the detail panel's report */
#endif

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "launcher_secret.h"  /* the hidden moment and Love All */
#include "launcher_themes.h"  /* the launcher's colour themes */

typedef char launcher_theme_ids_fit_the_settings[
    LAUNCHER_THEME_ID_MAX == PLAYER_THEME_ID_MAX &&
    LAUNCHER_THEME_UNLOCK_MAX == PLAYER_THEME_UNLOCK_MAX ? 1 : -1];

#ifdef NK_PLAYER_UI_REGRESSION_TEST
/* The achievement rows the title page drew this frame, for the regression report after it. */
static const PlayerUiAchievementRow *g_title_achievement_rows;
static size_t g_title_achievement_count;
#endif

/* Smooth motion's value on a game's page while that game runs its 60 FPS game logic. */
#define ENHANCED_SMOOTH_AT_60_VALUE NK_UI_STR(ADAPTER_SMOOTH_AT_60)

/* Status colours, from the theme's tokens. */
#define PLAYER_UI_READY (nk_ui_theme_n1()->ready)
#define PLAYER_UI_WARNING (nk_ui_theme_n1()->warning)
#define PLAYER_UI_INFO (nk_ui_theme_n1()->info)
#define PLAYER_UI_TITLE_CAP MAX_LIBRARY_GAMES
#define PLAYER_UI_PREREQ_CAP PACKAGE_BUILDER_MAX_PREREQUISITES
/* A blocking check's reason (up to its 2048-byte message) and the next step. */
#define PLAYER_UI_BLOCKED_REASON_MAX 2304

/* Build identity for the About page, injected by the Makefile and CMake:
   SOURCE_DATE_EPOCH when the build sets it, else the source commit's time. */
#ifndef NK_PLAYER_BUILD_EPOCH
#define NK_PLAYER_BUILD_EPOCH (-1LL)
#endif
#ifndef NK_PLAYER_BUILD_COMMIT
#define NK_PLAYER_BUILD_COMMIT ""
#endif

typedef struct {
    const char *key;
    const char *category;
    /* The player tier when a player's own run says more (or less) than the
       category's tier; NONE takes player_ui_tier_for_category(category). */
    PlayerUiTier tier;
    const char *expect;
    const char *known_issues;
} PlayerUiCompatibilityRecord;

/* What a player can expect from each title in the public compatibility table,
   in plain words, and its player tier (#1297). The category is the table's own
   word, kept for the developer view: it describes the project's test route,
   not this PC. Lookup is data driven so the screen never assigns a
   compatibility claim from layout code. */
static const PlayerUiCompatibilityRecord s_compatibility_records[] = {
    {"display-smoke-v1", "Works", PLAYER_UI_TIER_NONE,
     NK_UI_STR(ADAPTER_COMPAT_DISPLAY_SMOKE), ""},
    {"synthetic-allegrex-v1", "Works", PLAYER_UI_TIER_NONE,
     NK_UI_STR(ADAPTER_COMPAT_SYNTHETIC_RUNS_AND_CLOSES), ""},
    {"pspdev-phase5-v1", "Partially works", PLAYER_UI_TIER_WONT_BOOT,
     NK_UI_STR(ADAPTER_COMPAT_PSPDEV_WONT_BOOT),
     ""},
    {"synthetic-title2-v1", "Works", PLAYER_UI_TIER_NONE,
     NK_UI_STR(ADAPTER_COMPAT_SYNTHETIC_RUNS_AND_CLOSES), ""},
    {"showcase-scene-v1", "Partially works", PLAYER_UI_TIER_PLAYABLE,
     NK_UI_STR(ADAPTER_COMPAT_SHOWCASE_CUBE), ""},
    {"showcase-breakout-v1", "Partially works", PLAYER_UI_TIER_PLAYABLE,
     NK_UI_STR(ADAPTER_COMPAT_SHOWCASE_BREAKOUT), ""},
    /* Playable: full matches and Story Mode were played through (#1297). */
    {"UCUS98701", "Boots to a named boundary", PLAYER_UI_TIER_PLAYABLE,
     NK_UI_STR(ADAPTER_COMPAT_HST_MATCHES_AND_STORY),
     NK_UI_STR(ADAPTER_COMPAT_HST_LOAD_AUDIO)},
    {"UCUS-98701", "Boots to a named boundary", PLAYER_UI_TIER_PLAYABLE,
     NK_UI_STR(ADAPTER_COMPAT_HST_MATCHES_AND_STORY),
     NK_UI_STR(ADAPTER_COMPAT_HST_LOAD_AUDIO)},
    {"PAC-MAN Championship Edition", "Partially works", PLAYER_UI_TIER_NONE,
     NK_UI_STR(ADAPTER_COMPAT_PACMAN_FIRST_STAGE),
     ""}
};

/* The backups the Saves dialog keeps (#1883), newest first. */
#define PLAYER_UI_SAVES_LIST_CAP 64u

struct PlayerUiNk {
    SDL_Window *window;     /* the launcher window: Settings reads its display's refresh rate */
    bool clock_held;        /* frames are drawn at held_clock_s, not the time of day (#2124) */
    double held_clock_s;
    NkUi *ui;
    NkUiSdlr *backend;
    NkUiTtf *ttf;
    NkUiTextBackend text;
    NkUiSdlInput input;
    NkUiRouter *router;
    NkUiViewport viewport;
    size_t library_page;
    int enhanced_tab;       /* session UI state; never serialized with player settings */
    /* The art rest: the card focus rests on and since when, and the game whose
       picture is behind the library (SIZE_MAX: none) and when it stopped being
       shown, so it is kept while it fades out. */
    size_t rest_index;
    double rest_since_s;
    size_t backdrop_index;
    double backdrop_hidden_s;
    PlayerUiTitle titles[PLAYER_UI_TITLE_CAP];
    /* The library grid in its sort order: indices into titles, no demos; while
       the find box is open, only the games that match. */
    size_t library_grid[PLAYER_UI_TITLE_CAP];
    size_t library_grid_count;
    size_t library_game_total;
    /* The library's find box: open, what is typed, and whether SDL's text
       input is on for it. */
    bool find_open;
    char find_query[64];
    UiKeyboard find_keyboard;  /* edits find_query (#2206) */
    bool find_text_input;
    /* The control the box focuses, on the first frame that lays it out. */
    NkUiId find_focus;
    /* The nickname editor (#2197): open (the first-launch prompt, or Settings >
       Nickname), what is typed and its on-screen keyboard, whether the last Done
       was refused, the control the next frame focuses (0 for none), and the
       control focus returns to after it. */
    bool nickname_open;
    bool nickname_first_launch;
    bool nickname_is_server;
    bool nickname_is_port;      /* Online play's port offset (#1139) */
    bool nickname_is_ra_user;   /* RetroAchievements' sign-in form: its username (#1316) */
    bool nickname_is_ra_password; /* its password, shown as stars */
    bool nickname_refused;
    NkUiId nickname_focus;
    NkUiId nickname_return_focus;
    char nickname_text[PLAYER_UI_RA_PASSWORD_MAX + 1u];
    UiKeyboard nickname_keyboard;
    /* RetroAchievements' sign-in form (#1316): what SIGN IN sends once. It lives only here,
       is wiped as soon as the form's page closes or SIGN IN takes it, and at exit. */
    PlayerUiSignInForm ra_form;
    char status_text[PLAYER_UI_TITLE_CAP][256];
    char library_page_label[64];
    char flow_title[NK_MAX_TITLE_LEN + NK_MAX_DISC_ID_LEN + 16];
    /* The build screen's model on this frame: 1 the package is written, 0 it is
       building, -1 the frame drew no build screen. */
    int build_built;
    PlayerUiPrereqItem prereq_items[PLAYER_UI_PREREQ_CAP];
    char version_line[64];
    char data_root_line[NK_MAX_PATH + 48]; /* About: which data folder is in use */
    char wizard_detail[160];
    char wizard_blocked[PLAYER_UI_BLOCKED_REASON_MAX];
    char wizard_resolution[96];
    char wizard_controller[160];
    char wizard_summary[224];
    char disc_reason[PLAYER_UI_BLOCKED_REASON_MAX];
    char disc_profile[512];
    /* The files-to-add count line (#308): for the disc being added (game-found
       card and wizard), and for the selected library title. */
    char input_summary[128];
    char title_input_summary[128];
    char settings_controller[160];
    char settings_online_play_row[160];
    char settings_online_play_status[320];
    char settings_online_play_port[8];
    char settings_online_play_card_host[NK_ADHOC_SERVER_MAX_LEN + 1u];
    /* The Online play picker's rows (#1139): the shipped servers, then the list's. */
    PlayerUiOnlineServer settings_online_play_servers[PLAYER_UI_ONLINE_PLAY_SERVERS_MAX];
    /* The graphics preset's note on the main Settings page (#2150). */
    char settings_preset_note[192];
    char settings_interface_size[16];
    char settings_saves[NK_MAX_PATH];
    /* The play mode on the open title's page: what it turns on, and its last session, read
       at most every two seconds (the runtime writes the report when a game ends). */
    /* Every part named, with the 60 FPS and smooth-motion words (192) and a culling
       package's problem (PlayerTitleEnhancement.cull_problem, 160, after a 41-character
       sentence): the line is never cut. */
    char ace_line[PLAYER_PART_COUNT * 64 + 192 + 256];
    char enhanced_smooth_sentence[160];  /* the Smooth motion row's words under 60 FPS logic */
    char ace_last_session[200];
    char ace_last_disc[NK_MAX_DISC_ID_LEN];
    double ace_last_read_s;
    /* Settings > Enhanced: the title's name and the rows' "Library (16x)" words. */
    char enhanced_title[160];
    char enhanced_mode[48];
    const char *enhanced_modes[4];
    char enhanced_values[PLAYER_UI_ENHANCED_ROWS][48];
    char texpack_value[200];
    char texpack_sentence[sizeof(((PlayerTexturePackInfo *)0)->error) +
                          sizeof(NK_UI_STR(ADAPTER_TEXPACK_REFUSAL_DETAILS))];
    char enhanced_library_values[PLAYER_UI_ENHANCED_ROWS][48];
    char settings_tools[PLAYER_UI_SETTINGS_TOOLS][192];
    /* The factory reset check's folders, looked up when the check opens. */
    char reset_folders[PLAYER_UI_RESET_FOLDERS][NK_MAX_PATH];
    size_t reset_folder_count;
    bool reset_folders_ready;
    char update_folder[NK_MAX_PATH];
    char report_path[NK_MAX_PATH];
    char report_lines[PLAYER_UI_REPORT_LINES][112];
    char update_notice[96];
    char pad_bindings[PLAYER_UI_CONTROLLER_CONTROLS][128];
    InputBindingGlyph pad_glyphs[PLAYER_UI_CONTROLLER_CONTROLS];
    char pad_scope[96];
    char pad_status[400];
    char pad_deadzone[48];
    char pad_trigger[48];
    char pad_aim[32];            /* "Mouse aim: Off" (#1791) */
    char pad_aim_speed[32];      /* "Speed 100%" */
    char pad_aim_smoothing[32];  /* "Smoothing 0%" */
    char pad_live[PLAYER_UI_CONTROLLER_LIVE_LINES][96];
    PlayerView last_view;   /* the view drawn last, for the focus a Settings page returns to */
    unsigned press_screen;  /* the screen a pointer press can still finish on */
    unsigned flow_screen;   /* which flow screen drew last; 0 outside them */
    NkUiId pre_flow_focus;  /* the library focus to give back on leaving */
    int pre_flow_game_count; /* titles in the library when the flow opened */
    /* The title page's PLAY or BUILD PACKAGE went off while it had focus
       (STARTING..., a running game): focus returns to it when it comes back
       on, unless the user has moved since. title_primary_fallback is where
       focus went meanwhile. */
    bool title_primary_refocus;
    NkUiId title_primary_fallback;
    NkUiPromptImages prompt_images;  /* Kenney glyphs; empty ones keep the geometry */
    PlayerUiArt *art;                /* disc ICON0 / PIC1, decoded at run time */
    PlayerUiSounds *sounds;          /* optional launcher sounds (ui_sounds) */
    PlayerUiMusic *music;            /* optional launcher music (launcher_music, #2207) */
    PlayerUiSnd0 *snd0;              /* the open game page's SND0.AT3 (launcher_game_music) */
    size_t snd0_index;               /* the game whose page this frame shows, or SIZE_MAX */
    int snd0_reported;               /* the last PlayerUiSnd0State written to the log */
    bool window_unfocused;           /* the launcher window lost input focus */
    int music_reported;              /* the last PlayerUiMusicState written to the log */
    int pending_sound;               /* a PlayerUiSound for this frame, or -1 */
    NkUiId sound_focus;              /* focus after the last frame, for the move sound */
    /* Remove from library (#1304): the title the dialog is for, what it lists,
       and the saves box, which every dialog opens unticked. */
    PlayerRemovalPlan remove_plan;
    PlayerUiRemoveItem remove_items[PLAYER_UI_REMOVE_ITEM_CAP];
    size_t remove_item_count;
    int remove_index;
    char remove_disc[NK_MAX_DISC_ID_LEN];
    bool remove_delete_saves;
    bool remove_delete_disc_copy;
    bool remove_focus_pending;
    /* Saves (#1883): the title the dialog is for, its backups newest first and
       the page shown, the question it asks, and what the last action did. */
    int saves_index;
    char saves_disc[NK_MAX_DISC_ID_LEN];
    NkUiId saves_focus;                 /* focus to give next frame, or 0 */
    bool saves_waiting_pick;            /* Add a save opened the folder picker */
    bool saves_has;
    uint64_t saves_bytes;
    PlayerSaveBackup saves_list[PLAYER_UI_SAVES_LIST_CAP];
    size_t saves_list_count;
    size_t saves_total;
    size_t saves_first;
    PlayerUiSavesRow saves_rows[PLAYER_UI_SAVES_ROW_CAP];
    char saves_when[PLAYER_UI_SAVES_ROW_CAP][32];
    char saves_detail[PLAYER_UI_SAVES_ROW_CAP][64];
    PlayerUiSavesAsk saves_ask;
    size_t saves_ask_row;
    char saves_ask_path[NK_MAX_PATH];   /* the folder a replacing Add a save copies */
    char saves_ask_name[72];
    NkMemstickPlanUnit saves_bring_in;
    char saves_bring_in_problem[160];
    char saves_status[NK_MAX_PATH + 200];
    PlayerUiNoteKind saves_status_kind;
    /* Add to Steam (#2152): the title the dialog is for, what it found (Steam, the
       accounts, the shortcut), the account chosen and what the last add did. */
    int steam_index;
    PlayerSteamPlan steam_plan;
    size_t steam_chosen;
    NkUiId steam_focus;                 /* focus to give next frame, or 0 */
    char steam_account_labels[PLAYER_UI_STEAM_ACCOUNT_CAP][PLAYER_STEAM_NAME_MAX + 16];
    const char *steam_accounts[PLAYER_UI_STEAM_ACCOUNT_CAP];
    char steam_command[NK_MAX_PATH + PLAYER_STEAM_OPTIONS_MAX];
    char steam_blocked[640];
    char steam_status[NK_MAX_PATH + 400];
    PlayerUiNoteKind steam_status_kind;
    /* STOP GAME asks first: the title page the question was asked from. */
    int stop_index;
    char stop_disc[NK_MAX_DISC_ID_LEN];
    bool stop_focus_pending;
    /* The older-build prompt or launch reminder (PLAYER_UI_ROUTE_REBUILD) is
       open; Back closing it answers with the second choice. */
    bool rebuild_pushed;
    bool rebuild_focus_pending;
    char rebuild_text[256];
    char rebuild_notice[96];
    /* the data folder notice, the library file's notice and the drop summary together */
    char library_notice[4u * NK_MAX_PATH + 192u + 192u + 256u + 4u];
    char verify_notice[96];     /* the library's disc check line */
    /* A rebuilding card's status word with its progress ("Rebuilding 40%"),
       or a queued card's with its place ("Queued 2 of 4"). */
    char rebuild_word[PLAYER_UI_TITLE_CAP][32];
    /* A queued card's sentence with its place in the build queue. */
    char queue_detail[PLAYER_UI_TITLE_CAP][160];
    /* The build queue screen (PLAYER_UI_ROUTE_QUEUE): its page, its rows, and
       the game whose button keeps focus after a move or a removal. */
    size_t queue_first;
    PlayerUiQueueRow queue_rows[PLAYER_UI_QUEUE_ROW_CAP];
    char queue_status[PLAYER_UI_QUEUE_ROW_CAP][32];
    char queue_follow_disc[MAX_DISC_ID_LEN];
    NkUiId queue_follow_base;
    bool queue_follow_back;
    char build_queue_line[96];   /* the build card's "2 more games wait..." */
    /* After a removal the library focuses the tile that took its place. */
    bool library_focus_pending;
    size_t library_focus_index;
    /* The titles this frame shows (player_ui_nk_shown_titles). */
    int shown_titles[PLAYER_UI_SHOWN_TITLES_MAX];
    size_t shown_count;
    char cover_data_root[NK_MAX_PATH];
    /* The hidden moment (launcher_secret.h): the code's presses, the moment playing and the
       shapes it draws, the last frame's clock (the moment's handover to Love All), what the
       next frame starts, and whether anything still moves (the loop then draws at the
       display rate). secret_allowed is false while a game starts or runs. */
    LauncherSecretDetector secret_detector;
    LauncherMoment secret_moment;
    LauncherMomentShapes secret_shapes;
    double secret_prev_s;
    bool secret_allowed;
    bool secret_found;
    bool secret_animating;
    PlayerUiRoute secret_last_route;
    int16_t *secret_jingle;          /* the jingle's samples, made on first use */
    size_t secret_jingle_samples;
    /* Love All: whether the list rolls by itself, since when, and a scroll asked for. */
    bool love_all_rolling;
    double love_all_since_s;
    bool love_all_scroll_set;
    float love_all_scroll;
    const LauncherTheme *theme_drawn;  /* the launcher theme the frames are drawn with */
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    bool icon_reported[PLAYER_UI_TITLE_CAP];
    bool cover_reported[PLAYER_UI_TITLE_CAP];
    char cover_cache_reported[256];
    char status_reported[PLAYER_UI_TITLE_CAP][32]; /* the status word last printed */
    char play_reported[PLAYER_UI_TITLE_CAP][16];   /* the play mode badge last printed */
    /* Settings > Enhanced as last printed: the title, mode and sentence, then "|key=value" for each
       part, whose value is at most enhanced_values' 48 bytes. Sized by the part count, so one more
       part cannot cut the line short. */
    char enhanced_reported[PLAYER_PART_COUNT * 64 + 256];
    char enhanced_layout_reported[1024];
    char enhanced_panel_reported[1024]; /* the detail panel's text as last printed */
    char ace_line_reported[PLAYER_PART_COUNT * 64 + 192]; /* the play line last printed */
    char enhancement_tag_reported[PLAYER_UI_TITLE_CAP][16]; /* the ENHANCEMENTS tag printed */
    char preset_note_reported[200];                /* the global preset's page note */
    int disc_reported_view;  /* the game-found card whose rows were printed, or -1 */
#endif
    bool ready;
};

/* The clip reader: PSP_GAME/SND0.AT3 from the user's disc image, read into memory
   only (player_ui_snd0.h); a disc without one is silent. */
static PlayerUiSnd0ReadResult player_ui_read_snd0(const char *iso_path, uint8_t **bytes,
                                                  size_t *size, void *user,
                                                  bool (*cancel)(void *), void *cancel_user) {
    NkIconStatus status;
    (void)user;
    status = nk_iso_read_bounded_entry(iso_path, PLAYER_UI_SND0_PATH,
                                       PLAYER_UI_SND0_MAX_FILE_BYTES, bytes, size,
                                       cancel, cancel_user);
    if (status == NK_ICON_OK) return PLAYER_UI_SND0_READ_OK;
    return status == NK_ICON_ERR_MISSING || status == NK_ICON_ERR_INVALID_PARAM
               ? PLAYER_UI_SND0_READ_MISSING
               : PLAYER_UI_SND0_READ_FAILED;
}

/* The player's art reader: ICON0.PNG and PIC1.PNG from the user's disc image,
   through the ISO reader's own size and header checks. */
static void player_ui_read_disc_art(const char *iso_path, unsigned wanted,
                                    uint8_t **icon, size_t *icon_size,
                                    uint8_t **picture, size_t *picture_size) {
    NkIsoImageData icon_data;
    NkIsoImageData picture_data;
    unsigned iso_wanted = 0u;
    if ((wanted & PLAYER_UI_ART_ICON) != 0u) iso_wanted |= NK_ISO_ART_ICON;
    if ((wanted & PLAYER_UI_ART_PICTURE) != 0u) iso_wanted |= NK_ISO_ART_PICTURE;
    nk_iso_read_game_art(iso_path, iso_wanted, &icon_data, &picture_data);
    *icon = icon_data.status == NK_ICON_OK ? icon_data.data : NULL;
    *icon_size = icon_data.status == NK_ICON_OK ? icon_data.size : 0u;
    if (icon_data.status != NK_ICON_OK) free(icon_data.data);
    *picture = picture_data.status == NK_ICON_OK ? picture_data.data : NULL;
    *picture_size = picture_data.status == NK_ICON_OK ? picture_data.size : 0u;
    if (picture_data.status != NK_ICON_OK) free(picture_data.data);
}

static void player_ui_set_error(char *out, size_t out_size, const char *message) {
    if (out == NULL || out_size == 0u) return;
    snprintf(out, out_size, "%s", message != NULL ? message : NK_UI_STR(ADAPTER_UI_INIT_FAILED));
}

/* The launcher's display rule (the profile folder as "~"), applied to a
   string the page will shorten itself; nk_ui applies it again when it draws. */
static void player_ui_display_in_place(char *text, size_t size) {
    char shown[NK_MAX_PATH];
    if (text == NULL || size == 0u || strlen(text) >= sizeof(shown)) return;
    (void)player_display_text(text, shown, sizeof(shown));
    snprintf(text, size, "%s", shown);
}

/* The path is UTF-8 (SDL_GetBasePath), so it opens through nk_fopen_utf8: the
   narrow fopen reads it in the ANSI code page on Windows (nk_platform.h). */
static bool player_ui_file_exists(const char *path) {
    FILE *file;
    if (path == NULL || path[0] == '\0') return false;
    file = nk_fopen_utf8(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static bool player_ui_join_path(const char *root, const char *relative,
                                char *out, size_t out_size) {
    if (root == NULL || relative == NULL || out == NULL || out_size == 0u) {
        return false;
    }
    char separator = nk_platform_path_separator();
    return nk_shared_path_join(out, out_size, root, relative,
                               separator == '\\' ? '\\' : '/',
                               NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR,
                               NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH);
}

/* The fonts and the music track are read from beside the player only: `make
   player` and every install stage them there. A player never parses a font or a
   track from its working folder, which a shortcut or file association may set to
   any folder (a Downloads folder). The regression build alone, run from the
   source tree without staged assets, reads them from there; a test can turn that
   off to see what a release build does. */
static bool player_ui_assets_from_working_folder(void) {
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    return getenv("NK_UI_TEST_ASSETS_BESIDE_ONLY") == NULL;
#else
    return false;
#endif
}

static bool player_ui_add_font(NkUiTtf *ttf, int font, const char *base_path,
                               const char *relative_path) {
    char path[2u * NK_MAX_PATH];
    if (base_path != NULL &&
        player_ui_join_path(base_path, relative_path, path, sizeof(path)) &&
        player_ui_file_exists(path) && nk_ui_ttf_add_font(ttf, font, path)) {
        return true;
    }
    if (player_ui_assets_from_working_folder() && player_ui_file_exists(relative_path) &&
        nk_ui_ttf_add_font(ttf, font, relative_path)) {
        return true;
    }
    return false;
}

/* The launcher-unavailable reason names the fonts folder the way this
   platform writes paths: assets\fonts\ui on Windows, assets/fonts/ui
   elsewhere (#306). */
static void player_ui_set_fonts_missing(char *error, size_t error_size) {
    char sep = nk_platform_path_separator();
    char message[96];
    snprintf(message, sizeof(message), NK_UI_STR(ADAPTER_UI_FONTS_MISSING),
             sep, sep);
    player_ui_set_error(error, error_size, message);
}

/* The design system's eight font slots (nk_ui_theme.h). Every face falls back
   to Geist for Latin it lacks and to Noto Sans JP for Japanese titles. */
static bool player_ui_register_fonts(NkUiTtf *ttf, const char *base_path,
                                     char *error, size_t error_size) {
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    /* Stands in for a ZIP extracted without its fonts folder. */
    if (getenv("NK_UI_TEST_HIDE_UI_FONTS") != NULL) {
        player_ui_set_fonts_missing(error, error_size);
        return false;
    }
#endif
    for (int font = 0; font < NK_UI_FONT_COUNT; ++font) {
        if (!player_ui_add_font(ttf, font, base_path, nk_ui_font_path(font))) {
            player_ui_set_fonts_missing(error, error_size);
            return false;
        }
    }
    for (int font = 0; font < NK_UI_FONT_COUNT; ++font) {
        if (font == NK_UI_FONT_CJK) continue;
        if ((font != NK_UI_FONT_BODY &&
             !nk_ui_ttf_set_fallback(ttf, font, NK_UI_FONT_BODY)) ||
            !nk_ui_ttf_set_fallback(ttf, font, NK_UI_FONT_CJK)) {
            player_ui_set_error(error, error_size,
                                NK_UI_STR(ADAPTER_UI_FONT_FALLBACK_FAILED));
            return false;
        }
    }
    return true;
}

/* Clay's layout errors are for developers: the developer log gets them, the
   screen never does. */
static void player_ui_report_layout_error(void *user, const NkUiLayoutError *error) {
    int length = error->text_length > 512u ? 512 : (int)error->text_length;
    player_dev_log_add((PlayerApp *)user, "ui", "layout %s (%s): %.*s", error->kind,
                       error->fatal ? "frame dropped" : "frame drawn", length, error->text);
}

/* The launcher music's track: beside the player, as the fonts are (the
   regression build alone also looks in the working folder,
   player_ui_assets_from_working_folder). A track not found is still looked for
   beside the player, so the missing file is named there. */
static PlayerUiMusic *player_ui_music_create_beside(const char *base_path) {
    char path[2u * NK_MAX_PATH];
    const char *track = PLAYER_UI_MUSIC_TRACK;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    /* Stands in for a player folder whose track is missing or damaged. */
    const char *test_track = getenv("NK_UI_TEST_MUSIC_FILE");
    /* A decode that waits until it is cancelled, and an audio device that stops
       taking data once the music fades out. */
    player_ui_music_test_hold_decode(getenv("NK_UI_TEST_MUSIC_HOLD_DECODE") != NULL);
    player_ui_music_test_stall_release(getenv("NK_UI_TEST_MUSIC_STALL") != NULL);
    if (test_track != NULL && test_track[0] != '\0') {
        return player_ui_music_create(test_track, PLAYER_UI_MUSIC_TRACK);
    }
#endif
    if (base_path != NULL && player_ui_join_path(base_path, track, path, sizeof(path)) &&
        player_ui_file_exists(path)) {
        return player_ui_music_create(path, track);
    }
    if (player_ui_assets_from_working_folder() && player_ui_file_exists(track)) {
        return player_ui_music_create(track, track);
    }
    if (base_path != NULL && player_ui_join_path(base_path, track, path, sizeof(path))) {
        return player_ui_music_create(path, track);
    }
    /* No folder beside the player to name: the fonts failed the same way first. */
    return player_ui_assets_from_working_folder() ? player_ui_music_create(track, track)
                                                  : NULL;
}

bool player_ui_nk_built_with_launcher(void) {
    return nk_ui_sdlr_available() && nk_ui_ttf_available();
}

PlayerUiNk *player_ui_nk_create(SDL_Renderer *renderer, SDL_Window *window, PlayerApp *app,
                                char *error, size_t error_size) {
    PlayerUiNk *player;
    const char *base_path;
    NkUiConfig config;
    if (renderer == NULL) {
        player_ui_set_error(error, error_size,
                            NK_UI_STR(ADAPTER_UI_RENDERER_UNAVAILABLE));
        return NULL;
    }
    if (!nk_ui_sdlr_available() || !nk_ui_ttf_available()) {
        player_ui_set_error(error, error_size,
                            NK_UI_STR(ADAPTER_UI_RENDERER_DEPENDENCIES));
        return NULL;
    }
    player = (PlayerUiNk *)calloc(1u, sizeof(*player));
    if (player == NULL) {
        player_ui_set_error(error, error_size,
                            NK_UI_STR(ADAPTER_UI_RENDERER_STATE_FAILED));
        return NULL;
    }
    player->enhanced_tab = PLAYER_UI_ENHANCED_GROUP_PICTURE;
    player->window = window;
    /* A launcher that opens behind another window gets no focus event: the music
       waits for focus (SDL_EVENT_WINDOW_FOCUS_GAINED) rather than assuming it. */
    player->window_unfocused =
        window != NULL && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) == 0;
    player->rest_index = SIZE_MAX;
    player->backdrop_index = SIZE_MAX;
    player->router =nk_ui_router_create();
    if (player->router == NULL) {
        player_ui_set_error(error, error_size,
                            NK_UI_STR(ADAPTER_UI_ROUTER_FAILED));
        player_ui_nk_destroy(player);
        return NULL;
    }
    player->ttf = nk_ui_ttf_create(NULL, 0u);
    if (player->ttf == NULL) {
        player_ui_set_error(error, error_size, nk_ui_ttf_last_error());
        player_ui_nk_destroy(player);
        return NULL;
    }
    base_path = SDL_GetBasePath();
    if (!player_ui_register_fonts(player->ttf, base_path, error, error_size)) {
        player_ui_nk_destroy(player);
        return NULL;
    }
    player->text = nk_ui_ttf_backend(player->ttf);
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &player->text;
    /* The launcher's display rules, applied to every string it draws: the
       profile folder shows as "~". */
    config.display_text = player_display_text;
    config.report_error = player_ui_report_layout_error;
    config.report_user = app;
    player->ui = nk_ui_create(&config);
    if (player->ui == NULL) {
        player_ui_set_error(error, error_size,
                            NK_UI_STR(ADAPTER_UI_LAYOUT_FAILED));
        player_ui_nk_destroy(player);
        return NULL;
    }
    player->backend = nk_ui_sdlr_create(renderer, &player->text);
    if (player->backend == NULL) {
        player_ui_set_error(error, error_size,
                            NK_UI_STR(ADAPTER_UI_BACKEND_FAILED));
        player_ui_nk_destroy(player);
        return NULL;
    }
    /* Glyphs, art and sounds are extras: a failure leaves the geometric
       prompts, the placeholders and silence. */
    (void)nk_ui_prompt_images_load(&player->prompt_images);
    nk_ui_set_prompt_images(player->ui, &player->prompt_images);
    player->art = player_ui_art_create(player_ui_read_disc_art);
    if (!nk_platform_resolve_path(NK_PATH_DATA, player->cover_data_root,
                                  sizeof(player->cover_data_root))) {
        player->cover_data_root[0] = '\0';
    }
    player->sounds = player_ui_sounds_create();
    player->music = player_ui_music_create_beside(base_path);
    player->music_reported = PLAYER_UI_MUSIC_OFF;
    player->snd0 = player_ui_snd0_create(player_ui_read_snd0, NULL);
    player->snd0_index = SIZE_MAX;
    player->snd0_reported = PLAYER_UI_SND0_OFF;
    player->pending_sound = -1;
    player->last_view = VIEW_LIBRARY;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    player->disc_reported_view = -1;
    {
        /* NK_UI_TEST_ART_WAIT_PROBE=<iso path>: the host loop's wait with the art
           loader idle, then with a read of that image waiting to start. */
        const char *probe = getenv("NK_UI_TEST_ART_WAIT_PROBE");
        if (probe != NULL && probe[0] != '\0') {
            const NkUiImage *probe_icon = NULL;
            uint32_t idle_wait = player_ui_nk_wait_ms(player, 50u);
            player_ui_art_get(player->art, probe, false, &probe_icon, NULL);
            printf("[PLAYER_UI_TEST] art_wait idle_ms=%u busy=%d busy_ms=%u\n",
                   (unsigned)idle_wait, player_ui_art_busy(player->art) ? 1 : 0,
                   (unsigned)player_ui_nk_wait_ms(player, 50u));
            fflush(stdout);
        }
    }
#endif
    nk_ui_sdl_input_init(&player->input);
    player_ui_version_line(NK_PLAYER_VERSION, (long long)NK_PLAYER_BUILD_EPOCH,
                           player->version_line, sizeof(player->version_line));
    {
        char data_root[NK_MAX_PATH];
        if (nk_platform_resolve_app_data_dir(data_root, sizeof(data_root))) {
            (void)snprintf(player->data_root_line, sizeof(player->data_root_line),
                           NK_UI_STR(ADAPTER_DATA_FOLDER_PATH),
                           nk_portable_in_use() ? NK_UI_STR(ADAPTER_DATA_FOLDER_PORTABLE)
                                                : NK_UI_STR(ADAPTER_DATA_FOLDER),
                           data_root);
        }
    }
    player->ready = true;
    if (error != NULL && error_size > 0u) error[0] = '\0';
    return player;
}

void player_ui_nk_destroy(PlayerUiNk *player) {
    if (player == NULL) return;
    player_ui_sign_in_wipe(&player->ra_form);
    /* The editor's buffer may still hold a password typed into it. */
    {
        volatile char *text = player->nickname_text;
        for (size_t i = 0u; i < sizeof(player->nickname_text); ++i) text[i] = '\0';
    }
    player_ui_snd0_destroy(player->snd0);
    player_ui_music_destroy(player->music);
    player_ui_sounds_destroy(player->sounds);
    player_ui_art_destroy(player->art);
    nk_ui_sdlr_destroy(player->backend);
    nk_ui_destroy(player->ui);
    nk_ui_prompt_images_free(&player->prompt_images);
    nk_ui_ttf_destroy(player->ttf);
    nk_ui_router_destroy(player->router);
    free(player->secret_jingle);
    free(player);
}

void player_ui_nk_invalidate_cover_art(PlayerUiNk *player) {
    if (player == NULL) return;
    player_ui_art_invalidate_covers(player->art);
    for (size_t i = 0u; i < PLAYER_UI_TITLE_CAP; ++i) {
        player->titles[i].cover = NULL;
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    memset(player->cover_reported, 0, sizeof(player->cover_reported));
#endif
}

bool player_ui_nk_cover_art_reading(const PlayerUiNk *player) {
    return player != NULL && player_ui_art_reading_cover(player->art);
}

static bool player_ui_disc_view(PlayerView view) {
    return view == VIEW_INSPECTING || view == VIEW_SUPPORTED_TITLE ||
           view == VIEW_EXPERIMENTAL_TITLE || view == VIEW_UNSUPPORTED_TITLE;
}

static bool player_ui_settings_view(PlayerView view) {
    return view == VIEW_SETTINGS || view == VIEW_ONLINE_SETTINGS ||
           view == VIEW_ONLINE_PLAY_SETTINGS || view == VIEW_PREREQ_ABOUT ||
           view == VIEW_CONFIRM_REMOVE_TOOLS || view == VIEW_BUILD_CACHE ||
           view == VIEW_PSP_FONTS || view == VIEW_UPDATES ||
           view == VIEW_CONFIRM_FACTORY_RESET || view == VIEW_ACCESSIBILITY_SETTINGS ||
           view == VIEW_ACE_SETTINGS || view == VIEW_PROBLEM_REPORT ||
           view == VIEW_ONLINE_PLAY_CARD || view == VIEW_ONLINE_PLAY_SERVERS ||
           view == VIEW_ACHIEVEMENTS_CARD || view == VIEW_ACHIEVEMENTS_SIGN_IN ||
           view == VIEW_CHEAT_DB || view == VIEW_CHEAT_DB_BUSY || view == VIEW_CHEAT_DB_CONFIRM;
}

/* The views the next UI draws: the library (router screens), the setup wizard,
   build, build tool download and error screens a first run passes through, the
   card a disc gets when it is added, Settings with the controller settings, and
   the developer log. */
static bool player_ui_nk_flow_view(PlayerView view) {
    return view == VIEW_SETUP_WIZARD || view == VIEW_BUILDING_PACKAGE ||
           view == VIEW_PREREQ_CONSENT ||
           view == VIEW_PREREQ_PROGRESS || view == VIEW_ERROR ||
           player_ui_disc_view(view) || player_ui_settings_view(view) ||
           view == VIEW_CONTROLLER_SETTINGS || view == VIEW_DEVELOPER_LOG;
}

bool player_ui_nk_draws_view(int view_value) {
    PlayerView view = (PlayerView)view_value;
    return view == VIEW_LIBRARY || view == PLAYER_VIEW_READY_LIBRARY ||
           player_ui_nk_flow_view(view);
}

bool player_ui_nk_view_active(const PlayerUiNk *player, const PlayerApp *app) {
    return player != NULL && player->ready && app != NULL &&
           player_ui_nk_draws_view(app->active_view);
}

void player_ui_nk_hold_clock(PlayerUiNk *player, double now_s) {
    if (player == NULL || !isfinite(now_s)) return;
    player->clock_held = true;
    player->held_clock_s = now_s;
}

/* The clock the launcher draws with: the time since SDL started, or the held time. */
static double player_ui_nk_now_s(const PlayerUiNk *player) {
    if (player != NULL && player->clock_held) return player->held_clock_s;
    return (double)SDL_GetTicksNS() / 1000000000.0;
}

uint32_t player_ui_nk_wait_ms(const PlayerUiNk *player, uint32_t idle_ms) {
    double now_s = player_ui_nk_now_s(player);
    double wait_s;
    if (player == NULL) {
        return idle_ms;
    }
    /* Rounded up: a frame drawn a millisecond early would find nothing due. */
    wait_s = nk_ui_wait_s(player->ui, now_s, (double)idle_ms / 1000.0);
    /* A rest still under way wakes the loop when it ends. Once it has ended the
       picture's read is the art worker's: the loop polls about once a frame only
       while a read runs or waits to start, so a disc with no picture never keeps
       it from sleeping. */
    if (player->rest_index != SIZE_MAX && player->rest_index != player->backdrop_index) {
        double rest_s = player->rest_since_s + NK_UI_MOTION_ART_REST - now_s;
        if (rest_s > 0.0 && rest_s < wait_s) wait_s = rest_s;
    }
    /* The hidden moment and Love All's roll move every frame. */
    if (player->secret_animating && wait_s > NK_UI_MOTION_FRAME) wait_s = NK_UI_MOTION_FRAME;
    /* A game page's clip: a frame while it loads or fades. */
    {
        uint32_t snd0_ms = player_ui_snd0_wait_ms(player->snd0, (uint32_t)ceil(wait_s * 1000.0));
        if ((double)snd0_ms / 1000.0 < wait_s) wait_s = (double)snd0_ms / 1000.0;
    }
    /* Each read serves one disc and is collected on a frame; without this a
       library page filled in one card per idle wake (audit 2026-11-17). */
    return player_ui_art_wait_ms(player->art, (uint32_t)ceil(wait_s * 1000.0));
}

static void player_ui_format_size(char *out, size_t out_size, uint64_t bytes) {
    nk_shared_format_size(out, out_size, bytes);
}

static const char *player_ui_resolution_word(int scale) {
    switch (scale) {
        case 1: return NK_UI_STR(ADAPTER_RESOLUTION_NATIVE);
        case 2: return NK_UI_STR(ADAPTER_RESOLUTION_2X);
        case 3: return NK_UI_STR(ADAPTER_RESOLUTION_3X);
        case 4: return NK_UI_STR(ADAPTER_RESOLUTION_4X);
        case 8: return NK_UI_STR(ADAPTER_RESOLUTION_8X);
        default: return NK_UI_STR(ADAPTER_RESOLUTION_CUSTOM);
    }
}

/* A check's status word colour: only a check that keeps the disc out of the
   library is an error; anything else still to do is a warning (#1302). */
static uint32_t player_ui_check_rgba(const NkUiTheme *theme, const PlayerPreflightCheck *check) {
    if (check->status == PREFLIGHT_OK) return theme->ready;
    return player_preflight_check_blocks(check) ? theme->error : theme->warning;
}

static void player_ui_check_row(PlayerUiCheckRow *row, const NkUiTheme *theme,
                                const PlayerPreflightCheck *check) {
    row->name = player_preflight_label(check);
    row->message = player_preflight_plain_message(check);
    row->status = player_preflight_status_word(check);
    row->rgba = player_ui_check_rgba(theme, check);
}

/* The files a disc still needs (#308), from the program input its last check
   found. Only an encrypted disc still short of decrypted copies gets a guide;
   the names and folder point into that report, which outlives the frame. */
static void player_ui_input_guide(PlayerUiInputGuide *guide, const PlayerInputReport *input,
                                  char *summary, size_t summary_size) {
    size_t more;
    memset(guide, 0, sizeof(*guide));
    if (input == NULL || input->input_class != PLAYER_INPUT_ENCRYPTED_RETAIL ||
        input->file_count == 0u) {
        return;
    }
    for (size_t i = 0u; i < input->file_count && i < PLAYER_UI_INPUT_FILES; ++i) {
        guide->files[i].name = input->files[i].name;
        guide->files[i].source = input->files[i].source;
        guide->files[i].replace = input->files[i].state == PLAYER_INPUT_FILE_UNUSABLE;
        guide->file_count++;
    }
    more = input->files_needed > guide->file_count ? input->files_needed - guide->file_count : 0u;
    if (more > 0u) {
        snprintf(summary, summary_size, NK_UI_STR(ADAPTER_ENCRYPTED_FILES_READY_MORE),
                 (unsigned)input->encrypted_ready, (unsigned)input->encrypted_total,
                 (unsigned)more);
    } else {
        snprintf(summary, summary_size, NK_UI_STR(ADAPTER_ENCRYPTED_FILES_READY),
                 (unsigned)input->encrypted_ready, (unsigned)input->encrypted_total);
    }
    guide->summary = summary;
    guide->folder = input->folder;
}

/* The state's outcome kind as the screens' one; the two lists are in the same order. */
static PlayerUiNoteKind player_ui_note_kind(PlayerNoteKind kind) {
    switch (kind) {
        case PLAYER_NOTE_DONE: return PLAYER_UI_NOTE_DONE;
        case PLAYER_NOTE_WARNING: return PLAYER_UI_NOTE_WARNING;
        case PLAYER_NOTE_ERROR: return PLAYER_UI_NOTE_ERROR;
        case PLAYER_NOTE_NEUTRAL:
        default: return PLAYER_UI_NOTE_NEUTRAL;
    }
}

/* The PSP font lines the wizard's fonts step and Settings > PSP fonts list:
   Japanese, each Latin index, then Korean, up to `cap` lines. */
static size_t player_ui_font_lines(const PlayerApp *app, const char **lines, size_t cap) {
    size_t count = 0u;
    for (int item = 0; item < NK_FONT_LATIN_INDEX_COUNT + 2 && count < cap; ++item) {
        lines[count++] = item == 0
            ? app->wizard.font_slot_detail[NK_FONT_SLOT_JAPANESE]
            : item == NK_FONT_LATIN_INDEX_COUNT + 1
                ? app->wizard.font_slot_detail[NK_FONT_SLOT_KOREAN]
                : app->wizard.font_latin_index_detail[item - 1];
    }
    return count;
}

/* The setup wizard's model, from the player's wizard state. */
static bool player_ui_unrecorded_experimental(const GameRecord *game);

static PlayerUiAction player_ui_render_wizard(PlayerUiNk *player, PlayerApp *app,
                                              NkUiScale scale) {
    const NkUiTheme *theme = nk_ui_theme(player->ui);
    const GameRecord *game = &app->inspecting_game;
    PlayerUiWizardModel model;
    char size[48];
    memset(&model, 0, sizeof(model));
    model.host = nk_platform_host_kind();
    model.step = (int)app->wizard.step;
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    model.disc_id = game->disc_id;
    model.title_name = game->title_name;
    model.online_mode = app->settings.online_mode;
    model.developer_mode = app->settings.developer_mode;
    switch (app->wizard.step) {
        case WIZARD_STEP_SELECT_GAME:
            model.has_iso = game->iso_path[0] != '\0';
            model.iso_selected = app->wizard.iso_selected;
            /* A compressed image shows the file the user picked. */
            model.iso_path = game->source_image_path[0] ? game->source_image_path
                                                        : game->iso_path;
            player_ui_format_size(size, sizeof(size), game->iso_size_bytes);
            snprintf(player->wizard_detail, sizeof(player->wizard_detail),
                     NK_UI_STR(ADAPTER_DISC_SIZE_AND_ID), size,
                     game->disc_id[0] != '\0' ? game->disc_id : NK_UI_STR(ADAPTER_DISC_INSPECTED));
            model.iso_detail = player->wizard_detail;
            model.status_message = app->wizard.status_message;
            break;
        case WIZARD_STEP_INSPECT_VERIFY:
            model.extracting = app->wizard.is_extracting;
            model.converting_package = app->psn_conversion_active;
            model.extraction_complete = app->wizard.extraction_complete;
            model.extraction_failed = app->wizard.extraction_failed;
            model.experimental = game->is_experimental;
            model.recorded = game->is_experimental && !player_ui_unrecorded_experimental(game);
            model.supported = !game->is_experimental &&
                              (app->wizard.extraction_complete ||
                               game->status == NK_STATUS_VERIFIED);
            model.files_extracted = app->wizard.files_extracted;
            model.total_files = app->wizard.total_files;
            model.extraction_percent = app->wizard.extraction_percent;
            model.current_file = app->wizard.extraction_current_file;
            model.extraction_error = player_plain_reason(app->wizard.extraction_error);
            if (game->is_experimental) {
                const PlayerPreflightCheck *blocking =
                    player_preflight_blocking_check(&app->wizard.preflight);
                if (blocking != NULL) {
                    snprintf(player->wizard_blocked, sizeof(player->wizard_blocked),
                             NK_UI_STR(ADAPTER_DISC_REOPEN_COMPATIBILITY),
                             player_preflight_plain_message(blocking));
                    model.blocked_reason = player->wizard_blocked;
                    player_ui_input_guide(&model.input_guide, &app->wizard.preflight.input,
                                          player->input_summary,
                                          sizeof(player->input_summary));
                }
            }
            /* Plain names and reasons; the codes stay in the developer log (#1280).
               The experimental row is what the card's badge and note already say. */
            for (size_t i = 0u; i < app->wizard.preflight.count &&
                                model.check_count < PLAYER_UI_WIZARD_CHECKS; ++i) {
                const PlayerPreflightCheck *check = &app->wizard.preflight.checks[i];
                if (strcmp(check->code, "EXPERIMENTAL") == 0) continue;
                player_ui_check_row(&model.checks[model.check_count++], theme, check);
            }
            break;
        case WIZARD_STEP_SYSTEM_FONTS:
            if (app->wizard.font_slot_detail[0][0] == '\0') player_app_refresh_font_status(app);
            snprintf(player->wizard_resolution, sizeof(player->wizard_resolution),
                     NK_UI_STR(ADAPTER_DISC_RESOLUTION),
                     player_ui_resolution_word(app->settings.resolution_scale));
            snprintf(player->wizard_controller, sizeof(player->wizard_controller),
                     NK_UI_STR(ADAPTER_DISC_CONTROLLER),
                     app->settings.controller_connected ? app->settings.controller_name
                                                        : NK_UI_STR(ADAPTER_KEYBOARD_AND_MOUSE));
            model.resolution_line = player->wizard_resolution;
            model.controller_line = player->wizard_controller;
            model.font_line_count = player_ui_font_lines(
                app, model.font_lines, sizeof(model.font_lines) / sizeof(model.font_lines[0]));
            model.font_message = app->wizard.font_message;
            model.font_message_kind = player_ui_note_kind(app->wizard.font_message_kind);
            model.font_summary = app->wizard.font_summary;
            model.fonts_all_own = app->wizard.font_own_count >= NK_FONT_PSP_FONT_COUNT;
            break;
        case WIZARD_STEP_READY_LAUNCH:
            model.runtime_ready = game->is_prepared;
            snprintf(player->wizard_summary, sizeof(player->wizard_summary),
                     NK_UI_STR(ADAPTER_DISC_SUMMARY),
                     player_ui_resolution_word(app->settings.resolution_scale),
                     app->settings.fullscreen ? NK_UI_STR(ADAPTER_FULLSCREEN) : NK_UI_STR(ADAPTER_WINDOWED),
                     app->settings.master_volume, game->is_prepared
                         ? NK_UI_STR(ADAPTER_PACKAGE_READY) : NK_UI_STR(ADAPTER_PACKAGE_NOT_BUILT));
            model.summary_line = player->wizard_summary;
            break;
        case WIZARD_STEP_WELCOME:
        default:
            break;
    }
    return player_ui_screen_wizard(player->ui, &model);
}

/* The game-found card's model: the inspected disc and its checks. The check
   that keeps a disc out is the card's reason, not one of its rows. */
static PlayerUiAction player_ui_render_disc(PlayerUiNk *player, PlayerApp *app,
                                            NkUiScale scale, double now_s) {
    const NkUiTheme *theme = nk_ui_theme(player->ui);
    const GameRecord *game = &app->inspecting_game;
    const PlayerCompatibilityPreflight *preflight = &app->wizard.preflight;
    const PlayerPreflightCheck *blocking = player_preflight_blocking_check(preflight);
    PlayerUiDiscModel model;
    memset(&model, 0, sizeof(model));
    switch (app->active_view) {
        case VIEW_INSPECTING: model.kind = PLAYER_UI_DISC_CHECKING; break;
        case VIEW_SUPPORTED_TITLE: model.kind = PLAYER_UI_DISC_SUPPORTED; break;
        case VIEW_EXPERIMENTAL_TITLE:
            /* A disc on the compatibility list is found, not untested, as its
               title page's PLAYABLE says. */
            model.kind = blocking != NULL ? PLAYER_UI_DISC_BLOCKED
                : player_ui_unrecorded_experimental(game) ? PLAYER_UI_DISC_UNTESTED
                : PLAYER_UI_DISC_SUPPORTED;
            break;
        case VIEW_UNSUPPORTED_TITLE:
        default: model.kind = PLAYER_UI_DISC_UNSUPPORTED; break;
    }
    model.title_name = game->title_name;
    model.disc_id = game->disc_id;
    model.badge = nk_disc_id_is_non_retail_key(game->disc_id) ? NK_UI_STR(ADAPTER_BADGE_UNOFFICIAL) : NULL;
    model.iso_path = game->source_image_path[0] ? game->source_image_path : game->iso_path;
    if (model.kind == PLAYER_UI_DISC_BLOCKED) {
        snprintf(player->disc_reason, sizeof(player->disc_reason),
                 NK_UI_STR(ADAPTER_DISC_REOPEN), player_preflight_plain_message(blocking));
        model.reason = player->disc_reason;
        player_ui_input_guide(&model.input_guide, &app->wizard.preflight.input,
                              player->input_summary, sizeof(player->input_summary));
    }
    if ((model.kind == PLAYER_UI_DISC_SUPPORTED || model.kind == PLAYER_UI_DISC_UNSUPPORTED) &&
        player_app_title_profile_refusal_for_disc(app, game->disc_id, player->disc_profile,
                                                  sizeof(player->disc_profile))) {
        model.profile_details = player->disc_profile;
        if (model.kind == PLAYER_UI_DISC_SUPPORTED) {
            model.profile_note = NK_UI_STR(ADAPTER_DISC_PROFILE_FALLBACK);
        }
    }
    if (model.kind != PLAYER_UI_DISC_CHECKING) {
        for (size_t i = 0u; i < preflight->count && model.check_count < PLAYER_UI_DISC_CHECKS;
             ++i) {
            const PlayerPreflightCheck *check = &preflight->checks[i];
            /* The badge says whether the game was tested, and a disc read is
               only worth a row when it went wrong. */
            if (check == blocking || strcmp(check->code, "EXPERIMENTAL") == 0 ||
                (strcmp(check->code, "DISC_SFO") == 0 && check->status == PREFLIGHT_OK)) {
                continue;
            }
            player_ui_check_row(&model.checks[model.check_count++], theme, check);
        }
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (player->disc_reported_view != (int)app->active_view) {
        player->disc_reported_view = (int)app->active_view;
        printf("[PLAYER_UI_TEST] next_ui_disc kind=%d rows=%zu add=%d files=%zu\n",
               (int)model.kind, model.check_count,
               model.kind == PLAYER_UI_DISC_SUPPORTED || model.kind == PLAYER_UI_DISC_UNTESTED,
               model.input_guide.file_count);
        for (size_t i = 0u; i < model.input_guide.file_count; ++i) {
            printf("[PLAYER_UI_TEST] next_ui_disc_file name=%s replace=%d source=%s\n",
                   model.input_guide.files[i].name, model.input_guide.files[i].replace ? 1 : 0,
                   model.input_guide.files[i].source);
        }
        printf("[PLAYER_UI_TEST] next_ui_disc_badge disc=%s badge=%s\n", game->disc_id,
               model.badge != NULL ? model.badge : "Your disc");
        for (size_t i = 0u; i < model.check_count; ++i) {
            printf("[PLAYER_UI_TEST] next_ui_disc_row status=%s tone=%s label=%s message=%s\n",
                   model.checks[i].status,
                   model.checks[i].rgba == theme->error ? "error"
                       : model.checks[i].rgba == theme->ready ? "ready" : "warning",
                   model.checks[i].name, model.checks[i].message);
        }
        fflush(stdout);
    }
#endif
    model.now_s = player_app_reduce_motion(app) ? 0.0 : now_s;
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    return player_ui_screen_disc(player->ui, &model);
}

/* PlayerUiUpdateStep follows PlayerUpdatePhase value for value. */
typedef char player_ui_update_step_order[
    (PLAYER_UI_UPDATE_CHECKING == (int)PLAYER_UPDATE_CHECKING &&
     PLAYER_UI_UPDATE_DOWNLOADED == (int)PLAYER_UPDATE_DOWNLOADED &&
     PLAYER_UI_UPDATE_FAILED == (int)PLAYER_UPDATE_FAILED) ? 1 : -1];

/* The refresh rate of the display the launcher window is on, in Hz; 0 when SDL does not say.
   It is the launcher's best answer before a game window exists. */
static double player_ui_display_refresh_hz(SDL_Window *window) {
    const SDL_DisplayMode *mode;
    if (window == NULL) return 0.0;
    mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    return mode != NULL && mode->refresh_rate > 0.0f ? (double)mode->refresh_rate : 0.0;
}

typedef char player_ui_enhanced_rows_fit[PLAYER_PART_COUNT <= PLAYER_UI_ENHANCED_ROWS ? 1 : -1];
typedef struct {
    int part;
    PlayerUiEnhancedGroup group;
} PlayerUiEnhancedLayout;
#define PLAYER_UI_ENHANCED_LAYOUT_ENTRY(part, group) {part, group},
static const PlayerUiEnhancedLayout k_player_ui_enhanced_layout[] = {
    PLAYER_UI_ENHANCED_LAYOUT(PLAYER_UI_ENHANCED_LAYOUT_ENTRY)
};
#undef PLAYER_UI_ENHANCED_LAYOUT_ENTRY
typedef char player_ui_enhanced_layout_covers_every_part[
    sizeof(k_player_ui_enhanced_layout) / sizeof(k_player_ui_enhanced_layout[0]) ==
            PLAYER_PART_COUNT
        ? 1 : -1];
/* The part rows' element ids (PLAYER_UI_ID_ENHANCED_PART): each range ends before the next id in
   use, and the two hold every row. */
typedef char player_ui_enhanced_row_ids_fit[
    PLAYER_UI_ID_SETTINGS_ENHANCED_PART + PLAYER_UI_ENHANCED_FIRST_IDS <= PLAYER_UI_ID_SETTINGS_PROBLEM_REPORT
        ? 1 : -1];
typedef char player_ui_enhanced_more_row_ids_fit[
    PLAYER_UI_ID_SETTINGS_ENHANCED_PART_MORE + PLAYER_UI_ENHANCED_MORE_IDS <= PLAYER_UI_ID_DEVELOPER_LOG_COPY &&
            PLAYER_UI_ID_CONTROLLER_GLYPH_BASE + PLAYER_UI_ID_CONTROLLER_GLYPH_BASE_SPAN <=
                PLAYER_UI_ID_SETTINGS_ENHANCED_PART_MORE &&
            PLAYER_UI_ENHANCED_ROWS <= PLAYER_UI_ENHANCED_FIRST_IDS + PLAYER_UI_ENHANCED_MORE_IDS
        ? 1 : -1];

/* Why a title offers no Ace: it has no usable package of its own (#2055). */
#define PLAYER_UI_ACE_NEEDS_PACKAGE NK_UI_STR(ADAPTER_ACE_PACKAGE_REQUIRED)

static int player_ui_enhanced_value_index(int part, int value) {
    const PlayerEnhancedPartInfo *info = player_enhanced_part(part);
    for (int i = 0; info && i < info->value_count; ++i) {
        if (info->values[i] == value) return i;
    }
    return 0;
}

/* The part table says which parts act only in Ace (PlayerEnhancedPartInfo.ace_only). */
static const char *player_ui_enhanced_tag(int part) {
    const PlayerEnhancedPartInfo *info = player_enhanced_part(part);
    return info != NULL && info->ace_only ? NK_UI_STR(ADAPTER_ACE_ONLY) : NULL;
}

static const char *player_ui_play_mode_name(PlayerPlayMode mode) {
    switch (mode) {
        case PLAYER_PLAY_AUTHENTIC: return NK_UI_STR(ADAPTER_MODE_AUTHENTIC);
        case PLAYER_PLAY_ENHANCED: return NK_UI_STR(ADAPTER_MODE_ENHANCED);
        case PLAYER_PLAY_ACE: return NK_UI_STR(ADAPTER_MODE_ACE);
        default: return NK_UI_STR(ADAPTER_MODE_LIBRARY_DEFAULT);
    }
}

/* Settings > Enhanced (#2055): the library's page, or one title's
   (player_app_open_title_enhanced). The explicit layout table gives every part its visual group. */
static void player_ui_fill_enhanced(PlayerUiNk *player, const PlayerApp *app,
                                    PlayerUiSettingsModel *model) {
    const PlayerSettings *settings = &app->settings;
    int title = player_app_enhanced_page_title(app);
    bool library_on = settings->enhanced.library == PLAYER_PLAY_ENHANCED;
    model->enhanced_library_on = library_on;
    model->enhanced_title = NULL;
    model->enhanced_on = library_on;
    model->enhanced_mode_index = library_on ? 1 : 0;
    model->enhanced_mode_sentence =
        NK_UI_STR(ADAPTER_MODE_LIBRARY_DESCRIPTION);
    player->enhanced_modes[0] = player_ui_play_mode_name(PLAYER_PLAY_AUTHENTIC);
    player->enhanced_modes[1] = player_ui_play_mode_name(PLAYER_PLAY_ENHANCED);
    model->enhanced_modes = player->enhanced_modes;
    model->enhanced_mode_count = 2;
    model->enhanced_mode = model->enhanced_modes[model->enhanced_mode_index];
    bool logic_60 = false;
    bool smooth_on = player_settings_library_part(settings, PLAYER_PART_SMOOTH) != 0;
    PlayerPlayMode chosen = PLAYER_PLAY_LIBRARY;
    if (title >= 0) {
        const GameRecord *game = &app->games[title];
        chosen = player_app_title_play_mode(app, title);
        PlayerPlayResolved play;
        player_play_resolve(settings, game->disc_id, &play);
        /* What its launch runs (player_launch_work_run): with its 60 FPS game logic, smooth
           motion is off. */
        player_play_apply_logic_60(&play, player_app_title_60hz_on(app, title));
        logic_60 = play.logic_60;
        smooth_on = play.part[PLAYER_PART_SMOOTH] != 0;
        snprintf(player->enhanced_title, sizeof(player->enhanced_title), "%s",
                 game->title_name[0] ? game->title_name : game->disc_id);
        model->enhanced_title = player->enhanced_title;
        model->enhanced_on = play.on;
        /* A title's page steps Library, Authentic, Enhanced and (with a package) Ace. */
        snprintf(player->enhanced_mode, sizeof(player->enhanced_mode), NK_UI_STR(ADAPTER_MODE_LIBRARY_VALUE),
                 model->enhanced_mode);
        model->enhanced_mode = chosen == PLAYER_PLAY_LIBRARY ? player->enhanced_mode
                                                             : player_ui_play_mode_name(chosen);
        player->enhanced_modes[0] = player->enhanced_mode;
        player->enhanced_modes[1] = player_ui_play_mode_name(PLAYER_PLAY_AUTHENTIC);
        player->enhanced_modes[2] = player_ui_play_mode_name(PLAYER_PLAY_ENHANCED);
        player->enhanced_modes[3] = player_ui_play_mode_name(PLAYER_PLAY_ACE);
        model->enhanced_mode_count = app->title_enhancements[title].available ? 4 : 3;
        /* This stepper follows PlayerPlayMode order, beginning at Library. */
        model->enhanced_mode_index = chosen >= PLAYER_PLAY_LIBRARY &&
            (int)chosen < model->enhanced_mode_count
            ? (int)chosen : -1;
        model->enhanced_mode_sentence = app->title_enhancements[title].available
            ? NK_UI_STR(ADAPTER_MODE_ACE_DESCRIPTION)
            : NK_UI_STR(ADAPTER_MODE_ENHANCED_DESCRIPTION);
        /* The title's texture pack (contract 10.12): its folder's name and what it lists, as
           read when the page opened or the pack changed. */
        const char *pack = player_settings_title_pack(settings, game->disc_id);
        const PlayerTexturePackInfo *info = player_app_title_pack_info(app, title);
        model->texpack_row = true;
        model->texpack_chosen = pack != NULL;
        if (!pack) {
            snprintf(player->texpack_value, sizeof(player->texpack_value), "%s",
                     NK_UI_STR(ADAPTER_TEXPACK_NONE));
            snprintf(player->texpack_sentence, sizeof(player->texpack_sentence),
                     NK_UI_STR(ADAPTER_TEXPACK_CHOOSE));
        } else if (info && info->ok) {
            snprintf(player->texpack_value, sizeof(player->texpack_value), NK_UI_STR(ADAPTER_TEXPACK_COUNT),
                     info->name, info->textures,
                     info->textures == 1u ? NK_UI_STR(ADAPTER_TEXPACK_TEXTURE)
                                          : NK_UI_STR(ADAPTER_TEXPACK_TEXTURES));
            snprintf(player->texpack_sentence, sizeof(player->texpack_sentence),
                     NK_UI_STR(ADAPTER_TEXPACK_IN_USE));
        } else {
            snprintf(player->texpack_value, sizeof(player->texpack_value), "%s",
                     NK_UI_STR(ADAPTER_TEXPACK_REFUSED));
            snprintf(player->texpack_sentence, sizeof(player->texpack_sentence),
                     NK_UI_STR(ADAPTER_TEXPACK_REFUSAL_DETAILS),
                     info && info->error[0] ? info->error : NK_UI_STR(ADAPTER_TEXPACK_UNREAD));
        }
        model->texpack_value = player->texpack_value;
        model->texpack_sentence = player->texpack_sentence;
    }
    model->enhanced_tab = player->enhanced_tab;
    model->enhanced_row_count = 0;
    /* The library's value as a session uses it: the graphics preset's where it sets the
       part (#2150), which the library page names, "On (Ultra)". */
    const PlayerGraphicsPresetRow *preset =
        player_graphics_preset_row(player_app_graphics_preset(app));
    for (size_t layout_index = 0;
         layout_index < sizeof(k_player_ui_enhanced_layout) / sizeof(k_player_ui_enhanced_layout[0]);
         layout_index++) {
        const int p = k_player_ui_enhanced_layout[layout_index].part;
        const PlayerEnhancedPartInfo *info = player_enhanced_part(p);
        PlayerUiEnhancedRow *row = &model->enhanced_rows[model->enhanced_row_count++];
        int library = player_settings_library_part(settings, p);
        row->part = p;
        row->group = k_player_ui_enhanced_layout[layout_index].group;
        row->name = info->name;
        row->sentence = info->sentence;
        row->labels = info->labels;
        row->label_count = info->value_count;
        row->unavailable_label = 0;
        row->library_value = NULL;
        row->selected_index = player_ui_enhanced_value_index(p, library);
        row->inherited = false;
        row->disabled = false;
        row->disabled_reason = NULL;
        row->tag = player_ui_enhanced_tag(p);
        row->value = player_enhanced_part_label(p, library);
        row->on = library != 0;
        if (title < 0 && preset && preset->part[p] >= 0) {
            snprintf(player->enhanced_values[p], sizeof(player->enhanced_values[p]),
                     NK_UI_STR(ADAPTER_PART_VALUE_WITH_PRESET),
                     player_enhanced_part_label(p, library), preset->label);
            row->value = player->enhanced_values[p];
        }
        if (title >= 0) {
            int own = player_app_title_part(app, title, p);
            row->inherited = own < 0;
            row->selected_index = own < 0 ? 0 :
                player_ui_enhanced_value_index(p, own) + 1;
            snprintf(player->enhanced_library_values[p], sizeof(player->enhanced_library_values[p]),
                     NK_UI_STR(ADAPTER_MODE_LIBRARY_VALUE), player_enhanced_part_label(p, library));
            row->library_value = player->enhanced_library_values[p];
            if (own < 0) {
                snprintf(player->enhanced_values[p], sizeof(player->enhanced_values[p]),
                         NK_UI_STR(ADAPTER_MODE_LIBRARY_VALUE), player_enhanced_part_label(p, library));
                row->value = player->enhanced_values[p];
            } else {
                row->value = player_enhanced_part_label(p, own);
                row->on = own != 0;
            }
            if (p == PLAYER_PART_LOAD && !player_app_title_can_choose_short_waits(app, title)) {
                row->unavailable_label = player_ui_enhanced_value_index(p, ACE_LOAD_FAST) + 1;
                /* Keep the saved choice visible so it can be changed. In a title
                   without Ace it does not take effect on launch. */
                if ((own < 0 ? library : own) == ACE_LOAD_FAST) {
                    row->on = false;
                    row->sentence = NK_UI_STR(ADAPTER_LOADING_ACE_REQUIRED);
                }
            }
            /* The launch line carries smooth=off while the game runs its 60 FPS game logic:
               this build interpolates only a 30 Hz source (docs/ENHANCEMENT_CONTRACT.md 10.5).
               The row says so, and keeps the choice it holds for when that logic is off. */
            if (p == PLAYER_PART_SMOOTH && logic_60) {
                snprintf(player->enhanced_smooth_sentence,
                         sizeof(player->enhanced_smooth_sentence),
                         NK_UI_STR(ADAPTER_SMOOTH_AT_60_DESCRIPTION), row->value);
                row->value = ENHANCED_SMOOTH_AT_60_VALUE;
                row->sentence = player->enhanced_smooth_sentence;
                /* The row marks what runs, Off; its sentence names the choice it keeps. */
                row->selected_index = player_ui_enhanced_value_index(p, 0) + 1;
                row->inherited = false;
                row->on = false;
            }
        }
        /* Supersampling at a render scale of 3x or 4x (owner, 2026-11-15): where 4x is picked,
           the row says what it costs and that games use 2x until 4x anyway is picked. */
        if (p == PLAYER_PART_SSAA) {
            int own = title >= 0 ? player_app_title_part(app, title, p) : -1;
            row->sentence = player_ssaa_sentence(own >= 0 ? own : library,
                                                 player_settings_enhanced_render_scale(settings));
        }
        if (model->enhanced_on) {
            const char *unavailable =
                player_enhanced_part_unavailable_label(p, row->on, smooth_on);
            if (unavailable) {
                /* The row shows what runs, as its line did before the tabs: "Off" while Smooth
                   motion is on. The choice it keeps is in the settings and comes back with
                   Smooth motion off. Widescreen is the one part with such a value
                   (player_enhanced_part_unavailable_label). */
                row->disabled = true;
                row->disabled_reason = NK_UI_STR(ADAPTER_SMOOTH_BLOCKS_WIDESCREEN);
                row->value = unavailable;
                row->selected_index = player_ui_enhanced_value_index(p, 0) + (title >= 0 ? 1 : 0);
                row->inherited = false;
                row->on = false;
            }
        }
        if (!model->enhanced_on) {
            row->disabled = true;
            row->disabled_reason = NK_UI_STR(ADAPTER_PART_AUTHENTIC_DISABLES);
        } else if (title >= 0 && row->tag != NULL &&
                   (chosen != PLAYER_PLAY_ACE || !app->title_enhancements[title].available)) {
            /* A saved Ace whose package has gone is no Ace: its Ace-only rows stay dimmed. */
            row->disabled = true;
            row->disabled_reason = app->title_enhancements[title].available
                ? NK_UI_STR(ADAPTER_PART_ACE_REQUIRED)
                : PLAYER_UI_ACE_NEEDS_PACKAGE;
        } else if (p == PLAYER_PART_SMOOTH && title >= 0 && logic_60) {
            row->disabled = true;
            row->disabled_reason = NK_UI_STR(ADAPTER_PART_LOGIC_60_ON);
        }
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    {
        /* Whose page it is, its play mode and each row's value, as drawn; printed on change. */
        char line[sizeof(player->enhanced_reported)];
        int used = snprintf(line, sizeof(line), "title=%s|mode=%s|sentence=%s",
                            title >= 0 ? app->games[title].disc_id : "library",
                            model->enhanced_mode, model->enhanced_mode_sentence);
        for (int p = 0; p < (int)model->enhanced_row_count && used > 0 &&
                        (size_t)used < sizeof(line); p++) {
            used += snprintf(line + used, sizeof(line) - (size_t)used, "|%s=%s",
                             player_enhanced_part(model->enhanced_rows[p].part)->key,
                             model->enhanced_rows[p].value);
        }
        /* Supersampling's sentence, which warns at a render scale of 3x or 4x. */
        for (int p = 0; p < (int)model->enhanced_row_count; p++) {
            if (model->enhanced_rows[p].part == PLAYER_PART_SSAA && used > 0 &&
                (size_t)used < sizeof(line)) {
                used += snprintf(line + used, sizeof(line) - (size_t)used,
                                 "|ssaa_sentence=%s", model->enhanced_rows[p].sentence);
                break;
            }
        }
        if (strcmp(line, player->enhanced_reported) != 0) {
            snprintf(player->enhanced_reported, sizeof(player->enhanced_reported), "%s", line);
            printf("[PLAYER_UI_TEST] next_ui_enhanced_page %s\n", line);
        }
    }
    {
        char groups[768] = "";
        char line[sizeof(player->enhanced_layout_reported)];
        size_t used = 0u;
        PlayerUiEnhancedGroup previous = PLAYER_UI_ENHANCED_GROUP_COUNT;
        for (int i = 0; i < model->enhanced_row_count && used < sizeof(groups); i++) {
            const PlayerUiEnhancedRow *row = &model->enhanced_rows[i];
            if (row->group != previous) {
                int n = snprintf(groups + used, sizeof(groups) - used, "%s%s:",
                                 used == 0u ? "" : ";",
                                 player_ui_enhanced_group_label(row->group));
                if (n < 0 || (size_t)n >= sizeof(groups) - used) break;
                used += (size_t)n;
                previous = row->group;
            } else {
                int n = snprintf(groups + used, sizeof(groups) - used, ",");
                if (n < 0 || (size_t)n >= sizeof(groups) - used) break;
                used += (size_t)n;
            }
            {
                const char *key = player_enhanced_part(row->part)->key;
                int n = snprintf(groups + used, sizeof(groups) - used, "%s", key);
                if (n < 0 || (size_t)n >= sizeof(groups) - used) break;
                used += (size_t)n;
            }
        }
        snprintf(line, sizeof(line), "title=%s|preset=%s|groups=%s|tab=%d",
                 title >= 0 ? app->games[title].disc_id : "library",
                 model->preset != NULL ? model->preset : "Custom", groups,
                 model->enhanced_tab);
        if (strcmp(line, player->enhanced_layout_reported) != 0) {
            snprintf(player->enhanced_layout_reported, sizeof(player->enhanced_layout_reported),
                     "%s", line);
            printf("[PLAYER_UI_TEST] next_ui_enhanced_layout %s\n", line);
        }
    }
#endif
}

/* The Settings pages' model, from the player's settings. */
static PlayerUiAction player_ui_render_settings(PlayerUiNk *player, PlayerApp *app,
                                                NkUiScale scale) {
    PlayerUiSettingsModel model;
    memset(&model, 0, sizeof(model));
    switch (app->active_view) {
        case VIEW_ONLINE_SETTINGS: model.page = PLAYER_UI_SETTINGS_PAGE_ONLINE; break;
        case VIEW_CHEAT_DB:
        case VIEW_CHEAT_DB_BUSY: model.page = PLAYER_UI_SETTINGS_PAGE_CHEAT_DB; break;
        case VIEW_CHEAT_DB_CONFIRM: model.page = PLAYER_UI_SETTINGS_PAGE_CHEAT_DB_CONFIRM; break;
        case VIEW_ONLINE_PLAY_SETTINGS: model.page = PLAYER_UI_SETTINGS_PAGE_ONLINE_PLAY; break;
        case VIEW_ONLINE_PLAY_CARD: model.page = PLAYER_UI_SETTINGS_PAGE_ONLINE_PLAY_CARD; break;
        case VIEW_ONLINE_PLAY_SERVERS:
            model.page = PLAYER_UI_SETTINGS_PAGE_ONLINE_PLAY_SERVERS;
            break;
        case VIEW_ACHIEVEMENTS_CARD: model.page = PLAYER_UI_SETTINGS_PAGE_ACHIEVEMENTS_CARD; break;
        case VIEW_ACHIEVEMENTS_SIGN_IN:
            model.page = PLAYER_UI_SETTINGS_PAGE_ACHIEVEMENTS_SIGN_IN;
            break;
        case VIEW_ACCESSIBILITY_SETTINGS: model.page = PLAYER_UI_SETTINGS_PAGE_ACCESSIBILITY; break;
        case VIEW_ACE_SETTINGS: model.page = PLAYER_UI_SETTINGS_PAGE_ACE; break;
        case VIEW_PREREQ_ABOUT: model.page = PLAYER_UI_SETTINGS_PAGE_NOTICES; break;
        case VIEW_CONFIRM_REMOVE_TOOLS: model.page = PLAYER_UI_SETTINGS_PAGE_REMOVE_TOOLS; break;
        case VIEW_BUILD_CACHE: model.page = PLAYER_UI_SETTINGS_PAGE_BUILD_CACHE; break;
        case VIEW_UPDATES: model.page = PLAYER_UI_SETTINGS_PAGE_UPDATES; break;
        case VIEW_PROBLEM_REPORT: model.page = PLAYER_UI_SETTINGS_PAGE_PROBLEM_REPORT; break;
        case VIEW_CONFIRM_FACTORY_RESET:
            model.page = PLAYER_UI_SETTINGS_PAGE_FACTORY_RESET;
            if (!player->reset_folders_ready) {
                player->reset_folder_count = player_app_factory_reset_folders(
                    app, player->reset_folders, PLAYER_UI_RESET_FOLDERS);
                for (size_t i = 0; i < player->reset_folder_count; ++i) {
                    player_ui_display_in_place(player->reset_folders[i], NK_MAX_PATH);
                }
                player->reset_folders_ready = true;
            }
            for (size_t i = 0; i < player->reset_folder_count; ++i) {
                model.reset_folders[model.reset_folder_count++] = player->reset_folders[i];
            }
            break;
        case VIEW_PSP_FONTS:
            model.page = PLAYER_UI_SETTINGS_PAGE_FONTS;
            if (app->wizard.font_slot_detail[0][0] == '\0') player_app_refresh_font_status(app);
            model.font_line_count = player_ui_font_lines(
                app, model.font_lines, sizeof(model.font_lines) / sizeof(model.font_lines[0]));
            model.font_message = app->wizard.font_message;
            model.font_message_kind = player_ui_note_kind(app->wizard.font_message_kind);
            model.font_summary = app->wizard.font_summary;
            model.fonts_all_own = app->wizard.font_own_count >= NK_FONT_PSP_FONT_COUNT;
            break;
        case VIEW_SETTINGS:
        default: model.page = PLAYER_UI_SETTINGS_PAGE_MAIN; break;
    }
    model.resolution = player_ui_resolution_word(app->settings.resolution_scale);
    model.pacing = app->settings.fps_cap < 0 ? NK_UI_STR(SETTINGS_PACING_PSP_TIMING)
                                            : NK_UI_STR(SETTINGS_PACING_NO_EXTRA_CAP);
    model.pacing_sentence = NK_UI_STR(ADAPTER_PACING_DESCRIPTION);
    model.scaling = player_present_scaling_label(app->settings.present_scaling);
    model.preset = player_app_graphics_preset_label(app);
    model.vsync = app->settings.vsync;
    model.display_refresh_hz = player_ui_display_refresh_hz(player->window);
    model.fullscreen = app->settings.fullscreen;
    model.volume = app->settings.master_volume;
    model.ui_sounds = app->settings.ui_sounds;
    model.launcher_music = app->settings.launcher_music;
    model.launcher_music_value = player_ui_music_row_value(app->settings.launcher_music,
                                                           player_ui_music_state(player->music));
    model.music_volume = app->settings.launcher_music_volume;
    model.game_music = app->settings.launcher_game_music;
    model.launcher_fullscreen = app->settings.launcher_fullscreen;
    model.nickname = app->settings.nickname;
    model.build_speed = package_build_speed_name(app->settings.build_speed);
    switch (app->settings.build_speed) {
    case PACKAGE_BUILD_SPEED_BACKGROUND:
        model.build_speed_sentence = NK_UI_STR(ADAPTER_BUILD_SPEED_BACKGROUND);
        break;
    case PACKAGE_BUILD_SPEED_FASTEST:
        model.build_speed_sentence = NK_UI_STR(ADAPTER_BUILD_SPEED_FASTEST);
        break;
    default:
        model.build_speed_sentence = NK_UI_STR(ADAPTER_BUILD_SPEED_BALANCED);
        break;
    }
    player_ui_fill_enhanced(player, app, &model);
    if (app->settings.interface_size_pct > 0) {
        snprintf(player->settings_interface_size, sizeof(player->settings_interface_size),
                 NK_UI_STR(ADAPTER_INTERFACE_SIZE_PERCENT),
                 app->settings.interface_size_pct);
        model.interface_size = player->settings_interface_size;
    } else {
        model.interface_size = NK_UI_STR(ADAPTER_INTERFACE_SIZE_AUTO);
    }
    model.text_size = player_text_size_label(app->settings.text_size_pct);
    model.high_contrast_choice = player_system_choice_label(app->settings.high_contrast_choice);
    model.reduce_motion_choice = player_system_choice_label(app->settings.reduce_motion_choice);
    model.high_contrast = player_app_high_contrast_focus(app);
    model.reduce_motion = player_app_reduce_motion(app);
    model.system_name = nk_platform_host_kind() == NK_HOST_WINDOWS
        ? "Windows" : NK_UI_STR(ADAPTER_SYSTEM_FALLBACK);
    model.system_scale_pct = (int)(player->viewport.display_scale * 100.0f + 0.5f);
    model.system_high_contrast = app->system_high_contrast;
    model.system_reduced_motion = app->system_reduced_motion;
    model.launcher_theme = player_ui_launcher_theme_name(app);
    snprintf(player->settings_controller, sizeof(player->settings_controller), "%s",
             app->settings.controller_connected
                 ? (app->settings.controller_name[0] != '\0' ? app->settings.controller_name
                                                             : NK_UI_STR(ADAPTER_CONTROLLER_CONNECTED))
                 : NK_UI_STR(ADAPTER_KEYBOARD_AND_MOUSE));
    model.controller_line = player->settings_controller;
    /* nk_platform_get_path creates the folder, and it cannot change while the
       player runs, so it is looked up once. */
    if (player->settings_saves[0] == '\0' &&
        !nk_platform_get_path(NK_PATH_SAVES, player->settings_saves,
                              sizeof(player->settings_saves))) {
        player->settings_saves[0] = '\0';
    }
    model.saves_folder = player->settings_saves;
    model.online_mode = app->settings.online_mode;
    model.online_news = app->settings.online_news;
    model.online_reports = app->settings.online_reports;
    model.developer_mode = app->settings.developer_mode;
    {
        /* Online play (#1139): on only with the consent card's answer for the server. */
        const char *srv = app->settings.online_play_server[0] ? app->settings.online_play_server
                                                              : NK_ADHOC_DEFAULT_SERVER;
        const char *label = NULL;
        size_t rows = 0u;
        model.online_play = player_app_online_play_consented(&app->settings);
        model.online_play_server = srv;
        for (size_t i = 0u; i < nk_adhoc_server_preset_count() &&
                            rows < PLAYER_UI_ONLINE_PLAY_SERVERS_MAX; ++i) {
            const NkAdhocServerPreset *p = nk_adhoc_server_preset(i);
            if (!p || !p->host) continue;
            if (strcmp(srv, p->host) == 0) label = p->label;
            player->settings_online_play_servers[rows++] =
                (PlayerUiOnlineServer){p->label, p->host, "", false};
        }
        for (size_t i = 0u; i < app->online_play_list.count &&
                            rows < PLAYER_UI_ONLINE_PLAY_SERVERS_MAX; ++i) {
            const NkAdhocListEntry *e = &app->online_play_list.entries[i];
            if (!label && strcmp(srv, e->host) == 0) label = e->name;
            player->settings_online_play_servers[rows++] =
                (PlayerUiOnlineServer){e->name, e->host, e->location, true};
        }
        model.online_play_servers = player->settings_online_play_servers;
        model.online_play_server_count = rows;
        /* NULL where no list names it: a host typed in, or a downloaded list's entry while that
           list (kept one session) is not loaded; the page then says "Other server". */
        model.online_play_server_label = label;
        model.online_play_list_status = app->online_play_list_status;
        model.online_play_list_busy = app->online_play_list_busy;
        model.online_play_list_live = app->online_play_list_live;
        model.online_play_card_server = app->online_play_card_server;
        {
            /* The card names exactly what a game will contact: the runtime's own parser
               (nk_launch.c passes the same string to it). */
            uint16_t lobby_port = 0u;
            uint16_t data_port = 0u;
            if (nk_adhoc_relay_parse_server(app->online_play_card_server,
                                            player->settings_online_play_card_host,
                                            sizeof(player->settings_online_play_card_host),
                                            &lobby_port, &data_port)) {
                model.online_play_card_lobby_port = lobby_port;
                model.online_play_card_data_port = data_port;
            }
            model.online_play_card_host = player->settings_online_play_card_host;
        }
        snprintf(player->settings_online_play_row, sizeof(player->settings_online_play_row),
                 model.online_play ? NK_UI_STR(ADAPTER_ONLINE_PLAY_ON)
                                   : NK_UI_STR(ADAPTER_ONLINE_PLAY_OFF), srv);
        if (app->online_play_notice[0]) {
            snprintf(player->settings_online_play_status,
                     sizeof(player->settings_online_play_status), "%s", app->online_play_notice);
        } else if (model.online_play) {
            snprintf(player->settings_online_play_status,
                     sizeof(player->settings_online_play_status),
                     NK_UI_STR(ADAPTER_ONLINE_PLAY_ACTIVE), srv);
        } else {
            snprintf(player->settings_online_play_status,
                     sizeof(player->settings_online_play_status), "%s",
                     NK_UI_STR(ADAPTER_ONLINE_PLAY_INACTIVE));
        }
        model.online_play_row_value = player->settings_online_play_row;
        model.online_play_status = player->settings_online_play_status;
        model.online_play_console_id = app->online_play_console_id;
        model.online_play_console_id_shown = app->online_play_console_id_ok;
        model.online_play_console_id_locked = app->is_game_running;
        model.online_play_console_note = app->online_play_console_note;
        snprintf(player->settings_online_play_port, sizeof(player->settings_online_play_port),
                 "%u", (unsigned)(app->settings.online_play_port_offset <= 65535u
                                      ? app->settings.online_play_port_offset
                                      : NK_RELAY_PORT_OFFSET));
        model.online_play_port_offset = player->settings_online_play_port;
    }
    model.download_cover_art = app->settings.download_cover_art;
    /* RetroAchievements (#1316) is on the Online and developer page. */
    model.achievements_enabled = app->settings.achievements_enabled;
    model.achievements_available = app->achievements_store_available;
    model.achievements_signed_in = app->achievements_signed_in;
    model.achievements_account = app->achievements_account[0] ? app->achievements_account : NULL;
    model.achievements_status = app->achievements_status[0] ? app->achievements_status : NULL;
    model.achievements_username = player->ra_form.username;
    model.achievements_password_chars = player_ui_sign_in_password_chars(&player->ra_form);
    model.achievements_sign_in_pending = app->achievements_sign_in_pending;
    /* Cheats turn achievements off for every game, so this page says so too
       (player_app_achievements_block; the title page adds the 60 FPS package). */
    if (model.achievements_enabled) {
        const char *blocked = player_app_achievements_block(app, -1);
        if (blocked) model.achievements_status = blocked;
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    {
        /* The RetroAchievements account row's line each time it changes, in hex so every byte reads back. */
        static char reported[sizeof(app->achievements_status) + 8];
        const char *line = model.achievements_status ? model.achievements_status : "";
        if (strcmp(reported, line) != 0) {
            snprintf(reported, sizeof(reported), "%s", line);
            printf("[PLAYER_UI_TEST] next_ui_settings_achievements enabled=%d status_hex=",
                   model.achievements_enabled ? 1 : 0);
            for (const char *c = line; *c; c++) printf("%02x", (unsigned)(unsigned char)*c);
            printf("\n");
        }
    }
#endif
    model.cheats = app->settings.cheats;
    model.cheat_db_status = app->cheat_db.status;
    model.cheat_db_source = app->cheat_db.source.name;
    model.cheat_db_maintainer = app->cheat_db.source.maintainer;
    model.cheat_db_licence = app->cheat_db.source.licence;
    model.cheat_db_import = app->cheat_db.import_path[0] != '\0';
    model.cheat_db_replace = app->cheat_db.replace;
    model.cheat_db_download_available = app->cheat_db.download_available;
    model.cheat_db_busy = app->cheat_db.busy || app->cheat_db.picking;
    model.cheat_db_fetching_sources = app->cheat_db.fetching_sources;
    model.cover_art_cache_size = app->cover_art_cache_size_label;
    model.share_system_libraries = app->settings.share_system_libraries;
    /* The build tools and cache page says what its clear button did. */
    if (model.page == PLAYER_UI_SETTINGS_PAGE_BUILD_CACHE && app->settings_notice[0] != '\0') {
        model.notice = app->settings_notice;
    }
    /* Settings a newer Nakagawa saved are kept and not replaced; every page says
       so, since no change made on any of them is saved. */
    if (app->settings_newer_schema && app->settings_notice[0] != '\0') {
        model.notice = app->settings_notice;
    }
    /* The graphics preset's note (#2150) says where it applies, on the page that has its row:
       Settings > Enhanced. It is derived from the choice, so it goes with Custom and is back
       after a restart. */
    if (model.page == PLAYER_UI_SETTINGS_PAGE_ACE &&
        player_app_graphics_preset_note(app, player->settings_preset_note,
                                        sizeof(player->settings_preset_note))) {
        model.preset_notice = player->settings_preset_note;
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (model.page == PLAYER_UI_SETTINGS_PAGE_ACE) {
        /* The Enhanced page's first row shows the global preset; printed on change. */
        char line[sizeof(player->preset_note_reported)];
        snprintf(line, sizeof(line), "value=%s|note=%s", model.preset,
                 model.preset_notice != NULL ? model.preset_notice : "");
        if (strcmp(line, player->preset_note_reported) != 0) {
            snprintf(player->preset_note_reported, sizeof(player->preset_note_reported), "%s",
                     line);
            printf("[PLAYER_UI_TEST] next_ui_settings_preset %s\n", line);
        }
    }
    if (model.page == PLAYER_UI_SETTINGS_PAGE_BUILD_CACHE) {
        char line[sizeof(player->cover_cache_reported)];
        snprintf(line, sizeof(line), "size=%s|notice=%s",
                 model.cover_art_cache_size != NULL ? model.cover_art_cache_size : "",
                 model.notice != NULL ? model.notice : "");
        if (strcmp(line, player->cover_cache_reported) != 0) {
            snprintf(player->cover_cache_reported, sizeof(player->cover_cache_reported), "%s",
                     line);
            printf("[PLAYER_UI_TEST] next_ui_cover_cache %s\n", line);
        }
    }
#endif
    for (size_t i = 0u; i < app->prerequisites.items.count &&
                        model.tool_count < PLAYER_UI_SETTINGS_TOOLS; ++i) {
        const PackagePrerequisite *item = &app->prerequisites.items.items[i];
        char *line = player->settings_tools[model.tool_count];
        if (!item->installed) continue;
        snprintf(line, sizeof(player->settings_tools[0]), NK_UI_STR(ADAPTER_TOOL_DESCRIPTION), item->name,
                 item->version,
                 strcmp(item->license, "NOASSERTION") == 0 ? NK_UI_STR(ADAPTER_TOOL_LICENSE_UNREVIEWED)
                                                            : item->license);
        model.tools[model.tool_count++] = line;
    }
    if (model.page == PLAYER_UI_SETTINGS_PAGE_UPDATES) {
        const PlayerUpdateState *update = &app->update;
        const NkUpdateRelease *newest = &update->check.newest;
        bool checked = update->phase >= PLAYER_UPDATE_CURRENT &&
                       (update->phase != PLAYER_UPDATE_FAILED ||
                        update->failed_step != PLAYER_UPDATE_CHECKING);
        model.update_step = (PlayerUiUpdateStep)update->phase;
        model.update_failed_step = (PlayerUiUpdateStep)update->failed_step;
        model.check_updates_at_start = app->settings.check_updates_at_start;
        model.discord_presence = app->settings.discord_presence;
        model.discord_available = nk_discord_available();
        model.current_version = NK_PLAYER_VERSION;
        if (checked) {
            model.update_version = newest->version;
            model.update_prerelease = newest->prerelease;
            model.update_published = newest->published;
            model.update_notes = newest->notes;
            model.update_page_url = newest->page_url;
            model.update_has_zip = newest->has_zip;
            /* A copy the Windows installer put in place updates through the next
               release's Setup.exe: the release page, never a ZIP unpacked beside it. */
            model.update_installed_by_setup = nk_update_installed_by_setup(app->install_root);
            if (nk_update_install_supported() && !model.update_installed_by_setup) {
                /* Download is offered only when the ZIP can be checked: a key in this
                   build and a signature in the release (update_check.h). */
                model.update_unverifiable = !update->can_verify;
                model.update_unsigned = !newest->has_signature;
                model.update_can_download = newest->has_zip && update->check.newer &&
                                            update->can_verify && newest->has_signature;
            }
            model.update_zip_name = newest->zip_name;
            model.update_zip_bytes = newest->zip_size;
        }
        model.update_received_bytes = update->received_bytes;
        /* The folder the last step left something in: the new version, or the
           downloaded ZIP's folder. */
        player->update_folder[0] = '\0';
        if (update->phase == PLAYER_UPDATE_INSTALLED) {
            snprintf(player->update_folder, sizeof(player->update_folder), "%s",
                     update->install_dir);
        } else if (update->zip_path[0] != '\0') {
            size_t cut = strlen(update->zip_path);
            while (cut > 0u && update->zip_path[cut - 1u] != '/' &&
                   update->zip_path[cut - 1u] != '\\') {
                --cut;
            }
            snprintf(player->update_folder, sizeof(player->update_folder), "%.*s",
                     (int)(cut > 0u ? cut - 1u : 0u), update->zip_path);
        }
        /* The page shortens a long folder from the middle; the profile folder
           becomes "~" first, so a cut never keeps part of the user name. */
        player_ui_display_in_place(player->update_folder, sizeof(player->update_folder));
        model.update_folder = player->update_folder;
        model.update_error_title = update->error.title;
        model.update_error_message = update->error.message;
    }
    if (model.page == PLAYER_UI_SETTINGS_PAGE_PROBLEM_REPORT) {
        const PlayerReportState *report = &app->report;
        model.report_step = (PlayerUiReportStep)report->phase;
        model.report_command = report->command;
        model.report_message = report->message;
        snprintf(player->report_path, sizeof(player->report_path), "%s", report->path);
        player_ui_display_in_place(player->report_path, sizeof(player->report_path));
        model.report_path = player->report_path;
        for (int i = 0; i < report->file_count && (unsigned)i < PLAYER_UI_REPORT_LINES; ++i) {
            unsigned long long bytes = report->file_bytes[i];
            snprintf(player->report_lines[i], sizeof(player->report_lines[i]),
                     NK_UI_STR(ADAPTER_REPORT_FILE_SIZE),
                     report->file_names[i], bytes >= 1024u ? (bytes + 1023u) / 1024u : bytes,
                     bytes >= 1024u ? NK_UI_STR(ADAPTER_REPORT_KB) : NK_UI_STR(ADAPTER_REPORT_BYTES));
            model.report_lines[model.report_line_count++] = player->report_lines[i];
        }
        model.report_file_total = report->file_total > 0 ? (size_t)report->file_total : 0u;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        {
            static size_t reported_more = (size_t)-1;
            size_t more = player_ui_report_more_count(&model);
            if (more != reported_more) {
                reported_more = more;
                printf("[PLAYER_UI_TEST] next_ui_report_more lines=%zu total=%zu more=%zu\n",
                       model.report_line_count, model.report_file_total, more);
                fflush(stdout);
            }
        }
#endif
    }
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    {
        /* The tab is the page's session state: the screen hands a chosen one back here, apart
           from the frame's action. */
        int chosen_tab = -1;
        PlayerUiAction action;
        model.enhanced_tab_chosen = &chosen_tab;
        action = player_ui_screen_settings(player->ui, &model);
        if (chosen_tab >= 0 && chosen_tab < PLAYER_UI_ENHANCED_GROUP_COUNT)
            player->enhanced_tab = chosen_tab;
        return action;
    }
}

/* A share of the full axis range, as a whole percent. */
static int player_ui_axis_percent(int value) {
    return (value * 100) / 32767;
}

/* Whose names a controller's pills carry, from the glyph set it is shown in. */
static InputPadNames player_ui_pad_names(NkUiPromptStyle style) {
    switch (style) {
    case NK_UI_PROMPT_LETTERS_A_SOUTH: return INPUT_PAD_NAMES_A_SOUTH;
    case NK_UI_PROMPT_LETTERS_A_EAST: return INPUT_PAD_NAMES_A_EAST;
    case NK_UI_PROMPT_SHAPES: return INPUT_PAD_NAMES_SHAPES;
    default: return INPUT_PAD_NAMES_NEUTRAL;
    }
}

/* A binding's glyph as the prompt glyph that draws it (#1299): face buttons, the
   shoulders, Start and the D-pad as the prompt bar draws them, the arrow keys as the
   arrow cluster, and every other key or pad input as a keycap or pill with its legend. */
static PlayerUiBindingGlyph player_ui_binding_glyph(const InputBindingGlyph *glyph) {
    PlayerUiBindingGlyph out;
    memset(&out, 0, sizeof(out));
    out.shown = true;
    switch (glyph->kind) {
    case INPUT_GLYPH_FACE_SOUTH: out.action = NK_UI_PROMPT_ACT_CONFIRM; break;
    case INPUT_GLYPH_FACE_EAST: out.action = NK_UI_PROMPT_ACT_BACK; break;
    case INPUT_GLYPH_FACE_WEST: out.action = NK_UI_PROMPT_ACT_OPTIONS; break;
    case INPUT_GLYPH_FACE_NORTH: out.action = NK_UI_PROMPT_ACT_DETAILS; break;
    case INPUT_GLYPH_SHOULDER_LEFT: out.action = NK_UI_PROMPT_ACT_TAB_PREV; break;
    case INPUT_GLYPH_SHOULDER_RIGHT: out.action = NK_UI_PROMPT_ACT_TAB_NEXT; break;
    case INPUT_GLYPH_PAD_START: out.action = NK_UI_PROMPT_ACT_MENU; break;
    case INPUT_GLYPH_DPAD:
    case INPUT_GLYPH_ARROW_KEY: out.action = NK_UI_PROMPT_ACT_MOVE; break;
    case INPUT_GLYPH_PAD_LEGEND:
        out.action = NK_UI_PROMPT_ACT_TAB_PREV;   /* the shoulder's pill */
        out.label = glyph->legend;
        break;
    case INPUT_GLYPH_KEY:
        out.action = NK_UI_PROMPT_ACT_CONFIRM;    /* a keycap */
        out.label = glyph->legend;
        break;
    case INPUT_GLYPH_NONE:
    default: out.shown = false; break;
    }
    return out;
}

/* The controller settings model, from the player's input_settings state. */
static PlayerUiAction player_ui_render_controller(PlayerUiNk *player, PlayerApp *app,
                                                  NkUiScale scale) {
    const InputSettingsState *input = &app->input_settings;
    const int16_t *axes = app->host_axes_live;
    PlayerUiControllerModel model;
    memset(&model, 0, sizeof(model));
    model.keyboard = input->device == INPUT_DEVICE_KEYBOARD;
    model.capturing = input->capturing ? input->capture_control : -1;
    /* Keys as keycaps; the controller in the set of the pad the user would pick up,
       even while the keyboard drives the page. */
    model.glyph_style = model.keyboard ? NK_UI_PROMPT_KEYCAPS
                                       : nk_ui_input_pad_prompt_style(&player->input.input);
    for (size_t i = 0u; i < PLAYER_UI_CONTROLLER_CONTROLS; ++i) {
        model.names[i] = input_settings_control_name((int)i);
        player->pad_bindings[i][0] = '\0';
        memset(&player->pad_glyphs[i], 0, sizeof(player->pad_glyphs[i]));
        if (!input_settings_control_bindable(input, (int)i)) {
            /* The stick's own entry, and on the controller its keys: no row. */
        } else if (model.keyboard) {
            /* A key taken from another control swaps with this one's, so keys never
               conflict. */
            input_settings_format_key_binding(input, (int)i, player->pad_bindings[i],
                                              sizeof(player->pad_bindings[i]));
            input_settings_key_glyph(input, (int)i, &player->pad_glyphs[i]);
        } else {
            input_settings_format_binding(input, (int)i, player->pad_bindings[i],
                                          sizeof(player->pad_bindings[i]));
            model.conflicted[i] = input_settings_is_control_conflicted(input, (int)i);
            input_settings_pad_glyph(input, (int)i, player_ui_pad_names(model.glyph_style),
                                     &player->pad_glyphs[i]);
        }
        model.bindings[i] = player->pad_bindings[i];
        model.glyphs[i] = player_ui_binding_glyph(&player->pad_glyphs[i]);
    }
    model.scope_title = input_settings_is_title_scope(input);
    model.offer_scope = player_app_selected_disc_id(app) != NULL;
    if (model.scope_title) {
        snprintf(player->pad_scope, sizeof(player->pad_scope), NK_UI_STR(ADAPTER_CONTROL_SCOPE_GAME),
                 input_settings_scope_label(input));
    } else {
        snprintf(player->pad_scope, sizeof(player->pad_scope), NK_UI_STR(ADAPTER_CONTROL_SCOPE_GLOBAL));
    }
    model.scope = player->pad_scope;
    model.noted = input->note[0] != '\0' ? input->note_control : -1;
    model.stick_right = input_settings_stick_is_right(input);
    /* A write that failed comes first: the mapping on screen is not the one on disk. */
    if (input->has_save_diagnostic) {
        snprintf(player->pad_status, sizeof(player->pad_status), NK_UI_STR(ADAPTER_CONTROL_NOT_SAVED),
                 input->save_diagnostic,
                 input->leave_unsaved ? NK_UI_STR(ADAPTER_CONTROL_LEAVE_WITHOUT_SAVING) : "");
        model.status = player->pad_status;
        model.status_error = true;
    } else if (input->note[0] != '\0') {
        model.status = input->note;
        model.status_error = input->note_error;
    } else if (!model.keyboard && input_settings_has_conflicts(input)) {
        model.status = input_settings_get_conflict_summary(input);
        model.status_error = true;
    } else if (input->has_load_diagnostic) {
        snprintf(player->pad_status, sizeof(player->pad_status), "%s", input->load_diagnostic);
        model.status = player->pad_status;
    } else if (model.capturing >= 0) {
        snprintf(player->pad_status, sizeof(player->pad_status),
                 model.keyboard ? NK_UI_STR(ADAPTER_CONTROL_WAIT_KEY)
                                : NK_UI_STR(ADAPTER_CONTROL_WAIT_BUTTON),
                 input_settings_control_name(model.capturing),
                 (input_settings_get_capture_remaining_ms(input) + 999) / 1000);
        model.status = player->pad_status;
    } else if (model.keyboard) {
        model.status = NK_UI_STR(ADAPTER_CONTROL_CHOOSE_KEY);
    } else if (app->settings.controller_connected) {
        snprintf(player->pad_status, sizeof(player->pad_status),
                 NK_UI_STR(ADAPTER_CONTROL_CHOOSE_BUTTON),
                 app->settings.controller_name[0] != '\0' ? app->settings.controller_name
                                                          : NK_UI_STR(ADAPTER_CONTROLLER_FALLBACK));
        model.status = player->pad_status;
    } else {
        model.status = NK_UI_STR(ADAPTER_CONTROL_NO_CONTROLLER);
    }
    snprintf(player->pad_deadzone, sizeof(player->pad_deadzone), NK_UI_STR(ADAPTER_CONTROL_DEAD_ZONE),
             player_ui_axis_percent(input->profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner));
    snprintf(player->pad_trigger, sizeof(player->pad_trigger), NK_UI_STR(ADAPTER_CONTROL_TRIGGERS),
             player_ui_axis_percent(input->profile.trigger_threshold));
    model.deadzone = player->pad_deadzone;
    model.trigger = player->pad_trigger;
    /* Mouse aiming (#1791), from the mapping being edited. */
    model.aim_on = input->profile.aim.mouse;
    model.aim_invert_x = input->profile.aim.invert_x;
    model.aim_invert_y = input->profile.aim.invert_y;
    snprintf(player->pad_aim, sizeof(player->pad_aim), NK_UI_STR(ADAPTER_CONTROL_MOUSE_AIM),
             model.aim_on ? NK_UI_STR(ADAPTER_CONTROL_ON) : NK_UI_STR(ADAPTER_CONTROL_OFF));
    snprintf(player->pad_aim_speed, sizeof(player->pad_aim_speed), NK_UI_STR(ADAPTER_CONTROL_AIM_SPEED),
             (int)input->profile.aim.sensitivity);
    snprintf(player->pad_aim_smoothing, sizeof(player->pad_aim_smoothing),
             NK_UI_STR(ADAPTER_CONTROL_AIM_SMOOTHING),
             (int)input->profile.aim.smoothing);
    model.aim = player->pad_aim;
    model.aim_speed = player->pad_aim_speed;
    model.aim_smoothing = player->pad_aim_smoothing;
    if (!input_settings_is_calibrating(input) && input_settings_aim_is_open(input)) {
        model.page = PLAYER_UI_CONTROLLER_PAGE_AIM;
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        return player_ui_screen_controller(player->ui, &model);
    }
    switch (input_settings_is_calibrating(input) ? input_settings_get_calibration_stage(input)
                                                 : CALIBRATION_STAGE_INACTIVE) {
        case CALIBRATION_STAGE_REST: {
            int ms = input->calib.elapsed_ms > 1000 ? 1000 : input->calib.elapsed_ms;
            model.page = PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_REST;
            model.calibration_fraction = ms > 0 ? (float)ms / 1000.0f : 0.0f;
            snprintf(player->pad_live[0], sizeof(player->pad_live[0]),
                     NK_UI_STR(ADAPTER_CONTROL_STICK_REST), input->calib.sampled_rest_x,
                     input->calib.sampled_rest_y);
            snprintf(player->pad_live[1], sizeof(player->pad_live[1]),
                     NK_UI_STR(ADAPTER_CONTROL_TRIGGERS_REST), input->calib.sampled_rest_lt,
                     input->calib.sampled_rest_rt);
            model.live_count = 2u;
            break;
        }
        case CALIBRATION_STAGE_EXTREMES:
            model.page = PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_EXTREMES;
            snprintf(player->pad_live[0], sizeof(player->pad_live[0]),
                     NK_UI_STR(ADAPTER_CONTROL_STICK_X_SAMPLE), input->calib.sampled_min_x,
                     input->calib.sampled_max_x, axes[NK_HOST_AXIS_LEFTX]);
            snprintf(player->pad_live[1], sizeof(player->pad_live[1]),
                     NK_UI_STR(ADAPTER_CONTROL_STICK_Y_SAMPLE), input->calib.sampled_min_y,
                     input->calib.sampled_max_y, axes[NK_HOST_AXIS_LEFTY]);
            snprintf(player->pad_live[2], sizeof(player->pad_live[2]),
                     NK_UI_STR(ADAPTER_CONTROL_LEFT_TRIGGER_SAMPLE), input->calib.sampled_rest_lt,
                     input->calib.sampled_max_lt);
            snprintf(player->pad_live[3], sizeof(player->pad_live[3]),
                     NK_UI_STR(ADAPTER_CONTROL_RIGHT_TRIGGER_SAMPLE), input->calib.sampled_rest_rt,
                     input->calib.sampled_max_rt);
            model.live_count = 4u;
            break;
        case CALIBRATION_STAGE_RESULT:
            model.page = PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_RESULT;
            snprintf(player->pad_live[0], sizeof(player->pad_live[0]),
                     NK_UI_STR(ADAPTER_CONTROL_STICK_X_RESULT), input->calib.result_rest_x,
                     input->calib.result_min_x, input->calib.result_max_x);
            snprintf(player->pad_live[1], sizeof(player->pad_live[1]),
                     NK_UI_STR(ADAPTER_CONTROL_STICK_Y_RESULT), input->calib.result_rest_y,
                     input->calib.result_min_y, input->calib.result_max_y);
            snprintf(player->pad_live[2], sizeof(player->pad_live[2]),
                     NK_UI_STR(ADAPTER_CONTROL_TRIGGERS_RESULT), input->calib.result_trigger_rest,
                     input->calib.result_trigger_extreme);
            model.live_count = 3u;
            break;
        default: {
            uint8_t psp_x = 128u;
            uint8_t psp_y = 128u;
            size_t used;
            int pressed = 0;
            nk_input_profile_eval_analog(&input->profile, axes, &psp_x, &psp_y);
            snprintf(player->pad_live[0], sizeof(player->pad_live[0]), NK_UI_STR(ADAPTER_CONTROL_STICK_LIVE),
                     axes[NK_HOST_AXIS_LEFTX], axes[NK_HOST_AXIS_LEFTY]);
            snprintf(player->pad_live[1], sizeof(player->pad_live[1]),
                     NK_UI_STR(ADAPTER_CONTROL_PSP_STICK_LIVE),
                     (unsigned)psp_x, (unsigned)psp_y,
                     psp_x != 128u || psp_y != 128u ? NK_UI_STR(ADAPTER_CONTROL_STICK_MOVING)
                                                   : NK_UI_STR(ADAPTER_CONTROL_STICK_IN_DEAD_ZONE));
            snprintf(player->pad_live[2], sizeof(player->pad_live[2]),
                     NK_UI_STR(ADAPTER_CONTROL_TRIGGERS_LIVE),
                     axes[NK_HOST_AXIS_LEFT_TRIGGER], axes[NK_HOST_AXIS_RIGHT_TRIGGER]);
            used = (size_t)snprintf(player->pad_live[3], sizeof(player->pad_live[3]),
                                    NK_UI_STR(ADAPTER_CONTROL_BUTTONS_DOWN));
            for (int b = 0; b < NK_HOST_BUTTON_COUNT && pressed < 4 &&
                            used < sizeof(player->pad_live[3]); ++b) {
                if (!app->host_buttons_live[b]) continue;
                used += (size_t)snprintf(player->pad_live[3] + used,
                                         sizeof(player->pad_live[3]) - used, "%s%s",
                                         pressed > 0 ? ", " : "",
                                         nk_host_button_name((NkHostGamepadButton)b));
                ++pressed;
            }
            if (pressed == 0) {
                snprintf(player->pad_live[3] + used, sizeof(player->pad_live[3]) - used,
                         NK_UI_STR(ADAPTER_CONTROL_NO_BUTTONS_DOWN));
            }
            model.live_count = 4u;
            break;
        }
    }
    for (size_t i = 0u; i < model.live_count; ++i) model.live[i] = player->pad_live[i];
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    return player_ui_screen_controller(player->ui, &model);
}

/* The developer log panel's model: the log lines player_state.c keeps. */
static PlayerUiAction player_ui_render_developer_log(PlayerUiNk *player, PlayerApp *app,
                                                     NkUiScale scale) {
    PlayerUiDeveloperLogModel model;
    size_t count = player_dev_log_count(app);
    memset(&model, 0, sizeof(model));
    if (count > PLAYER_UI_DEVELOPER_LOG_LINES) count = PLAYER_UI_DEVELOPER_LOG_LINES;
    for (size_t i = 0u; i < count; ++i) model.lines[i] = player_dev_log_line(app, i);
    model.line_count = count;
    model.notice = app->dev_log.notice;
    model.inspecting = app->dev_log.inspect_layout;
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    return player_ui_screen_developer_log(player->ui, &model);
}

/* The build queue screen's games: the one building, then the ones that wait,
   in queue order, as indices into app->rebuild_queue.entries, with each one's
   library index in `games`. A game no longer in the library is left out (the
   queue skips it). Each entry's game is looked up once. */
static size_t player_ui_queue_list(const PlayerApp *app, int *list, int *games, size_t cap) {
    static int entry_game[MAX_LIBRARY_GAMES];
    const PlayerRebuildQueue *queue = &app->rebuild_queue;
    int entries = queue->count < MAX_LIBRARY_GAMES ? queue->count : MAX_LIBRARY_GAMES;
    size_t count = 0u;
    if (!queue->active) return 0u;
    for (int i = 0; i < entries; ++i) {
        entry_game[i] = queue->entries[i].state == PLAYER_REBUILD_ACTIVE ||
                        queue->entries[i].state == PLAYER_REBUILD_PENDING
            ? player_app_find_game_by_disc_id(app, queue->entries[i].disc_id) : -1;
    }
    for (int pass = 0; pass < 2; ++pass) {
        PlayerRebuildState want = pass == 0 ? PLAYER_REBUILD_ACTIVE : PLAYER_REBUILD_PENDING;
        for (int i = 0; i < entries && count < cap; ++i) {
            if (queue->entries[i].state != want || entry_game[i] < 0) continue;
            games[count] = entry_game[i];
            list[count++] = i;
        }
    }
    return count;
}

static size_t player_ui_queue_find(const PlayerApp *app, const int *list, size_t total,
                                   const char *disc_id) {
    for (size_t i = 0u; i < total; ++i) {
        if (strcmp(app->rebuild_queue.entries[list[i]].disc_id, disc_id) == 0) return i;
    }
    return total;
}

/* The build queue (PLAYER_UI_ROUTE_QUEUE): a page of rows, and after Up,
   Down or Remove the focus stays with the game it moved (or, after a
   removal, the next game's Remove). The changes themselves go to main.c. */
static PlayerUiAction player_ui_render_queue(PlayerUiNk *player, const PlayerApp *app,
                                             NkUiScale scale) {
    static int list[MAX_LIBRARY_GAMES];
    static int list_games[MAX_LIBRARY_GAMES];
    const PlayerRebuildQueue *queue = &app->rebuild_queue;
    PlayerUiQueueModel model;
    PlayerUiAction result;
    size_t total = player_ui_queue_list(app, list, list_games, MAX_LIBRARY_GAMES);
    size_t follow = total;
    size_t shown;
    if (player->queue_follow_disc[0] != '\0') {
        follow = player_ui_queue_find(app, list, total, player->queue_follow_disc);
        if (follow < total) player->queue_first = follow - follow % PLAYER_UI_QUEUE_ROW_CAP;
    }
    if (player->queue_first >= total) {
        player->queue_first = total > 0u
            ? (total - 1u) - (total - 1u) % PLAYER_UI_QUEUE_ROW_CAP : 0u;
    }
    shown = total - player->queue_first;
    if (shown > PLAYER_UI_QUEUE_ROW_CAP) shown = PLAYER_UI_QUEUE_ROW_CAP;
    for (size_t r = 0u; r < shown; ++r) {
        size_t at = player->queue_first + r;
        const PlayerRebuildEntry *entry = &queue->entries[list[at]];
        int game = list_games[at];
        PlayerUiQueueRow *row = &player->queue_rows[r];
        bool first_waiting = at == 0u ||
            (at == 1u && queue->entries[list[0]].state == PLAYER_REBUILD_ACTIVE);
        memset(row, 0, sizeof(*row));
        row->title = app->games[game].title_name[0] ? app->games[game].title_name
                                                    : app->games[game].disc_id;
        row->title_index = (size_t)game;
        row->building = entry->state == PLAYER_REBUILD_ACTIVE;
        if (row->building) {
            float fraction = app->build_session.is_building &&
                strcmp(app->build_session.disc_id, entry->disc_id) == 0
                ? package_builder_compile_fraction(&app->build_session) : -1.0f;
            if (fraction >= 0.0f) {
                snprintf(player->queue_status[r], sizeof(player->queue_status[r]),
                         NK_UI_STR(ADAPTER_QUEUE_BUILD_PROGRESS), (int)(fraction * 100.0f + 0.5f));
            } else {
                snprintf(player->queue_status[r], sizeof(player->queue_status[r]), "%s",
                         NK_UI_STR(ADAPTER_QUEUE_BUILDING));
            }
        } else {
            snprintf(player->queue_status[r], sizeof(player->queue_status[r]), "%s",
                     first_waiting ? (queue->paused || queue->held ? NK_UI_STR(ADAPTER_QUEUE_PAUSED)
                                                                   : NK_UI_STR(ADAPTER_QUEUE_NEXT))
                                       : NK_UI_STR(ADAPTER_QUEUE_WAITING));
        }
        row->status = player->queue_status[r];
        row->can_move_up = !row->building && at > 0u &&
            queue->entries[list[at - 1u]].state == PLAYER_REBUILD_PENDING;
        row->can_move_down = !row->building && at + 1u < total;
    }
    if (follow < total && follow >= player->queue_first &&
        follow < player->queue_first + shown) {
        const PlayerUiQueueRow *row = &player->queue_rows[follow - player->queue_first];
        NkUiId slot = (NkUiId)(follow - player->queue_first);
        NkUiId base = player->queue_follow_base;
        if (base == PLAYER_UI_ID_QUEUE_UP_BASE && !row->can_move_up) {
            base = row->can_move_down ? PLAYER_UI_ID_QUEUE_DOWN_BASE
                                      : PLAYER_UI_ID_QUEUE_REMOVE_BASE;
        } else if (base == PLAYER_UI_ID_QUEUE_DOWN_BASE && !row->can_move_down) {
            base = row->can_move_up ? PLAYER_UI_ID_QUEUE_UP_BASE
                                    : PLAYER_UI_ID_QUEUE_REMOVE_BASE;
        }
        nk_ui_focus(player->ui, row->building ? PLAYER_UI_ID_QUEUE_BACK : base + slot);
    } else if (player->queue_follow_disc[0] != '\0' || player->queue_follow_back) {
        nk_ui_focus(player->ui, PLAYER_UI_ID_QUEUE_BACK);
    }
    player->queue_follow_disc[0] = '\0';
    player->queue_follow_back = false;
    memset(&model, 0, sizeof(model));
    model.rows = player->queue_rows;
    model.row_count = shown;
    model.first_row = player->queue_first;
    model.total = total;
    model.paused = queue->active && queue->paused;
    model.held = player_app_build_queue_held(app);
    model.building = total > 0u && queue->entries[list[0]].state == PLAYER_REBUILD_ACTIVE;
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    result = player_ui_screen_queue(player->ui, &model);
    if (result.kind == PLAYER_UI_ACTION_QUEUE) {
        PlayerUiQueueOp op = (PlayerUiQueueOp)result.primary;
        size_t at = result.title_index < (size_t)app->game_count
            ? player_ui_queue_find(app, list, total, app->games[result.title_index].disc_id)
            : total;
        if (op == PLAYER_UI_QUEUE_PAGE_BACK || op == PLAYER_UI_QUEUE_PAGE_NEXT) {
            if (op == PLAYER_UI_QUEUE_PAGE_BACK && player->queue_first >= PLAYER_UI_QUEUE_ROW_CAP) {
                player->queue_first -= PLAYER_UI_QUEUE_ROW_CAP;
            } else if (op == PLAYER_UI_QUEUE_PAGE_NEXT &&
                       player->queue_first + PLAYER_UI_QUEUE_ROW_CAP < total) {
                player->queue_first += PLAYER_UI_QUEUE_ROW_CAP;
            }
            result.kind = PLAYER_UI_ACTION_NONE;
        } else if ((op == PLAYER_UI_QUEUE_EARLIER || op == PLAYER_UI_QUEUE_LATER) && at < total) {
            snprintf(player->queue_follow_disc, sizeof(player->queue_follow_disc), "%s",
                     queue->entries[list[at]].disc_id);
            player->queue_follow_base = op == PLAYER_UI_QUEUE_EARLIER
                ? PLAYER_UI_ID_QUEUE_UP_BASE : PLAYER_UI_ID_QUEUE_DOWN_BASE;
        } else if (op == PLAYER_UI_QUEUE_REMOVE && at < total) {
            /* The next game's Remove takes the focus, or the one before. */
            size_t next = at + 1u < total ? at + 1u : at > 0u ? at - 1u : total;
            if (next < total) {
                snprintf(player->queue_follow_disc, sizeof(player->queue_follow_disc), "%s",
                         queue->entries[list[next]].disc_id);
                player->queue_follow_base = PLAYER_UI_ID_QUEUE_REMOVE_BASE;
            } else {
                player->queue_follow_back = true;
            }
        }
        /* Pause and Resume are one button: the focus stays on it. */
    }
    return result;
}

static PlayerUiAction player_ui_render_flow(PlayerUiNk *player, PlayerApp *app,
                                            NkUiScale scale, double now_s) {
    PlayerUiAction none = {PLAYER_UI_ACTION_NONE, PLAYER_UI_PRIMARY_NONE, 0u, -1};
    if (app->active_view == VIEW_SETUP_WIZARD) {
        return player_ui_render_wizard(player, app, scale);
    }
    if (app->active_view == VIEW_CONTROLLER_SETTINGS) {
        return player_ui_render_controller(player, app, scale);
    }
    if (app->active_view == VIEW_DEVELOPER_LOG) {
        return player_ui_render_developer_log(player, app, scale);
    }
    if (player_ui_disc_view(app->active_view)) {
        return player_ui_render_disc(player, app, scale, now_s);
    }
    if (player_ui_settings_view(app->active_view)) {
        return player_ui_render_settings(player, app, scale);
    }
    if (app->active_view == VIEW_BUILDING_PACKAGE) {
        const PackageBuildSession *build = &app->build_session;
        PlayerUiBuildModel model;
        int count = build->output_line_count;
        int first = count > 4 ? count - 4 : 0;
        memset(&model, 0, sizeof(model));
        if (build->title_name[0] != '\0') {
            snprintf(player->flow_title, sizeof(player->flow_title), NK_UI_STR(ADAPTER_FLOW_TITLE_WITH_STATUS),
                     build->title_name, build->disc_id);
        } else {
            snprintf(player->flow_title, sizeof(player->flow_title), NK_UI_STR(ADAPTER_FLOW_PACKAGE_BUILD),
                     build->disc_id);
        }
        model.title_line = player->flow_title;
        model.built = build->is_complete && !build->is_failed;
        player->build_built = model.built ? 1 : 0;
        switch (build->current_stage) {
            case PACKAGE_BUILD_STAGE_EXTRACT: model.stage = 1; break;
            case PACKAGE_BUILD_STAGE_COMPILE: model.stage = 2; break;
            case PACKAGE_BUILD_STAGE_PACKAGE: model.stage = 3; break;
            case PACKAGE_BUILD_STAGE_COMPLETE: model.stage = 4; break;
            case PACKAGE_BUILD_STAGE_PREFLIGHT:
            default: model.stage = 0; break;
        }
        model.fraction = package_builder_compile_fraction(build);
        model.notice = package_builder_compile_notice(build);
        model.speed_line = package_builder_speed_line(build);
        model.message = build->current_message;
        model.elapsed_ms = build->elapsed_ms;
        for (int i = first; i < count && model.line_count < 4u; ++i) {
            model.lines[model.line_count++] = package_builder_get_output_line(build, i);
        }
        model.now_s = player_app_reduce_motion(app) ? 0.0 : now_s;
        {
            int waiting = 0;
            for (int i = 0; app->rebuild_queue.active && i < app->rebuild_queue.count; ++i) {
                if (app->rebuild_queue.entries[i].state == PLAYER_REBUILD_PENDING) waiting++;
            }
            if (waiting > 0) {
                if (waiting == 1) {
                    snprintf(player->build_queue_line, sizeof(player->build_queue_line), "%s",
                             NK_UI_STR(ADAPTER_BUILD_QUEUE_ONE_MORE));
                } else {
                    snprintf(player->build_queue_line, sizeof(player->build_queue_line),
                             NK_UI_STR(ADAPTER_BUILD_QUEUE_MORE_GAMES), waiting);
                }
                model.queue_line = player->build_queue_line;
            }
        }
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        return player_ui_screen_build(player->ui, &model);
    }
    if (app->active_view == VIEW_PREREQ_CONSENT || app->active_view == VIEW_PREREQ_PROGRESS) {
        const PlayerPrerequisiteState *pre = &app->prerequisites;
        PlayerUiPrereqModel model;
        size_t count = pre->items.count < PLAYER_UI_PREREQ_CAP ? pre->items.count
                                                                : PLAYER_UI_PREREQ_CAP;
        memset(&model, 0, sizeof(model));
        for (size_t i = 0u; i < count; ++i) {
            const PackagePrerequisite *item = &pre->items.items[i];
            player->prereq_items[i].name = item->name;
            player->prereq_items[i].version = item->version;
            player->prereq_items[i].host = item->host;
            player->prereq_items[i].license = item->license;
            player->prereq_items[i].bytes = (unsigned long long)item->size_bytes;
        }
        model.in_progress = app->active_view == VIEW_PREREQ_PROGRESS;
        model.items = player->prereq_items;
        model.item_count = count;
        model.total_bytes = (unsigned long long)pre->total_bytes;
        model.current_item = pre->current_item[0] != '\0' ? pre->current_item
            : (pre->phase == PLAYER_PREREQ_BOOTSTRAP ? "cpython-embed-amd64" : NULL);
        model.item_received_bytes = (unsigned long long)pre->item_received_bytes;
        model.item_total_bytes = (unsigned long long)pre->item_total_bytes;
        model.total_received_bytes = (unsigned long long)pre->total_received_bytes;
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        return player_ui_screen_prereq(player->ui, &model);
    }
    if (app->active_view == VIEW_ERROR) {
        const ErrorState *error = &app->last_error;
        bool launch_readiness = player_error_leads_with_message(error);
        PlayerUiErrorModel model;
        memset(&model, 0, sizeof(model));
        model.badge = player_error_badge(error);
        model.title = error->title;
        /* The failed stage is in the copied details, not on the card (#1286). */
        model.failed_stage = NULL;
        model.retry_label = player_error_retry_label(error);
        model.copy_available = true;
        model.copied = error->details_copied;
        model.report_available = player_error_offers_report(error) &&
            player_app_find_game_by_disc_id(app, error->subject_disc_id) >= 0;
        model.known_issue = player_error_known_issue(error);
        /* Plain errors lead with the sentence and show the technical boundary
           as details. */
        model.message = launch_readiness ? error->message
            : (error->boundary_text[0] != '\0' ? error->boundary_text : error->message);
        model.details = error->details[0] != '\0' ? error->details
            : (launch_readiness ? error->boundary_text : "");
        model.log_path = error->log_file_path;
        model.recovery_label = error->recovery_action_label;
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        return player_ui_screen_error(player->ui, &model);
    }
    return none;
}

/* The flow screens open with focus on their main action. The view changes after
   the frame that asked for it, so without this the first frame of a new screen
   would still focus the control that opened it, and a press on that frame would
   be lost. Leaving the flow gives the library back the focus it had. */
/* The flow screen the app is on: its view, plus the sub-screen some views
   step through (the wizard's step and copy, a finished build, a calibration
   step, an update step). 0 off the flow views. */
/* The Controls page's screens after the mapping (0): the calibration steps 1 to 3
   (GuidedCalibrationStage), then the mouse aiming settings (#1791). */
#define PLAYER_UI_CONTROLLER_AIM_SCREEN 4u
typedef char player_ui_aim_screen_follows_the_calibration_steps[
    PLAYER_UI_CONTROLLER_AIM_SCREEN > (unsigned)CALIBRATION_STAGE_RESULT ? 1 : -1];

static unsigned player_ui_flow_screen(const PlayerApp *app) {
    unsigned screen;
    if (!player_ui_nk_flow_view(app->active_view)) return 0u;
    screen = (unsigned)app->active_view * 64u + 1u;
    switch (app->active_view) {
        case VIEW_SETUP_WIZARD:
            screen += (unsigned)app->wizard.step * 2u + (app->wizard.is_extracting ? 1u : 0u);
            break;
        case VIEW_BUILDING_PACKAGE:
            if (app->build_session.is_complete && !app->build_session.is_failed) screen += 1u;
            break;
        case VIEW_CONTROLLER_SETTINGS:
            /* The calibration steps are 1 to 3; the mouse aiming settings (#1791) are the
               screen after them. */
            screen += input_settings_is_calibrating(&app->input_settings)
                ? (unsigned)input_settings_get_calibration_stage(&app->input_settings)
                : input_settings_aim_is_open(&app->input_settings)
                    ? PLAYER_UI_CONTROLLER_AIM_SCREEN
                    : (unsigned)CALIBRATION_STAGE_INACTIVE;
            break;
        case VIEW_UPDATES: screen += (unsigned)app->update.phase; break;
        case VIEW_PROBLEM_REPORT: screen += (unsigned)app->report.phase; break;
        default: break;
    }
    return screen;
}

static void player_ui_flow_focus(PlayerUiNk *player, const PlayerApp *app) {
    unsigned screen = player_ui_flow_screen(app);
    NkUiId focus = 0u;
    if (player_ui_nk_flow_view(app->active_view)) {
        switch (app->active_view) {
            case VIEW_SETUP_WIZARD:
                focus = app->wizard.step == WIZARD_STEP_INSPECT_VERIFY &&
                        app->wizard.is_extracting
                    ? PLAYER_UI_ID_WIZARD_CANCEL : PLAYER_UI_ID_WIZARD_PRIMARY;
                break;
            case VIEW_BUILDING_PACKAGE:
                /* CANCEL BUILD is never the default focus, so a stray Enter or
                   South cannot stop a build; a move reaches it. A finished
                   build focuses BACK TO LIBRARY. */
                if (app->build_session.is_complete && !app->build_session.is_failed) {
                    focus = PLAYER_UI_ID_BUILD_ACTION;
                }
                break;
            case VIEW_PREREQ_CONSENT: focus = PLAYER_UI_ID_PREREQ_ACCEPT; break;
            case VIEW_PREREQ_PROGRESS: focus = PLAYER_UI_ID_PREREQ_CANCEL; break;
            /* The error card draws its own buttons (scr_error.c), never the
               disc card's: its recovery action has the focus. */
            case VIEW_ERROR: focus = PLAYER_UI_ID_ERROR_RECOVER; break;
            case VIEW_INSPECTING:
            case VIEW_SUPPORTED_TITLE:
            case VIEW_EXPERIMENTAL_TITLE:
            case VIEW_UNSUPPORTED_TITLE: focus = PLAYER_UI_ID_DISC_PRIMARY; break;
            case VIEW_SETTINGS:
                /* Back from one of its pages, focus the button that opened it. */
                focus = player->last_view == VIEW_ONLINE_SETTINGS ? PLAYER_UI_ID_SETTINGS_ONLINE
                    : player->last_view == VIEW_ACCESSIBILITY_SETTINGS
                        ? PLAYER_UI_ID_SETTINGS_ACCESSIBILITY
                    : player->last_view == VIEW_ACE_SETTINGS ? PLAYER_UI_ID_SETTINGS_ACE_OPEN
                    : player->last_view == VIEW_PREREQ_ABOUT ? PLAYER_UI_ID_SETTINGS_NOTICES
                    : player->last_view == VIEW_BUILD_CACHE ||
                            player->last_view == VIEW_CONFIRM_REMOVE_TOOLS
                        ? PLAYER_UI_ID_SETTINGS_BUILD_CACHE
                    : player->last_view == VIEW_PSP_FONTS ? PLAYER_UI_ID_SETTINGS_FONTS
                    : player->last_view == VIEW_UPDATES ? PLAYER_UI_ID_SETTINGS_UPDATES
                    : player->last_view == VIEW_PROBLEM_REPORT
                        ? PLAYER_UI_ID_SETTINGS_PROBLEM_REPORT
                    : player->last_view == VIEW_CONFIRM_FACTORY_RESET
                        ? PLAYER_UI_ID_SETTINGS_FACTORY_RESET
                    : player->last_view == VIEW_CONTROLLER_SETTINGS
                        ? PLAYER_UI_ID_SETTINGS_CONTROLLER
                        : PLAYER_UI_ID_SETTINGS_RESOLUTION;
                break;
            case VIEW_CONTROLLER_SETTINGS: {
                /* Each calibration step is a screen of its own; the mapping
                   opens on the first button, or on Calibrate after a step. The
                   mouse aiming settings (#1791) open on their switch, and the
                   mapping after them on the button that opened them. */
                GuidedCalibrationStage stage =
                    input_settings_is_calibrating(&app->input_settings)
                        ? input_settings_get_calibration_stage(&app->input_settings)
                        : CALIBRATION_STAGE_INACTIVE;
                bool aiming = stage == CALIBRATION_STAGE_INACTIVE &&
                              input_settings_aim_is_open(&app->input_settings);
                focus = stage == CALIBRATION_STAGE_REST ? PLAYER_UI_ID_CONTROLLER_CALIBRATION_CANCEL
                    : stage != CALIBRATION_STAGE_INACTIVE
                        ? PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY
                    : aiming ? PLAYER_UI_ID_CONTROLLER_AIM_TOGGLE
                    : player->flow_screen == screen + PLAYER_UI_CONTROLLER_AIM_SCREEN
                        ? PLAYER_UI_ID_CONTROLLER_AIM_OPEN
                    : player->flow_screen > screen &&
                            player->flow_screen < screen + PLAYER_UI_CONTROLLER_AIM_SCREEN
                        ? PLAYER_UI_ID_CONTROLLER_CALIBRATE
                        : PLAYER_UI_ID_CONTROLLER_BUTTON(0u);
                break;
            }
            case VIEW_DEVELOPER_LOG: focus = PLAYER_UI_ID_DEVELOPER_LOG_CLOSE; break;
            case VIEW_PSP_FONTS: focus = PLAYER_UI_ID_SETTINGS_PSP_FONTS; break;
            case VIEW_UPDATES:
                /* Each step's result is a screen of its own, focused on what comes
                   next. CANCEL is the download's only control; a finished check
                   with nothing newer, a failure or a release without a Windows ZIP
                   focuses CHECK FOR UPDATES. */
                switch (app->update.phase) {
                    case PLAYER_UPDATE_AVAILABLE:
                        focus = nk_update_install_supported() &&
                                !nk_update_installed_by_setup(app->install_root) &&
                                app->update.check.newest.has_zip &&
                                app->update.check.newest.has_signature &&
                                app->update.can_verify
                            ? PLAYER_UI_ID_UPDATE_DOWNLOAD : PLAYER_UI_ID_UPDATE_PAGE;
                        break;
                    case PLAYER_UPDATE_DOWNLOADING: focus = PLAYER_UI_ID_UPDATE_CANCEL; break;
                    case PLAYER_UPDATE_DOWNLOADED: focus = PLAYER_UI_ID_UPDATE_INSTALL; break;
                    case PLAYER_UPDATE_INSTALLED: focus = PLAYER_UI_ID_UPDATE_FOLDER; break;
                    case PLAYER_UPDATE_CHECKING:
                    case PLAYER_UPDATE_INSTALLING: focus = PLAYER_UI_ID_SETTINGS_BACK; break;
                    default: focus = PLAYER_UI_ID_UPDATE_CHECK; break;
                }
                break;
            case VIEW_PROBLEM_REPORT:
                focus = app->report.phase == PLAYER_REPORT_PREVIEWED
                    ? PLAYER_UI_ID_REPORT_CREATE : PLAYER_UI_ID_SETTINGS_BACK;
                break;
            case VIEW_ONLINE_SETTINGS:
                /* The Online mode switch is drawn only while online mode is stored
                   On (scr_settings.c); otherwise Developer mode is the first row.
                   Coming back from the Online play page returns to its row. */
                if (player->last_view == VIEW_ONLINE_PLAY_SETTINGS) {
                    focus = PLAYER_UI_ID_SETTINGS_ONLINE_PLAY;
                } else if (player->last_view == VIEW_CHEAT_DB ||
                           player->last_view == VIEW_CHEAT_DB_BUSY) {
                    focus = PLAYER_UI_ID_SETTINGS_CHEAT_DB;
                } else {
                    focus = app->settings.online_mode ? PLAYER_UI_ID_SETTINGS_ONLINE_MODE
                                                      : PLAYER_UI_ID_SETTINGS_DEVELOPER;
                }
                break;
            case VIEW_CHEAT_DB:
                focus = app->cheat_db.download_available && !app->cheat_db.busy
                    ? PLAYER_UI_ID_CHEAT_DB_DOWNLOAD : PLAYER_UI_ID_CHEAT_DB_IMPORT;
                break;
            case VIEW_CHEAT_DB_BUSY:
            case VIEW_CHEAT_DB_CONFIRM:
                focus = PLAYER_UI_ID_SETTINGS_BACK;
                break;
            case VIEW_ONLINE_PLAY_SETTINGS:
                /* Back from the picker, its Server row; else the switch. */
                focus = player->last_view == VIEW_ONLINE_PLAY_SERVERS
                    ? PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_PRESET
                    : PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_TOGGLE;
                break;
            /* The card's safe answer, NOT NOW, has the focus. */
            case VIEW_ONLINE_PLAY_CARD: focus = PLAYER_UI_ID_ONLINE_PLAY_NOT_NOW; break;
            /* RetroAchievements' card: NOT NOW too; its sign-in form: Username. */
            case VIEW_ACHIEVEMENTS_CARD: focus = PLAYER_UI_ID_ACHIEVEMENTS_NOT_NOW; break;
            case VIEW_ACHIEVEMENTS_SIGN_IN: focus = PLAYER_UI_ID_ACHIEVEMENTS_USERNAME; break;
            case VIEW_ONLINE_PLAY_SERVERS: focus = PLAYER_UI_ID_ONLINE_PLAY_SERVER_BASE; break;
            case VIEW_ACCESSIBILITY_SETTINGS: focus = PLAYER_UI_ID_SETTINGS_INTERFACE_SIZE; break;
            /* Settings > Enhanced, the library's or a game's, opens on its first row. */
            case VIEW_ACE_SETTINGS: focus = PLAYER_UI_ID_SETTINGS_PRESET; break;
            case VIEW_PREREQ_ABOUT:
            case VIEW_CONFIRM_REMOVE_TOOLS:
            /* The safe answer, Keep everything, has the focus. */
            case VIEW_CONFIRM_FACTORY_RESET: focus = PLAYER_UI_ID_SETTINGS_BACK; break;
            case VIEW_BUILD_CACHE:
                /* Back from the removal check, focus the button that opened it. */
                focus = player->last_view == VIEW_CONFIRM_REMOVE_TOOLS
                    ? PLAYER_UI_ID_SETTINGS_REMOVE_TOOLS : PLAYER_UI_ID_SETTINGS_SHARE_LIBRARIES;
                break;
            default: break;
        }
    }
    player->last_view = app->active_view;
    if (app->active_view != VIEW_CONFIRM_FACTORY_RESET) player->reset_folders_ready = false;
    if (screen == player->flow_screen) return;
    if (player->flow_screen == 0u) {
        player->pre_flow_focus = nk_ui_focused(player->ui);
        player->pre_flow_game_count = app->game_count;
    }
    if (screen != 0u) {
        nk_ui_focus(player->ui, focus);
    } else if (app->game_count > player->pre_flow_game_count &&
               app->selected_game_index >= 0 &&
               app->selected_game_index < app->game_count) {
        /* A flow that added a game (ADD TO LIBRARY) ends on its card. */
        player->library_focus_pending = true;
        player->library_focus_index = (size_t)app->selected_game_index;
    } else if (player->pre_flow_focus != 0u) {
        nk_ui_focus(player->ui, player->pre_flow_focus);
    }
    player->flow_screen = screen;
}

#ifdef NK_PLAYER_UI_REGRESSION_TEST
const PlayerUiTitle *player_ui_nk_test_title(const PlayerUiNk *player, size_t index) {
    return player != NULL && index < PLAYER_UI_TITLE_CAP ? &player->titles[index] : NULL;
}

int player_ui_nk_test_route(const PlayerUiNk *player) {
    return player != NULL ? (int)nk_ui_router_current(player->router) : 0;
}

int player_ui_nk_test_build_built(const PlayerUiNk *player) {
    return player != NULL ? player->build_built : -1;
}

unsigned player_ui_nk_test_focus(const PlayerUiNk *player) {
    return player != NULL && player->ui != NULL ? (unsigned)nk_ui_focused(player->ui) : 0u;
}

const char *player_ui_nk_test_find(const PlayerUiNk *player) {
    return player_ui_nk_find_open(player) ? player->find_query : NULL;
}

size_t player_ui_nk_test_library_grid(const PlayerUiNk *player, size_t *total) {
    if (total != NULL) *total = player != NULL ? player->library_game_total : 0u;
    return player != NULL ? player->library_grid_count : 0u;
}

const char *player_ui_nk_test_nickname(const PlayerUiNk *player, char *count, size_t cap) {
    if (count != NULL && cap > 0u) count[0] = '\0';
    if (player == NULL || !player->nickname_open) return NULL;
    if (count != NULL) (void)ui_keyboard_count_text(&player->nickname_keyboard, count, cap);
    if (player->nickname_is_ra_password) {
        /* A password is reported as the editor shows it: one star per character. */
        static char stars[PLAYER_UI_RA_PASSWORD_MAX + 1u];
        size_t n = 0u;
        for (const unsigned char *c = (const unsigned char *)player->nickname_text;
             *c != '\0' && n + 1u < sizeof(stars); ++c) {
            if ((*c & 0xC0u) != 0x80u) stars[n++] = '*';
        }
        stars[n] = '\0';
        return stars;
    }
    return player->nickname_text;
}

bool player_ui_nk_test_laid_out(const PlayerUiNk *player) {
    return player != NULL && player->ui != NULL && nk_ui_laid_out(player->ui);
}

int player_ui_nk_test_device_resets(const PlayerUiNk *player) {
    NkUiSdlrStats stats;
    if (player == NULL || player->backend == NULL) return -1;
    nk_ui_sdlr_get_stats(player->backend, &stats);
    return (int)stats.device_resets;
}

int player_ui_nk_test_backdrop(const PlayerUiNk *player, bool *shown) {
    if (shown != NULL) *shown = false;
    if (player == NULL || player->backdrop_index == SIZE_MAX) return -1;
    if (shown != NULL) *shown = player->backdrop_hidden_s >= player->rest_since_s &&
                                player->rest_index == player->backdrop_index;
    return (int)player->backdrop_index;
}
#endif

/* Opens the Remove from library dialog for a title, listing what Nakagawa
   built for it as it is on disk now. */
static bool player_ui_open_remove(PlayerUiNk *player, const PlayerApp *app, int index) {
    static const char *const labels[PLAYER_REMOVAL_KIND_COUNT] = {
        NK_UI_STR(ADAPTER_REMOVE_BUILT_PACKAGE), NK_UI_STR(ADAPTER_REMOVE_BUILD_CACHE),
        NK_UI_STR(ADAPTER_REMOVE_COPIED_DISC_FILES)
    };
    if (!player_app_plan_game_removal(app, index, &player->remove_plan)) return false;
    player->remove_item_count = 0u;
    for (int kind = 0; kind < PLAYER_REMOVAL_KIND_COUNT; ++kind) {
        if (!player->remove_plan.kind_present[kind] ||
            player->remove_item_count >= PLAYER_UI_REMOVE_ITEM_CAP) {
            continue;
        }
        player->remove_items[player->remove_item_count].label = labels[kind];
        player->remove_items[player->remove_item_count].bytes =
            player->remove_plan.kind_bytes[kind];
        ++player->remove_item_count;
    }
    player->remove_index = index;
    snprintf(player->remove_disc, sizeof(player->remove_disc), "%s",
             app->games[index].disc_id);
    player->remove_delete_saves = false;
    player->remove_delete_disc_copy = false;
    player->remove_focus_pending = true;
    return true;
}

/* Saves (#1883). The dialog reads the save folder and the backups when it
   opens and after each action, never every frame. */
static void player_ui_saves_bytes(char *out, size_t cap, uint64_t bytes) {
    if (bytes < 1024u) {
        (void)snprintf(out, cap, NK_UI_STR(ADAPTER_SIZE_BYTES), (unsigned)bytes);
    } else if (bytes < 1024u * 1024u) {
        (void)snprintf(out, cap, NK_UI_STR(ADAPTER_SIZE_KB), (double)bytes / 1024.0);
    } else if (bytes < 1024ull * 1024u * 1024u) {
        (void)snprintf(out, cap, NK_UI_STR(ADAPTER_SIZE_MB), (double)bytes / (1024.0 * 1024.0));
    } else {
        (void)snprintf(out, cap, NK_UI_STR(ADAPTER_SIZE_GB), (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

static void player_ui_saves_reload(PlayerUiNk *player, const PlayerApp *app, int index) {
    static const char *const kinds[PLAYER_SAVE_BACKUP_KIND_COUNT] = {
        NK_UI_STR(ADAPTER_SAVES_BACKUP_REGULAR), NK_UI_STR(ADAPTER_SAVES_BACKUP_BEFORE_RESTORE),
        NK_UI_STR(ADAPTER_SAVES_BACKUP_BEFORE_ADD), NK_UI_STR(ADAPTER_SAVES_BACKUP_BEFORE_REMOVE)
    };
    char folder[NK_MAX_PATH];
    uint64_t bytes = 0u;
    player->saves_has = false;
    player->saves_bytes = 0u;
    if (player_app_save_folder(&app->games[index], folder, sizeof(folder)) &&
        !nk_platform_path_missing(folder) && nk_platform_tree_size(folder, NULL, &bytes) &&
        bytes > 0u) {
        player->saves_has = true;
        player->saves_bytes = bytes;
    }
    NkMemstickCollisionResult offer = player_app_bring_in_offer(app, index, &player->saves_bring_in);
    player->saves_bring_in_problem[0] = '\0';
    if (offer != NK_MEMSTICK_COLLISION_COPIED && offer != NK_MEMSTICK_COLLISION_LAYOUT_BLOCKED &&
        offer != NK_MEMSTICK_COLLISION_INVALID) {
        (void)snprintf(player->saves_bring_in_problem, sizeof(player->saves_bring_in_problem),
                       NK_UI_STR(REMOVE_SAVES_BRING_IN_INSPECT_FAILED),
                       nk_memstick_bring_in_result_code(offer));
    }
    player->saves_list_count = player_app_list_save_backups(
        app, index, player->saves_list, PLAYER_UI_SAVES_LIST_CAP, &player->saves_total);
    if (player->saves_first >= player->saves_list_count) player->saves_first = 0u;
    for (size_t row = 0u; row < PLAYER_UI_SAVES_ROW_CAP; ++row) {
        size_t at = player->saves_first + row;
        const PlayerSaveBackup *backup;
        const char *st;
        char size[24];
        if (at >= player->saves_list_count) break;
        backup = &player->saves_list[at];
        st = backup->stamp;
        /* YYYYMMDD-HHMMSS as YYYY-MM-DD HH:MM:SS */
        (void)snprintf(player->saves_when[row], sizeof(player->saves_when[row]),
                       "%.4s-%.2s-%.2s %.2s:%.2s:%.2s", st, st + 4, st + 6, st + 9, st + 11,
                       st + 13);
        if (backup->sequence > 1u) {
            size_t used = strlen(player->saves_when[row]);
            (void)snprintf(player->saves_when[row] + used,
                           sizeof(player->saves_when[row]) - used, " (%u)", backup->sequence);
        }
        player_ui_saves_bytes(size, sizeof(size), backup->bytes);
        (void)snprintf(player->saves_detail[row], sizeof(player->saves_detail[row]),
                       NK_UI_STR(ADAPTER_SAVES_BACKUP_DETAIL),
                       (unsigned)backup->kind < PLAYER_SAVE_BACKUP_KIND_COUNT
                           ? kinds[backup->kind] : NK_UI_STR(ADAPTER_SAVES_BACKUP_REGULAR),
                       size);
        player->saves_rows[row].when = player->saves_when[row];
        player->saves_rows[row].detail = player->saves_detail[row];
    }
}

static size_t player_ui_saves_page_rows(const PlayerUiNk *player) {
    size_t rows;
    if (player->saves_first >= player->saves_list_count) return 0u;
    rows = player->saves_list_count - player->saves_first;
    return rows > PLAYER_UI_SAVES_ROW_CAP ? PLAYER_UI_SAVES_ROW_CAP : rows;
}

static void player_ui_saves_status(PlayerUiNk *player, PlayerUiNoteKind kind, const char *text) {
    (void)snprintf(player->saves_status, sizeof(player->saves_status), "%s", text);
    player->saves_status_kind = kind;
}

static NkUiId player_ui_saves_initial_focus(const PlayerUiNk *player) {
    if (player->saves_bring_in.name[0]) return PLAYER_UI_ID_SAVES_BRING_IN;
    return player->saves_has ? PLAYER_UI_ID_SAVES_BACK_UP : PLAYER_UI_ID_SAVES_ADD;
}

static const char *player_ui_saves_bring_in_note(NkMemstickPlanDecision decision) {
    switch (decision) {
    case NK_MEMSTICK_PLAN_KEEP: return NK_UI_STR(REMOVE_SAVES_BRING_IN_KEEP);
    case NK_MEMSTICK_PLAN_DEDUPLICATE: return NK_UI_STR(REMOVE_SAVES_BRING_IN_IDENTICAL);
    case NK_MEMSTICK_PLAN_STAGE: return NK_UI_STR(REMOVE_SAVES_BRING_IN_ACTIVE);
    }
    return NULL;
}

/* Add to Steam (#2152): finds Steam, its accounts and the shortcut; writes nothing. */
static void player_ui_open_steam(PlayerUiNk *player, const PlayerApp *app, int index) {
    PlayerSteamPlan *plan = &player->steam_plan;
    const char *base;
    size_t length;
    player->steam_index = index;
    player->steam_chosen = 0u;
    player->steam_status[0] = '\0';
    player->steam_status_kind = PLAYER_UI_NOTE_NEUTRAL;
    player->steam_blocked[0] = '\0';
    (void)player_app_plan_steam_shortcut(app, index, NULL, NULL, plan);
    if (!plan->ready) {
        (void)player_steam_outcome_text(plan, player->steam_blocked,
                                        sizeof(player->steam_blocked));
    }
    for (size_t i = 0u; i < plan->account_count && i < PLAYER_UI_STEAM_ACCOUNT_CAP; ++i) {
        if (plan->account_names[i][0] != '\0') {
            (void)snprintf(player->steam_account_labels[i],
                           sizeof(player->steam_account_labels[i]), "%s",
                           plan->account_names[i]);
        } else {
            (void)snprintf(player->steam_account_labels[i],
                           sizeof(player->steam_account_labels[i]), NK_UI_STR(ADAPTER_STEAM_ACCOUNT_FALLBACK),
                           plan->accounts[i].id);
        }
        player->steam_accounts[i] = player->steam_account_labels[i];
    }
    /* The program's file name and the launch options: what Steam starts. */
    length = strlen(plan->exe);
    while (length > 0u && !nk_platform_is_path_separator(plan->exe[length - 1u])) length--;
    base = plan->exe + length;
    {
        char options[PLAYER_STEAM_OPTIONS_MAX];
        (void)player_display_text(plan->launch_options, options, sizeof(options));
        (void)snprintf(player->steam_command, sizeof(player->steam_command), "%s %s",
                       base[0] != '\0' ? base : "nakagawa_player", options);
    }
    player->steam_focus = plan->ready ? PLAYER_UI_ID_STEAM_ADD : PLAYER_UI_ID_STEAM_BACK;
}

/* One Add to Steam dialog control: choose an account, or write the shortcut. */
static void player_ui_steam_op(PlayerUiNk *player, unsigned op) {
    PlayerSteamPlan *plan = &player->steam_plan;
    if (!plan->ready) return;
    if (op >= PLAYER_UI_STEAM_ACCOUNT_BASE) {
        size_t row = (size_t)(op - PLAYER_UI_STEAM_ACCOUNT_BASE);
        if (row < plan->account_count) {
            player->steam_chosen = row;
            player->steam_status[0] = '\0';
            player->steam_focus = (NkUiId)(PLAYER_UI_ID_STEAM_ACCOUNT_BASE + (NkUiId)row);
        }
    } else if (op == PLAYER_UI_STEAM_ADD) {
        char stamp[24];
        PlayerSteamOutcome outcome;
        player_app_save_stamp_now(stamp, sizeof(stamp));
        outcome = player_steam_add(plan, player->steam_chosen, NULL, stamp);
        (void)player_steam_outcome_text(plan, player->steam_status,
                                        sizeof(player->steam_status));
        player->steam_status_kind =
            outcome == PLAYER_STEAM_ADDED || outcome == PLAYER_STEAM_UPDATED
                ? PLAYER_UI_NOTE_DONE
                : outcome == PLAYER_STEAM_ALREADY ? PLAYER_UI_NOTE_NEUTRAL
                                                  : PLAYER_UI_NOTE_WARNING;
        player->steam_focus = player_steam_outcome_ok(outcome) ? PLAYER_UI_ID_STEAM_BACK
                                                               : PLAYER_UI_ID_STEAM_ADD;
    }
}

static void player_ui_open_saves(PlayerUiNk *player, const PlayerApp *app, int index) {
    player->saves_index = index;
    (void)snprintf(player->saves_disc, sizeof(player->saves_disc), "%s",
                   app->games[index].disc_id);
    player->saves_first = 0u;
    player->saves_ask = PLAYER_UI_SAVES_ASK_NONE;
    player->saves_waiting_pick = false;
    player->saves_status[0] = '\0';
    player->saves_status_kind = PLAYER_UI_NOTE_NEUTRAL;
    player_ui_saves_reload(player, app, index);
    player->saves_focus = player_ui_saves_initial_focus(player);
}

/* Add a save with the folder picked: a save of that name already there asks
   before it is replaced. */
static void player_ui_saves_add(PlayerUiNk *player, PlayerApp *app, int index,
                                const char *picked, bool replace) {
    char stamp[24];
    char added[72];
    char error[NK_MAX_PATH + 160];
    char text[sizeof(player->saves_status)];
    PlayerSavesResult result;
    added[0] = '\0';
    error[0] = '\0';
    player_app_save_stamp_now(stamp, sizeof(stamp));
    result = player_app_add_save(app, index, picked, replace, stamp, added, sizeof(added), error,
                                 sizeof(error));
    if (result == PLAYER_SAVES_EXISTS) {
        (void)snprintf(player->saves_ask_path, sizeof(player->saves_ask_path), "%s", picked);
        (void)snprintf(player->saves_ask_name, sizeof(player->saves_ask_name), "%s", added);
        player->saves_ask = PLAYER_UI_SAVES_ASK_REPLACE;
        player->saves_focus = PLAYER_UI_ID_SAVES_CANCEL;
        return;
    }
    player->saves_ask = PLAYER_UI_SAVES_ASK_NONE;
    player->saves_focus = PLAYER_UI_ID_SAVES_ADD;
    if (result == PLAYER_SAVES_DONE) {
        player->saves_first = 0u;
        player_ui_saves_reload(player, app, index);
        (void)snprintf(text, sizeof(text), NK_UI_STR(ADAPTER_SAVES_ADD_RESULT),
                       replace ? NK_UI_STR(ADAPTER_SAVES_REPLACED) : NK_UI_STR(ADAPTER_SAVES_ADDED), added,
                       replace ? NK_UI_STR(ADAPTER_SAVES_REPLACE_BACKUP) : "",
                       error[0] != '\0' ? " " : "");
        if (error[0] != '\0') {
            size_t used = strlen(text);
            (void)snprintf(text + used, sizeof(text) - used, "%s", error);
        }
        player_ui_saves_status(player, error[0] != '\0' ? PLAYER_UI_NOTE_WARNING
                                                         : PLAYER_UI_NOTE_DONE, text);
        return;
    }
    player_ui_saves_status(player, PLAYER_UI_NOTE_NEUTRAL, "");
    player_app_set_error(app, "SAVES_ADD_FAILED", NK_UI_STR(ADAPTER_SAVES_ADD_ERROR_TITLE),
                         error[0] != '\0' ? error : NK_UI_STR(ADAPTER_SAVES_ADD_ERROR),
                         NK_UI_STR(ADAPTER_RECOVERY_BACK),
                         VIEW_LIBRARY);
}

/* One Saves dialog control. Back up and Restore run here, on this frame. */
static void player_ui_saves_bring_in(PlayerUiNk *player, PlayerApp *app, int index) {
    NkMemstickPlanUnit selected = player->saves_bring_in;
    NkMemstickCollisionResult result = player_app_bring_in(app, index, &selected);
    char text[sizeof(player->saves_status)];
    player->saves_ask = PLAYER_UI_SAVES_ASK_NONE;
    player_ui_saves_reload(player, app, index);
    player->saves_focus = player_ui_saves_initial_focus(player);
    if (result == NK_MEMSTICK_COLLISION_COPIED) {
        if (selected.decision == NK_MEMSTICK_PLAN_KEEP)
            (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_SAVES_BRING_IN_KEPT), selected.name);
        else if (selected.decision == NK_MEMSTICK_PLAN_DEDUPLICATE)
            (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_SAVES_BRING_IN_ALREADY), selected.name);
        else
            (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_SAVES_BRING_IN_DONE), selected.name);
        player_ui_saves_status(player, PLAYER_UI_NOTE_DONE, text);
    } else if (result == NK_MEMSTICK_COLLISION_STICK_IN_USE) {
        player_ui_saves_status(player, PLAYER_UI_NOTE_WARNING, NK_UI_STR(REMOVE_SAVES_BRING_IN_BUSY));
    } else {
        (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_SAVES_BRING_IN_FAILED),
                       nk_memstick_bring_in_result_code(result));
        player_ui_saves_status(player, PLAYER_UI_NOTE_WARNING, text);
    }
    fprintf(stderr, "SAVES_BRING_IN %s: %s\n", nk_memstick_bring_in_result_code(result),
            player->saves_status);
}

static void player_ui_saves_op(PlayerUiNk *player, PlayerApp *app, int index, unsigned op,
                               PlayerUiAction *out_action) {
    char stamp[24];
    char path[NK_MAX_PATH];
    char error[NK_MAX_PATH + 160];
    char text[sizeof(player->saves_status)];
    PlayerSavesResult result;
    bool blocked = player_app_saves_blocker(app, index) != NULL;
    error[0] = '\0';
    if (op == PLAYER_UI_SAVES_OPEN_FOLDER) {
        if (out_action != NULL) {
            out_action->kind = PLAYER_UI_ACTION_OPEN_SAVE_FOLDER;
            out_action->primary = PLAYER_UI_PRIMARY_NONE;
            out_action->title_index = (size_t)index;
        }
    } else if (op == PLAYER_UI_SAVES_OLDER) {
        if (player->saves_first + PLAYER_UI_SAVES_ROW_CAP < player->saves_list_count) {
            player->saves_first += PLAYER_UI_SAVES_ROW_CAP;
        }
        player_ui_saves_reload(player, app, index);
        player->saves_focus = player->saves_first + PLAYER_UI_SAVES_ROW_CAP <
                                      player->saves_list_count
                                  ? PLAYER_UI_ID_SAVES_OLDER : PLAYER_UI_ID_SAVES_NEWER;
    } else if (op == PLAYER_UI_SAVES_NEWER) {
        player->saves_first = player->saves_first > PLAYER_UI_SAVES_ROW_CAP
                                  ? player->saves_first - PLAYER_UI_SAVES_ROW_CAP : 0u;
        player_ui_saves_reload(player, app, index);
        player->saves_focus = player->saves_first > 0u ? PLAYER_UI_ID_SAVES_NEWER
                                                       : PLAYER_UI_ID_SAVES_OLDER;
    } else if (blocked) {
        player->saves_ask = PLAYER_UI_SAVES_ASK_NONE;
    } else if (op == PLAYER_UI_SAVES_BACK_UP) {
        player_app_save_stamp_now(stamp, sizeof(stamp));
        result = player_app_back_up_saves(app, index, stamp, PLAYER_SAVE_BACKUP_MANUAL, path,
                                          sizeof(path), error, sizeof(error));
        player->saves_first = 0u;
        player_ui_saves_reload(player, app, index);
        if (result == PLAYER_SAVES_DONE) {
            player_ui_saves_status(player, PLAYER_UI_NOTE_DONE,
                                   NK_UI_STR(ADAPTER_SAVES_BACKUP_SUCCESS));
        } else if (result == PLAYER_SAVES_NOTHING) {
            player_ui_saves_status(player, PLAYER_UI_NOTE_NEUTRAL,
                                   NK_UI_STR(ADAPTER_SAVES_NONE_TO_BACK_UP));
        } else {
            player_ui_saves_status(player, PLAYER_UI_NOTE_NEUTRAL, "");
            player_app_set_error(app, "SAVES_BACKUP_FAILED", NK_UI_STR(ADAPTER_SAVES_BACKUP_ERROR_TITLE),
                                 error[0] != '\0' ? error
                                                  : NK_UI_STR(ADAPTER_SAVES_BACKUP_ERROR),
                                 NK_UI_STR(ADAPTER_RECOVERY_BACK), VIEW_LIBRARY);
        }
    } else if (op == PLAYER_UI_SAVES_ADD) {
        player->saves_waiting_pick = true;
        app->saves_pick_ready = false;
        app->saves_pick_index = index;
        app->request_save_folder_picker = true;
        player_ui_saves_status(player, PLAYER_UI_NOTE_NEUTRAL,
                               NK_UI_STR(ADAPTER_SAVES_PICK_FOLDER));
    } else if (op == PLAYER_UI_SAVES_BRING_IN && !blocked && player->saves_bring_in.name[0]) {
        player->saves_ask = PLAYER_UI_SAVES_ASK_BRING_IN;
        player->saves_focus = PLAYER_UI_ID_SAVES_CANCEL;
    } else if (op >= PLAYER_UI_SAVES_RESTORE_BASE) {
        size_t row = (size_t)(op - PLAYER_UI_SAVES_RESTORE_BASE);
        if (row < player_ui_saves_page_rows(player)) {
            player->saves_ask = PLAYER_UI_SAVES_ASK_RESTORE;
            player->saves_ask_row = row;
            player->saves_focus = PLAYER_UI_ID_SAVES_CANCEL;
        }
    } else if (op == PLAYER_UI_SAVES_CANCEL) {
        player->saves_focus = player->saves_ask == PLAYER_UI_SAVES_ASK_BRING_IN
                            ? PLAYER_UI_ID_SAVES_BRING_IN
                            : player->saves_has ? PLAYER_UI_ID_SAVES_BACK_UP : PLAYER_UI_ID_SAVES_ADD;
        player->saves_ask = PLAYER_UI_SAVES_ASK_NONE;
    } else if (op == PLAYER_UI_SAVES_CONFIRM && player->saves_ask == PLAYER_UI_SAVES_ASK_BRING_IN) {
        player_ui_saves_bring_in(player, app, index);
    } else if (op == PLAYER_UI_SAVES_CONFIRM &&
               player->saves_ask == PLAYER_UI_SAVES_ASK_REPLACE) {
        (void)snprintf(path, sizeof(path), "%s", player->saves_ask_path);
        player_ui_saves_add(player, app, index, path, true);
    } else if (op == PLAYER_UI_SAVES_CONFIRM &&
               player->saves_ask == PLAYER_UI_SAVES_ASK_RESTORE &&
               player->saves_ask_row < player_ui_saves_page_rows(player)) {
        char kept[NK_MAX_PATH];
        char when[32];
        const PlayerSaveBackup *backup =
            &player->saves_list[player->saves_first + player->saves_ask_row];
        (void)snprintf(path, sizeof(path), "%s", backup->path);
        (void)snprintf(when, sizeof(when), "%s", player->saves_when[player->saves_ask_row]);
        player->saves_ask = PLAYER_UI_SAVES_ASK_NONE;
        player->saves_focus = PLAYER_UI_ID_SAVES_BACK_UP;
        kept[0] = '\0';
        player_app_save_stamp_now(stamp, sizeof(stamp));
        result = player_app_restore_saves(app, index, path, stamp, kept, sizeof(kept), error,
                                          sizeof(error));
        player->saves_first = 0u;
        player_ui_saves_reload(player, app, index);
        if (result == PLAYER_SAVES_DONE) {
            (void)snprintf(text, sizeof(text),
                           NK_UI_STR(ADAPTER_SAVES_RESTORE_RESULT), when,
                           kept[0] != '\0'
                               ? NK_UI_STR(ADAPTER_SAVES_RESTORE_OLD_FOLDER)
                               : "",
                           error[0] != '\0' ? " " : "");
            if (error[0] != '\0') {
                size_t used = strlen(text);
                (void)snprintf(text + used, sizeof(text) - used, "%s", error);
            }
            player_ui_saves_status(player, error[0] != '\0' ? PLAYER_UI_NOTE_WARNING
                                                              : PLAYER_UI_NOTE_DONE, text);
        } else {
            player_ui_saves_status(player, PLAYER_UI_NOTE_NEUTRAL, "");
            player_app_set_error(app, "SAVES_RESTORE_FAILED", NK_UI_STR(ADAPTER_SAVES_RESTORE_ERROR_TITLE),
                                 error[0] != '\0' ? error
                                                  : NK_UI_STR(ADAPTER_SAVES_RESTORE_ERROR),
                                 NK_UI_STR(ADAPTER_RECOVERY_BACK), VIEW_LIBRARY);
        }
    }
}

bool player_ui_nk_at_library_root(const PlayerUiNk *player) {
    return player != NULL && player->ready &&
           nk_ui_router_current(player->router) == PLAYER_UI_ROUTE_LIBRARY &&
           nk_ui_router_depth(player->router) == 1u;
}

size_t player_ui_nk_shown_titles(const PlayerUiNk *player, int *out, size_t cap) {
    size_t count;
    if (player == NULL || out == NULL) return 0u;
    count = player->shown_count < cap ? player->shown_count : cap;
    memcpy(out, player->shown_titles, count * sizeof(out[0]));
    return count;
}

/* The library's find box. From a hardware keyboard, which types straight into
   it, focus goes to DONE, so Enter moves on to the matches; otherwise to the
   on-screen keyboard's q, ready for a controller or the mouse. */
static void player_ui_find_open(PlayerUiNk *player, bool from_keyboard) {
    player->find_open = true;
    player->find_query[0] = '\0';
    ui_keyboard_init(&player->find_keyboard, player->find_query,
                     sizeof(player->find_query) - 1u, player_ui_find_accepts, NULL);
    player->library_page = 0u;
    player->find_focus = from_keyboard
        ? PLAYER_UI_ID_LIBRARY_FIND_DONE
        : PLAYER_UI_ID_LIBRARY_FIND_KEY_BASE + UI_KEYBOARD_KEY_DEFAULT;
}

/* Clears and closes the box; focus goes back to FIND. */
static void player_ui_find_close(PlayerUiNk *player) {
    player->find_open = false;
    player->find_query[0] = '\0';
    player->library_page = 0u;
    player->find_focus = PLAYER_UI_ID_LIBRARY_FIND;
}

/* A hardware keyboard's text (SDL's UTF-8), typed through the on-screen keyboard's
   filter and cap (player_ui_find_accepts): control characters and bytes that are not
   a whole UTF-8 character are left out. */
static void player_ui_find_type(PlayerUiNk *player, const char *text) {
    if (ui_keyboard_type(&player->find_keyboard, text)) player->library_page = 0u;
    /* As in a box opened from the keyboard: Enter after typing moves on to the
       matches, never types the on-screen key that had focus. */
    player->find_focus = PLAYER_UI_ID_LIBRARY_FIND_DONE;
}

/* Removes the last character, all of its UTF-8 bytes. */
static void player_ui_find_delete(PlayerUiNk *player) {
    if (ui_keyboard_delete(&player->find_keyboard)) player->library_page = 0u;
    player->find_focus = PLAYER_UI_ID_LIBRARY_FIND_DONE;
}

/* The nickname editor starts empty, showing the nickname in use until
   something is typed, and opens on its keyboard's first letter. Its keyboard
   takes only what nk_nickname_valid would, up to NK_NICKNAME_MAX_BYTES. */
static void player_ui_nickname_open(PlayerUiNk *player, bool first_launch, bool server_mode) {
    player->nickname_open = true;
    player->nickname_first_launch = first_launch && !server_mode;
    player->nickname_is_server = server_mode;
    player->nickname_is_port = false;
    player->nickname_is_ra_user = false;
    player->nickname_is_ra_password = false;
    player->nickname_refused = false;
    player->nickname_focus = PLAYER_UI_ID_NICKNAME_KEY_BASE + UI_KEYBOARD_KEY_DEFAULT;
    player->nickname_text[0] = '\0';
    ui_keyboard_init(&player->nickname_keyboard, player->nickname_text,
                     server_mode ? NK_ADHOC_SERVER_MAX_LEN : NK_NICKNAME_MAX_BYTES,
                     server_mode ? player_ui_server_accepts : player_ui_nickname_accepts, NULL);
}

/* The same editor for Online play's port offset (#1139): digits only, five at most. */
static void player_ui_port_open(PlayerUiNk *player) {
    player_ui_nickname_open(player, false, false);
    player->nickname_is_port = true;
    player->nickname_focus = PLAYER_UI_ID_NICKNAME_KEY_BASE; /* "1", the first digit key */
    ui_keyboard_init(&player->nickname_keyboard, player->nickname_text, 5u,
                     player_ui_port_accepts, NULL);
}

/* The same editor for RetroAchievements' sign-in form (#1316): it opens on what the form holds,
   so a typo can be deleted, and its Done copies the text back into the form. */
static void player_ui_ra_field_open(PlayerUiNk *player, bool password) {
    const char *from = password ? player->ra_form.password : player->ra_form.username;
    player_ui_nickname_open(player, false, false);
    player->nickname_is_ra_user = !password;
    player->nickname_is_ra_password = password;
    ui_keyboard_init(&player->nickname_keyboard, player->nickname_text,
                     password ? PLAYER_UI_RA_PASSWORD_MAX : PLAYER_UI_RA_USERNAME_MAX,
                     password ? player_ui_ra_password_accepts : player_ui_ra_username_accepts,
                     NULL);
    (void)snprintf(player->nickname_text, sizeof(player->nickname_text), "%s", from);
}

/* What Done saves: the typed text without the spaces (U+0020) before and after
   it, so "  Kai " is "Kai" and a nickname of spaces alone is empty, which
   nk_nickname_valid refuses. The typed text itself is left as it is. */
static const char *player_ui_nickname_trimmed(const char *text, char *out, size_t cap) {
    size_t start = 0u, end = strlen(text);
    while (start < end && text[start] == ' ') start++;
    while (end > start && text[end - 1u] == ' ') end--;
    if (end - start >= cap) end = start + cap - 1u;
    memcpy(out, text + start, end - start);
    out[end - start] = '\0';
    return out;
}

static void player_ui_nickname_close(PlayerUiNk *player) {
    volatile char *text = player->nickname_text;
    player->nickname_open = false;
    player->nickname_is_server = false;
    player->nickname_is_port = false;
    player->nickname_is_ra_user = false;
    player->nickname_is_ra_password = false;
    player->nickname_refused = false;
    /* The whole buffer: it may have held a password. */
    for (size_t i = 0u; i < sizeof(player->nickname_text); ++i) text[i] = '\0';
}

/* A physical keyboard types through the on-screen keyboard's rules (its filter
   and the 32-byte cap), and moves the focus to Done, so Enter after typing saves.
   Done still checks the whole rule (nk_nickname_valid). */
static void player_ui_nickname_type(PlayerUiNk *player, const char *text) {
    (void)ui_keyboard_type(&player->nickname_keyboard, text);
    player->nickname_focus = PLAYER_UI_ID_NICKNAME_SAVE;
    player->nickname_refused = false;
}

/* Removes the last character, all of its UTF-8 bytes. */
static void player_ui_nickname_delete(PlayerUiNk *player) {
    (void)ui_keyboard_delete(&player->nickname_keyboard);
    player->nickname_focus = PLAYER_UI_ID_NICKNAME_SAVE;
    player->nickname_refused = false;
}

bool player_ui_nk_take_sign_in(PlayerUiNk *player, char *username, size_t username_cap,
                               char *password, size_t password_cap) {
    return player != NULL && player_ui_sign_in_take(&player->ra_form, username, username_cap,
                                                    password, password_cap);
}

bool player_ui_nk_nickname_open(const PlayerUiNk *player) {
    return player != NULL && player->ready && player->nickname_open;
}

bool player_ui_nk_find_open(const PlayerUiNk *player) {
    return player != NULL && player->ready && player->find_open &&
           nk_ui_router_current(player->router) == PLAYER_UI_ROUTE_LIBRARY;
}

/* A text box (the find box, the nickname editor) takes what is typed: SDL's text
   input brings the characters, Backspace deletes one, and a printable key is not
   also a shortcut (D, Q, E). Returns true when the event was the box's. */
static bool player_ui_take_typing(PlayerUiNk *player, const SDL_Event *event,
                                  void (*type)(PlayerUiNk *, const char *),
                                  void (*erase)(PlayerUiNk *)) {
    if (event->type == SDL_EVENT_TEXT_INPUT && event->text.text != NULL) {
        type(player, event->text.text);
        return true;
    }
    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
        bool control = (event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0;
        if (event->key.key == SDLK_BACKSPACE) {
            if (event->type == SDL_EVENT_KEY_DOWN) erase(player);
            return true;
        }
        if (!control && event->key.key >= 0x20u && event->key.key < 0x7Fu) return true;
    }
    return false;
}

/* Love All's list: Up, Down and the page keys scroll it (it has one control, Back), and any
   scroll stops it rolling by itself. True when the event was the list's. */
static bool player_ui_love_all_take(PlayerUiNk *player, const NkUiEvent *translated) {
    NkUiScrollState state;
    bool vertical = translated->type == NK_UI_EV_NAV &&
                    (translated->dir == NK_UI_DIR_UP || translated->dir == NK_UI_DIR_DOWN);
    bool forward;
    float step;
    if (translated->type == NK_UI_EV_WHEEL) player->love_all_rolling = false;
    if (!translated->down || !(vertical || translated->type == NK_UI_EV_PAGE)) return false;
    player->love_all_rolling = false;
    if (nk_ui_scroll_state(player->ui, player_ui_love_all_list_id(), &state)) {
        forward = translated->dir == NK_UI_DIR_DOWN || translated->dir == NK_UI_DIR_RIGHT;
        step = translated->type == NK_UI_EV_PAGE ? state.viewport * 0.8f : 72.0f;
        player->love_all_scroll = state.offset + (forward ? step : -step);
        if (player->love_all_scroll > state.max_offset) player->love_all_scroll = state.max_offset;
        if (player->love_all_scroll < 0.0f) player->love_all_scroll = 0.0f;
        player->love_all_scroll_set = true;
    }
    return true;
}

/* A key or pad press as the launcher reads it (the same keys nk_ui_sdl_input gives Back and
   Select: Esc, which Backspace already is here, the pad's B; Enter, the pad's A), for the
   code. Releases, repeats, the stick and the pointer are no press. */
static NkUiEvent player_ui_secret_press(const SDL_Event *event) {
    NkUiEvent press;
    memset(&press, 0, sizeof(press));
    press.type = NK_UI_EV_POINTER; /* no press */
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
        press.down = true;
        switch (event->key.key) {
            case SDLK_UP: press.type = NK_UI_EV_NAV; press.dir = NK_UI_DIR_UP; break;
            case SDLK_DOWN: press.type = NK_UI_EV_NAV; press.dir = NK_UI_DIR_DOWN; break;
            case SDLK_LEFT: press.type = NK_UI_EV_NAV; press.dir = NK_UI_DIR_LEFT; break;
            case SDLK_RIGHT: press.type = NK_UI_EV_NAV; press.dir = NK_UI_DIR_RIGHT; break;
            case SDLK_ESCAPE: press.type = NK_UI_EV_BACK; break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER: press.type = NK_UI_EV_ACTIVATE; break;
            default: press.type = NK_UI_EV_PAGE; break; /* another key: the code starts again */
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        press.down = true;
        switch (event->gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: press.type = NK_UI_EV_NAV; press.dir = NK_UI_DIR_UP; break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                press.type = NK_UI_EV_NAV; press.dir = NK_UI_DIR_DOWN; break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                press.type = NK_UI_EV_NAV; press.dir = NK_UI_DIR_LEFT; break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                press.type = NK_UI_EV_NAV; press.dir = NK_UI_DIR_RIGHT; break;
            case SDL_GAMEPAD_BUTTON_EAST: press.type = NK_UI_EV_BACK; break;
            case SDL_GAMEPAD_BUTTON_SOUTH: press.type = NK_UI_EV_ACTIVATE; break;
            default: press.type = NK_UI_EV_PAGE; break;
        }
    }
    return press;
}

/* The classic cheat code (launcher_secret.h), from a controller or the keyboard only, in the
   launcher while no game starts or runs and no text box is open. True when the press is the
   code's alone: the B after the eight directions, and the A that completes it. */
bool player_ui_nk_take_secret(PlayerUiNk *player, const SDL_Event *event) {
    NkUiEvent press;
    if (player == NULL || event == NULL || !player->ready || !player->secret_allowed ||
        player->nickname_open || player_ui_nk_find_open(player)) {
        return false;
    }
    press = player_ui_secret_press(event);
    switch (launcher_secret_feed(&player->secret_detector, launcher_secret_key_for_event(&press),
                                 player_ui_nk_now_s(player))) {
        case LAUNCHER_SECRET_FOUND:
            player->secret_found = true;
            return true;
        case LAUNCHER_SECRET_SWALLOW:
            return true;
        case LAUNCHER_SECRET_PASS:
        default:
            return false;
    }
}

bool player_ui_nk_handle_event(PlayerUiNk *player, const SDL_Event *event) {
    NkUiEvent translated[8];
    size_t count;
    double now_s;
    if (player == NULL || event == NULL) return false;
    if (event->type == SDL_EVENT_RENDER_DEVICE_RESET) {
        /* SDL3: after a device reset every texture must be made again (a Direct3D
           device lost to a driver reset or update). The backend drops its atlas, image
           and waves textures and the next frame uploads what it draws. */
        nk_ui_sdlr_reset(player->backend);
        return true;
    }
    if (!player->ready) return false;
    now_s = player_ui_nk_now_s(player);
    if (player->nickname_open) {
        /* The nickname editor takes what is typed as the find box does; Esc or the
           pad's B is its Skip or Cancel, below. */
        if (player_ui_take_typing(player, event, player_ui_nickname_type,
                                  player_ui_nickname_delete)) {
            return true;
        }
    } else if (player_ui_nk_find_open(player)) {
        /* Esc or the pad's B clears and closes the find box, below. */
        if (player_ui_take_typing(player, event, player_ui_find_type, player_ui_find_delete)) {
            return true;
        }
    } else if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
               nk_ui_router_current(player->router) == PLAYER_UI_ROUTE_LIBRARY &&
               player->library_game_total > 0u &&
               (event->key.key == SDLK_SLASH ||
                (event->key.key == SDLK_F && (event->key.mod & SDL_KMOD_CTRL) != 0))) {
        /* "/" or Ctrl+F opens the box from a hardware keyboard. */
        player_ui_find_open(player, true);
        return true;
    }
    count = nk_ui_sdl_input_translate(&player->input, event, now_s,
                                     translated,
                                     sizeof(translated) / sizeof(translated[0]));
    for (size_t i = 0u; i < count; ++i) {
        if (nk_ui_router_current(player->router) == PLAYER_UI_ROUTE_LOVE_ALL &&
            player_ui_love_all_take(player, &translated[i])) {
            continue;
        }
        if (translated[i].type == NK_UI_EV_BACK && translated[i].down) {
            player->pending_sound = PLAYER_UI_SOUND_BACK;
        } else if (translated[i].type == NK_UI_EV_ACTIVATE && translated[i].down &&
                   !translated[i].repeat && player->pending_sound < 0) {
            player->pending_sound = PLAYER_UI_SOUND_CONFIRM;
        }
        if (translated[i].type == NK_UI_EV_BACK && player->nickname_open) {
            /* The editor answers Back itself (Skip or Cancel); the routes stay. */
            if (translated[i].down) nk_ui_push_event(player->ui, &translated[i]);
        } else if (translated[i].type == NK_UI_EV_BACK && player_ui_nk_find_open(player)) {
            if (translated[i].down) player_ui_find_close(player);
        } else if (translated[i].type == NK_UI_EV_BACK &&
                   nk_ui_router_current(player->router) == PLAYER_UI_ROUTE_SAVES &&
                   player->saves_ask != PLAYER_UI_SAVES_ASK_NONE) {
            /* The Saves card's question answers Back with Cancel and the card
               stays (docs/PLAYER_UI_DESIGN.md); the router would close it. */
            if (translated[i].down) nk_ui_push_event(player->ui, &translated[i]);
        } else if (translated[i].type == NK_UI_EV_BACK) {
            nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
            (void)nk_ui_router_handle_event(player->router, &translated[i]);
        } else {
            nk_ui_push_event(player->ui, &translated[i]);
        }
    }
    return true;
}

/* A tapped Back on the prompt bar is the system's Back, so every view goes back the way it
   takes Android's back button (the host gives that key Esc's routes), and touch stays the
   active input. */
static void player_ui_push_system_back(const PlayerUiNk *player) {
    SDL_Event back;
    memset(&back, 0, sizeof(back));
    back.type = SDL_EVENT_KEY_DOWN;
    back.key.windowID = player->window != NULL ? SDL_GetWindowID(player->window) : 0u;
    back.key.key = SDLK_AC_BACK;
    back.key.scancode = SDL_SCANCODE_AC_BACK;
    back.key.down = true;
    (void)SDL_PushEvent(&back);
    back.type = SDL_EVENT_KEY_UP;
    back.key.down = false;
    (void)SDL_PushEvent(&back);
}

void player_ui_nk_note_window_focus(PlayerUiNk *player, bool focused) {
    if (player == NULL) return;
    player->window_unfocused = !focused;
}

/* Sounds follow the setting, and stay closed while a game starts or runs so its own
   audio is left alone: the launch starts the game's process only once they have let
   go of the device (player_ui_nk_launcher_audio_closed). */
static void player_ui_nk_follow_sounds(PlayerUiNk *player, const PlayerApp *app) {
    player_ui_sounds_set_enabled(player->sounds, app->settings.ui_sounds &&
                                                     !app->is_game_running &&
                                                     !app->launch_pending);
}

/* A clip that could not be read or decoded, or no audio device for it, is logged
   once by state, never with the game's path or title. A disc without one is not. */
static void player_ui_nk_report_snd0(PlayerUiNk *player, PlayerApp *app) {
    PlayerUiSnd0State state = player_ui_snd0_state(player->snd0);
    if ((int)state == player->snd0_reported) return;
    player->snd0_reported = (int)state;
    if (state == PLAYER_UI_SND0_DAMAGED || state == PLAYER_UI_SND0_NO_DEVICE) {
        const char *code = state == PLAYER_UI_SND0_DAMAGED ? "GAME_MUSIC_DAMAGED"
                                                           : "GAME_MUSIC_NO_DEVICE";
        fprintf(stderr, "[PLAYER] %s: %s\n", code, player_ui_snd0_detail(player->snd0));
        player_dev_log_add(app, "audio", "%s: %s", code, player_ui_snd0_detail(player->snd0));
    }
}

void player_ui_nk_update_music(PlayerUiNk *player, PlayerApp *app) {
    PlayerUiMusicRequest request;
    PlayerUiMusicState state;
    if (player == NULL || app == NULL) return;
    /* The launch waits on the sounds as on the music, so they follow it here too. */
    player_ui_nk_follow_sounds(player, app);
    request.enabled = app->settings.launcher_music;
    /* A game that is starting counts as running: the music fades out and lets go
       of the audio device before the launch starts the game's process. */
    request.in_launcher = !app->is_game_running && !app->launch_pending;
    request.focused = !player->window_unfocused;
    request.volume = app->settings.launcher_music_volume;
    {
        /* The open game page's own clip, at the Music volume; the launcher music
           pauses while one loads or plays, and picks up where it stopped. */
        PlayerUiSnd0Request clip;
        clip.enabled = app->settings.launcher_game_music;
        clip.in_launcher = request.in_launcher;
        clip.focused = request.focused;
        clip.volume = request.volume;
        clip.iso_path = player->snd0_index < (size_t)(app->game_count > 0 ? app->game_count : 0)
                            ? app->games[player->snd0_index].iso_path
                            : NULL;
        player_ui_snd0_update(player->snd0, &clip, SDL_GetTicks());
        if (player_ui_snd0_quiets_music(player->snd0)) request.focused = false;
        player_ui_nk_report_snd0(player, app);
    }
    player_ui_music_update(player->music, &request, SDL_GetTicks());
    {
        uint32_t timeout_ms = player_ui_music_take_release_timeout(player->music);
        if (timeout_ms != 0u) {
            fprintf(stderr,
                    "[PLAYER] LAUNCHER_MUSIC_RELEASE_TIMEOUT: the audio device stopped taking "
                    "data; released after %u ms\n", (unsigned)timeout_ms);
            player_dev_log_add(app, "audio",
                               "LAUNCHER_MUSIC_RELEASE_TIMEOUT: the audio device stopped "
                               "taking data; released after %u ms", (unsigned)timeout_ms);
        }
    }
    state = player_ui_music_state(player->music);
    if ((int)state == player->music_reported) return;
    player->music_reported = (int)state;
    if (state == PLAYER_UI_MUSIC_MISSING || state == PLAYER_UI_MUSIC_DAMAGED ||
        state == PLAYER_UI_MUSIC_NO_DEVICE) {
        const char *code = state == PLAYER_UI_MUSIC_MISSING ? "LAUNCHER_MUSIC_MISSING"
                           : state == PLAYER_UI_MUSIC_DAMAGED ? "LAUNCHER_MUSIC_DAMAGED"
                                                              : "LAUNCHER_MUSIC_NO_DEVICE";
        fprintf(stderr, "[PLAYER] %s: %s\n", code, player_ui_music_detail(player->music));
        player_dev_log_add(app, "audio", "%s: %s", code, player_ui_music_detail(player->music));
    }
}

bool player_ui_nk_launcher_audio_closed(const PlayerUiNk *player) {
    return player == NULL || (!player_ui_music_holds_audio(player->music) &&
                              !player_ui_music_decoding(player->music) &&
                              !player_ui_snd0_holds_audio(player->snd0) &&
                              !player_ui_snd0_decoding(player->snd0) &&
                              !player_ui_sounds_hold_audio(player->sounds));
}

bool player_ui_nk_ui_sounds_hold_audio(const PlayerUiNk *player) {
    return player != NULL && player_ui_sounds_hold_audio(player->sounds);
}

bool player_ui_nk_music_decoding(const PlayerUiNk *player) {
    return player != NULL && player_ui_music_decoding(player->music);
}

int player_ui_nk_music_state(const PlayerUiNk *player) {
    return player != NULL ? (int)player_ui_music_state(player->music) : (int)PLAYER_UI_MUSIC_OFF;
}

const char *player_ui_nk_music_state_name(const PlayerUiNk *player) {
    return player_ui_music_state_name((PlayerUiMusicState)player_ui_nk_music_state(player));
}

const char *player_ui_nk_snd0_state_name(const PlayerUiNk *player) {
    return player_ui_snd0_state_name(player != NULL ? player_ui_snd0_state(player->snd0)
                                                    : PLAYER_UI_SND0_OFF);
}

void player_ui_nk_note_gamepad(PlayerUiNk *player, uint32_t which, bool connected) {
    SDL_Event event;
    if (player == NULL || !player->ready) return;
    memset(&event, 0, sizeof(event));
    event.type = connected ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
    event.gdevice.which = (SDL_JoystickID)which;
    (void)player_ui_nk_handle_event(player, &event);
}

static PlayerUiTier player_ui_record_tier(const PlayerUiCompatibilityRecord *record) {
    return record->tier != PLAYER_UI_TIER_NONE ? record->tier
                                               : player_ui_tier_for_category(record->category);
}

/* Compatibility categories are documentation keys used for tier mapping. Only
   the developer view's display label is localized. */
static const char *player_ui_compatibility_category_label(const char *category) {
    static const struct {
        const char *category;
        const char *label;
    } labels[] = {
        {"Works", NK_UI_STR(ADAPTER_COMPAT_WORKS)},
        {"Partially works", NK_UI_STR(ADAPTER_COMPAT_PARTIAL)},
        {"Boots to a named boundary", NK_UI_STR(ADAPTER_COMPAT_NAMED_BOUNDARY)},
        {"Experimental", NK_UI_STR(ADAPTER_COMPAT_EXPERIMENTAL)},
        {"Untested", NK_UI_STR(ADAPTER_COMPAT_UNTESTED)},
        {"Not started", NK_UI_STR(ADAPTER_COMPAT_NOT_STARTED)},
    };
    for (size_t i = 0u; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        if (strcmp(category, labels[i].category) == 0) return labels[i].label;
    }
    return category;
}

/* The title's compatibility record: its player tier, what to expect and its
   known issues, in plain words. Returns the docs/COMPATIBILITY.md category for
   the developer view. A title with no record gets a sentence for its general
   state and the tier its category maps to. */
static const char *player_ui_compatibility_words(const GameRecord *game, const char **expect,
                                                 const char **known_issues,
                                                 PlayerUiTier *record_tier) {
    *expect = NK_UI_STR(ADAPTER_COMPAT_NO_TEST);
    *known_issues = "";
    *record_tier = PLAYER_UI_TIER_NONE;
    if (game == NULL) return "Untested";
    for (size_t i = 0u;
         i < sizeof(s_compatibility_records) / sizeof(s_compatibility_records[0]);
         ++i) {
        if ((game->title_id[0] != '\0' &&
             strcmp(game->title_id, s_compatibility_records[i].key) == 0) ||
            (game->disc_id[0] != '\0' &&
             strcmp(game->disc_id, s_compatibility_records[i].key) == 0) ||
            (game->title_name[0] != '\0' &&
             strcmp(game->title_name, s_compatibility_records[i].key) == 0)) {
            *expect = s_compatibility_records[i].expect;
            *known_issues = s_compatibility_records[i].known_issues;
            *record_tier = player_ui_record_tier(&s_compatibility_records[i]);
            return s_compatibility_records[i].category;
        }
    }
    if (game->is_experimental) {
        if (game->status == NK_STATUS_BOOTS || game->status == NK_STATUS_PLAYABLE) {
            *expect = NK_UI_STR(ADAPTER_COMPAT_STARTED_UNVERIFIED);
            return "Experimental";
        }
        *expect = NK_UI_STR(ADAPTER_COMPAT_NO_BOOT_EVIDENCE);
        return "Untested";
    }
    switch (game->status) {
        case NK_STATUS_BOOTS:
            *expect = NK_UI_STR(ADAPTER_COMPAT_UNSUPPORTED_BOUNDARY);
            return "Boots to a named boundary";
        case NK_STATUS_PLAYABLE:
            *expect = NK_UI_STR(ADAPTER_COMPAT_PARTS_UNVERIFIED);
            return "Partially works";
        case NK_STATUS_UNIDENTIFIED:
        case NK_STATUS_IDENTIFIED: return "Untested";
        case NK_STATUS_SUPPORTED_PREPARATION:
        case NK_STATUS_PREPARED:
        case NK_STATUS_VERIFIED:
        default:
            *expect = NK_UI_STR(ADAPTER_COMPAT_CANNOT_RUN);
            return "Not started";
    }
}

#ifdef NK_PLAYER_UI_REGRESSION_TEST
void player_ui_nk_test_report_compatibility(void) {
    for (size_t i = 0u;
         i < sizeof(s_compatibility_records) / sizeof(s_compatibility_records[0]);
         ++i) {
        const PlayerUiCompatibilityRecord *record = &s_compatibility_records[i];
        const char *tier = player_ui_tier_word(player_ui_record_tier(record));
        const char *category_tier =
            player_ui_tier_word(player_ui_tier_for_category(record->category));
        char key[96];
        snprintf(key, sizeof(key), "%s", record->key);
        for (char *q = key; *q; q++) if (*q == ' ') *q = '_';
        printf("[PLAYER_UI_TEST] compatibility key=%s category_tier=%s tier=%s\n", key,
               category_tier ? category_tier : "none", tier ? tier : "none");
    }
}
#endif

static bool player_ui_has_seen_boot(const GameRecord *game) {
    return game != NULL && (game->status == NK_STATUS_BOOTS ||
                            game->status == NK_STATUS_PLAYABLE);
}

/* An experimental entry nobody has recorded a result for. A disc with a
   compatibility record (a catalogued game whose title settings are local,
   such as Hot Shots Tennis) is not called Untested beside its PLAYABLE. */
static bool player_ui_unrecorded_experimental(const GameRecord *game) {
    const char *expect;
    const char *known_issues;
    PlayerUiTier record_tier;
    if (game == NULL || !game->is_experimental) return false;
    (void)player_ui_compatibility_words(game, &expect, &known_issues, &record_tier);
    return record_tier == PLAYER_UI_TIER_NONE;
}

/* `unrecorded`: the title is an experimental entry nobody has recorded a result for
   (player_ui_unrecorded_experimental), looked up once per title. */
static const char *player_ui_library_status_word(const GameRecord *game,
                                                 bool package_ready, bool unrecorded) {
    if (package_ready) return NK_UI_STR(ADAPTER_LIBRARY_READY);
    if (unrecorded) {
        return player_ui_has_seen_boot(game)
            ? NK_UI_STR(ADAPTER_COMPAT_EXPERIMENTAL) : NK_UI_STR(ADAPTER_COMPAT_UNTESTED);
    }
    return NK_UI_STR(ADAPTER_LIBRARY_NEEDS_SETUP);
}

/* Status colours follow the design system: Ready teal, experimental and
   untested info blue (with their own shapes), needs setup warning gold. */
static uint32_t player_ui_library_status_color(bool package_ready, bool unrecorded) {
    if (package_ready) return PLAYER_UI_READY;
    if (unrecorded) return PLAYER_UI_INFO;
    return PLAYER_UI_WARNING;
}

/* The project's own showcase demos carry project-drawn pictures. */
static unsigned player_ui_art_kind(const GameRecord *game) {
    if (game == NULL) return NK_UI_ART_NONE;
    if (strcmp(game->title_id, "showcase-scene-v1") == 0) return NK_UI_ART_SHOWCASE_SCENE;
    if (strcmp(game->title_id, "showcase-breakout-v1") == 0) return NK_UI_ART_SHOWCASE_BREAKOUT;
    return NK_UI_ART_NONE;
}

static const char *player_ui_library_status_marker(const GameRecord *game,
                                                   bool package_ready, bool unrecorded) {
    if (package_ready) return "●";
    if (unrecorded) {
        return player_ui_has_seen_boot(game) ? "◆" : "?";
    }
    return "▲";
}

/* The play mode on a title (#2055; src/rt/ace.h): the library card's badge, and for the open
   title the ENHANCEMENTS card's buttons, its line and the last session's line. */
static void player_ui_title_ace(PlayerUiNk *player, const PlayerApp *app, int index,
                                PlayerUiTitle *out) {
    const GameRecord *game = &app->games[index];
    PlayerPlayResolved play;
    player_play_resolve(&app->settings, game->disc_id, &play);
    bool smooth_wanted = play.on && play.part[PLAYER_PART_SMOOTH] != 0;
    player_play_apply_logic_60(&play, out->enhancement_on);
    out->ace_shown = !out->is_demo;
    out->ace_in_effect = play.on;
    out->play_badge = out->is_demo ? NULL
                    : play.mode == PLAYER_PLAY_ACE ? NK_UI_STR(ADAPTER_MODE_ACE)
                    : play.mode == PLAYER_PLAY_ENHANCED ? NK_UI_STR(ADAPTER_MODE_ENHANCED) : NULL;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (index >= 0 && index < PLAYER_UI_TITLE_CAP) {
        /* The library card's badge and the title's own choice, printed when either changes. */
        char reported[sizeof(player->play_reported[0])];
        const char *badge = out->play_badge != NULL ? out->play_badge : "none";
        snprintf(reported, sizeof(reported), "%s|%d", badge, (int)play.chosen);
        if (strcmp(player->play_reported[index], reported) != 0) {
            snprintf(player->play_reported[index], sizeof(player->play_reported[index]), "%s",
                     reported);
            printf("[PLAYER_UI_TEST] next_ui_play_badge index=%d disc=%s badge=%s chosen=%s\n",
                   index, game->disc_id, badge, player_play_mode_name(play.chosen));
        }
    }
#endif
    switch (play.chosen) {
        case PLAYER_PLAY_AUTHENTIC: out->ace_label = NK_UI_STR(ADAPTER_TITLE_MODE_AUTHENTIC); break;
        case PLAYER_PLAY_ENHANCED: out->ace_label = NK_UI_STR(ADAPTER_TITLE_MODE_ENHANCED); break;
        case PLAYER_PLAY_ACE: out->ace_label = NK_UI_STR(ADAPTER_TITLE_MODE_ACE); break;
        default:
            out->ace_label = play.on ? NK_UI_STR(ADAPTER_TITLE_MODE_LIBRARY_ENHANCED)
                                     : NK_UI_STR(ADAPTER_TITLE_MODE_LIBRARY_AUTHENTIC);
            break;
    }
    out->ace_enabled = out->ace_shown && !app->launch_pending && !app->is_game_running &&
        !(app->build_session.is_building &&
          strcmp(app->build_session.disc_id, game->disc_id) == 0);
    out->options_shown = out->ace_shown;
    out->options_enabled = out->ace_enabled;
    out->ace_line = "";
    out->ace_last_session = "";
    /* The 60 FPS switch is Ace's own part: it shows while Ace is in effect (the session flag
       makes a title on Enhanced with a usable package run as Ace). Authentic is the game as
       released, with no 60 FPS game logic. */
    if (play.mode == PLAYER_PLAY_AUTHENTIC) {
        out->enhancement_shown = false;
        out->enhancement_enabled = false;
        out->enhancement_on = false;
    } else if (play.mode != PLAYER_PLAY_ACE && !out->enhancement_session) {
        out->enhancement_shown = false;
    }
    if (index != app->selected_game_index || !out->ace_shown) return;
    if (play.mode == PLAYER_PLAY_AUTHENTIC) {
        out->ace_line = NK_UI_STR(ADAPTER_TITLE_AUTHENTIC_DESCRIPTION);
    } else {
        /* Every part: each as long as its part[64], with the ", " before it. */
        char words[PLAYER_PART_COUNT * (64 + 2)] = "";
        size_t used = 0;
        size_t count = 0;
        for (int p = 0; p < PLAYER_PART_COUNT; p++) {
            const PlayerEnhancedPartInfo *info = player_enhanced_part(p);
            char part[64];
            if (info == NULL || play.part[p] == 0) continue;
            /* "16x texture filtering" for a part with levels, else "crisp system text". */
            if (info->value_count > 2) {
                snprintf(part, sizeof(part), NK_UI_STR(ADAPTER_TITLE_PART_WITH_VALUE),
                         player_enhanced_part_label(p, play.part[p]),
                         info->name);
            } else {
                snprintf(part, sizeof(part), "%s", info->name);
            }
            /* Lower case, but for a "2D": the Smart 2D filter reads "smart 2D filter". */
            for (char *c = part; *c; c++) {
                if (*c == 'D' && c > part && c[-1] >= '0' && c[-1] <= '9') continue;
                *c = (char)tolower((unsigned char)*c);
            }
            int n = snprintf(words + used, sizeof(words) - used, "%s%s", count ? ", " : "", part);
            if (n < 0 || used + (size_t)n >= sizeof(words)) break;
            used += (size_t)n;
            count++;
        }
        bool ace = play.mode == PLAYER_PLAY_ACE;
        /* Smooth motion turned on, but off for this launch: the line says why (contract
           10.5), as the launch line carries smooth=off. */
        bool smooth_held = smooth_wanted && play.logic_60;
        /* Wider culling on with a culling package the player cannot use (contract 11.3.6):
           the launch keeps the title's other build, and the line says why by name. */
        const char *cull_problem = player_app_title_cull_problem(app, index);
        snprintf(player->ace_line, sizeof(player->ace_line), NK_UI_STR(ADAPTER_TITLE_ENHANCED_SUMMARY),
                 ace ? NK_UI_STR(ADAPTER_MODE_ACE) : NK_UI_STR(ADAPTER_MODE_ENHANCED),
                 ace && out->enhancement_on ? NK_UI_STR(ADAPTER_TITLE_LOGIC_60) : "",
                 ace && out->enhancement_on && count ? ", " : "",
                 count ? words : (ace && out->enhancement_on ? "" : NK_UI_STR(ADAPTER_TITLE_ALL_PARTS_OFF)),
                 smooth_held ? NK_UI_STR(ADAPTER_TITLE_SMOOTH_HELD) : "",
                 cull_problem[0] ? NK_UI_STR(ADAPTER_TITLE_CULL_UNSUPPORTED) : "",
                 cull_problem);
        out->ace_line = player->ace_line;
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (strcmp(player->ace_line_reported, out->ace_line) != 0) {
        snprintf(player->ace_line_reported, sizeof(player->ace_line_reported), "%s",
                 out->ace_line);
        printf("[PLAYER_UI_TEST] next_ui_title_play_line disc=%s line=%s\n", game->disc_id,
               out->ace_line);
    }
#endif
    {
        double now_s = player_ui_nk_now_s(player);
        if (strcmp(player->ace_last_disc, game->disc_id) != 0 || now_s - player->ace_last_read_s >= 2.0) {
            snprintf(player->ace_last_disc, sizeof(player->ace_last_disc), "%s", game->disc_id);
            player->ace_last_read_s = now_s;
            (void)player_app_ace_last_session(app, index, player->ace_last_session,
                                              sizeof(player->ace_last_session));
        }
        out->ace_last_session = player->ace_last_session;
    }
}

static PlayerUiPrimaryAction player_ui_fill_title(PlayerUiNk *player,
                                                   const PlayerApp *app,
                                                   int index,
                                                   PlayerUiTitle *out) {
    const GameRecord *game;
    NkRuntimePackageStatus package_status;
    const char *reason;
    bool package_ready;
    bool checking;
    bool check_failed;
    bool starting;
    bool running;
    const PlayerPreflightCheck *build_blocker;
    const char *compatibility_category;
    PlayerUiTier record_tier;
    bool unrecorded;
    PlayerUiPrimaryAction primary = PLAYER_UI_PRIMARY_NONE;
    if (player == NULL || app == NULL || out == NULL || index < 0 ||
        index >= app->game_count || index >= (int)PLAYER_UI_TITLE_CAP) {
        return primary;
    }
    game = &app->games[index];
    /* One compatibility lookup for the title: its words and tier, and whether it is
       an experimental entry nobody has recorded (player_ui_unrecorded_experimental). */
    compatibility_category = player_ui_compatibility_words(
        game, &out->compatibility_expect, &out->compatibility_known_issues, &record_tier);
    unrecorded = game->is_experimental && record_tier == PLAYER_UI_TIER_NONE;
    package_status = player_app_cached_runtime_package_status(app, game);
    package_ready = player_app_cached_game_has_runtime(app, game);
    checking = player_app_runtime_package_check_pending(app, game);
    check_failed = player_app_runtime_package_check_failed(app, game);
    reason = player_app_runtime_package_reason(app, game);
    /* This title is the game being started or the one that runs now. A game
       whose window is not up yet is still starting. */
    starting = game->disc_id[0] != '\0' &&
        ((app->launch_pending &&
          strcmp(app->launch_pending_disc_id, game->disc_id) == 0) ||
         (app->is_game_running && !app->child_window_ready &&
          strcmp(app->launch_session.disc_id, game->disc_id) == 0));
    running = !starting && game->disc_id[0] != '\0' && app->is_game_running &&
              strcmp(app->launch_session.disc_id, game->disc_id) == 0;
    player_library_status_text(package_ready, checking, check_failed,
                               game->assets_staged,
                               game->extracted_asset_count, reason,
                               player->status_text[index],
                               sizeof(player->status_text[index]));
    out->title = game->title_name;
    out->disc_id = game->disc_id;
    out->library_status = player_ui_library_status_word(game, package_ready, unrecorded);
    out->library_status_detail = package_ready || checking
        ? "" : player->status_text[index];
    /* The package card's text follows the package, not the primary action
       (#1297): a ready package stays ready while its game runs. */
    if (starting) {
        out->package_state = PLAYER_UI_PACKAGE_STARTING;
    } else if (running) {
        out->package_state = PLAYER_UI_PACKAGE_RUNNING;
    } else if (app->build_session.is_building &&
               strcmp(app->build_session.disc_id, game->disc_id) == 0 &&
               strcmp(app->build_session.enhancement_package_id,
                      game->enhancement_package_id) == 0) {
        /* A build of the other variant is not this package's build. */
        out->package_state = PLAYER_UI_PACKAGE_BUILDING;
    } else if (package_ready) {
        out->package_state = PLAYER_UI_PACKAGE_READY;
    } else if (check_failed && !checking) {
        /* A failed check leaves the status unknown: it is the failure, with
           its retry, not a check still running (#2071). */
        out->package_state = PLAYER_UI_PACKAGE_CHECK_FAILED;
    } else if (checking || package_status == NK_RUNTIME_PACKAGE_UNKNOWN) {
        out->package_state = PLAYER_UI_PACKAGE_CHECKING;
    } else {
        out->package_state = PLAYER_UI_PACKAGE_MISSING;
    }
    build_blocker = (package_ready || checking || check_failed || starting || running)
        ? NULL : player_app_package_build_preflight_blocker(app, index);
    out->build_preflight_message = build_blocker
        ? player_preflight_plain_message(build_blocker) : "";
    out->build_preflight_folder = build_blocker
        ? app->package_build_preflight.action_folder : "";
    out->compatibility_category = app->settings.developer_mode
        ? player_ui_compatibility_category_label(compatibility_category) : NULL;
    out->compatibility_tier = record_tier != PLAYER_UI_TIER_NONE
        ? record_tier : player_ui_tier_for_category(compatibility_category);
    /* The disc's executable is encrypted: the player decrypts it from the
       disc (#295). The title screen shows this only while the package still
       needs it. */
    out->input_note =
        game->executable_eboot_kind == NK_ISO_EXEC_PSP_ENCRYPTED &&
        game->executable_selection == NK_ISO_EXEC_SELECTION_NONE
            ? NK_UI_STR(ADAPTER_TITLE_DECRYPT_LOCALLY)
            : "";
    out->status_rgba = player_ui_library_status_color(package_ready, unrecorded);
    out->status_marker = player_ui_library_status_marker(game, package_ready, unrecorded);
    if (starting) {
        out->library_status = NK_UI_STR(ADAPTER_LIBRARY_STARTING);
        out->status_rgba = PLAYER_UI_INFO;
        out->status_marker = "◆";
    } else if (out->package_state == PLAYER_UI_PACKAGE_CHECKING && !unrecorded) {
        /* Launcher review B4: until the background check has looked at the
           package, the card does not know it needs setup. */
        out->library_status = NK_UI_STR(ADAPTER_LIBRARY_CHECKING);
        out->status_rgba = PLAYER_UI_INFO;
        out->status_marker = "◆";
    } else if (running) {
        out->library_status = NK_UI_STR(ADAPTER_LIBRARY_RUNNING);
        out->status_rgba = PLAYER_UI_READY;
        out->status_marker = "●";
    }
    /* An older Nakagawa built this package: it still
       plays, and the cards say what a rebuild would do, that it is queued or
       running, or that its disc is missing. */
    PlayerRebuildCard rebuild;
    bool older = !starting && !running && player_app_rebuild_card(app, game, &rebuild);
    out->rebuild_offered = false;
    if (older) {
        bool queued = !rebuild.rebuilding && rebuild.queue_place > 0;
        if (rebuild.rebuilding && rebuild.fraction >= 0.0f) {
            snprintf(player->rebuild_word[index], sizeof(player->rebuild_word[index]),
                     NK_UI_STR(ADAPTER_LIBRARY_ACTIVITY_PROGRESS), rebuild.word,
                     (int)(rebuild.fraction * 100.0f + 0.5f));
            out->library_status = player->rebuild_word[index];
        } else if (queued) {
            snprintf(player->rebuild_word[index], sizeof(player->rebuild_word[index]),
                     NK_UI_STR(ADAPTER_LIBRARY_ACTIVITY_PLACE), rebuild.word,
                     rebuild.queue_place, rebuild.queue_total);
            out->library_status = player->rebuild_word[index];
        } else {
            out->library_status = rebuild.word;
        }
        out->library_status_detail = rebuild.detail;
        if (queued) {
            snprintf(player->queue_detail[index], sizeof(player->queue_detail[index]),
                     NK_UI_STR(ADAPTER_LIBRARY_QUEUE_PLACE), rebuild.queue_place,
                     rebuild.queue_total, rebuild.detail);
            out->library_status_detail = player->queue_detail[index];
        }
        /* A first build that waits or runs is news, not a warning. */
        bool calm = rebuild.rebuilding || (queued && rebuild.kind == PLAYER_QUEUE_BUILD);
        out->status_rgba = calm ? PLAYER_UI_INFO : PLAYER_UI_WARNING;
        out->status_marker = calm ? "◆" : "▲";
        if (rebuild.rebuilding) out->package_state = PLAYER_UI_PACKAGE_BUILDING;
        out->rebuild_offered = rebuild.can_rebuild && package_ready && !app->launch_pending &&
                               !app->is_game_running && !app->build_session.is_building;
    }
    out->art_kind = player_ui_art_kind(game);
    /* The showcase packages beside the player are the demos under TEST THIS
       PC. The --demo fixtures are the project's test programs, not a disc the
       user added. */
    out->is_demo = game->is_showcase_package;
    /* A disc whose own ID is not a retail ID (a beta or a prototype) has a
       library key Nakagawa derived from its contents (nk_iso.h). */
    out->badge = out->is_demo || out->art_kind != NK_UI_ART_NONE ? NK_UI_STR(ADAPTER_BADGE_SHOWCASE)
               : game->is_sample ? NK_UI_STR(ADAPTER_BADGE_TEST_PROGRAM)
               : nk_disc_id_is_non_retail_key(game->disc_id) ? NK_UI_STR(ADAPTER_BADGE_UNOFFICIAL)
               : NK_UI_STR(ADAPTER_BADGE_YOUR_DISC);
    /* Disc verification: a checked image says how it compares with the user's
       disc list; the check's progress is the library's heading line, so the
       badge changes once. A beta's own badge says more than "Unknown dump", and
       the title page's sentence covers both. */
    if (!out->is_demo && !game->is_sample && out->art_kind == NK_UI_ART_NONE) {
        const char *tier = player_disc_tier_badge(game->disc_verification);
        if (tier != NULL && !(game->disc_verification == NK_DISC_TIER_UNKNOWN &&
                              nk_disc_id_is_non_retail_key(game->disc_id))) {
            out->badge = tier;
        }
    }
    out->disc_check_note = out->is_demo || game->is_sample
        ? NULL : player_disc_tier_note(game->disc_verification);
    /* Disc art is asked for by the screen that shows it
       (player_ui_title_art), not for every title in the library. */
    out->icon = NULL;
    out->picture = NULL;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (strcmp(player->status_reported[index], out->library_status) != 0) {
        snprintf(player->status_reported[index], sizeof(player->status_reported[index]), "%s",
                 out->library_status);
        printf("[PLAYER_UI_TEST] next_ui_library_status index=%d status=%s\n", index,
               out->library_status);
        printf("[PLAYER_UI_TEST] next_ui_library_badge index=%d disc=%s badge=%s\n", index,
               game->disc_id, out->badge);
    }
#endif
    out->save_folder_enabled = game->disc_id[0] != '\0';
    out->can_remove = player_app_removal_blocker(app, index) == NULL;
    /* Only the title page offers Add to Steam, so only it pays for the shared
       disc ID scan (player_ui_fill_title_page): the library fills every title
       each frame. */
    out->can_add_to_steam = false;
    {
        const PlayerTitleEnhancement *enhancement = &app->title_enhancements[index];
        bool saved = player_app_title_60hz_saved(app, game->disc_id);
        out->enhancement_shown = enhancement->available || saved;
        out->enhancement_on = player_app_title_60hz_on(app, index);
        out->enhancement_session = app->experimental_60hz_session &&
                                   enhancement->available && !saved;
        out->enhancement_released = player_app_title_60hz_released(app, index);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        /* The word scr_title.c tags the card with, from the value the screen reads. */
        const char *tag = !out->enhancement_shown ? "none"
                          : out->enhancement_released ? "Optional" : "Experimental";
        if (strcmp(player->enhancement_tag_reported[index], tag) != 0) {
            snprintf(player->enhancement_tag_reported[index],
                     sizeof(player->enhancement_tag_reported[index]), "%s", tag);
            printf("[PLAYER_UI_TEST] next_ui_enhancement_tag disc=%s tag=%s\n", game->disc_id,
                   tag);
        }
#endif
        out->enhancement_enabled = !app->launch_pending && !app->is_game_running &&
            !out->enhancement_session &&
            !(app->build_session.is_building &&
              strcmp(app->build_session.disc_id, game->disc_id) == 0);
        out->enhancement_problem = player_app_title_60hz_blocked(app, index)
            ? (enhancement->problem[0] ? enhancement->problem
                                       : NK_UI_STR(ADAPTER_TITLE_ENHANCEMENT_MISSING))
            : "";
    }
    player_ui_title_ace(player, app, index, out);
    out->primary_action_enabled = false;
    out->primary_action = PLAYER_UI_PRIMARY_NONE;
    if (app->launch_pending) {
        /* The launch worker hands back STOP GAME or an error card. */
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_STARTING);
    } else if (app->is_game_running) {
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_STOP);
        out->primary_action_enabled = true;
        primary = PLAYER_UI_PRIMARY_STOP_GAME;
    } else if (older && rebuild.rebuilding) {
        /* Its package is about to be replaced (or written for the first time,
           from the build queue); the package card says so. */
        out->primary_action_label = rebuild.kind == PLAYER_QUEUE_BUILD
            ? NK_UI_STR(ADAPTER_TITLE_ACTION_BUILDING) : NK_UI_STR(ADAPTER_TITLE_ACTION_REBUILDING);
    } else if (player_app_title_60hz_blocked(app, index)) {
        /* 60 FPS game logic is on but its package is gone: neither variant
           builds or plays until the user turns it off or brings it back. */
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_ACE_UNAVAILABLE);
    } else if (package_ready) {
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_PLAY);
        out->primary_action_enabled = true;
        primary = PLAYER_UI_PRIMARY_PLAY;
    } else if (check_failed && !checking) {
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_RETRY_PACKAGE_CHECK);
        out->primary_action_enabled = true;
        primary = PLAYER_UI_PRIMARY_RETRY_PACKAGE_CHECK;
    } else if (checking || package_status == NK_RUNTIME_PACKAGE_UNKNOWN) {
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_CHECKING_PACKAGE);
    } else if (package_status == NK_RUNTIME_PACKAGE_STALE) {
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_REBUILD_PACKAGE);
        if (!build_blocker) {
            out->primary_action_enabled = true;
            primary = PLAYER_UI_PRIMARY_BUILD;
        }
    } else if (package_status == NK_RUNTIME_PACKAGE_MISSING) {
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_BUILD_PACKAGE);
        if (!build_blocker) {
            out->primary_action_enabled = true;
            primary = PLAYER_UI_PRIMARY_BUILD;
        }
    } else if (package_status == NK_RUNTIME_PACKAGE_INCOMPATIBLE) {
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_REBUILD_PACKAGE);
        out->primary_action_enabled = true;
        primary = PLAYER_UI_PRIMARY_REBUILD;
        (void)player_incompatible_reason_line(reason, player->status_text[index],
                                              sizeof(player->status_text[index]));
    } else if (game->assets_staged) {
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_RUNTIME_REQUIRED);
    } else {
        out->primary_action_label = NK_UI_STR(ADAPTER_TITLE_ACTION_PREPARATION_UNAVAILABLE);
    }
    {
        /* The build queue: a game BUILD PACKAGE would build can wait its turn
           instead, and one that waits can leave. */
        int queue_total = 0;
        int queue_place = player_app_build_queue_place(app, game->disc_id, &queue_total);
        bool building_this = app->build_session.is_building &&
            strcmp(app->build_session.disc_id, game->disc_id) == 0;
        out->queue_add_offered = queue_place == 0 && primary == PLAYER_UI_PRIMARY_BUILD &&
                                 !building_this;
        out->queue_remove_offered = queue_place > 0 && !(older && rebuild.rebuilding);
    }
    out->primary_action = primary;
    return primary;
}

static void player_ui_fill_titles(PlayerUiNk *player, const PlayerApp *app,
                                  size_t count) {
    for (size_t i = 0u; i < count; ++i) {
        (void)player_ui_fill_title(player, app, (int)i, &player->titles[i]);
    }
}

/* Asks the art cache for one title's disc icon (and its picture, for the
   title page). Only titles on screen ask: the cache holds PLAYER_UI_ART_SLOTS
   discs, and a library that asked for every title on every frame filled it
   with the first ones, so later pages never showed their art. */
static void player_ui_note_shown(PlayerUiNk *player, size_t index) {
    if (player->shown_count < PLAYER_UI_SHOWN_TITLES_MAX && index <= (size_t)INT_MAX) {
        player->shown_titles[player->shown_count++] = (int)index;
    }
}

static void player_ui_title_art(PlayerUiNk *player, const PlayerApp *app, size_t index,
                                bool want_picture) {
    PlayerUiTitle *title = &player->titles[index];
    char cover_path[NK_MAX_PATH];
    const char *cover_arg = NULL;
    player_ui_note_shown(player, index);
    if (title->art_kind != NK_UI_ART_NONE) return;
    if (app->settings.download_cover_art &&
        app->cover_art_state[index] == PLAYER_COVER_CACHED &&
        player->cover_data_root[0] != '\0' &&
        nk_cover_art_cache_path(player->cover_data_root, app->games[index].content_sha1,
                                cover_path, sizeof(cover_path))) {
        cover_arg = cover_path;
    }
    player_ui_art_get_title(player->art, app->games[index].iso_path, cover_arg, want_picture,
                            &title->icon, want_picture ? &title->picture : NULL,
                            &title->cover);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (title->icon != NULL && !player->icon_reported[index]) {
        player->icon_reported[index] = true;
        printf("[PLAYER_UI_TEST] next_ui_disc_icon index=%zu width=%u height=%u\n", index,
               (unsigned)title->icon->width, (unsigned)title->icon->height);
    }
    if (title->cover != NULL) {
        if (!player->cover_reported[index]) {
            player->cover_reported[index] = true;
            printf("[PLAYER_UI_TEST] next_ui_cover_art index=%zu width=%u height=%u\n", index,
                   (unsigned)title->cover->width, (unsigned)title->cover->height);
        }
    } else {
        player->cover_reported[index] = false;
    }
#endif
}

/* Sorts the library grid: the user's games (not the demos) in the order the
   settings ask for. */
static const PlayerApp *g_sort_app;

/* "YYYY-MM-DD HH:MM" when the game was played, as player_state stamps it, or
   NULL for "Never" and older records. */
static const char *player_ui_played_at(const GameRecord *game) {
    return game->last_played[0] >= '0' && game->last_played[0] <= '9' ? game->last_played : NULL;
}

/* By name, ignoring ASCII case, then by disc id, so the order is stable. */
static int player_ui_compare_name(const void *a, const void *b) {
    const GameRecord *x = &g_sort_app->games[*(const size_t *)a];
    const GameRecord *y = &g_sort_app->games[*(const size_t *)b];
    const unsigned char *p = (const unsigned char *)x->title_name;
    const unsigned char *q = (const unsigned char *)y->title_name;
    while (*p != '\0' && tolower(*p) == tolower(*q)) {
        ++p;
        ++q;
    }
    if (tolower(*p) != tolower(*q)) return tolower(*p) < tolower(*q) ? -1 : 1;
    return strcmp(x->disc_id, y->disc_id);
}

/* The most recently played first (the stamp sorts as text); games never
   played after them, in the order they were added, as the library was shown
   before it had a sort. */
static int player_ui_compare_played(const void *a, const void *b) {
    size_t i = *(const size_t *)a;
    size_t j = *(const size_t *)b;
    const char *x = player_ui_played_at(&g_sort_app->games[i]);
    const char *y = player_ui_played_at(&g_sort_app->games[j]);
    if (x != NULL && y != NULL && strcmp(x, y) != 0) return strcmp(y, x);
    if ((x == NULL) != (y == NULL)) return x == NULL ? 1 : -1;
    return i < j ? -1 : i > j ? 1 : 0;
}

/* The library keeps games in the order they were added: the last added first. */
static int player_ui_compare_added(const void *a, const void *b) {
    size_t x = *(const size_t *)a;
    size_t y = *(const size_t *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Whether `text` holds `query`, ignoring ASCII case. */
static bool player_ui_contains(const char *text, const char *query) {
    size_t length = strlen(query);
    for (const char *start = text; *start != '\0'; ++start) {
        size_t i = 0u;
        while (i < length && start[i] != '\0' &&
               tolower((unsigned char)start[i]) == tolower((unsigned char)query[i])) {
            ++i;
        }
        if (i == length) return true;
    }
    return length == 0u;
}

static void player_ui_library_order(PlayerUiNk *player, const PlayerApp *app, size_t count) {
    size_t n = 0u;
    size_t total = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if (player->titles[i].is_demo) continue;
        ++total;
        /* The find box keeps the games whose name or disc ID holds what is typed. */
        if (player->find_open &&
            !player_ui_contains(app->games[i].title_name, player->find_query) &&
            !player_ui_contains(app->games[i].disc_id, player->find_query)) {
            continue;
        }
        player->library_grid[n++] = i;
    }
    player->library_grid_count = n;
    player->library_game_total = total;
    g_sort_app = app;
    qsort(player->library_grid, n, sizeof(player->library_grid[0]),
          app->settings.library_sort == PLAYER_LIBRARY_SORT_NAME ? player_ui_compare_name
          : app->settings.library_sort == PLAYER_LIBRARY_SORT_ADDED ? player_ui_compare_added
          : player_ui_compare_played);
    g_sort_app = NULL;
}

/* The library's titles and grid, for the grid helpers. */
static PlayerUiLibraryModel player_ui_library_view(const PlayerUiNk *player, size_t count) {
    PlayerUiLibraryModel view;
    memset(&view, 0, sizeof(view));
    view.titles = player->titles;
    view.title_count = count;
    view.grid = player->library_grid;
    view.grid_count = player->library_grid_count;
    view.host = nk_platform_host_kind();
    return view;
}

/* The disc art of the games on the library page shown: grid positions
   [page * page_size, (page + 1) * page_size), as the library screen lays
   them out. Every page shows the demos. */
static void player_ui_library_page_art(PlayerUiNk *player, const PlayerApp *app,
                                       size_t count, size_t page, size_t page_size) {
    size_t first;
    if (page_size == 0u || page > SIZE_MAX / page_size) return;
    first = page * page_size;
    for (size_t i = 0u; i < count; ++i) {
        if (player->titles[i].is_demo) player_ui_note_shown(player, i);
    }
    for (size_t position = first;
         position < player->library_grid_count && position - first < page_size; ++position) {
        player_ui_title_art(player, app, player->library_grid[position], false);
    }
}


/* The art rest (docs/PLAYER_UI_DESIGN.md, Motion): focus resting on a game's card
   for NK_UI_MOTION_ART_REST asks for that disc's picture, and once it is read it
   fades in behind the library. Moving on fades it out, so browsing past cards reads
   no pictures and changes nothing behind them. Not for the demos, whose art is the
   project's own drawing, not while finding, and not under Reduce motion. */
static size_t player_ui_focused_title_index(NkUiId focused, size_t count);
static void player_ui_library_backdrop(PlayerUiNk *player, const PlayerApp *app, size_t count,
                                       double now_s, PlayerUiLibraryModel *model) {
    size_t focused = player_ui_focused_title_index(nk_ui_focused(player->ui), count);
    bool shown = false;
    if (player_app_reduce_motion(app) || model->find_open ||
        (focused < count && player->titles[focused].art_kind != NK_UI_ART_NONE)) {
        focused = SIZE_MAX;
    }
    if (focused >= count) focused = SIZE_MAX;
    if (focused != player->rest_index) {
        player->rest_index = focused;
        player->rest_since_s = now_s;
    }
    if (focused != SIZE_MAX && now_s - player->rest_since_s >= NK_UI_MOTION_ART_REST) {
        player_ui_title_art(player, app, focused, true);
        if (player->titles[focused].picture != NULL) {
            player->backdrop_index = focused;
            shown = true;
        }
    }
    if (player->backdrop_index >= count) {
        player->backdrop_index = SIZE_MAX;
        return;
    }
    if (shown) {
        player->backdrop_hidden_s = now_s;
    } else if (now_s - player->backdrop_hidden_s > NK_UI_MOTION_SLOW + 0.1) {
        player->backdrop_index = SIZE_MAX;   /* faded out */
        return;
    }
    /* Asked for every frame it is drawn, so the cache keeps it through the fade. */
    player_ui_title_art(player, app, player->backdrop_index, true);
    model->backdrop = player->titles[player->backdrop_index].picture;
    model->backdrop_shown = shown;
    if (model->backdrop == NULL) player->backdrop_index = SIZE_MAX;
}

static size_t player_ui_focused_title_index(NkUiId focused, size_t count) {
    uint32_t value = focused;
    if (value < PLAYER_UI_ID_LIBRARY_ITEM_BASE) return count;
    value -= PLAYER_UI_ID_LIBRARY_ITEM_BASE;
    return value < count ? (size_t)value : count;
}

/* Shows library page `page`. A card with focus hands it to the card in the
   same place on the new page (the last one when the page is shorter), since
   the page shown follows the focused card. The grid's last slot is the add
   card only outside find mode; while finding, every slot is a match. */
static void player_ui_library_turn_page(PlayerUiNk *player, size_t count, size_t page_size,
                                        size_t slots, size_t page) {
    NkUiId focused = nk_ui_focused(player->ui);
    PlayerUiLibraryModel view = player_ui_library_view(player, count);
    size_t index = player_ui_focused_title_index(focused, count);
    size_t slot = SIZE_MAX;
    if (index < count) {
        size_t position = player_ui_library_grid_position(&view, index);
        if (position < count) slot = position % page_size;
    } else if (focused == PLAYER_UI_ID_LIBRARY_ADD) {
        slot = (slots - 1u) % page_size;
    }
    player->library_page = page;
    if (slot != SIZE_MAX) {
        size_t position = page * page_size + slot;
        if (position >= slots) position = slots - 1u;
        index = !player->find_open && position + 1u == slots
            ? count : player_ui_library_grid_title(&view, position);
        nk_ui_focus(player->ui, index < count ? PLAYER_UI_ID_LIBRARY_ITEM(index)
                                              : PLAYER_UI_ID_LIBRARY_ADD);
    }
}

#ifdef NK_PLAYER_UI_REGRESSION_TEST
/* Settings > Enhanced's detail panel as drawn, printed on change: the focused control, how
   many text runs are shown on the panel, how many of those run past its box (across, or down
   where its scrolling column leaves them showing), and their text in drawing order, each run
   ending in '^'. */
static void player_ui_report_enhanced_panel(PlayerUiNk *player, const NkUiDrawList *draw) {
    const NkUiDrawCmd *panel = NULL;
    char line[sizeof(player->enhanced_panel_reported)];
    char text[768];
    size_t used = 0u;
    int runs = 0;
    int outside = 0;
    text[0] = '\0';
    for (uint32_t i = 0u; i < draw->ncmd && panel == NULL; ++i) {
        if (draw->cmd[i].kind == NK_UI_DRAW_RECTANGLE &&
            draw->cmd[i].element_id == PLAYER_UI_ID_ENHANCED_DETAIL) {
            panel = &draw->cmd[i];
        }
    }
    if (panel == NULL) {
        player->enhanced_panel_reported[0] = '\0';
        return;
    }
    for (uint32_t i = 0u; i < draw->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &draw->cmd[i];
        const NkUiTextRun *run = cmd->kind == NK_UI_DRAW_TEXT
                                     ? (const NkUiTextRun *)cmd->custom : NULL;
        int top = cmd->bounds[1] > cmd->clip[1] ? cmd->bounds[1] : cmd->clip[1];
        int bottom = cmd->bounds[3] < cmd->clip[3] ? cmd->bounds[3] : cmd->clip[3];
        if (run == NULL || run->text == NULL || bottom <= top ||
            cmd->bounds[0] < panel->bounds[0] || cmd->bounds[0] >= panel->bounds[2] ||
            bottom <= panel->bounds[1] || top >= panel->bounds[3]) {
            continue;
        }
        ++runs;
        if (cmd->bounds[2] > panel->bounds[2] || bottom > panel->bounds[3]) ++outside;
        for (size_t c = 0u; c <= run->length && used + 1u < sizeof(text); ++c) {
            char ch = c < run->length ? run->text[c] : '^';
            if (ch != '|' && ch != '\n') text[used++] = ch;
        }
        text[used] = '\0';
    }
    (void)snprintf(line, sizeof(line), "focus=%u|runs=%d|outside=%d|text=%s",
                   (unsigned)nk_ui_focused(player->ui), runs, outside, text);
    if (strcmp(line, player->enhanced_panel_reported) != 0) {
        (void)snprintf(player->enhanced_panel_reported, sizeof(player->enhanced_panel_reported),
                       "%s", line);
        printf("[PLAYER_UI_TEST] next_ui_enhanced_panel %s\n", line);
    }
}
#endif

/* ---- The hidden moment, Love All and the launcher theme (launcher_secret.h) ------------- */

/* The unlocks kept and today's local date, for the themes' rules. */
static LauncherThemeContext player_ui_theme_context(const PlayerApp *app) {
    LauncherThemeContext context;
    time_t now = time(NULL);
    const struct tm *local = localtime(&now);
    memset(&context, 0, sizeof(context));
    context.unlocks = (const char (*)[LAUNCHER_THEME_ID_MAX])app->settings.theme_unlocks;
    context.unlock_count = app->settings.theme_unlock_count > 0
        ? (size_t)app->settings.theme_unlock_count : 0u;
    if (local != NULL) {
        context.month = local->tm_mon + 1;
        context.day = local->tm_mday;
    }
    return context;
}

const char *player_ui_launcher_theme_name(const PlayerApp *app) {
    LauncherThemeContext context;
    if (app == NULL) return NULL;
    context = player_ui_theme_context(app);
    if (launcher_theme_available_count(&context) < 2u) return NULL;
    return launcher_theme_pick(app->settings.launcher_theme, &context)->name;
}

void player_ui_cycle_launcher_theme(PlayerApp *app) {
    LauncherThemeContext context;
    if (app == NULL) return;
    context = player_ui_theme_context(app);
    (void)player_app_set_launcher_theme(app,
                                        launcher_theme_next(app->settings.launcher_theme,
                                                            &context)->id);
}

/* The frames are drawn with the theme the settings pick, from the next frame on. */
static void player_ui_follow_theme(PlayerUiNk *player, const PlayerApp *app) {
    LauncherThemeContext context = player_ui_theme_context(app);
    const LauncherTheme *theme = launcher_theme_pick(app->settings.launcher_theme, &context);
    if (theme != player->theme_drawn) {
        NkUiTheme colors;
        launcher_theme_colors(theme, &colors);
        nk_ui_set_theme(player->ui, &colors);
        player->theme_drawn = theme;
    }
}

/* The greeting for this PC's local time. The UI tests give the time in NK_UI_TEST_LOCAL_TIME
   ("HH:MM"), so a picture says the same words whenever it is taken. */
static const char *player_ui_secret_greeting(void) {
    int hour = 12;
    int minute = 0;
    time_t now;
    const struct tm *local;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    const char *fake = getenv("NK_UI_TEST_LOCAL_TIME");
    if (fake != NULL && sscanf(fake, "%d:%d", &hour, &minute) == 2) {
        return launcher_secret_greeting(hour, minute);
    }
#endif
    now = time(NULL);
    local = localtime(&now);
    if (local != NULL) {
        hour = local->tm_hour;
        minute = local->tm_min;
    }
    return launcher_secret_greeting(hour, minute);
}

static void player_ui_secret_jingle(PlayerUiNk *player) {
    if (player->secret_jingle == NULL) {
        size_t samples = launcher_jingle_pcm(NULL, 0u, LAUNCHER_JINGLE_RATE);
        player->secret_jingle = samples > 0u ? (int16_t *)malloc(samples * sizeof(int16_t)) : NULL;
        if (player->secret_jingle == NULL) return;
        player->secret_jingle_samples =
            launcher_jingle_pcm(player->secret_jingle, samples, LAUNCHER_JINGLE_RATE);
    }
    player_ui_sounds_play_pcm(player->sounds, player->secret_jingle,
                              player->secret_jingle_samples, LAUNCHER_JINGLE_RATE);
}

static void player_ui_love_all_open(PlayerUiNk *player, bool reduce_motion, double now_s) {
    nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
    if (!nk_ui_router_push(player->router, PLAYER_UI_ROUTE_LOVE_ALL)) return;
    player->love_all_rolling = !reduce_motion;
    player->love_all_since_s = now_s;
    player->love_all_scroll = 0.0f;
    player->love_all_scroll_set = true;
    player->secret_last_route = PLAYER_UI_ROUTE_LOVE_ALL;
}

/* Before the frame: start what the code or leaving Love All asks for, and hand the code's
   moment over to Love All once the ripple is done. */
static void player_ui_secret_begin(PlayerUiNk *player, PlayerApp *app, double now_s) {
    PlayerUiRoute route = nk_ui_router_current(player->router);
    bool reduce_motion = player_app_reduce_motion(app);
    LauncherMomentFrame frame;
    player->secret_allowed = !app->is_game_running && !app->launch_pending;
    if (!player->secret_allowed) {
        launcher_secret_reset(&player->secret_detector);
        player->secret_found = false;
    }
    if (player->secret_found) {
        player->secret_found = false;
        launcher_moment_start(&player->secret_moment, LAUNCHER_MOMENT_FOUND, now_s, reduce_motion,
                              player_ui_sounds_enabled(player->sounds), player_ui_secret_greeting());
        (void)player_app_record_theme_unlock(app, LAUNCHER_THEME_UNLOCK_WAVE);
        if (player->secret_moment.sound) player_ui_secret_jingle(player);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        printf("[PLAYER_UI_TEST] launcher_secret found motion=%d sound=%d toast=%s\n",
               player->secret_moment.motion ? 1 : 0, player->secret_moment.sound ? 1 : 0,
               player->secret_moment.toast);
#endif
    } else if (player->secret_last_route == PLAYER_UI_ROUTE_LOVE_ALL &&
               route != PLAYER_UI_ROUTE_LOVE_ALL) {
        launcher_moment_start(&player->secret_moment, LAUNCHER_MOMENT_FAREWELL, now_s,
                              reduce_motion, false, LAUNCHER_SECRET_FAREWELL);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        printf("[PLAYER_UI_TEST] launcher_secret left_love_all toast=%s\n",
               LAUNCHER_SECRET_FAREWELL);
#endif
    }
    frame = launcher_moment_frame(&player->secret_moment, player->secret_prev_s, now_s);
    /* Love All opens over the routes; a Settings page or the nickname editor drawn over
       them keeps the moment and leaves the page for About. */
    if (frame.open_love_all && route != PLAYER_UI_ROUTE_LOVE_ALL && !player->nickname_open &&
        !player_ui_nk_flow_view(app->active_view)) {
        player_ui_love_all_open(player, reduce_motion, now_s);
        route = nk_ui_router_current(player->router);
    }
    if (route == PLAYER_UI_ROUTE_LOVE_ALL && player->love_all_rolling) {
        /* A slow roll after a moment's rest, until its end or the user scrolls. */
        NkUiScrollState state;
        double t = now_s - player->love_all_since_s - 1.2;
        player->love_all_scroll = t > 0.0 ? (float)(t * 28.0) : 0.0f;
        player->love_all_scroll_set = true;
        if (nk_ui_scroll_state(player->ui, player_ui_love_all_list_id(), &state) && t > 0.0 &&
            state.max_offset > 0.0f && player->love_all_scroll >= state.max_offset) {
            player->love_all_rolling = false;
        }
    }
    if (!frame.active) player->secret_moment.kind = LAUNCHER_MOMENT_NONE;
    player->secret_last_route = route;
    player->secret_prev_s = now_s;
    player->secret_animating = frame.active ||
                               (route == PLAYER_UI_ROUTE_LOVE_ALL && player->love_all_rolling);
}

static PlayerUiAction player_ui_render_love_all(PlayerUiNk *player, NkUiScale scale) {
    PlayerUiLoveAllModel model;
    memset(&model, 0, sizeof(model));
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    model.scroll_set = player->love_all_scroll_set;
    model.scroll_offset = player->love_all_scroll;
    player->love_all_scroll_set = false;
    return player_ui_screen_love_all(player->ui, &model);
}

/* After the screen: the moment's frame over it. */
static void player_ui_secret_draw(PlayerUiNk *player, NkUiScale scale, double now_s) {
    LauncherMomentFrame frame = launcher_moment_frame(&player->secret_moment, now_s, now_s);
    launcher_moment_draw(player->ui, &player->secret_shapes, &frame, player->secret_moment.toast,
                         scale.logical_w, scale.logical_h);
}

bool player_ui_nk_render(PlayerUiNk *player, PlayerApp *app,
                         SDL_Renderer *renderer, SDL_Window *window,
                         PlayerUiAction *out_action) {
    int pixel_w = 0;
    int pixel_h = 0;
    int window_w = 0;
    int window_h = 0;
    float display_scale;
    float point_to_pixel;
    double now_s;
    NkUiScale scale;
    NkUiEvent repeats[8];
    size_t repeat_count;
    const NkUiDrawList *draw_list;
    PlayerUiAction action = {PLAYER_UI_ACTION_NONE, PLAYER_UI_PRIMARY_NONE, 0u, -1};
    PlayerUiAction rebuild_answer = {PLAYER_UI_ACTION_NONE, PLAYER_UI_PRIMARY_NONE, 0u, -1};
    PlayerUiRoute route;
    if (out_action != NULL) {
        out_action->kind = PLAYER_UI_ACTION_NONE;
        out_action->primary = PLAYER_UI_PRIMARY_NONE;
        out_action->title_index = 0u;
    }
    if (player == NULL || !player->ready || app == NULL || renderer == NULL ||
        window == NULL) {
        return false;
    }
    player->build_built = -1;
    if (!SDL_GetRenderOutputSize(renderer, &pixel_w, &pixel_h) ||
        pixel_w <= 0 || pixel_h <= 0) {
        return false;
    }
    (void)SDL_GetWindowSize(window, &window_w, &window_h);
    if ((player_ui_nk_find_open(player) || player->nickname_open) != player->find_text_input) {
        /* SDL's text input is on while the find box or the nickname editor is
           open, so typing (and an input method's text) reaches it; off otherwise. */
        player->find_text_input = player_ui_nk_find_open(player) || player->nickname_open;
        if (player->find_text_input) {
            (void)SDL_StartTextInput(window);
        } else {
            (void)SDL_StopTextInput(window);
        }
    }
    display_scale = SDL_GetWindowDisplayScale(window);
    if (!(display_scale > 0.0f)) display_scale = 1.0f;
    player->viewport.pixel_w = pixel_w;
    player->viewport.pixel_h = pixel_h;
    player->viewport.display_scale = display_scale;
    {
        /* Prompts follow the last input device ("Match input"), and an Automatic
           accessibility setting follows the system's, read every frame so a
           change there applies at once. */
        PlayerUiDisplay display;
        player_app_read_system_preferences(app);
        memset(&display, 0, sizeof(display));
        display.reduce_motion = player_app_reduce_motion(app);
        display.high_contrast_focus = player_app_high_contrast_focus(app);
        display.interface_size_pct = app->settings.interface_size_pct;
        display.text_size_pct = app->settings.text_size_pct;
        display.touch = nk_ui_input_touch_active(&player->input.input);
        player_ui_apply_display(player->ui, &player->viewport, &display,
                                nk_ui_input_prompt_style(&player->input.input));
    }
    /* The launcher's screens are laid out for at least its design size
       (1280 x 720); a smaller window, down to the 960 x 540 minimum, draws the
       same layout smaller instead of pushing buttons off the window. */
    player->viewport.min_logical_w = (float)PLAYER_UI_DESIGN_WIDTH;
    player->viewport.min_logical_h = (float)PLAYER_UI_DESIGN_HEIGHT;
    scale = nk_ui_scale_for_viewport(&player->viewport);
    point_to_pixel = window_w > 0 ? (float)pixel_w / (float)window_w
                                  : display_scale;
    nk_ui_sdl_input_set_event_scale(&player->input,
                                    point_to_pixel / scale.density);
    nk_ui_set_viewport(player->ui, &player->viewport);
    nk_ui_set_layout_debug(player->ui,
                           app->settings.developer_mode && app->dev_log.inspect_layout);
    nk_ui_sdlr_set_scale(player->backend, scale.density);
    now_s = player_ui_nk_now_s(player);
    repeat_count = nk_ui_input_poll(&player->input.input, now_s, repeats,
                                    sizeof(repeats) / sizeof(repeats[0]));
    for (size_t i = 0u; i < repeat_count; ++i) {
        nk_ui_push_event(player->ui, &repeats[i]);
    }
    player_ui_art_pump(player->art);
    player->snd0_index = SIZE_MAX;   /* set again below while a game's page shows */
    player_ui_nk_follow_sounds(player, app);
    player_ui_follow_theme(player, app);
    player_ui_secret_begin(player, app, now_s);
    {
        /* The owner's first run: one click on COPY FILES AND CONTINUE swapped
           in the copy screen, whose CANCEL sits where the button was, and the
           rest of the click cancelled the copy. A click that began on another
           screen activates nothing on this one. */
        unsigned screen = player_ui_flow_screen(app) * 16u +
                          (unsigned)nk_ui_router_current(player->router);
        if (screen != player->press_screen) {
            nk_ui_cancel_press(player->ui);
            player->press_screen = screen;
        }
    }
    nk_ui_begin(player->ui, now_s);
    player->shown_count = 0u;
    {
        NkUiId restore = nk_ui_router_take_focus_restore(player->router);
        if (restore != 0u) nk_ui_focus(player->ui, restore);
    }
    route = nk_ui_router_current(player->router);
    player_ui_flow_focus(player, app);
    if (!player_ui_nk_flow_view(app->active_view)) {
        /* Games an older Nakagawa built: player_state says when the prompt or
           a launch's reminder is up, and the route follows it, over the
           library or the title page. Back pops the route; that answers with
           the second choice (Later, Play anyway). */
        bool rebuild_modal = player_app_rebuild_prompt_visible(app) ||
                             player_app_rebuild_reminder_visible(app);
        if (player->rebuild_pushed && route != PLAYER_UI_ROUTE_REBUILD) {
            player->rebuild_pushed = false;
            if (rebuild_modal) {
                rebuild_answer.kind = PLAYER_UI_ACTION_REBUILD;
                rebuild_answer.primary = (PlayerUiPrimaryAction)(
                    player_app_rebuild_reminder_visible(app) ? PLAYER_UI_REBUILD_PLAY_ANYWAY
                                                             : PLAYER_UI_REBUILD_LATER);
            }
        } else if (rebuild_modal && (route == PLAYER_UI_ROUTE_LIBRARY ||
                                     route == PLAYER_UI_ROUTE_TITLE)) {
            nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
            if (nk_ui_router_push(player->router, PLAYER_UI_ROUTE_REBUILD)) {
                player->rebuild_pushed = true;
                player->rebuild_focus_pending = true;
                route = PLAYER_UI_ROUTE_REBUILD;
            }
        } else if (!rebuild_modal && route == PLAYER_UI_ROUTE_REBUILD) {
            (void)nk_ui_router_pop(player->router);
            player->rebuild_pushed = false;
            route = nk_ui_router_current(player->router);
        }
    }
    /* RetroAchievements' sign-in form (#1316) keeps what was typed only while its page or one of
       its fields is open: any other page wipes it. */
    if (app->active_view != VIEW_ACHIEVEMENTS_SIGN_IN && !player->nickname_is_ra_user &&
        !player->nickname_is_ra_password &&
        (player->ra_form.username[0] != '\0' || player->ra_form.password[0] != '\0')) {
        player_ui_sign_in_wipe(&player->ra_form);
    }
    /* The nickname editor (#2197) draws over every screen while it is open: the
       first-launch prompt until it is answered, or Settings > Nickname. */
    if (!player->nickname_open && player_app_nickname_prompt_visible(app)) {
        player->nickname_return_focus = nk_ui_focused(player->ui);
        player_ui_nickname_open(player, true, false);
    } else if (!player->nickname_open && player->nickname_return_focus != 0u) {
        nk_ui_focus(player->ui, player->nickname_return_focus);
        player->nickname_return_focus = 0u;
    }
    if (player->nickname_open) {
        PlayerUiNicknameModel model;
        memset(&model, 0, sizeof(model));
        if (player->nickname_focus != 0u) {
            nk_ui_focus(player->ui, player->nickname_focus);
            player->nickname_focus = 0u;
        }
        model.first_launch = player->nickname_first_launch;
        model.server_mode = player->nickname_is_server;
        model.port_mode = player->nickname_is_port;
        model.ra_username_mode = player->nickname_is_ra_user;
        model.ra_password_mode = player->nickname_is_ra_password;
        model.keyboard = &player->nickname_keyboard;
        model.current = player->nickname_is_ra_user || player->nickname_is_ra_password ? NULL
                        : player->nickname_is_port ? player->settings_online_play_port
                        : player->nickname_is_server ? app->settings.online_play_server
                                                     : app->settings.nickname;
        model.refused = player->nickname_refused;
        if (model.refused && model.server_mode) {
            /* The Done refused said why: the text is as it was then (an edit clears refused). */
            char typed[NK_ADHOC_SERVER_MAX_LEN + 1u];
            model.refused_reason = nk_adhoc_relay_server_problem(
                player_ui_nickname_trimmed(player->nickname_text, typed, sizeof(typed)));
        }
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        action = player_ui_screen_nickname(player->ui, &model);
        if (action.kind == PLAYER_UI_ACTION_NICKNAME &&
            (unsigned)action.primary == PLAYER_UI_NICKNAME_EDITED) {
            player->nickname_refused = false;
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_NICKNAME &&
                   (player->nickname_is_ra_user || player->nickname_is_ra_password)) {
            const bool save = (unsigned)action.primary == PLAYER_UI_NICKNAME_SAVE;
            /* RetroAchievements' sign-in form (#1316): Done keeps what is typed, untrimmed
               (a password may begin or end with a space); Cancel keeps the form as it was.
               Focus goes back to the field, or to SIGN IN once both are filled. */
            const bool password = player->nickname_is_ra_password;
            if (save) {
                char *into = password ? player->ra_form.password : player->ra_form.username;
                const size_t cap = password ? sizeof(player->ra_form.password)
                                            : sizeof(player->ra_form.username);
                (void)snprintf(into, cap, "%s", player->nickname_text);
            }
            player->nickname_return_focus =
                save && player->ra_form.username[0] && player->ra_form.password[0]
                    ? PLAYER_UI_ID_ACHIEVEMENTS_SIGN_IN
                    : password ? PLAYER_UI_ID_ACHIEVEMENTS_PASSWORD
                               : PLAYER_UI_ID_ACHIEVEMENTS_USERNAME;
            player_ui_nickname_close(player);
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_NICKNAME) {
            /* Skip keeps the value; a Save the rule refuses keeps the editor open, with the old
               value, and says why. */
            char trimmed[NK_ADHOC_SERVER_MAX_LEN + 1u];
            bool save = (unsigned)action.primary == PLAYER_UI_NICKNAME_SAVE;
            const char *chosen = player_ui_nickname_trimmed(player->nickname_text, trimmed,
                                                            sizeof(trimmed));
            /* Other server opens the consent card for it, as any server picked does. */
            bool done = player->nickname_is_port
                ? !save || player_app_set_online_play_port_offset(app, chosen)
                : player->nickname_is_server
                ? !save || player_app_open_online_play_card(app, chosen)
                : player->nickname_first_launch
                    ? player_app_nickname_prompt_answer(app, save ? chosen : NULL)
                    : !save || player_app_set_nickname(app, chosen);
            if (done && save && player->nickname_is_server) {
                player->nickname_return_focus = PLAYER_UI_ID_ONLINE_PLAY_NOT_NOW;
            }
            if (done) {
                player_ui_nickname_close(player);
            } else {
                player->nickname_refused = true;
            }
            action.kind = PLAYER_UI_ACTION_NONE;
        }
    } else if (player_ui_nk_flow_view(app->active_view)) {
        action = player_ui_render_flow(player, app, scale, now_s);
        if (action.kind == PLAYER_UI_ACTION_SETTINGS &&
            (unsigned)action.primary == PLAYER_UI_SETTINGS_EDIT_NICKNAME) {
            player->nickname_return_focus = PLAYER_UI_ID_SETTINGS_NICKNAME;
            player_ui_nickname_open(player, false, false);
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_SETTINGS &&
                   (unsigned)action.primary == PLAYER_UI_SETTINGS_EDIT_ONLINE_PLAY_SERVER) {
            player->nickname_return_focus = PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_SERVER;
            player_ui_nickname_open(player, false, true);
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_SETTINGS &&
                   (unsigned)action.primary == PLAYER_UI_SETTINGS_EDIT_ONLINE_PLAY_PORT) {
            player->nickname_return_focus = PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_PORT;
            player_ui_port_open(player);
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_SETTINGS &&
                   ((unsigned)action.primary == PLAYER_UI_SETTINGS_EDIT_ACHIEVEMENTS_USERNAME ||
                    (unsigned)action.primary == PLAYER_UI_SETTINGS_EDIT_ACHIEVEMENTS_PASSWORD)) {
            const bool password =
                (unsigned)action.primary == PLAYER_UI_SETTINGS_EDIT_ACHIEVEMENTS_PASSWORD;
            player->nickname_return_focus = password ? PLAYER_UI_ID_ACHIEVEMENTS_PASSWORD
                                                     : PLAYER_UI_ID_ACHIEVEMENTS_USERNAME;
            player_ui_ra_field_open(player, password);
            action.kind = PLAYER_UI_ACTION_NONE;
        }
        if ((player_ui_settings_view(app->active_view) ||
             app->active_view == VIEW_CONTROLLER_SETTINGS) &&
            (action.kind == PLAYER_UI_ACTION_OPEN_LIBRARY ||
             action.kind == PLAYER_UI_ACTION_OPEN_ABOUT ||
             action.kind == PLAYER_UI_ACTION_OPEN_SETTINGS)) {
            /* A top-bar section closes Settings and opens that screen; the
               SETTINGS tab and Start close it, as Done does. */
            if (action.kind == PLAYER_UI_ACTION_OPEN_ABOUT) {
                if (route != PLAYER_UI_ROUTE_ABOUT) {
                    nk_ui_router_set_focus(player->router, PLAYER_UI_ID_TAB_ABOUT);
                    (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_ABOUT);
                }
                player->pre_flow_focus = PLAYER_UI_ID_TAB_ABOUT;
            } else if (action.kind == PLAYER_UI_ACTION_OPEN_LIBRARY) {
                (void)nk_ui_router_replace(player->router, PLAYER_UI_ROUTE_LIBRARY);
                player->pre_flow_focus = PLAYER_UI_ID_TAB_LIBRARY;
            }
            if (app->active_view == VIEW_CONTROLLER_SETTINGS) {
                action.kind = PLAYER_UI_ACTION_CONTROLLER;
                action.primary = (PlayerUiPrimaryAction)PLAYER_UI_CONTROLLER_CLOSE;
            } else {
                action.kind = PLAYER_UI_ACTION_SETTINGS;
                action.primary = (PlayerUiPrimaryAction)PLAYER_UI_SETTINGS_DONE;
            }
        }
        if (action.kind != PLAYER_UI_ACTION_NONE && out_action != NULL) {
            *out_action = action;
        }
    } else if (route == PLAYER_UI_ROUTE_LIBRARY) {
        size_t count = app->game_count > 0 ? (size_t)app->game_count : 0u;
        size_t columns = 1u;
        size_t page_size;
        size_t demo_count;
        size_t games;
        size_t slots;
        PlayerUiLibraryModel view;
        if (count > PLAYER_UI_TITLE_CAP) count = PLAYER_UI_TITLE_CAP;
        player_ui_fill_titles(player, app, count);
        if (!player->find_open && nk_ui_take_event(player->ui, NK_UI_EV_OPTIONS, NULL) &&
            player_ui_library_demo_count(player->titles, count) < count) {
            /* The library's Options action (the pad's west button, Space) is Find. */
            player_ui_find_open(player, false);
        }
        if (player->find_focus != 0u) {
            /* This frame lays the box out (or FIND, once it closed). */
            nk_ui_focus(player->ui, player->find_focus);
            player->find_focus = 0u;
        }
        player_ui_library_order(player, app, count);
        view = player_ui_library_view(player, count);
        /* The grid holds the user's games and ends with the add card, so it has
           one slot more than games; the demos sit under it, under TEST THIS PC. */
        demo_count = player_ui_library_demo_count(player->titles, count);
        games = player->library_grid_count;
        /* While finding, the box takes TEST THIS PC's room, and the grid is the
           matches alone, without the add card. */
        page_size = player_ui_library_page_size_at(scale.logical_w, scale.logical_h,
                                                   nk_ui_text_scale(player->ui),
                                                   player->find_open, demo_count, &columns);
        slots = player->find_open ? games : games > 0u ? games + 1u : 0u;
        {
            size_t focused_index = player_ui_focused_title_index(
                nk_ui_focused(player->ui), count);
            if (focused_index < count) {
                size_t position = player_ui_library_grid_position(&view, focused_index);
                app->selected_game_index = (int)focused_index;
                if (position < count) player->library_page = position / page_size;
            }
        }
        if (nk_ui_focused(player->ui) == PLAYER_UI_ID_LIBRARY_ADD && slots > 0u) {
            player->library_page = (slots - 1u) / page_size;
        }
        if (player->library_focus_pending) {
            size_t position = player_ui_library_grid_position(&view, player->library_focus_index);
            player->library_focus_pending = false;
            if (position < count) {
                nk_ui_focus(player->ui, PLAYER_UI_ID_LIBRARY_ITEM(player->library_focus_index));
                app->selected_game_index = (int)player->library_focus_index;
                player->library_page = position / page_size;
            } else if (games > 0u) {
                nk_ui_focus(player->ui, PLAYER_UI_ID_LIBRARY_ADD);
                player->library_page = (slots - 1u) / page_size;
            } else {
                nk_ui_focus(player->ui, PLAYER_UI_ID_LIBRARY_EMPTY_ADD);
            }
        }
        if (slots == 0u) player->library_page = 0u;
        else if (player->library_page > (slots - 1u) / page_size) {
            player->library_page = (slots - 1u) / page_size;
        }
        if (slots > page_size) {
            /* Page Down and Page Up, the pad's triggers and the mouse wheel turn
               the page, so a library of hundreds of games is not a hundred
               presses of NEXT. */
            NkUiNavDir dir = NK_UI_DIR_NONE;
            size_t last_page = (slots - 1u) / page_size;
            while (nk_ui_take_event(player->ui, NK_UI_EV_PAGE, &dir) ||
                   nk_ui_take_event(player->ui, NK_UI_EV_WHEEL, &dir)) {
                size_t page = player->library_page;
                if ((dir == NK_UI_DIR_RIGHT || dir == NK_UI_DIR_DOWN) && page < last_page) {
                    ++page;
                } else if ((dir == NK_UI_DIR_LEFT || dir == NK_UI_DIR_UP) && page > 0u) {
                    --page;
                } else {
                    continue;
                }
                player_ui_library_turn_page(player, count, page_size, slots, page);
            }
        }
        if (games > 0u) {
            /* Home and End: the first game, or the last on the last page. */
            NkUiNavDir dir = NK_UI_DIR_NONE;
            while (nk_ui_take_event(player->ui, NK_UI_EV_EDGE, &dir)) {
                size_t position = dir == NK_UI_DIR_DOWN ? games - 1u : 0u;
                size_t index = player_ui_library_grid_title(&view, position);
                if (index >= count) continue;
                player->library_page = position / page_size;
                nk_ui_focus(player->ui, PLAYER_UI_ID_LIBRARY_ITEM(index));
                app->selected_game_index = (int)index;
            }
        }
        player_ui_library_page_art(player, app, count, player->library_page, page_size);
        PlayerUiLibraryModel model = view;
        model.sort_label = player_library_sort_label(app->settings.library_sort);
        model.find_open = player->find_open;
        model.find_query = player->find_query;
        model.find_keyboard = player->find_open ? &player->find_keyboard : NULL;
        model.game_total = player->library_game_total;
        model.page_index = player->library_page;
        model.page_size = page_size;
        model.columns = columns;
        model.selected_index = app->selected_game_index >= 0
            ? (size_t)app->selected_game_index : 0u;
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        if (slots > page_size) {
            size_t page_count = (slots + page_size - 1u) / page_size;
            snprintf(player->library_page_label,
                     sizeof(player->library_page_label), NK_UI_STR(ADAPTER_LIBRARY_PAGE_NUMBER),
                     player->library_page + 1u, page_count);
            model.page_label = player->library_page_label;
        } else {
            player->library_page_label[0] = '\0';
            model.page_label = player->library_page_label;
        }
        if (app->update.notice) {
            snprintf(player->update_notice, sizeof(player->update_notice),
                     NK_UI_STR(ADAPTER_LIBRARY_UPDATE_NOTICE), app->update.check.newest.version);
            model.update_notice = player->update_notice;
        }
        /* The data folder notice (#324), the library file's notice and the one summary of files
           dropped together (#2007), in that order. */
        const char *notices[3] = {app->data_folder_notice, app->library_notice, app->drop_notice};
        player->library_notice[0] = '\0';
        for (size_t i = 0u; i < 3u; ++i) {
            if (notices[i][0] == '\0') continue;
            size_t used = strlen(player->library_notice);
            snprintf(player->library_notice + used, sizeof(player->library_notice) - used, "%s%s",
                     used ? " " : "", notices[i]);
        }
        if (player->library_notice[0] != '\0') model.library_notice = player->library_notice;
        if (player_app_rebuild_banner_visible(app)) {
            (void)player_app_rebuild_banner_text(app, player->rebuild_notice,
                                                 sizeof(player->rebuild_notice),
                                                 &model.rebuild_notice_offers_all);
            model.rebuild_notice = player->rebuild_notice;
            model.rebuild_notice_offers_queue = player_app_build_queue_has_waiting(app);
        }
        if (player_app_disc_check_line(app, player->verify_notice, sizeof(player->verify_notice),
                                       &model.verify_offers_cancel)) {
            model.verify_notice = player->verify_notice;
        }
        player_ui_library_backdrop(player, app, count, now_s, &model);
        action = player_ui_screen_library(player->ui, &model);
        if (action.kind == PLAYER_UI_ACTION_OPEN_TITLE && action.title_index < count) {
            app->selected_game_index = (int)action.title_index;
            nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
            (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_TITLE);
            /* A page opened while PLAY is still off (its package check has not ended)
               puts focus on the next button; arm the refocus so PLAY takes it back
               when it turns on, unless the user moves first. */
            player->title_primary_refocus = true;
            player->title_primary_fallback = 0u;
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_PAGE_PREVIOUS) {
            if (player->library_page > 0u) --player->library_page;
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_PAGE_NEXT) {
            if ((player->library_page + 1u) * page_size < slots) {
                ++player->library_page;
            }
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_FIND_OPEN) {
            player_ui_find_open(player, false);
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_FIND_KEY) {
            if (action.title_index == PLAYER_UI_FIND_DONE) {
                /* Done: on to the first match, the box stays open with it. */
                nk_ui_focus(player->ui, games > 0u
                                            ? PLAYER_UI_ID_LIBRARY_ITEM(player->library_grid[0])
                                            : PLAYER_UI_ID_LIBRARY_FIND_DONE);
            } else if (action.title_index == PLAYER_UI_FIND_CANCEL) {
                player_ui_find_close(player);
            } else {
                /* A key edited the text (or the letter case): the matches start over. */
                player->library_page = 0u;
            }
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_CYCLE_SORT) {
            /* The next frame lays the grid out in the new order, from its
               first page; focus stays on the button. */
            player_app_cycle_library_sort(app);
            player->library_page = 0u;
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_QUEUE &&
                   (PlayerUiQueueOp)action.primary == PLAYER_UI_QUEUE_OPEN) {
            player->queue_first = 0u;
            player->queue_follow_disc[0] = '\0';
            player->queue_follow_back = false;
            nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
            (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_QUEUE);
            action.kind = PLAYER_UI_ACTION_NONE;
        }
    } else if (route == PLAYER_UI_ROUTE_TITLE) {
        int index = app->selected_game_index;
        if (index < 0 || index >= app->game_count ||
            index >= (int)PLAYER_UI_TITLE_CAP) {
            nk_ui_router_replace(player->router, PLAYER_UI_ROUTE_LIBRARY);
        } else {
            PlayerUiTitleModel model;
            const GameRecord *game = &app->games[index];
            /* The page plays this game's own startup clip (player_ui_snd0.h). */
            player->snd0_index = (size_t)index;
            if (!app->launch_pending && !app->is_game_running &&
                !player_app_cached_game_has_runtime(app, game) &&
                !player_app_runtime_package_check_pending(app, game) &&
                !player_app_runtime_package_check_failed(app, game)) {
                (void)player_app_build_package_preflight(app, index, false);
            }
            /* The page shows this title alone, so only it is filled. */
            (void)player_ui_fill_title(player, app, index, &player->titles[index]);
            {
                char value[PLAYER_LAUNCH_TITLE_VALUE_MAX];
                player->titles[index].can_add_to_steam =
                    player_app_launch_title_value(app, index, value, sizeof(value));
            }
            player_ui_title_art(player, app, (size_t)index, true);
            memset(&model, 0, sizeof(model));
            model.host = nk_platform_host_kind();
            model.title = &player->titles[index];
            model.title_index = (size_t)index;
            if (app->settings.achievements_enabled) {
                /* Static: the rows outlive this function, for the frame that draws them. */
                static PlayerUiAchievementRow rows[PLAYER_ACHIEVEMENTS_LIST_MAX];
                if (app->achievements_list_loaded &&
                    strcmp(app->achievements_list_disc_id, game->disc_id) == 0) {
                    model.achievements_status = app->achievements_status[0]
                        ? app->achievements_status : NK_UI_STR(ADAPTER_ACHIEVEMENT_SET_READY);
                    model.achievement_count = app->achievements_list_count <
                            PLAYER_ACHIEVEMENTS_LIST_MAX
                        ? app->achievements_list_count : PLAYER_ACHIEVEMENTS_LIST_MAX;
                    model.achievement_total = app->achievements_list_total;
                    for (size_t i = 0; i < model.achievement_count; i++) {
                        const PlayerAchievementRow *row = &app->achievements_list[i];
                        rows[i].title = row->title;
                        rows[i].description = row->description;
                        rows[i].points = row->points;
                        rows[i].unlocked = row->unlocked;
                        rows[i].badge = row->badge;
                    }
                    model.achievement_rows = rows;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
                    g_title_achievement_rows = rows;
                    g_title_achievement_count = model.achievement_count;
#endif
                    model.achievements_unlocked = app->achievements_unlocked;
                    model.achievements_points_unlocked = app->achievements_points_unlocked;
                    model.achievements_points_total = app->achievements_points_total;
                    /* The page scrolls and lists every row the player keeps; only a set larger
                       than that says how many it shows and where the rest are. */
                    if (model.achievement_total > model.achievement_count)
                        (void)snprintf(model.achievements_more, sizeof model.achievements_more,
                                       NK_UI_STR(ADAPTER_ACHIEVEMENT_MORE),
                                       model.achievement_count, model.achievement_total);
                } else if (!app->achievements_signed_in) {
                    model.achievements_status = NK_UI_STR(ADAPTER_ACHIEVEMENT_SIGN_IN);
                } else {
                    model.achievements_status = NK_UI_STR(ADAPTER_ACHIEVEMENT_PLAY_TO_LOAD);
                }
                /* With cheats or the 60 FPS game-logic package nothing is earned, and the page
                   says why before the game starts (player_app_achievements_block), in the
                   notice shape. */
                model.achievements_notice = player_app_achievements_block(app, index);
            }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
            {
                /* The card's sentence (its notice, else its status line) each time it changes,
                   in hex so every byte reads back, then each row once per change: its state,
                   whether it shows a badge picture or the page's own icon, and its title. */
                static char reported[sizeof app->achievements_status + MAX_DISC_ID_LEN +
                                     sizeof model.achievements_more + 8];
                char now[sizeof(reported)];
                const char *said = model.achievements_notice ? model.achievements_notice
                                                             : model.achievements_status;
                snprintf(now, sizeof(now), "%s|%s|%s", game->disc_id, said ? said : "",
                         model.achievements_more);
                if (strcmp(reported, now) != 0) {
                    snprintf(reported, sizeof(reported), "%s", now);
                    printf("[PLAYER_UI_TEST] next_ui_title_achievements disc=%s status_hex=",
                           game->disc_id);
                    for (const char *c = said; c && *c; c++)
                        printf("%02x", (unsigned)(unsigned char)*c);
                    printf(" more_hex=");
                    for (const char *c = model.achievements_more; *c; c++)
                        printf("%02x", (unsigned)(unsigned char)*c);
                    printf(" notice=%d rows=%zu unlocked=%u points=%u/%u\n",
                           model.achievements_notice ? 1 : 0, model.achievement_count,
                           (unsigned)model.achievements_unlocked,
                           (unsigned)model.achievements_points_unlocked,
                           (unsigned)model.achievements_points_total);
                    for (size_t i = 0; i < model.achievement_count; i++) {
                        printf("[PLAYER_UI_TEST] next_ui_title_achievement disc=%s index=%zu "
                               "state=%s icon=%s points=%u title_hex=",
                               game->disc_id, i,
                               model.achievement_rows[i].unlocked ? "unlocked" : "locked",
                               model.achievement_rows[i].badge ? "badge" : "drawn",
                               (unsigned)model.achievement_rows[i].points);
                        for (const char *c = model.achievement_rows[i].title; c && *c; c++)
                            printf("%02x", (unsigned)(unsigned char)*c);
                        printf("\n");
                    }
                }
            }
#endif
            /* The build check's refusal is on the page only while the build
               waits for it; so are the files it waits for (#308). */
            player_ui_input_guide(&model.input_guide,
                                  player->titles[index].build_preflight_message[0] != '\0'
                                      ? player_app_package_build_input(app, index) : NULL,
                                  player->title_input_summary,
                                  sizeof(player->title_input_summary));
            model.stalled_game_notice_visible =
                player->titles[index].package_state == PLAYER_UI_PACKAGE_RUNNING &&
                app->stalled_game_notice_visible;
            model.stalled_game_dark_picture = app->stalled_game_dark_picture;
            (void)player_app_render_notice(app, app->games[index].disc_id,
                                           &model.render_notice_heading,
                                           &model.render_notice_text);
            /* The font note is about the next start, so a running game's page leaves it out. */
            if (!app->is_game_running) {
                if (app->wizard.font_slot_detail[0][0] == '\0') {
                    player_app_refresh_font_status(app);
                }
                model.font_note_text = player_app_font_note(app, index);
            }
            model.now_s =player_app_reduce_motion(app) ? 0.0 : now_s;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            {
                /* Keep PLAY or BUILD PACKAGE focused across a game's start
                   and exit: while it is off (STARTING..., STOP GAME is not
                   offered as a default) focus waits on the fallback, and
                   comes back when it is on again unless the user moved. */
                const PlayerUiTitle *shown = &player->titles[index];
                bool primary_default = shown->primary_action_enabled &&
                    (shown->primary_action == PLAYER_UI_PRIMARY_PLAY ||
                     shown->primary_action == PLAYER_UI_PRIMARY_BUILD);
                NkUiId focused = nk_ui_focused(player->ui);
                if (!primary_default) {
                    if (focused == PLAYER_UI_ID_TITLE_PRIMARY) {
                        player->title_primary_refocus = true;
                        player->title_primary_fallback = 0u;
                    } else if (player->title_primary_refocus &&
                               player->title_primary_fallback == 0u &&
                               !shown->primary_action_enabled) {
                        /* Only an action that is off (STARTING...) pushes focus away,
                           so only then is the next button the fallback; leaving STOP
                           GAME, which stays on, is the user's own move (#2002).
                           A page just opened still shows the library's focus for a
                           frame or two; the fallback is the page's own button. */
                        if (focused >= 0x2000u && focused < 0x3000u) {
                            player->title_primary_fallback = focused;
                        }
                    } else if (player->title_primary_refocus &&
                               focused != player->title_primary_fallback) {
                        player->title_primary_refocus = false;
                    }
                } else if (player->title_primary_refocus) {
                    /* No fallback recorded yet means the action was off for a
                       single frame: focus is where the fallback put it. */
                    if (player->title_primary_fallback == 0u ||
                        focused == player->title_primary_fallback ||
                        focused == PLAYER_UI_ID_TITLE_PRIMARY) {
                        nk_ui_focus(player->ui, PLAYER_UI_ID_TITLE_PRIMARY);
                    }
                    player->title_primary_refocus = false;
                }
            }
            action = player_ui_screen_title(player->ui, &model);
            if (action.kind == PLAYER_UI_ACTION_BACK) {
                nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
                (void)nk_ui_router_pop(player->router);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_TOGGLE_60FPS) {
                if (player->titles[index].enhancement_enabled) {
                    player_app_set_title_60hz(app, index,
                                              !player->titles[index].enhancement_on);
                }
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_CYCLE_PLAY_MODE) {
                if (player->titles[index].ace_enabled) player_app_cycle_title_play_mode(app, index);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_OPEN_PLAY_OPTIONS) {
                if (player->titles[index].options_enabled) {
                    nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
                    player_app_open_title_enhanced(app, index);
                }
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_FONT_NOTE_GET_FONTS) {
                /* main.c opens Settings > PSP fonts and looks for a PSP; Back comes here. */
                nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
            } else if (action.kind == PLAYER_UI_ACTION_OPEN_SAVES) {
                if (player->titles[index].save_folder_enabled) {
                    player_ui_open_saves(player, app, index);
                    nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
                    (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_SAVES);
                }
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_ADD_TO_STEAM) {
                if (player->titles[index].can_add_to_steam) {
                    player_ui_open_steam(player, app, index);
                    nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
                    (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_STEAM);
                }
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_REMOVE_TITLE) {
                if (player_ui_open_remove(player, app, index)) {
                    nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
                    (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_REMOVE);
                } else {
                    /* No plan, so nothing is removed: say why rather than ignore the press. */
                    const char *blocker = player_app_removal_blocker(app, index);
                    player_app_set_error(
                        app, "REMOVE_PLAN_FAILED", NK_UI_STR(ADAPTER_REMOVE_ERROR_TITLE),
                        blocker != NULL
                            ? blocker
                            : NK_UI_STR(ADAPTER_REMOVE_PATH_TOO_LONG),
                        NK_UI_STR(ADAPTER_RECOVERY_BACK), VIEW_LIBRARY);
                }
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_PRIMARY &&
                       action.primary == PLAYER_UI_PRIMARY_STOP_GAME) {
                /* Stopping loses what was not saved, so it asks first, as
                   quitting with a game running does. */
                player->stop_index = index;
                snprintf(player->stop_disc, sizeof(player->stop_disc), "%s",
                         app->games[index].disc_id);
                player->stop_focus_pending = true;
                nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
                (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_STOP);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_PRIMARY ||
                       action.kind == PLAYER_UI_ACTION_OPEN_SAVE_FOLDER ||
                       action.kind == PLAYER_UI_ACTION_REPORT_PROBLEM) {
                if (out_action != NULL) *out_action = action;
            }
        }
    } else if (route == PLAYER_UI_ROUTE_STOP) {
        int index = player->stop_index;
        /* The question goes when there is nothing left to stop: the game
           ended by itself, or its title left the library. */
        if (!app->is_game_running || index < 0 || index >= app->game_count ||
            strcmp(app->games[index].disc_id, player->stop_disc) != 0) {
            (void)nk_ui_router_pop(player->router);
        } else {
            PlayerUiStopModel model;
            if (player->stop_focus_pending) {
                nk_ui_focus(player->ui, PLAYER_UI_ID_STOP_KEEP);
                player->stop_focus_pending = false;
            }
            memset(&model, 0, sizeof(model));
            /* STOP GAME stops the game that runs, which may not be this page's. */
            model.title = app->launch_session.game_title[0] != '\0'
                ? app->launch_session.game_title : app->games[index].title_name;
            model.title_index = (size_t)index;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            action = player_ui_screen_stop(player->ui, &model);
            if (action.kind == PLAYER_UI_ACTION_BACK) {
                (void)nk_ui_router_pop(player->router);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_PRIMARY) {
                if (out_action != NULL) *out_action = action;
                (void)nk_ui_router_pop(player->router);
                action.kind = PLAYER_UI_ACTION_NONE;
            }
        }
    } else if (route == PLAYER_UI_ROUTE_REMOVE) {
        int index = player->remove_index;
        /* The dialog closes if its title left the library or can no longer
           be removed (a build or a launch started under it). */
        if (index < 0 || index >= app->game_count ||
            strcmp(app->games[index].disc_id, player->remove_disc) != 0 ||
            player_app_removal_blocker(app, index) != NULL) {
            (void)nk_ui_router_pop(player->router);
        } else {
            PlayerUiRemoveModel model;
            if (player->remove_focus_pending) {
                nk_ui_focus(player->ui, PLAYER_UI_ID_REMOVE_CANCEL);
                player->remove_focus_pending = false;
            }
            memset(&model, 0, sizeof(model));
            model.title = app->games[index].title_name;
            model.disc_id = app->games[index].disc_id;
            model.title_index = (size_t)index;
            model.items = player->remove_items;
            model.item_count = player->remove_item_count;
            model.has_saves = player->remove_plan.has_saves;
            model.saves_bytes = player->remove_plan.saves_bytes;
            model.delete_saves = player->remove_delete_saves;
            model.has_disc_copy = player->remove_plan.has_disc_copy;
            model.disc_copy_bytes = player->remove_plan.disc_copy_bytes;
            model.delete_disc_copy = player->remove_delete_disc_copy;
            model.files_shared = player->remove_plan.files_shared;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            action = player_ui_screen_remove(player->ui, &model);
            if (action.kind == PLAYER_UI_ACTION_BACK) {
                (void)nk_ui_router_pop(player->router);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_REMOVE_TOGGLE_SAVES) {
                player->remove_delete_saves = !player->remove_delete_saves;
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_REMOVE_TOGGLE_DISC_COPY) {
                player->remove_delete_disc_copy = !player->remove_delete_disc_copy;
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_REMOVE_CONFIRM) {
                /* The removal runs after this frame; the library then
                   focuses the tile that takes this one's place. */
                if (out_action != NULL) *out_action = action;
                while (nk_ui_router_depth(player->router) > 1u) {
                    (void)nk_ui_router_pop(player->router);
                }
                player->library_focus_pending = true;
                player->library_focus_index = (size_t)index;
                action.kind = PLAYER_UI_ACTION_NONE;
            }
        }
    } else if (route == PLAYER_UI_ROUTE_SAVES) {
        int index = player->saves_index;
        /* The dialog closes if its title left the library. */
        if (index < 0 || index >= app->game_count ||
            strcmp(app->games[index].disc_id, player->saves_disc) != 0) {
            player->saves_waiting_pick = false;
            app->saves_pick_ready = false;
            (void)nk_ui_router_pop(player->router);
        } else {
            PlayerUiSavesModel model;
            const char *blocked = player_app_saves_blocker(app, index);
            if (app->saves_pick_ready) {
                /* The folder Add a save picked (main.c takes it from the picker). */
                bool ours = player->saves_waiting_pick && app->saves_pick_index == index;
                app->saves_pick_ready = false;
                player->saves_waiting_pick = false;
                player_ui_saves_status(player, PLAYER_UI_NOTE_NEUTRAL, "");
                if (ours && blocked == NULL) {
                    player_ui_saves_add(player, app, index, app->saves_picked, false);
                }
            }
            if (blocked != NULL) player->saves_ask = PLAYER_UI_SAVES_ASK_NONE;
            if (player->saves_focus != 0u) {
                nk_ui_focus(player->ui, player->saves_focus);
                player->saves_focus = 0u;
            }
            memset(&model, 0, sizeof(model));
            model.title = app->games[index].title_name;
            model.title_index = (size_t)index;
            model.has_saves = player->saves_has;
            model.saves_bytes = player->saves_bytes;
            model.rows = player->saves_rows;
            model.row_count = player_ui_saves_page_rows(player);
            model.first_row = player->saves_first;
            model.total = player->saves_total;
            model.listed = player->saves_list_count;
            model.ask = player->saves_ask;
            model.ask_row = player->saves_ask_row;
            model.ask_name = player->saves_ask_name;
            if (player->saves_bring_in.name[0]) {
                model.bring_in_name = player->saves_bring_in.name;
                model.bring_in_note = player_ui_saves_bring_in_note(player->saves_bring_in.decision);
            }
            model.bring_in_problem = player->saves_bring_in_problem;
            model.status = player->saves_status;
            model.status_kind = player->saves_status_kind;
            model.blocked = blocked;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            action = player_ui_screen_saves(player->ui, &model);
            if (action.kind == PLAYER_UI_ACTION_BACK) {
                if (player->saves_ask != PLAYER_UI_SAVES_ASK_NONE) {
                    /* Back answers the question with Cancel first. */
                    player_ui_saves_op(player, app, index, PLAYER_UI_SAVES_CANCEL, NULL);
                } else {
                    player->saves_waiting_pick = false;
                    (void)nk_ui_router_pop(player->router);
                }
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_SAVES) {
                player_ui_saves_op(player, app, index, (unsigned)action.primary, out_action);
                action.kind = PLAYER_UI_ACTION_NONE;
            }
        }
    } else if (route == PLAYER_UI_ROUTE_STEAM) {
        int index = player->steam_index;
        /* The dialog closes if its title left the library. */
        if (index < 0 || index >= app->game_count ||
            strcmp(app->games[index].disc_id, player->steam_plan.disc_id) != 0) {
            (void)nk_ui_router_pop(player->router);
        } else {
            PlayerUiSteamModel model;
            if (player->steam_focus != 0u) {
                nk_ui_focus(player->ui, player->steam_focus);
                player->steam_focus = 0u;
            }
            memset(&model, 0, sizeof(model));
            model.title = player->steam_plan.app_name;
            model.title_index = (size_t)index;
            model.accounts = player->steam_accounts;
            model.account_count = player->steam_plan.account_count;
            model.chosen = player->steam_chosen;
            model.command = player->steam_command;
            model.portable = player->steam_plan.portable;
            model.blocked = player->steam_plan.ready ? NULL : player->steam_blocked;
            model.status = player->steam_status;
            model.status_kind = player->steam_status_kind;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            action = player_ui_screen_steam(player->ui, &model);
            if (action.kind == PLAYER_UI_ACTION_BACK) {
                (void)nk_ui_router_pop(player->router);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_STEAM) {
                player_ui_steam_op(player, (unsigned)action.primary);
                action.kind = PLAYER_UI_ACTION_NONE;
            }
        }
    } else if (route == PLAYER_UI_ROUTE_QUEUE) {
        action = player_ui_render_queue(player, app, scale);
        if (action.kind == PLAYER_UI_ACTION_BACK) {
            (void)nk_ui_router_pop(player->router);
            action.kind = PLAYER_UI_ACTION_NONE;
        }
    } else if (route == PLAYER_UI_ROUTE_REBUILD) {
        PlayerUiRebuildModel model;
        memset(&model, 0, sizeof(model));
        if (player->rebuild_focus_pending) {
            nk_ui_focus(player->ui, PLAYER_UI_ID_REBUILD_PRIMARY);
            player->rebuild_focus_pending = false;
        }
        if (player_app_rebuild_reminder_visible(app)) {
            int index = player_app_find_game_by_disc_id(app, app->outdated.reminder_disc_id);
            model.title = index >= 0 && app->games[index].title_name[0]
                ? app->games[index].title_name : NK_UI_STR(ADAPTER_TITLE_FALLBACK);
            model.text = PLAYER_REBUILD_REMINDER_TEXT;
        } else {
            (void)player_app_rebuild_prompt_text(app, player->rebuild_text,
                                                 sizeof(player->rebuild_text));
            model.text = player->rebuild_text;
        }
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        action = player_ui_screen_rebuild(player->ui, &model);
        if (action.kind == PLAYER_UI_ACTION_REBUILD) {
            if (out_action != NULL) *out_action = action;
            (void)nk_ui_router_pop(player->router);
            player->rebuild_pushed = false;
            action.kind = PLAYER_UI_ACTION_NONE;
        }
    } else if (route == PLAYER_UI_ROUTE_ABOUT) {
        PlayerUiAboutModel model;
        memset(&model, 0, sizeof(model));
        model.version_line = player->version_line;
        model.build_commit = NK_PLAYER_BUILD_COMMIT;
        model.data_root_line = player->data_root_line;
        model.love_all = player_app_has_theme_unlock(app, LAUNCHER_THEME_UNLOCK_WAVE);
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        action = player_ui_screen_about(player->ui, &model);
        if (action.kind == PLAYER_UI_ACTION_OPEN_LOVE_ALL) {
            player_ui_love_all_open(player, player_app_reduce_motion(app), now_s);
            action.kind = PLAYER_UI_ACTION_NONE;
        }
    } else if (route == PLAYER_UI_ROUTE_LOVE_ALL) {
        action = player_ui_render_love_all(player, scale);
        if (action.kind == PLAYER_UI_ACTION_BACK) {
            (void)nk_ui_router_pop(player->router);
            action.kind = PLAYER_UI_ACTION_NONE;
        }
    }
    /* The top-bar sections switch the root screen. */
    if (action.kind == PLAYER_UI_ACTION_OPEN_ABOUT) {
        /* About opens over the screen it was chosen from, so Back returns
           there with the ABOUT tab focused. */
        nk_ui_router_set_focus(player->router, PLAYER_UI_ID_TAB_ABOUT);
        (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_ABOUT);
        nk_ui_focus(player->ui, PLAYER_UI_ID_TAB_ABOUT);
        action.kind = PLAYER_UI_ACTION_NONE;
    } else if (action.kind == PLAYER_UI_ACTION_OPEN_LIBRARY) {
        (void)nk_ui_router_replace(player->router, PLAYER_UI_ROUTE_LIBRARY);
        nk_ui_focus(player->ui, PLAYER_UI_ID_TAB_LIBRARY);
        action.kind = PLAYER_UI_ACTION_NONE;
    }
    /* The action is handed back before the frame is finished: its route has
       already changed (a confirmation dialog closed, the rebuild prompt was
       answered), so a frame that cannot be drawn must not drop it. */
    if (action.kind == PLAYER_UI_ACTION_NONE) action = rebuild_answer;
    if (out_action != NULL && action.kind != PLAYER_UI_ACTION_NONE) {
        *out_action = action;
    }
    nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
    player_ui_secret_draw(player, scale, now_s);
    draw_list = nk_ui_end(player->ui);
    if (nk_ui_take_prompt_back(player->ui)) player_ui_push_system_back(player);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (g_title_achievement_count) {
        player_ui_title_report_achievement_rows(draw_list, g_title_achievement_count,
                                                g_title_achievement_rows);
        g_title_achievement_count = 0u;
    }
#endif
    /* The inspector's own close button turns it off in Clay; the setting follows, so the
       next frame does not turn the panel on again. */
    if (app->settings.developer_mode && app->dev_log.inspect_layout &&
        !nk_ui_layout_debug(player->ui)) {
        app->dev_log.inspect_layout = false;
        player_dev_log_add(app, "ui", "layout inspector off (closed from its panel)");
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    /* Stands in for a frame nk_ui could not finish, on the frame that confirms
       Remove from library. */
    if (getenv("NK_UI_TEST_FAIL_REMOVE_CONFIRM_FRAME") != NULL && out_action != NULL &&
        out_action->kind == PLAYER_UI_ACTION_REMOVE_CONFIRM) {
        puts("[PLAYER_UI_TEST] UI_FRAME_NOT_DRAWN");
        draw_list = NULL;
    }
#endif
    if (draw_list == NULL) return false;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    player_ui_report_enhanced_panel(player, draw_list);
#endif
    {
        NkUiId focus_now = nk_ui_focused(player->ui);
        if (player->pending_sound < 0 && player->sound_focus != 0u && focus_now != 0u &&
            focus_now != player->sound_focus) {
            player->pending_sound = PLAYER_UI_SOUND_MOVE;
        }
        player->sound_focus = focus_now;
        if (player->pending_sound >= 0) {
            player_ui_sounds_play(player->sounds, (PlayerUiSound)player->pending_sound);
            player->pending_sound = -1;
        }
    }
    {
        /* The window's ground is the theme's page colour (N1: #0B0F14). */
        uint32_t page = nk_ui_theme(player->ui)->surface;
        (void)SDL_SetRenderDrawColor(renderer, (Uint8)(page >> 24), (Uint8)(page >> 16),
                                     (Uint8)(page >> 8), 255u);
    }
    (void)SDL_RenderClear(renderer);
    if (!nk_ui_sdlr_render(player->backend, draw_list)) return false;
    SDL_RenderPresent(renderer);
    return true;
}
