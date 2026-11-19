/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_font.h"
#include "nk_types.h"
#include "nk_platform.h"
#include "nk_shared_helpers.h"
#include "nk_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -----------------------------------------------------------------------------
 * Inspection: the native reader's verdict, the slot rule, and the file identity
 * -------------------------------------------------------------------------- */
static const char *const kSlotRole[NK_FONT_SLOT_COUNT] = { "Japanese", "Latin", "Korean" };
static const char *const kSlotId[NK_FONT_SLOT_COUNT] = { "japanese", "latin", "korean" };
static const char *const kSlotFile[NK_FONT_SLOT_COUNT] = {
    NK_FONT_SLOT_JAPANESE_FILE, NK_FONT_SLOT_LATIN_FILE, NK_FONT_SLOT_KOREAN_FILE
};

static const char *const kLatinIndexFile[NK_FONT_LATIN_INDEX_COUNT] = {
    "ltn0.pgf",  "ltn1.pgf",  "ltn2.pgf",  "ltn3.pgf",
    "ltn4.pgf",  "ltn5.pgf",  "ltn6.pgf",  "ltn7.pgf",
    "ltn8.pgf",  "ltn9.pgf",  "ltn10.pgf", "ltn11.pgf",
    "ltn12.pgf", "ltn13.pgf", "ltn14.pgf", "ltn15.pgf",
};

const char *nk_font_latin_index_filename(int index) {
    return (unsigned)index < NK_FONT_LATIN_INDEX_COUNT ? kLatinIndexFile[index] : NULL;
}

static const char *const kProjectLatinIndexFile[NK_FONT_LATIN_INDEX_COUNT] = {
    NK_FONT_SLOT_LATIN_FILE, "nkltn1.pgf",  "nkltn2.pgf",  "nkltn3.pgf",
    "nkltn4.pgf",            "nkltn5.pgf",  "nkltn6.pgf",  "nkltn7.pgf",
    NK_FONT_PROJECT_LATIN_SMALL_FILE, "nkltn9.pgf", "nkltn10.pgf", "nkltn11.pgf",
    "nkltn12.pgf",           "nkltn13.pgf", "nkltn14.pgf", "nkltn15.pgf",
};

const char *nk_font_project_latin_index_file(int index) {
    return (unsigned)index < NK_FONT_LATIN_INDEX_COUNT ? kProjectLatinIndexFile[index] : NULL;
}

const char *nk_font_slot_psp_filename(NkFontSlot slot) {
    switch (slot) {
    case NK_FONT_SLOT_JAPANESE: return "jpn0.pgf";
    case NK_FONT_SLOT_KOREAN:   return "kr0.pgf";
    default:                    return NULL;
    }
}

int nk_font_latin_index_from_filename(const char *name) {
    if (!name) return -1;
    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) {
        const char *candidate = kLatinIndexFile[index];
        const unsigned char *a = (const unsigned char *)name;
        const unsigned char *b = (const unsigned char *)candidate;
        while (*a && *b && tolower(*a) == tolower(*b)) { a++; b++; }
        if (*a == 0 && *b == 0) return index;
    }
    return -1;
}

static void font_copy_text(char *out, size_t out_len, const char *text) {
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "%s", text ? text : "");
}

static const char *font_refusal_text(PgfRefusal refusal) {
    switch (refusal) {
    case PGF_REFUSE_TOO_LARGE: return "larger than the 16 MiB reader ceiling";
    case PGF_REFUSE_TRUNCATED: return "truncated";
    case PGF_REFUSE_HEADER_OFFSET: return "header offset is not zero";
    case PGF_REFUSE_MAGIC: return "invalid PGF magic (expected PGF0)";
    case PGF_REFUSE_REVISION: return "unsupported revision or version";
    case PGF_REFUSE_HEADER_SIZE: return "header size does not match the revision";
    case PGF_REFUSE_GLYPH_RANGE: return "corrupt glyph indices (first exceeds last)";
    case PGF_REFUSE_COUNTS: return "count or bit width out of range";
    case PGF_REFUSE_NO_GLYPHS: return "no glyph records";
    case PGF_REFUSE_GLYPH: return "a glyph record fails the reader's checks";
    case PGF_REFUSE_NONE: return "";
    }
    return "refused by the reader";
}

static void font_sha256_hex(const uint8_t *bytes, size_t length, char out[65]) {
    NkSha256 sha;
    uint8_t digest[32];
    nk_sha256_init(&sha);
    nk_sha256_update(&sha, bytes, length);
    nk_sha256_finish(&sha, digest);
    for (size_t i = 0; i < 32; i++) {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
    out[64] = '\0';
}

/* -----------------------------------------------------------------------------
 * Replacement fonts (nk_font_is_replacement). tools/nk_core/fonts.py holds the same rule and the
 * same digests (is_replacement_font, KNOWN_REPLACEMENT_SHA256); tools/test_font_provisioning.py
 * compares the two.
 * -------------------------------------------------------------------------- */
/* Where a PGF header holds its font name, and its width (docs/cleanroom/PGF_SPEC.md, 3.2). */
#define NK_FONT_NAME_OFFSET 0x35u
#define NK_FONT_NAME_SIZE 64u
/* The font name of every stand-in this project converts from an open font starts with this
 * ("Nakagawa Open Latin", "Nakagawa Open Japanese", "Nakagawa Open Korean": mk/housekeeping.mk,
 * and tools/ttf2pgf.py's default name). */
static const char kProjectStandInNamePrefix[] = "Nakagawa Open ";

/* SHA-256 of each version of the open replacement fonts other programs ship in place of a PSP's
 * own (jpn0.pgf, kr0.pgf and ltn0.pgf to ltn15.pgf). Their headers carry the console fonts'
 * family names, so only the exact files are recognised. Each group names the public repository,
 * the revision that first held those bytes and the folder; the list covers every version in
 * those folders' history up to hrydgard/ppsspp@5193e380a57bf2401f00cf60c9c382cd26bdcb4d and
 * jpcsp/jpcsp@00ac4d6daedd89c0492093ae83fb0d3d819af414, except one jpn0.pgf
 * (hrydgard/ppsspp@25aebba067a85fd949563099a015e7522442c83b) whose origin its history does not
 * establish. It names replacement files only, never a PSP's own font. */
static const char *const kKnownReplacementSha256[] = {
    /* hrydgard/ppsspp@1429d0206ac152fe4c77936f57016c9279467b12 (2013-02-27), flash0/font/ */
    "8e2cd9c996e7cd5d229612cadd7bb03a82ccc8728b198568df8b4f585786cb5a", /* kr0.pgf */
    "e59233e26b58a7ffb4a0eead399116c92dd30dd13ef888e46ebde3bc08045a18", /* jpn0.pgf */
    "fc6c6a61f741d806102eb9d4b53e8661e44a886a824518c347b52ea492100cc3", /* ltn0.pgf */
    "37a8261dd27b7fbc5f1b273f393bce956fbe9cabcbb9978cfc7acb06fd03ceea", /* ltn1.pgf */
    "96ca233797aae69ae06473aef930b78ed0b003555394b127bda5c049eef32954", /* ltn2.pgf */
    "8c6e59844c0cf3274c92b71fb74616d3512200075761386a045fb6dbffb39f02", /* ltn3.pgf */
    "ab09ddda69d8141ecb6738f0267cd086b1cf3eeadb8bef93c129e86f2261b06e", /* ltn4.pgf */
    "c8b5fbdd4eca5eba3e6e5d993bc5879fd2208f94669b98620e56f691989eda40", /* ltn5.pgf */
    "633d44c2af710235404c70cab4bc2a0b2dff8efcf7eb0dcb874f8d24da5d032d", /* ltn6.pgf */
    "6ae771b2c07773f2a1435d8af652477fb2ec86075b89c6917150ba1e8d7af75d", /* ltn7.pgf */
    "3ce9134e64ee9af66bd177d9821213cbe7e7e7e4a05697e61fe8afaf88ceb3c1", /* ltn8.pgf */
    "f5f036a9e7063a6cfe64b1cdbe9164d0fe8f946c3fb4caf0a610b882764dcee9", /* ltn9.pgf */
    "7459613b939ae8adc7942f0b6a4c0141e5a5efcc00871755c5e46209dd44ff1d", /* ltn10.pgf */
    "b5cb9aa151075e0d70419b935012ae052de122c672b9581f686528bef1284c62", /* ltn11.pgf */
    "b57089eca4c0f1645aac463ee91bb7404150901dd6b0f45c67be805b8b262fdd", /* ltn12.pgf */
    "45e6187a5de1f03fe9faa4f4e5dfabbe707389b275851fda47af2c4bfa39bf22", /* ltn13.pgf */
    "cb7456d9ad273e33b79351bbeff6439d2735f470c7bfe9b46ca262ebbd73365d", /* ltn14.pgf */
    "ebcdde936664990ff4d447988bb50d39b7d42a1d242909c8430368044b73a0d6", /* ltn15.pgf */
    /* hrydgard/ppsspp@69fe13e6c30b616a28d4f083f52c11b22ae36a25 (2013-03-17), flash0/font/ */
    "0095817395400b683ad5d79c107aba6dde5d6bdcb7d4f5c0e36cd996d3fb32b4", /* jpn0.pgf */
    /* hrydgard/ppsspp@9a114587a3eb378351448422390aff10ab2602e4 (2013-03-17), flash0/font/ */
    "fa843d54502597b11a393226d8c3cb3360230feb8cd053dff29a0b0afc960635", /* jpn0.pgf */
    /* hrydgard/ppsspp@0436b751a70c4a6b374f3dca777f13e0d42f8855 (2013-08-24), flash0/font/ */
    "b4ee2e944bfe59ef0b8e40f54f92a0954373b95636e7f1519128809f406a2a41", /* jpn0.pgf */
    /* hrydgard/ppsspp@77c1e96588718b83527fac9dfe395c2e5eddf4f7 (2020-10-09), assets/flash0/font/ */
    "521e8c40955da80e28354ac2adff243f9dff071b177cdc72461376a837a92639", /* jpn0.pgf */
    /* hrydgard/ppsspp@f68e0fecd4f52b9836b8aa031248f2a1d3489bce (2020-10-26), assets/flash0/font/ */
    "d283601e6c410c9be0dbb279b76bd87cb1d9ef6a2600cf04fe54f7054ea85a8d", /* jpn0.pgf */
    /* hrydgard/ppsspp@3831d2346d08ac7d7a8d64c39cb38593e2df3be4 (2020-10-27), assets/flash0/font/ */
    "ede37dd375560fcf34d27da61078d38319a484030d684eda905df3f77ac8ddee", /* ltn1.pgf */
    "79c5fa821dad8413bc5a0c6243a69cf89fb94fbe30579fffba6f980e2e201bea", /* ltn3.pgf */
    "0061fabc877438ae7554e704e93e9e767905b15155a9db2e6a17339a7ceb8338", /* ltn5.pgf */
    "7a05364683f6ed03a87f7a578046306a6c31c2db4f9153d9c1ae6792dc95c973", /* ltn7.pgf */
    /* hrydgard/ppsspp@41ee5fbd931c796d188d35e6df43156dea25985f (2020-10-28), assets/flash0/font/ */
    "b2cd3644892a4792f85d28a7005ecb9a240546671b3d1fca58f7d6552a6c94d9", /* ltn1.pgf */
    "e6683d4e13ab7b5c1601e09b42ae20fddb99bdacb58839f949ffd7615b101347", /* ltn3.pgf */
    "d575de373d3184170cbd10b370b7fde9ad60012e7afeb1c6c058d2f13f0dc7fa", /* ltn5.pgf */
    "4c5a11c72446b4ce4495fd8825f4aaca46b2874f651def4c53ab8bb8ebbce71e", /* ltn7.pgf */
    /* hrydgard/ppsspp@06b0e33839b4260caa7f950df7e7a19961395e70 (2020-10-31), assets/flash0/font/ */
    "59ee75b2be664f68730e312a98ce3bb03fb2177ba24aab73d321ed79bed27332", /* ltn0.pgf */
    "d8fcf2a699adfdd4beca3dcc52c73d93817c4c9445f356ee34ffd2a22298258d", /* ltn2.pgf */
    "a3357b82525953a8eec36ae9cc3bd8c256fae7cc1aa379fa798ba0cb979b9f44", /* ltn4.pgf */
    /* hrydgard/ppsspp@737e0d55d4a06cb026ec453010e1860ecc23df97 (2020-11-22), assets/flash0/font/ */
    "c6f58823cd316dd1840a4dc5b1aaded4ce50a02d3afc7859af96e287b4c5c67c", /* ltn0.pgf */
    "076ef1e86b7d090cc8b5064cda6eb8f4d7889967b23e7379fce0ef2cfa76d94a", /* ltn1.pgf */
    "a7369bc141f1d808c300258130fa231e26d007a33ac03eb345a2220a14c60f45", /* ltn2.pgf */
    "252e3c4dd7844aeb48e183539ade79c83388126402959b7fa7aee9206404bca1", /* ltn3.pgf */
    "2136dc41c671e4b3c9cdbd6323f3b21ca4b7dfc83bd0575e7f19b0cf21e94c20", /* ltn4.pgf */
    "24ca2b1fb1d4078104fcb6c34c7e5bc60fae6f06e38c442d8089d7c97bc023b0", /* ltn5.pgf */
    "22e0036e0cdcfc4ef5ac27d289804d06b91efd2e3b549983efa0e6c32967e978", /* ltn6.pgf */
    "c3c8cc74d74efdc0523670d1a08b0ae523977a154f7bd8d870fd284e71579746", /* ltn7.pgf */
    /* hrydgard/ppsspp@f1a3ba7f5669204a964e4a49c0354d8ab64e6dbc (2020-11-22), assets/flash0/font/ */
    "9a0424fa5050317ef1a15601ef3a26a33ac99ade3adaf6f31dda056113660ca5", /* jpn0.pgf */
    /* hrydgard/ppsspp@388ac3c9aeb69c86e269eb1ad16fb8d1ef2a5fcd (2020-11-23), assets/flash0/font/ */
    "5a612817cc78058603d90e349ba80347ad53e85b251b2b7046e207d7e5e7383f", /* jpn0.pgf */
    /* hrydgard/ppsspp@b5e23007700fc026c4fddea45e87f345d2a41d32 (2020-11-29), assets/flash0/font/ */
    "115d7de21446aa7b7122ab750f3b6d68e265cb5b62f3c8a36c8766272d8331ae", /* ltn0.pgf */
    "eff4a780810c8caee666723e12291b750af2348706aaf81a3767b709956a29fb", /* ltn2.pgf */
    "6058470e62f172ff6c87633cbd02fe9b9f05a9e9f8cddbf2f48b94aeb944e876", /* ltn8.pgf */
    "7c010ab6616417a7a60f81afd6163849fd9c68e753148a9909f907e03b6d077d", /* ltn9.pgf */
    "d4d6f486ba26f3eca714bbb6def86e48b5eee4b6f1ece4ec2076aec7e1bffb0e", /* ltn10.pgf */
    "9a6733f492460b9f75f25a1187408b94e57415bb3c6fa8d36241312f2168c2cd", /* ltn11.pgf */
    "cefc0b3b6e48d8bd0df51acaf8771ece87336af3aaa930019d9b0fe29d5ee1a6", /* ltn12.pgf */
    "ddf07858d55ea1fae19ba26410d5aa7da6028e8004c7abbc23d06371cb081af7", /* ltn13.pgf */
    "d246772639659f028737eb30263b4a4185e0035849ce73996abcac1fc54ad5a4", /* ltn14.pgf */
    "35e699c79f38b685293f792f35e9e7aae8b8033fd77779db47d3d76378b052db", /* ltn15.pgf */
    /* hrydgard/ppsspp@75bdb5ff1723bd39574baf11336c5c8a3173deec (2020-11-30), assets/flash0/font/ */
    "7147db48e15e2f41c4da5214c4c14b4252c71fb164a9b407e6f7df5c4f5ccf21", /* kr0.pgf */
    /* hrydgard/ppsspp@0c243ee13909a1256de4be2b9d374aa6f7b46d38 (2020-12-08), assets/flash0/font/ */
    "bb414a4fac24c503ff07d78aa6c8c08958ba7c518440450da4c6ac44523c2921", /* kr0.pgf */
    "59254bec7f0370424a904469e84b1068f9fa6cea355f201696e798e5888de34f", /* jpn0.pgf */
    "e4f98d7f221eb0121d385844555450224aaf89d347458df94f9ef2a529212107", /* ltn0.pgf */
    "3ae0732b667b60aaa7b3f204c6ba42ec3955330cc9fc48831579cf8630f50eb4", /* ltn1.pgf */
    "eafc02b3f0e2025463f7a32e62367945eb40bc33a43a16998ca4b2d7b694f218", /* ltn2.pgf */
    "c33820c387b0c99e6409128b45e53015b62200fd4b862b0f1173b015be4f597d", /* ltn3.pgf */
    "4ae00e18621e20af98601813d1ac8d20c481ca1df97b11f38ef5251bd621a30c", /* ltn4.pgf */
    "7ba3a04d602bf57df9c1c40ec03dc5d902a7225d3d5efa3a1fda47503e3e588b", /* ltn5.pgf */
    "79188057278018b192a3a9ba69f230ba5234665eb66c8b4985cb2ee31138c5db", /* ltn6.pgf */
    "ee009e747e6d1fc41852607cf2ebb12967bb43f2a8de98be6ee70ea6a55236de", /* ltn7.pgf */
    "87e75290cace40b36fc6ee8942dc282544699b04ce58309067f1f20a65a7cd7f", /* ltn8.pgf */
    "bf67c7bd1b2cec1d7735de774afd70400d2ad58d12297f1e0c999fc6cab84b8d", /* ltn9.pgf */
    "50594f5eba9b1951109826d0016b801badcc7c3421084d8e096168234aa2a610", /* ltn10.pgf */
    "ff3cd634fbb9e3a5e426dc7d415a6308c23943718d8984f860c71fe1bbfad13f", /* ltn11.pgf */
    "5d1962bcf461dbf586e3657d738823203462d9feb1c67f6191ed6eac58bbc49b", /* ltn12.pgf */
    "f742eb3d1305efa0f33b3817ae3c164250b23c6283b654223eab60788739bb50", /* ltn13.pgf */
    "07781f2e3f3d278a033289c77b781b007d8e34ef0ac710e64a4282cc7e4205c8", /* ltn14.pgf */
    "0aefe7c513ea24dccdcd03d1119b08cb1175b5135df2a87458a4404295fe83db", /* ltn15.pgf */
    /* hrydgard/ppsspp@b4847976d35c33ea3daa57ef040ea3da2b939964 (2020-12-17), assets/flash0/font/ */
    "3cbede6c4147e05e9a1dcb24d34c4967119238b6111bb259e97c5e74e46de787", /* kr0.pgf */
    "378d4466b61f362621ef7d5df5ad96e23c0c1104f57bcbed07a6a490325d23d2", /* jpn0.pgf */
    "1c98682617515e60f2e8c7a0ef98cccc7bb1bb9ef6f98634daa40e255a418f69", /* ltn0.pgf */
    "697f4848f6506490e808520a0ca80eb4479d18efc151d8061626b369a88ecd38", /* ltn2.pgf */
    "207ff74d27d4dca556ae03d2d29741a2de30d649b3180e0d78487e68816d21c7", /* ltn4.pgf */
    "95f9e2ea3c7b6841d09140a378467312c14350b45b90e5c43e4651f91df1960b", /* ltn6.pgf */
    /* hrydgard/ppsspp@f001ee284e418c53fa59ce9a7583e75afb4f839a (2020-12-17), assets/flash0/font/ */
    "f66da94ce73b699ca54a82677cebcf537a74b904c7a93d2ec224d0a78393b166", /* ltn0.pgf */
    "d498b083046a23e91d31af5e20c87a48ec80cb46ba6a87f3706e6d597a7fdd33", /* ltn2.pgf */
    "d420943080c1a86731b0f1110d4aafddae962f2d76bdc52e3c24eb92015359a4", /* ltn4.pgf */
    "2158626b212831feb66cfea8cfc1be243803014c2a9ca1649b3ebf60bb8448c9", /* ltn6.pgf */
    /* hrydgard/ppsspp@9fe10351c43e4ee21e02db54caaacf257ddd1fa6 (2020-12-31), assets/flash0/font/ */
    "1c797e9c36b5637db7d855f31d6a70109d7cac823b98cce82863cd9b1c9f1cf0", /* jpn0.pgf */
    /* hrydgard/ppsspp@95e6e6c83944149dc2346851fb2fe25afae91406 (2021-01-10), assets/flash0/font/ */
    "e22b1d1f81b7239483ab00f7d84bfce118b71df84f1ae779509d90fbae419df5", /* jpn0.pgf */
    "4621ff6a474a0cd1f36dc6555759ded517a5937d6b97f09d77957f9fd293d845", /* ltn0.pgf */
    "d428bd0794086ea00156433bedc6fa2c28be83785b9209f6b3b01190446365d6", /* ltn1.pgf */
    "ac4041e5c9c9bb87929c4fa1da58eb53a9cc68f0c2a91c10969ab0c0daf9eb42", /* ltn2.pgf */
    "d8ad51e08bd754936d2285300e179d4fb92750e1714ce29c452ebb74a2669e40", /* ltn3.pgf */
    "2f5024dcef4009bdefe915840f21b1df46c6a4bd67d473649af99dc420b45160", /* ltn4.pgf */
    "c194fd0a25da1d85bad01b180bd70704e3958e485e82f1246699d93a75bac7fc", /* ltn5.pgf */
    "601f745eff6a0d8117ee90d39b2d7bfb38f02a6d0ea6d66746f9908d6687f046", /* ltn6.pgf */
    "7a39f9b2cc9cd4c57cd8324b280cf0c74e7e82ac36e4a88e3fdea256a3a0eecc", /* ltn7.pgf */
    "859935a6ff9cb264c5e4bdcedf41cde8c69c35629aa9dc15a5d8d74d5922d2e2", /* ltn8.pgf */
    "9ab574ca7ff8a4960678b546b7c55e4831816777d69d8078a8490fd21bc1f9b3", /* ltn9.pgf */
    "64e174205e27b9f6ec3d68dad93bb1f0e8c1bb6ecdaf1f33b675046f3889a67d", /* ltn10.pgf */
    "ee8df0e6e9927496126e0a72c3804c770f598f6429081d150002d94138f8c451", /* ltn11.pgf */
    "763e7ebbc4d2992fb6f7e5fca3968d43b7f16bc48e24c4367e9efa6d4bb412cf", /* ltn12.pgf */
    "730d7be0c7aee29fbd3f7da75cb28822cc7c7c50b3dba37efe9e1b7e1dea5eb5", /* ltn13.pgf */
    "cb527e38be7f633ecb52ca2e0c19cf5dfa85b9bf799d060f0de81beb4b9b9f0a", /* ltn14.pgf */
    "3b91c9d4175feaeeff2f9b7a427011c8456921e4b40ce1ac0e0797c809364362", /* ltn15.pgf */
    /* hrydgard/ppsspp@3540d5c5aaf7728800b14b21fb4193dbdc0c69d4 (2021-01-11), assets/flash0/font/ */
    "55d4454f5002c2aeb35b5845c3cfe320f6a2c42d376efde717794a1a65c2d8d5", /* jpn0.pgf */
    "b496d93c68194b086a66617936b800f21423f2b095a22f723fca7b47e0ba3b9c", /* ltn0.pgf */
    "e0208dc5f6f6d1c987f8b18eb1a9a51fb1c5625b4d282832f9e300222849da63", /* ltn1.pgf */
    "64d71054a2bf82e14343b357009339f4b10bca4f16a54b439d8cf15679986165", /* ltn2.pgf */
    "99c0b3ed0e720b69364377ae8168f137ed644781c57a8d5eebdb9124244224a3", /* ltn3.pgf */
    "2ea31544bd466b837c08b57758f4a5149c774c5f2a6f9474ede3672f66b1d4ae", /* ltn4.pgf */
    "bcb9ae29ca5b76b60dd755cd5ac197b7c7276cf6b13809c0a1b9561f757b3d24", /* ltn5.pgf */
    "3ca71b8d7108c68727dcd30f0a59d322ff94a3d0515ed40c6d554bedcfe82951", /* ltn6.pgf */
    "0b4e52dd08d7906f6129f1fd36a9d4f783f6f831011193ccbde2c16a7b6475f1", /* ltn7.pgf */
    "45e57df9359fc18cc6afb3dac9226f9e6fb44fd779dc1685606c5a7bff930716", /* ltn8.pgf */
    "236100fdc101f544f67f88a13ab99c7a109de16366c08ee42840803ad0ba3be3", /* ltn9.pgf */
    "7e9fdb33f0090762b074ec837a7c78c4eb2ec8721c28af9f4d8850b2b2b6fb99", /* ltn10.pgf */
    "efa2f7bb60ccebd6399caa48c988945f639351b9f563429667be6bce17c50c4d", /* ltn11.pgf */
    "5a8ba0844769b3385ca393f18d9c20ac626afed006663005714f27fae9853b7b", /* ltn12.pgf */
    "78fc49f8a6cb71d6582babe8a5dc24504ab02b111a6363b7c41e710a6fc8b254", /* ltn13.pgf */
    "e881bfce6405f017f6e189bb73b22c52654e90d0f37e692e89371de660a863c9", /* ltn14.pgf */
    "24d770ee71c3dfaa153955871d341e508a517f5ac0208626c13ae6e5635ae016", /* ltn15.pgf */
    /* hrydgard/ppsspp@dc34bea8d4dee77629da3cbaedc83fc8c05af159 (2021-01-19), assets/flash0/font/ */
    "6b0b3a8be9e709d6af02223d077675e1530f9879f42656377a9d8d42e2614333", /* kr0.pgf */
    "f91a8071bef137b43bebdae6ea72eb3fbf19a4ce5fa5a0860b20c5498b624b62", /* jpn0.pgf */
    "fe9ed5904dffacdeccfcb1f0a71ecdbbcda9eddf15f6e3e31f7397cf7a9b8a34", /* ltn0.pgf */
    "5e731e3815bea7264b4e71f476232821e9c70f2afc7a3d36d6d166700a29c658", /* ltn1.pgf */
    "750b03759e941747821942c24bdcd77104cfc444b587478afef1d3b6576ae2ec", /* ltn2.pgf */
    "1ca894be5f14557ec37688067d250c8e2aab1e739a623cb941a3a100e82a4148", /* ltn3.pgf */
    "d62b340de7e296376753a37c58740d6a2e0ce9f0526139455225e40e1e64cd9e", /* ltn4.pgf */
    "30ddcf60ebf0cb09f45bac4ad54c355198b758430811ca24bcc29d34f5c7ecee", /* ltn5.pgf */
    "abdd38713bd7265201099560b7f19ed3db219be1dee1844980f535129b0ab097", /* ltn6.pgf */
    "680fad898cf0e3ddd780f212f04dad5a195db63d4c7fc1ea95c01756f7887c84", /* ltn7.pgf */
    "519df2fc812cffa7128e5f97e88adffb3ea215f0cb6d272031e2637e084a92d7", /* ltn8.pgf */
    "32d9b38b1df172db43bc71d0bd647c916d3a43f9dea6a6b0f1ce74fd658473aa", /* ltn9.pgf */
    "f02240d67dcaacb02926beca7c64c50d29c0c9adb79c676ebdd3ffaebfa3b16b", /* ltn10.pgf */
    "a84d95ba545bdf8d000eb53b8ce792c84cedd3d4e335e88d7578d42be6d41644", /* ltn11.pgf */
    "02edcd49609c847cead4a0f89517709d5b55eb26e49bd72a873a763004bf2e02", /* ltn12.pgf */
    "3df05e0f06d39f94dee6054037aa35986246277752fcf1d7c193018b189e4e8b", /* ltn13.pgf */
    "fbf4442a023b1bc83a5a00086b16dfb402f364654ab809e1f6913da877cb1453", /* ltn14.pgf */
    "7e54faefa7fa575dcc8c5242b40b51f52db764b73ef65efdd559cff58ddb7c3e", /* ltn15.pgf */
    /* hrydgard/ppsspp@62047545f04cc1852908506695ff657a4cd9d50b (2021-01-20), assets/flash0/font/ */
    "0c9a7b89c178a98984ed3b8ea95f0f3edbae1d851a9ca79361ac859d5a7c784b", /* jpn0.pgf */
    /* hrydgard/ppsspp@f1ccccdc9a1d6dfef32597b4e7932059231be3b6 (2021-01-22), assets/flash0/font/ */
    "83f901f65a74c8a51de0e56b414e0bfe6d7861334d19bf57b4e0b6d5397a86ca", /* jpn0.pgf */
    /* hrydgard/ppsspp@8c4f11cb6afa6bdc81ae202ee5237801f2443a90 (2021-01-22), assets/flash0/font/ */
    "9dabcc129bfee943f0e0cad99cc5bcb7b8f6e6d0fc39531b45b7c7be57fa9f29", /* jpn0.pgf */
    /* hrydgard/ppsspp@6e2430fbdbb6716b0965d837330666f89a27be85 (2021-01-24), assets/flash0/font/ */
    "dc9f7408cb80f4b66f5a0b40dafe239122f06f71fc86c27dffea332ee26a8813", /* jpn0.pgf */
    /* hrydgard/ppsspp@5742f2f389f754313161a6f2ac523d9e16a33bd8 (2021-01-28), assets/flash0/font/ */
    "9f318d25a1c2a7e572985c511a6e3f982041cffd0faf8e6ff64da8a87a675a43", /* ltn0.pgf */
    "0cb72bdce33780b78596001fdc6842c244aac06e813fddf28e3437e4db45dd5b", /* ltn1.pgf */
    "7ba261c4c756ea62e831cff12fcbe67d56a1c2a15e45d48696d982dc914f4a5b", /* ltn2.pgf */
    "82bf8f0b60fd7e513560b29c52f6c721ade05910a144704c17f78aba298d9b08", /* ltn3.pgf */
    "203885da7342bb5d77072f5a3732a199046f37b3b2d26abbecb0b76aa03db1a0", /* ltn4.pgf */
    "e30b45662d37516c590ca070c831b44a6883587e223abd28082cb017623474f7", /* ltn5.pgf */
    "5294d702a2d1cadfbeb8b8a1e2d8f7704ad073104a67b938eb03e215e431a438", /* ltn6.pgf */
    "9a4d35eeeee93fe8b5434e8dff79dabf7bbd914ad0d6461fb60d36eb178bf2c8", /* ltn7.pgf */
    /* hrydgard/ppsspp@da40ed6e81b19b562fe9a58c19df5c039e59db0d (2021-01-30), assets/flash0/font/ */
    "c8620bdd244585bbf993a82903168f6ec8464d4cbcad238d4e0ca5623c1ba3c1", /* ltn8.pgf */
    "e250f2782e0c72707ce16f73331e2604daa84c488c8b9c959e2296867c1710bf", /* ltn9.pgf */
    "ff8fcdebca739eaa81c07caabd78b3e3eafb89cac0e68caf7aaf7a2b25514e85", /* ltn10.pgf */
    "ebb94efd7d18143d6c6d54ae7aa5cc801d8c56dc7ca613c779cc60dc8b640a16", /* ltn11.pgf */
    "c71f3dee57b8a105a88791dd041a008adb918b14a3892a1b18ed6a63f721201a", /* ltn12.pgf */
    "25b0efa79cf1b6656b35356483ad2b1191626550d59b5943ed681e82b945bd73", /* ltn13.pgf */
    "bc2c9389ac33f120b4be4fd3efc467de12fd886ec147969a4309ea270ccc1da3", /* ltn14.pgf */
    "4ab43137ebed97d8bbec829b32d4b343f9895e0b14ec1c884c1b44dfa35d5204", /* ltn15.pgf */
    /* hrydgard/ppsspp@42f28ce41ef9964d616cdc09379dd982e1e33943 (2021-02-06), assets/flash0/font/ */
    "ee27c33b91430335d50c5ae4de1eb5bba84893802ea38727d153d2a59a7e16d2", /* jpn0.pgf */
    /* hrydgard/ppsspp@41927a5c39951cb068805b86d3dcd74417e40845 (2021-02-15), assets/flash0/font/ */
    "a59fca642fb5d501e67ee57d95407415d7cd9e8fad2e2fadfe868d713912c628", /* jpn0.pgf */
    /* hrydgard/ppsspp@848142d2aca94eb9d7178ee28f17aa0c4ea1a94f (2021-02-23), assets/flash0/font/ */
    "fe976fafcf4078ca30b9ec0ffa1146030db4c890ec4b8bfe002127fbb1136c1a", /* jpn0.pgf */
    "9d31c490fc956621b1707562d99cecbdb4fd5995e81fe9bc2d47a7d277207040", /* ltn0.pgf */
    "ecfdfc8c7f1ee367dda81ff3177af626b5bf64014315ce0c389903c40964b4bf", /* ltn2.pgf */
    "5cf844980333075ecdab91c9ed0f9200a7343bbfea46b9205b02f97b99f73d4b", /* ltn4.pgf */
    "a0fb14123ceb1ef81cb230cbf0688de594d0725de234a2ad710cd9b89391c7e9", /* ltn6.pgf */
    /* hrydgard/ppsspp@a384b62247f1971555fb68f796c01df8fb8f984a (2021-03-14), assets/flash0/font/ */
    "1184f16bc89bad1fea49093806784470281af54f279b57ef0378ce33e6603870", /* ltn0.pgf */
    "250babd8a242d7eaaa84284c78d5e2273b29e61d0d8c01b65414e3248d7c1164", /* ltn4.pgf */
    /* hrydgard/ppsspp@09f90969ecf0c02271149062e6167fbc13cd4f26 (2021-06-18), assets/flash0/font/ */
    "75b0c9e8fc6aee7945c818e68315afacc61e479e37adebe32dae21cf0ed8d5a9", /* jpn0.pgf */
    /* hrydgard/ppsspp@082bf0d67cbdf7d359fa12c24ca4600819d00ad2 (2021-07-04), assets/flash0/font/ */
    "3a2c2a331ad2e84da8866aa37a305217cc47d884cc7a58a70ce4fce7f3e6e07a", /* jpn0.pgf */
    /* hrydgard/ppsspp@b51451d6720e2cabfba2709dc3c58941e987a3bb (2022-11-30), assets/flash0/font/ */
    "63267092a2079f386b56ac7bf99ca4334362e23fe0ae0d010833e866c22d7356", /* jpn0.pgf */
    /* hrydgard/ppsspp@eb65f586ec3203250dfd8ce63c536cb1bdb48a4b (2022-12-12), assets/flash0/font/ */
    "4051120c55c7e1cebeee62f823ebb37a71d79a2a8024b5a84cdb06594b9bf463", /* jpn0.pgf */
    /* hrydgard/ppsspp@e4aa9e517f3cee51081b474906bc3f0c53bd31de (2023-01-29), assets/flash0/font/ */
    "d363cec75fecfafd1511e370e48c748c30199c694b83a4026be34d799b192917", /* jpn0.pgf */
    /* jpcsp/jpcsp@8da2d2271d2b68e89abf3ee8b4bee444e1d5dafb (2011-12-25), flash0/font/ */
    "6bfe027829042be4188378eb49408c5eb59c8ad19bff0128da1ef1f6f56e8977", /* kr0.pgf */
    "2e86bcaff9832579c9fc1e260edeb70370927f36eeb710aa73c9839df9dbb505", /* jpn0.pgf */
    /* jpcsp/jpcsp@ed2de4afb7e449b0bf0ebbc6da3e1dd9d6f473a3 (2014-10-26), flash0/font/ */
    "90ba7e28b905ac5604511270ad10043253f0c6cf998950499e2b72fc81d465ae", /* jpn0.pgf */
    /* jpcsp/jpcsp@506827cfca8b745fae5ade39da8961446cb8a84e (2020-11-16), flash0/font/ */
    "5fc7dfe74fc519b63954e03bba2ee122808420ad131ddff1d1557ee3bcda915e", /* jpn0.pgf */
};

size_t nk_font_known_replacement_count(void) {
    return sizeof(kKnownReplacementSha256) / sizeof(kKnownReplacementSha256[0]);
}

bool nk_font_known_replacement_sha256(const char *sha256_hex) {
    if (!sha256_hex) return false;
    for (size_t i = 0; i < nk_font_known_replacement_count(); i++) {
        if (strcmp(kKnownReplacementSha256[i], sha256_hex) == 0) return true;
    }
    return false;
}

bool nk_font_is_replacement(const uint8_t *bytes, size_t length, const char *sha256_hex) {
    const size_t prefix = sizeof(kProjectStandInNamePrefix) - 1u;
    if (bytes && prefix <= NK_FONT_NAME_SIZE && length >= NK_FONT_NAME_OFFSET + prefix &&
        memcmp(bytes + NK_FONT_NAME_OFFSET, kProjectStandInNamePrefix, prefix) == 0) {
        return true;
    }
    return nk_font_known_replacement_sha256(sha256_hex);
}

int nk_font_classify_verdict(const PgfVerdict *verdict, char *reason, size_t reason_len) {
    if (!verdict || verdict->refusal != PGF_REFUSE_NONE) {
        font_copy_text(reason, reason_len, "the reader refused the file");
        return -1;
    }
    if (verdict->has_hangul && verdict->has_kana) {
        font_copy_text(reason, reason_len, "covers both Hangul and kana, so it names no slot");
        return -1;
    }
    if (verdict->has_hangul) return NK_FONT_SLOT_KOREAN;
    if (verdict->has_kana) return NK_FONT_SLOT_JAPANESE;
    if (verdict->has_latin) return NK_FONT_SLOT_LATIN;
    font_copy_text(reason, reason_len, "covers none of the slot probe characters");
    return -1;
}

/* Read a whole open file no larger than the reader ceiling, from its start, and leave the
 * handle at its start again. The caller frees *out. */
static bool font_read_stream(FILE *f, uint8_t **out, size_t *out_len,
                             char *out_error, size_t error_len) {
    int64_t size;
    uint8_t *bytes;
    size_t received;
    *out = NULL;
    *out_len = 0;
    if (fseek(f, 0, SEEK_END) != 0 || (size = nk_ftell64(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        font_copy_text(out_error, error_len, "cannot read the file size");
        return false;
    }
    if ((uint64_t)size > NK_FONT_PGF_MAX_BYTES) {
        font_copy_text(out_error, error_len, "larger than the 16 MiB reader ceiling");
        return false;
    }
    bytes = (uint8_t *)malloc(size > 0 ? (size_t)size : 1u);
    if (!bytes) {
        font_copy_text(out_error, error_len, "out of memory");
        return false;
    }
    received = size > 0 ? fread(bytes, 1u, (size_t)size, f) : 0u;
    if (received != (size_t)size || ferror(f) || fseek(f, 0, SEEK_SET) != 0) {
        free(bytes);
        font_copy_text(out_error, error_len, "cannot read the file");
        return false;
    }
    *out = bytes;
    *out_len = (size_t)size;
    return true;
}

/* Read a whole file no larger than the reader ceiling. The caller frees *out. */
static bool font_read_file(const char *path, uint8_t **out, size_t *out_len,
                           char *out_error, size_t error_len) {
    FILE *f = nk_fopen_utf8(path, "rb");
    bool ok;
    *out = NULL;
    *out_len = 0;
    if (!f) {
        font_copy_text(out_error, error_len, "cannot open the file");
        return false;
    }
    ok = font_read_stream(f, out, out_len, out_error, error_len);
    fclose(f);
    return ok;
}

/* Check bytes already in memory. Returns true when the reader accepts them. */
static bool font_inspect_bytes(const uint8_t *bytes, size_t length, NkFontInspection *out) {
    memset(out, 0, sizeof(*out));
    out->size = (uint64_t)length;
    out->slot = -1;
    font_sha256_hex(bytes, length, out->sha256);
    out->replacement = nk_font_is_replacement(bytes, length, out->sha256);
    if (!pgf_validate_memory(bytes, length, &out->verdict)) {
        font_copy_text(out->slot_reason, sizeof(out->slot_reason),
                       font_refusal_text(out->verdict.refusal));
        return false;
    }
    out->slot = nk_font_classify_verdict(&out->verdict, out->slot_reason, sizeof(out->slot_reason));
    return true;
}

bool nk_font_inspect_pgf(const char *file_path, NkFontInspection *out,
                         char *out_error, size_t error_len) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    bool accepted;
    if (!file_path || !*file_path || !out) {
        font_copy_text(out_error, error_len, "no font file was named");
        return false;
    }
    if (!font_read_file(file_path, &bytes, &length, out_error, error_len)) return false;
    accepted = font_inspect_bytes(bytes, length, out);
    free(bytes);
    if (!accepted) {
        char text[NK_FONT_DETAIL_MAX * 2];
        snprintf(text, sizeof(text), "the PGF reader refused the file: %s", out->slot_reason);
        font_copy_text(out_error, error_len, text);
        return false;
    }
    return true;
}

bool nk_font_validate_pgf(const char *file_path, uint64_t *out_size,
                          char *out_sha256, char *out_error, size_t error_len) {
    NkFontInspection inspection;
    if (!nk_font_inspect_pgf(file_path, &inspection, out_error, error_len)) return false;
    if (out_size) *out_size = inspection.size;
    if (out_sha256) memcpy(out_sha256, inspection.sha256, sizeof(inspection.sha256));
    return true;
}

/* -----------------------------------------------------------------------------
 * The schema-2 imported-font manifest keeps generic slots in `files` and indexed Latin files
 * in the optional `latin_indices` object under <user data>/fonts/v2/manifest.json.
 * -------------------------------------------------------------------------- */
typedef struct {
    bool present[NK_FONT_SLOT_COUNT];
    uint64_t size[NK_FONT_SLOT_COUNT];
    char sha256[NK_FONT_SLOT_COUNT][65];
    bool latin_index_present[NK_FONT_LATIN_INDEX_COUNT];
    uint64_t latin_index_size[NK_FONT_LATIN_INDEX_COUNT];
    char latin_index_sha256[NK_FONT_LATIN_INDEX_COUNT][65];
} FontManifest;

bool nk_font_get_cache_dir(const char *user_data_root, char *out_dir, size_t max_len) {
    if (!out_dir || max_len == 0) return false;
    char base[NK_MAX_PATH];
    if (user_data_root && *user_data_root) {
        snprintf(base, sizeof(base), "%s", user_data_root);
    } else {
        if (!nk_platform_get_path(NK_PATH_DATA, base, sizeof(base))) return false;
    }
    char sep = nk_platform_path_separator();
    int written = snprintf(out_dir, max_len, "%s%c%s%c%s", base, sep,
                           NK_FONT_CACHE_PARENT, sep, NK_FONT_CACHE_SUBDIR);
    return written > 0 && (size_t)written < max_len;
}

static void font_join(char *out, size_t out_len, const char *dir, const char *name) {
    (void)nk_shared_path_join(out, out_len, dir, name, nk_platform_path_separator(),
                              NK_SHARED_PATH_JOIN_ALWAYS, 0u);
}

/* The staging name for a target path: the target with ".tmp" appended. */
static void font_staging_name(char *out, size_t out_len, const char *target) {
    size_t length = strlen(target);
    if (out_len == 0) return;
    if (length + 5u > out_len) {
        out[0] = '\0';
        return;
    }
    memcpy(out, target, length);
    memcpy(out + length, ".tmp", 4u);
    out[length + 4u] = '\0';
}

static NkFontStatus font_load_manifest(const char *cache_dir, FontManifest *manifest,
                                       char *message, size_t message_len) {
    char path[NK_MAX_PATH * 2];
    FILE *mf;
    long length;
    char *json_buf;
    size_t read_bytes;
    char parse_err[128] = "";
    NkJsonNode *root;
    NkJsonNode *version;
    NkJsonNode *files;
    int64_t schema = 0;
    size_t count;

    memset(manifest, 0, sizeof(*manifest));
    font_join(path, sizeof(path), cache_dir, NK_FONT_MANIFEST_NAME);
    if (!nk_platform_file_exists(path)) {
        font_copy_text(message, message_len, "No imported PSP font.");
        return NK_FONT_STATUS_MISSING;
    }
    mf = nk_fopen_utf8(path, "rb");
    if (!mf || fseek(mf, 0, SEEK_END) != 0 || (length = ftell(mf)) <= 0 || length > 1024 * 1024 ||
        fseek(mf, 0, SEEK_SET) != 0) {
        if (mf) fclose(mf);
        font_copy_text(message, message_len, "PSP font manifest is unreadable; run fonts import <folder>.");
        return NK_FONT_STATUS_INVALID;
    }
    json_buf = (char *)malloc((size_t)length + 1u);
    if (!json_buf) {
        fclose(mf);
        return NK_FONT_STATUS_INVALID;
    }
    read_bytes = fread(json_buf, 1u, (size_t)length, mf);
    fclose(mf);
    json_buf[read_bytes] = '\0';
    root = nk_json_parse(json_buf, read_bytes, parse_err, sizeof(parse_err));
    free(json_buf);
    if (!root || !nk_json_is_object(root)) {
        if (root) nk_json_free(root);
        font_copy_text(message, message_len, "PSP font manifest is malformed; run fonts import <folder>.");
        return NK_FONT_STATUS_INVALID;
    }
    version = nk_json_obj_get(root, "schema_version");
    if (!version || !nk_json_get_int64(version, &schema) || schema != NK_FONT_CACHE_SCHEMA_VERSION) {
        nk_json_free(root);
        font_copy_text(message, message_len, "PSP font manifest schema is not version 2; run fonts import <folder>.");
        return NK_FONT_STATUS_INVALID;
    }
    files = nk_json_obj_get(root, "files");
    if (!files || !nk_json_is_object(files)) {
        nk_json_free(root);
        font_copy_text(message, message_len, "PSP font manifest lists no files; run fonts import <folder>.");
        return NK_FONT_STATUS_INVALID;
    }
    for (count = 0; count < files->u.obj.count; count++) {
        const char *name = files->u.obj.members[count].key;
        NkJsonNode *entry = files->u.obj.members[count].val;
        NkJsonNode *slot_node;
        NkJsonNode *size_node;
        NkJsonNode *sha_node;
        NkJsonNode *source_node;
        const char *slot_text;
        const char *sha_text;
        const char *source_text;
        int64_t size = 0;
        int slot = -1;
        int i;
        bool entry_ok = false;
        if (!name || !entry || !nk_json_is_object(entry)) break;
        for (i = 0; i < NK_FONT_SLOT_COUNT; i++) {
            if (strcmp(name, kSlotFile[i]) == 0) slot = i;
        }
        if (slot < 0 || manifest->present[slot]) break;
        slot_node = nk_json_obj_get(entry, "slot");
        size_node = nk_json_obj_get(entry, "size");
        sha_node = nk_json_obj_get(entry, "sha256");
        source_node = nk_json_obj_get(entry, "source");
        slot_text = nk_json_get_string(slot_node);
        sha_text = nk_json_get_string(sha_node);
        source_text = nk_json_get_string(source_node);
        entry_ok = slot_text && strcmp(slot_text, kSlotId[slot]) == 0 &&
                   size_node && nk_json_get_int64(size_node, &size) && size > 0 &&
                   sha_text && strlen(sha_text) == 64u &&
                   source_text && strcmp(source_text, NK_FONT_MANIFEST_SOURCE_USER) == 0;
        if (!entry_ok) break;
        manifest->present[slot] = true;
        manifest->size[slot] = (uint64_t)size;
        snprintf(manifest->sha256[slot], sizeof(manifest->sha256[slot]), "%s", sha_text);
    }
    if (count < files->u.obj.count) {
        nk_json_free(root);
        font_copy_text(message, message_len,
                       "PSP font manifest has an entry this build does not accept; run fonts import <folder>.");
        return NK_FONT_STATUS_INVALID;
    }
    {
        NkJsonNode *latin_indices = nk_json_obj_get(root, "latin_indices");
        if (latin_indices) {
            if (!nk_json_is_object(latin_indices)) {
                nk_json_free(root);
                font_copy_text(message, message_len,
                               "PSP font manifest has invalid Latin index entries; run fonts import <folder>.");
                return NK_FONT_STATUS_INVALID;
            }
            for (count = 0; count < latin_indices->u.obj.count; count++) {
                const char *name = latin_indices->u.obj.members[count].key;
                NkJsonNode *entry = latin_indices->u.obj.members[count].val;
                NkJsonNode *slot_node;
                NkJsonNode *index_node;
                NkJsonNode *size_node;
                NkJsonNode *sha_node;
                NkJsonNode *source_node;
                const char *slot_text;
                const char *sha_text;
                const char *source_text;
                int64_t index_value = -1;
                int64_t size = 0;
                int index = nk_font_latin_index_from_filename(name);
                bool entry_ok;
                if (index < 0 || strcmp(name, kLatinIndexFile[index]) != 0 ||
                    !entry || !nk_json_is_object(entry)) break;
                slot_node = nk_json_obj_get(entry, "slot");
                index_node = nk_json_obj_get(entry, "index");
                size_node = nk_json_obj_get(entry, "size");
                sha_node = nk_json_obj_get(entry, "sha256");
                source_node = nk_json_obj_get(entry, "source");
                slot_text = nk_json_get_string(slot_node);
                sha_text = nk_json_get_string(sha_node);
                source_text = nk_json_get_string(source_node);
                entry_ok = !manifest->latin_index_present[index] && slot_text &&
                           strcmp(slot_text, "latin") == 0 && index_node &&
                           nk_json_get_int64(index_node, &index_value) && index_value == index &&
                           size_node && nk_json_get_int64(size_node, &size) && size > 0 &&
                           sha_text && strlen(sha_text) == 64u && source_text &&
                           strcmp(source_text, NK_FONT_MANIFEST_SOURCE_USER) == 0;
                if (!entry_ok) break;
                manifest->latin_index_present[index] = true;
                manifest->latin_index_size[index] = (uint64_t)size;
                snprintf(manifest->latin_index_sha256[index],
                         sizeof(manifest->latin_index_sha256[index]), "%s", sha_text);
            }
            if (count < latin_indices->u.obj.count) {
                nk_json_free(root);
                font_copy_text(message, message_len,
                               "PSP font manifest has a Latin index entry this build does not accept; run fonts import <folder>.");
                return NK_FONT_STATUS_INVALID;
            }
        }
    }
    {
        bool has_entry = files->u.obj.count != 0u;
        for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++)
            if (manifest->latin_index_present[index]) has_entry = true;
        if (!has_entry) {
            nk_json_free(root);
            font_copy_text(message, message_len,
                           "PSP font manifest lists no files; run fonts import <folder>.");
            return NK_FONT_STATUS_INVALID;
        }
    }
    nk_json_free(root);
    font_copy_text(message, message_len, "Imported PSP fonts are listed in the manifest.");
    return NK_FONT_STATUS_OK;
}

/* Check one slot's bytes against its manifest entry: the reader accepts them, they name the slot,
 * and their size and SHA-256 match the entry. On success *replacement (when given) says whether
 * they are a replacement font. */
static bool font_check_slot_bytes(int slot, const FontManifest *manifest, const uint8_t *bytes,
                                  size_t length, char *reason, size_t reason_len,
                                  bool *replacement) {
    NkFontInspection inspection;
    if (!font_inspect_bytes(bytes, length, &inspection)) {
        snprintf(reason, reason_len, "the reader refused it: %s", inspection.slot_reason);
        return false;
    }
    if (inspection.slot != slot) {
        snprintf(reason, reason_len, "it does not name the %s slot", kSlotRole[slot]);
        return false;
    }
    if (inspection.size != manifest->size[slot] || strcmp(inspection.sha256, manifest->sha256[slot]) != 0) {
        font_copy_text(reason, reason_len, "its size or SHA-256 differs from the manifest");
        return false;
    }
    if (replacement) *replacement = inspection.replacement;
    return true;
}

/* Check one manifest entry against its file: the file exists, its size and SHA-256 match,
 * the reader accepts it, and it names the slot the manifest gives. */
static bool font_check_slot_file(const char *cache_dir, int slot, const FontManifest *manifest,
                                 char *reason, size_t reason_len, bool *replacement) {
    char path[NK_MAX_PATH * 2];
    uint8_t *bytes = NULL;
    size_t length = 0;
    bool ok;
    font_join(path, sizeof(path), cache_dir, kSlotFile[slot]);
    if (!font_read_file(path, &bytes, &length, reason, reason_len)) return false;
    ok = font_check_slot_bytes(slot, manifest, bytes, length, reason, reason_len, replacement);
    free(bytes);
    return ok;
}

static bool font_check_latin_index_bytes(int index, const FontManifest *manifest,
                                        const uint8_t *bytes, size_t length,
                                        char *reason, size_t reason_len, bool *replacement) {
    NkFontInspection inspection;
    if (!font_inspect_bytes(bytes, length, &inspection)) {
        snprintf(reason, reason_len, "the reader refused it: %s", inspection.slot_reason);
        return false;
    }
    if (inspection.slot != NK_FONT_SLOT_LATIN) {
        font_copy_text(reason, reason_len, "it does not name the Latin slot");
        return false;
    }
    if (inspection.size != manifest->latin_index_size[index] ||
        strcmp(inspection.sha256, manifest->latin_index_sha256[index]) != 0) {
        font_copy_text(reason, reason_len, "its size or SHA-256 differs from the manifest");
        return false;
    }
    if (replacement) *replacement = inspection.replacement;
    return true;
}

static bool font_check_latin_index_file(const char *cache_dir, int index,
                                        const FontManifest *manifest,
                                        char *reason, size_t reason_len, bool *replacement) {
    char path[NK_MAX_PATH * 2];
    uint8_t *bytes = NULL;
    size_t length = 0;
    bool ok;
    const char *name = nk_font_latin_index_filename(index);
    if (!name) {
        font_copy_text(reason, reason_len, "the Latin index is outside the supported range");
        return false;
    }
    font_join(path, sizeof(path), cache_dir, name);
    if (!font_read_file(path, &bytes, &length, reason, reason_len)) return false;
    ok = font_check_latin_index_bytes(index, manifest, bytes, length, reason, reason_len, replacement);
    free(bytes);
    return ok;
}

NkFontStatus nk_font_check_cache(const char *user_data_root, char *out_message, size_t message_max_len) {
    char cache_dir[NK_MAX_PATH];
    FontManifest manifest;
    NkFontStatus status;
    char message[NK_FONT_TEXT_MAX] = "";
    if (!nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        font_copy_text(out_message, message_max_len, "PSP font cache location is unavailable.");
        return NK_FONT_STATUS_MISSING;
    }
    status = font_load_manifest(cache_dir, &manifest, message, sizeof(message));
    if (status == NK_FONT_STATUS_OK) {
        for (int slot = 0; slot < NK_FONT_SLOT_COUNT && status == NK_FONT_STATUS_OK; slot++) {
            char reason[NK_FONT_TEXT_MAX] = "";
            if (!manifest.present[slot]) continue;
            if (!font_check_slot_file(cache_dir, slot, &manifest, reason, sizeof(reason), NULL)) {
                snprintf(message, sizeof(message), "Imported %s font is invalid: %.400s; run fonts import <folder>.",
                         kSlotRole[slot], reason);
                status = NK_FONT_STATUS_INVALID;
            }
        }
        for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT && status == NK_FONT_STATUS_OK; index++) {
            char reason[NK_FONT_TEXT_MAX] = "";
            if (!manifest.latin_index_present[index]) continue;
            if (!font_check_latin_index_file(cache_dir, index, &manifest, reason, sizeof(reason), NULL)) {
                snprintf(message, sizeof(message), "Imported Latin index %d font is invalid: %.400s; run fonts import <folder>.",
                         index + 1, reason);
                status = NK_FONT_STATUS_INVALID;
            }
        }
    }
    font_copy_text(out_message, message_max_len, message);
    return status;
}

NkFontSlotCheck nk_font_check_cache_slot_file(const char *user_data_root, NkFontSlot slot,
                                              FILE *cache_file, char *reason, size_t reason_len,
                                              bool *replacement) {
    char cache_dir[NK_MAX_PATH];
    FontManifest manifest;
    NkFontStatus status;
    char message[NK_FONT_TEXT_MAX] = "";
    uint8_t *bytes = NULL;
    size_t length = 0;
    bool ok;
    if (replacement) *replacement = false;
    if (reason_len > 0) reason[0] = '\0';
    if (!cache_file || (unsigned)slot >= NK_FONT_SLOT_COUNT) {
        font_copy_text(reason, reason_len, "no cache file or font slot was named");
        return NK_FONT_SLOT_CHECK_FILE_INVALID;
    }
    if (!nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        font_copy_text(reason, reason_len, "the per-user font cache location is unavailable");
        return NK_FONT_SLOT_CHECK_NO_MANIFEST;
    }
    status = font_load_manifest(cache_dir, &manifest, message, sizeof(message));
    if (status == NK_FONT_STATUS_MISSING) {
        font_copy_text(reason, reason_len,
                       "the font cache has no manifest.json (import the font from your PSP to write one)");
        return NK_FONT_SLOT_CHECK_NO_MANIFEST;
    }
    if (status != NK_FONT_STATUS_OK) {
        font_copy_text(reason, reason_len, message);
        return NK_FONT_SLOT_CHECK_MANIFEST_INVALID;
    }
    if (!manifest.present[slot]) {
        snprintf(reason, reason_len, "the manifest does not list the %s slot", kSlotRole[slot]);
        return NK_FONT_SLOT_CHECK_NOT_LISTED;
    }
    if (!font_read_stream(cache_file, &bytes, &length, reason, reason_len)) return NK_FONT_SLOT_CHECK_FILE_INVALID;
    ok = font_check_slot_bytes((int)slot, &manifest, bytes, length, reason, reason_len,
                               replacement);
    free(bytes);
    return ok ? NK_FONT_SLOT_CHECK_OK : NK_FONT_SLOT_CHECK_FILE_INVALID;
}

NkFontSlotCheck nk_font_check_cache_latin_index_file(const char *user_data_root, int index,
                                                      FILE *cache_file, char *reason,
                                                      size_t reason_len) {
    char cache_dir[NK_MAX_PATH];
    FontManifest manifest;
    NkFontStatus status;
    char message[NK_FONT_TEXT_MAX] = "";
    uint8_t *bytes = NULL;
    size_t length = 0;
    bool ok;
    if (reason_len > 0) reason[0] = '\0';
    if ((unsigned)index >= NK_FONT_LATIN_INDEX_COUNT || !cache_file) {
        font_copy_text(reason, reason_len, "no cache file or Latin index was named");
        return NK_FONT_SLOT_CHECK_FILE_INVALID;
    }
    if (!nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        font_copy_text(reason, reason_len, "the per-user font cache location is unavailable");
        return NK_FONT_SLOT_CHECK_NO_MANIFEST;
    }
    status = font_load_manifest(cache_dir, &manifest, message, sizeof(message));
    if (status == NK_FONT_STATUS_MISSING) {
        font_copy_text(reason, reason_len,
                       "the font cache has no manifest.json (import the font from your PSP to write one)");
        return NK_FONT_SLOT_CHECK_NO_MANIFEST;
    }
    if (status != NK_FONT_STATUS_OK) {
        font_copy_text(reason, reason_len, message);
        return NK_FONT_SLOT_CHECK_MANIFEST_INVALID;
    }
    if (!manifest.latin_index_present[index]) {
        snprintf(reason, reason_len, "the manifest does not list Latin index %d", index + 1);
        return NK_FONT_SLOT_CHECK_NOT_LISTED;
    }
    if (!font_read_stream(cache_file, &bytes, &length, reason, reason_len))
        return NK_FONT_SLOT_CHECK_FILE_INVALID;
    ok = font_check_latin_index_bytes(index, &manifest, bytes, length, reason, reason_len, NULL);
    free(bytes);
    return ok ? NK_FONT_SLOT_CHECK_OK : NK_FONT_SLOT_CHECK_FILE_INVALID;
}

void nk_font_slot_states(const char *user_data_root, const char *project_root,
                         NkFontSlotState states[NK_FONT_SLOT_COUNT]) {
    char cache_dir[NK_MAX_PATH];
    FontManifest manifest;
    char manifest_message[NK_FONT_TEXT_MAX] = "";
    NkFontStatus manifest_status = NK_FONT_STATUS_MISSING;
    if (!states) return;
    memset(&manifest, 0, sizeof(manifest));
    if (nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        manifest_status = font_load_manifest(cache_dir, &manifest, manifest_message, sizeof(manifest_message));
    }
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        NkFontSlotState *state = &states[slot];
        char reason[NK_FONT_TEXT_MAX] = "";
        memset(state, 0, sizeof(*state));
        state->source = NK_FONT_SOURCE_NONE;
        if (manifest_status == NK_FONT_STATUS_OK && manifest.present[slot]) {
            bool replacement = false;
            if (font_check_slot_file(cache_dir, slot, &manifest, reason, sizeof(reason), &replacement)) {
                state->source = NK_FONT_SOURCE_USER;
                state->replacement = replacement;
                if (replacement) {
                    /* Short enough for one line of Settings > PSP fonts; the count above it
                       says it is not the PSP's own. */
                    snprintf(state->detail, sizeof(state->detail), "%s: a replacement font.",
                             kSlotRole[slot]);
                } else {
                    snprintf(state->detail, sizeof(state->detail), "%s: your imported font.",
                             kSlotRole[slot]);
                }
                continue;
            }
        } else if (manifest_status == NK_FONT_STATUS_INVALID) {
            font_copy_text(reason, sizeof(reason), manifest_message);
        }
        if (project_root && *project_root) {
            char project_dir[NK_MAX_PATH * 2];
            char project_path[NK_MAX_PATH * 2];
            NkFontInspection inspection;
            char error[NK_FONT_DETAIL_MAX] = "";
            snprintf(project_dir, sizeof(project_dir), "%s%c%s", project_root,
                     nk_platform_path_separator(), NK_FONT_PROJECT_SUBDIR);
            font_join(project_path, sizeof(project_path), project_dir, kSlotFile[slot]);
            if (nk_platform_file_exists(project_path) &&
                nk_font_inspect_pgf(project_path, &inspection, error, sizeof(error)) &&
                inspection.slot == slot) {
                state->source = NK_FONT_SOURCE_PROJECT;
                snprintf(state->detail, sizeof(state->detail), "%s: project font.", kSlotRole[slot]);
                continue;
            }
        }
        if (reason[0]) {
            snprintf(state->detail, sizeof(state->detail), "%s: missing (%.400s).", kSlotRole[slot], reason);
        } else {
            snprintf(state->detail, sizeof(state->detail),
                     "%s: missing. Import a font from your PSP or install the project font.", kSlotRole[slot]);
        }
    }
}

bool nk_font_resolve_latin_index(const char *user_data_root, const char *project_font_dir,
                                 int index, NkFontLatinResolution *out) {
    char cache_dir[NK_MAX_PATH];
    char manifest_message[NK_FONT_TEXT_MAX] = "";
    FontManifest manifest;
    NkFontStatus manifest_status = NK_FONT_STATUS_MISSING;
    if (!out || (unsigned)index >= NK_FONT_LATIN_INDEX_COUNT) return false;
    out->source = NK_FONT_SOURCE_NONE;
    out->served_index = -3;
    out->project_file = NULL;
    out->project_style_index = -1;
    out->replacement = false;
    memset(&manifest, 0, sizeof(manifest));
    if (nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir)))
        manifest_status = font_load_manifest(cache_dir, &manifest, manifest_message,
                                             sizeof(manifest_message));
    if (manifest_status == NK_FONT_STATUS_OK) {
        char reason[NK_FONT_TEXT_MAX] = "";
        int has_latin_indices = 0;
        if (manifest.latin_index_present[index] &&
            font_check_latin_index_file(cache_dir, index, &manifest, reason, sizeof(reason),
                                        &out->replacement)) {
            out->source = NK_FONT_SOURCE_USER;
            out->served_index = index;
            return true;
        }
        int group_start = (index / NK_FONT_LATIN_SIZE_GROUP_COUNT) * NK_FONT_LATIN_SIZE_GROUP_COUNT;
        int group_end = group_start + NK_FONT_LATIN_SIZE_GROUP_COUNT;
        for (int distance = 1; distance < NK_FONT_LATIN_SIZE_GROUP_COUNT; distance++) {
            int candidates[2] = { index - distance, index + distance };
            for (int side = 0; side < 2; side++) {
                int candidate = candidates[side];
                if (candidate < group_start || candidate >= group_end ||
                    !manifest.latin_index_present[candidate]) continue;
                reason[0] = '\0';
                if (!font_check_latin_index_file(cache_dir, candidate, &manifest,
                                                 reason, sizeof(reason), &out->replacement)) continue;
                out->source = NK_FONT_SOURCE_USER;
                out->served_index = candidate;
                return true;
            }
        }
        for (int candidate = 0; candidate < NK_FONT_LATIN_INDEX_COUNT; candidate++)
            has_latin_indices |= manifest.latin_index_present[candidate];
        /* A cache created before per-index imports contains only nkltn.pgf. Keep serving that
         * face for all indices even when the project font is now available. */
        if (!has_latin_indices && manifest.present[NK_FONT_SLOT_LATIN] &&
            font_check_slot_file(cache_dir, NK_FONT_SLOT_LATIN, &manifest,
                                 reason, sizeof(reason), &out->replacement)) {
            out->source = NK_FONT_SOURCE_USER;
            out->served_index = -2;
            return true;
        }
    }

    if (project_font_dir && *project_font_dir) {
        /* The index's own style first, then the regular face of its size, then the larger size's
         * regular face (an older project folder has only that one). */
        const int group_regular = index >= NK_FONT_LATIN_SIZE_GROUP_COUNT ? NK_FONT_LATIN_SIZE_GROUP_COUNT : 0;
        const char *const candidates[3] = {
            kProjectLatinIndexFile[index], kProjectLatinIndexFile[group_regular], NK_FONT_SLOT_LATIN_FILE
        };
        const int candidate_style[3] = { index, group_regular, 0 };
        for (int candidate = 0; candidate < 3; candidate++) {
            char project_path[NK_MAX_PATH * 2];
            NkFontInspection inspection;
            char error[NK_FONT_DETAIL_MAX] = "";
            font_join(project_path, sizeof(project_path), project_font_dir, candidates[candidate]);
            if (nk_platform_file_exists(project_path) &&
                nk_font_inspect_pgf(project_path, &inspection, error, sizeof(error)) &&
                inspection.slot == NK_FONT_SLOT_LATIN) {
                out->source = NK_FONT_SOURCE_PROJECT;
                out->served_index = -1;
                out->project_file = candidates[candidate];
                out->project_style_index = candidate_style[candidate];
                return true;
            }
        }
    }

    /* Keep a legacy Latin import as the last fallback for mixed/newer caches when the project
     * Latin PGF is unavailable. Its lack of an index is explicit in the resolution and status. */
    if (manifest_status == NK_FONT_STATUS_OK && manifest.present[NK_FONT_SLOT_LATIN]) {
        char reason[NK_FONT_TEXT_MAX] = "";
        if (font_check_slot_file(cache_dir, NK_FONT_SLOT_LATIN, &manifest,
                                 reason, sizeof(reason), &out->replacement)) {
            out->source = NK_FONT_SOURCE_USER;
            out->served_index = -2;
            return true;
        }
    }
    return false;
}

void nk_font_latin_index_states(const char *user_data_root, const char *project_font_dir,
                                NkFontLatinIndexState states[NK_FONT_LATIN_INDEX_COUNT]) {
    if (!states) return;
    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) {
        NkFontLatinResolution resolution;
        const char *requested = nk_font_latin_index_filename(index);
        bool resolved = nk_font_resolve_latin_index(user_data_root, project_font_dir, index, &resolution);
        states[index].source = resolved ? resolution.source : NK_FONT_SOURCE_NONE;
        states[index].served_index = resolved ? resolution.served_index : -3;
        states[index].replacement = resolved && resolution.source == NK_FONT_SOURCE_USER &&
                                    resolution.replacement;
        if (!resolved) {
            snprintf(states[index].detail, sizeof(states[index].detail),
                     "L%d missing %s", index + 1, requested);
        } else if (resolution.source == NK_FONT_SOURCE_USER && resolution.served_index == index) {
            /* An imported replacement font serves the index: it is not the user's own. */
            snprintf(states[index].detail, sizeof(states[index].detail),
                     "L%d %s %s", index + 1, states[index].replacement ? "replacement" : "exact",
                     requested);
        } else if (resolution.source == NK_FONT_SOURCE_USER && resolution.served_index >= 0) {
            snprintf(states[index].detail, sizeof(states[index].detail),
                     "L%d missing; fallback %s", index + 1,
                     nk_font_latin_index_filename(resolution.served_index));
        } else if (resolution.source == NK_FONT_SOURCE_PROJECT) {
            snprintf(states[index].detail, sizeof(states[index].detail),
                     "L%d missing; project %s", index + 1, resolution.project_file);
        } else {
            snprintf(states[index].detail, sizeof(states[index].detail),
                     "L%d missing; legacy nkltn.pgf", index + 1);
        }
    }
}

/* -----------------------------------------------------------------------------
 * Writing: file replacement and the manifest
 * -------------------------------------------------------------------------- */
static bool font_write_bytes(const char *path, const uint8_t *bytes, size_t length) {
    FILE *f = nk_fopen_utf8(path, "wb");
    bool ok;
    if (!f) return false;
    ok = length == 0u || fwrite(bytes, 1u, length, f) == length;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* Write the manifest for the slots marked present. An empty manifest is not written. */
static bool font_write_manifest(const char *cache_dir, const FontManifest *manifest) {
    char path[NK_MAX_PATH * 2];
    char tmp[NK_MAX_PATH * 2];
    char stamp[32] = "unknown";
    char text[8192];
    size_t used = 0;
    bool any = false;
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    font_join(path, sizeof(path), cache_dir, NK_FONT_MANIFEST_NAME);
    font_staging_name(tmp, sizeof(tmp), path);
    if (utc) strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", utc);
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) if (manifest->present[slot]) any = true;
    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++)
        if (manifest->latin_index_present[index]) any = true;
    if (!any) return false;
    used += (size_t)snprintf(text + used, sizeof(text) - used,
                             "{\n  \"schema_version\": %d,\n  \"import_time\": \"%s\",\n  \"files\": {\n",
                             NK_FONT_CACHE_SCHEMA_VERSION, stamp);
    any = false;
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        if (!manifest->present[slot]) continue;
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "%s    \"%s\": {\"slot\": \"%s\", \"size\": %llu, \"sha256\": \"%s\", "
                                 "\"source\": \"%s\", \"reader_version\": %d}",
                                 any ? ",\n" : "", kSlotFile[slot], kSlotId[slot],
                                 (unsigned long long)manifest->size[slot], manifest->sha256[slot],
                                 NK_FONT_MANIFEST_SOURCE_USER, NK_FONT_READER_VERSION);
        any = true;
    }
    used += (size_t)snprintf(text + used, sizeof(text) - used, "\n  },\n  \"latin_indices\": {");
    any = false;
    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) {
        if (!manifest->latin_index_present[index]) continue;
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "%s\n    \"%s\": {\"slot\": \"latin\", \"index\": %d, \"size\": %llu, \"sha256\": \"%s\", "
                                 "\"source\": \"%s\", \"reader_version\": %d}",
                                 any ? "," : "", kLatinIndexFile[index], index,
                                 (unsigned long long)manifest->latin_index_size[index],
                                 manifest->latin_index_sha256[index], NK_FONT_MANIFEST_SOURCE_USER,
                                 NK_FONT_READER_VERSION);
        any = true;
    }
    used += (size_t)snprintf(text + used, sizeof(text) - used, "\n  }\n}\n");
    if (used >= sizeof(text)) return false;
    if (!font_write_bytes(tmp, (const uint8_t *)text, used)) return false;
    if (nk_rename_utf8(tmp, path) != 0) {
        nk_remove_utf8(tmp);
        return false;
    }
    return true;
}

/* -----------------------------------------------------------------------------
 * Import and removal
 * -------------------------------------------------------------------------- */
typedef struct {
    char names[NK_FONT_IMPORT_MAX_FILES][256];
    int count;
    bool overflow;
} FontFolderScan;

/* A name of at least one character before ".pgf", in any case. */
static bool font_name_is_pgf(const char *name) {
    size_t length = strlen(name);
    if (length < 5u) return false;
    return tolower((unsigned char)name[length - 4u]) == '.' &&
           tolower((unsigned char)name[length - 3u]) == 'p' &&
           tolower((unsigned char)name[length - 2u]) == 'g' &&
           tolower((unsigned char)name[length - 1u]) == 'f';
}

static bool font_collect_pgf(const char *name, void *context) {
    FontFolderScan *scan = (FontFolderScan *)context;
    if (!font_name_is_pgf(name)) return true;
    if (scan->count >= NK_FONT_IMPORT_MAX_FILES) {
        scan->overflow = true;
        return false;
    }
    snprintf(scan->names[scan->count], sizeof(scan->names[scan->count]), "%s", name);
    scan->count++;
    return true;
}

static void font_sort_names(FontFolderScan *scan) {
    for (int i = 1; i < scan->count; i++) {
        char held[256];
        int j = i;
        memcpy(held, scan->names[i], sizeof(held));
        while (j > 0 && strcmp(scan->names[j - 1], held) > 0) {
            memcpy(scan->names[j], scan->names[j - 1], sizeof(held));
            j--;
        }
        memcpy(scan->names[j], held, sizeof(held));
    }
}

/* -----------------------------------------------------------------------------
 * The font-copy folder on a connected PSP
 * -------------------------------------------------------------------------- */
typedef struct {
    char *out;
    size_t out_len;
    int found;
} FontPspSearch;

static bool font_any_pgf(const char *name, void *context) {
    if (!font_name_is_pgf(name)) return true;
    *(bool *)context = true;
    return false;
}

/* <root>/<first>[/<second>] in out; false when it does not fit. */
static bool font_drive_path(char *out, size_t out_len, const char *root, const char *first,
                            const char *second) {
    const char separator = nk_platform_path_separator();
    const size_t length = strlen(root);
    const bool ends_with_separator = length > 0u &&
                                     (root[length - 1u] == '/' || root[length - 1u] == '\\');
    int written = snprintf(out, out_len, "%s%s%s%s%s", root,
                           ends_with_separator ? "" : (separator == '\\' ? "\\" : "/"),
                           first, second ? (separator == '\\' ? "\\" : "/") : "",
                           second ? second : "");
    return written > 0 && (size_t)written < out_len;
}

static bool font_folder_has_pgf(const char *folder) {
    bool any = false;
    return nk_platform_dir_exists(folder) && nk_platform_list_files(folder, font_any_pgf, &any) &&
           any;
}

static bool font_psp_root(const char *root, void *context) {
    FontPspSearch *search = (FontPspSearch *)context;
    char folder[NK_MAX_PATH];
    char mark_a[NK_MAX_PATH];
    char mark_b[NK_MAX_PATH];
    size_t length;
    if (root[0] == '\0') return true;
    /* The system storage's font folder counts only beside its kd and vsh folders, so another
       drive that happens to have a "font" folder is not taken for a PSP. */
    if (!(font_drive_path(folder, sizeof(folder), root, NK_FONT_PSP_SYSTEM_FOLDER, NULL) &&
          font_drive_path(mark_a, sizeof(mark_a), root, NK_FONT_PSP_SYSTEM_MARK_A, NULL) &&
          font_drive_path(mark_b, sizeof(mark_b), root, NK_FONT_PSP_SYSTEM_MARK_B, NULL) &&
          nk_platform_dir_exists(mark_a) && nk_platform_dir_exists(mark_b) &&
          font_folder_has_pgf(folder)) &&
        !(font_drive_path(folder, sizeof(folder), root, NK_FONT_PSP_COPY_PARENT,
                          NK_FONT_PSP_COPY_FOLDER) &&
          font_folder_has_pgf(folder))) {
        return true;
    }
    if (search->found == 0) {
        length = strlen(folder);
        if (length >= search->out_len) return true;
        memcpy(search->out, folder, length + 1u);
    }
    search->found++;
    return true;
}

int nk_font_find_psp_folder_in(const char *const *roots, int root_count, char *out,
                               size_t out_len) {
    FontPspSearch search = { out, out_len, 0 };
    if (!out || out_len == 0u) return 0;
    out[0] = '\0';
    for (int i = 0; roots && i < root_count; i++) {
        if (roots[i] != NULL) (void)font_psp_root(roots[i], &search);
    }
    return search.found;
}

int nk_font_find_psp_folder(char *out, size_t out_len) {
    FontPspSearch search = { out, out_len, 0 };
    if (!out || out_len == 0u) return 0;
    out[0] = '\0';
    nk_platform_list_drive_roots(font_psp_root, &search);
    return search.found;
}

int nk_font_own_font_count_from_states(
    const NkFontSlotState states[NK_FONT_SLOT_COUNT],
    const NkFontLatinIndexState latin_states[NK_FONT_LATIN_INDEX_COUNT]) {
    int own = 0;
    if (!states || !latin_states) return 0;
    if (states[NK_FONT_SLOT_JAPANESE].source == NK_FONT_SOURCE_USER &&
        !states[NK_FONT_SLOT_JAPANESE].replacement) own++;
    if (states[NK_FONT_SLOT_KOREAN].source == NK_FONT_SOURCE_USER &&
        !states[NK_FONT_SLOT_KOREAN].replacement) own++;
    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) {
        if (latin_states[index].source == NK_FONT_SOURCE_USER &&
            latin_states[index].served_index == index && !latin_states[index].replacement) {
            own++;
        }
    }
    return own;
}

int nk_font_own_font_count(const char *user_data_root, const char *project_root) {
    NkFontSlotState states[NK_FONT_SLOT_COUNT];
    NkFontLatinIndexState latin_states[NK_FONT_LATIN_INDEX_COUNT];
    char project_font_dir[NK_MAX_PATH * 2];
    nk_font_slot_states(user_data_root, project_root, states);
    snprintf(project_font_dir, sizeof(project_font_dir), "%s%c%s",
             project_root ? project_root : ".", nk_platform_path_separator(),
             NK_FONT_PROJECT_SUBDIR);
    nk_font_latin_index_states(user_data_root, project_font_dir, latin_states);
    return nk_font_own_font_count_from_states(states, latin_states);
}

static void font_append_text(char *out, size_t out_len, const char *text);

/* Remove every staged slot and Latin index file an import may have left in the cache. */
static void font_remove_staged(const char *cache_dir) {
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        char target[NK_MAX_PATH * 2];
        char staged[NK_MAX_PATH * 2];
        font_join(target, sizeof(target), cache_dir, kSlotFile[slot]);
        font_staging_name(staged, sizeof(staged), target);
        if (staged[0] != '\0') nk_remove_utf8(staged);
    }
    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) {
        char target[NK_MAX_PATH * 2];
        char staged[NK_MAX_PATH * 2];
        font_join(target, sizeof(target), cache_dir, kLatinIndexFile[index]);
        font_staging_name(staged, sizeof(staged), target);
        if (staged[0] != '\0') nk_remove_utf8(staged);
    }
}

bool nk_font_import_folder(const char *user_data_root, const char *folder,
                           const char *const choose[NK_FONT_SLOT_COUNT],
                           NkFontImportResult *result, char *out_error, size_t error_len) {
    FontFolderScan scan;
    NkFontInspection inspections[NK_FONT_IMPORT_MAX_FILES];
    int candidates[NK_FONT_SLOT_COUNT][NK_FONT_IMPORT_MAX_FILES];
    int candidate_count[NK_FONT_SLOT_COUNT] = { 0, 0, 0 };
    int selected[NK_FONT_SLOT_COUNT] = { -1, -1, -1 };
    int latin_candidates[NK_FONT_LATIN_INDEX_COUNT][NK_FONT_IMPORT_MAX_FILES];
    int latin_candidate_count[NK_FONT_LATIN_INDEX_COUNT] = { 0 };
    int latin_selected[NK_FONT_LATIN_INDEX_COUNT];
    char cache_dir[NK_MAX_PATH];
    FontManifest manifest;
    char manifest_message[NK_FONT_TEXT_MAX] = "";
    NkFontStatus manifest_status;
    uint8_t *bytes_for_slot[NK_FONT_SLOT_COUNT] = { NULL, NULL, NULL };
    size_t length_for_slot[NK_FONT_SLOT_COUNT] = { 0, 0, 0 };

    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) latin_selected[index] = -1;

    if (!result || !folder || !*folder) {
        font_copy_text(out_error, error_len, "no folder was given");
        return false;
    }
    memset(result, 0, sizeof(*result));
    memset(&scan, 0, sizeof(scan));
    if (!nk_platform_list_files(folder, font_collect_pgf, &scan)) {
        font_copy_text(out_error, error_len, "the folder cannot be read");
        return false;
    }
    if (scan.overflow) {
        snprintf(out_error, error_len, "the folder holds more than %d .pgf files", NK_FONT_IMPORT_MAX_FILES);
        return false;
    }
    if (scan.count == 0) {
        font_copy_text(out_error, error_len, "no .pgf files are in the folder");
        return false;
    }
    font_sort_names(&scan);
    if (!nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir)) ||
        !nk_platform_mkdir_p_private(cache_dir)) {
        font_copy_text(out_error, error_len, "the PSP font cache folder cannot be created");
        return false;
    }

    /* Read and classify every candidate. Only the metadata is kept; the bytes are read again
     * for the chosen file, so the copy is the file that was checked. */
    result->file_count = scan.count;
    for (int i = 0; i < scan.count; i++) {
        char path[NK_MAX_PATH * 2];
        uint8_t *bytes = NULL;
        size_t length = 0;
        char error[NK_FONT_DETAIL_MAX] = "";
        NkFontFileOutcome *outcome = &result->files[i];
        snprintf(outcome->name, sizeof(outcome->name), "%s", scan.names[i]);
        outcome->slot = -1;
        outcome->latin_index = -1;
        font_join(path, sizeof(path), folder, scan.names[i]);
        if (!font_read_file(path, &bytes, &length, error, sizeof(error))) {
            snprintf(outcome->detail, sizeof(outcome->detail), "refused: %s", error);
            continue;
        }
        bool accepted = font_inspect_bytes(bytes, length, &inspections[i]);
        outcome->replacement = accepted && inspections[i].replacement;
        if (!accepted || inspections[i].slot < 0) {
            snprintf(outcome->detail, sizeof(outcome->detail), "refused: %s", inspections[i].slot_reason);
        } else {
            int latin_index = nk_font_latin_index_from_filename(scan.names[i]);
            if (latin_index >= 0) {
                if (inspections[i].slot != NK_FONT_SLOT_LATIN) {
                    snprintf(outcome->detail, sizeof(outcome->detail),
                             "refused: %s does not name a Latin font", kLatinIndexFile[latin_index]);
                } else {
                    outcome->slot = NK_FONT_SLOT_LATIN;
                    outcome->latin_index = latin_index;
                    snprintf(outcome->detail, sizeof(outcome->detail),
                             "names Latin index %d", latin_index + 1);
                    latin_candidates[latin_index][latin_candidate_count[latin_index]++] = i;
                }
            } else {
                outcome->slot = inspections[i].slot;
                snprintf(outcome->detail, sizeof(outcome->detail), "names the %s slot", kSlotRole[outcome->slot]);
                candidates[outcome->slot][candidate_count[outcome->slot]++] = i;
            }
        }
        free(bytes);
    }

    /* Choose one file per slot: the named choice, else the only candidate. A slot with several
     * candidates and no choice is left alone. */
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        const char *want = choose ? choose[slot] : NULL;
        if (want && *want) {
            bool found = false;
            for (int c = 0; c < candidate_count[slot]; c++) {
                int index = candidates[slot][c];
                if (strcmp(result->files[index].name, want) == 0) {
                    selected[slot] = index;
                    found = true;
                }
            }
            if (!found) {
                snprintf(out_error, error_len, "the chosen %s file does not name that slot", kSlotRole[slot]);
                for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
                return false;
            }
        } else if (candidate_count[slot] == 1) {
            selected[slot] = candidates[slot][0];
        } else if (candidate_count[slot] > 1) {
            for (int c = 0; c < candidate_count[slot]; c++) {
                NkFontFileOutcome *outcome = &result->files[candidates[slot][c]];
                snprintf(outcome->detail, sizeof(outcome->detail),
                         "not imported: %d files name the %s slot; choose one",
                         candidate_count[slot], kSlotRole[slot]);
            }
        }
        if (selected[slot] >= 0) {
            for (int c = 0; c < candidate_count[slot]; c++) {
                NkFontFileOutcome *outcome = &result->files[candidates[slot][c]];
                if (candidates[slot][c] != selected[slot]) {
                    snprintf(outcome->detail, sizeof(outcome->detail), "not chosen for the %s slot",
                             kSlotRole[slot]);
                }
            }
        }
    }

    /* The firmware filenames themselves select the Latin registry index. Duplicate names that
     * differ only in case are ambiguous and remain unimported instead of silently choosing one. */
    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) {
        if (latin_candidate_count[index] == 1) {
            latin_selected[index] = latin_candidates[index][0];
        } else if (latin_candidate_count[index] > 1) {
            for (int c = 0; c < latin_candidate_count[index]; c++) {
                NkFontFileOutcome *outcome = &result->files[latin_candidates[index][c]];
                snprintf(outcome->detail, sizeof(outcome->detail),
                         "not imported: multiple files name Latin index %d", index + 1);
            }
        }
    }

    /* Read each chosen file once more, check the bytes that will be copied, and stage them. */
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        char source[NK_MAX_PATH * 2];
        char error[NK_FONT_DETAIL_MAX] = "";
        NkFontInspection checked;
        if (selected[slot] < 0) continue;
        font_join(source, sizeof(source), folder, result->files[selected[slot]].name);
        if (!font_read_file(source, &bytes_for_slot[slot], &length_for_slot[slot], error, sizeof(error)) ||
            !font_inspect_bytes(bytes_for_slot[slot], length_for_slot[slot], &checked) ||
            checked.slot != slot ||
            strcmp(checked.sha256, inspections[selected[slot]].sha256) != 0) {
            snprintf(out_error, error_len, "the chosen %s file changed or failed its check; try again", kSlotRole[slot]);
            for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
            return false;
        }
    }

    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) {
        char source[NK_MAX_PATH * 2];
        char target[NK_MAX_PATH * 2];
        char staged[NK_MAX_PATH * 2];
        char error[NK_FONT_DETAIL_MAX] = "";
        NkFontInspection checked;
        uint8_t *bytes = NULL;
        size_t length = 0;
        if (latin_selected[index] < 0) continue;
        font_join(source, sizeof(source), folder, result->files[latin_selected[index]].name);
        if (!font_read_file(source, &bytes, &length, error, sizeof(error)) ||
            !font_inspect_bytes(bytes, length, &checked) ||
            checked.slot != NK_FONT_SLOT_LATIN ||
            strcmp(checked.sha256, inspections[latin_selected[index]].sha256) != 0) {
            snprintf(out_error, error_len,
                     "the chosen Latin index %d file changed or failed its check; try again", index + 1);
            for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
            free(bytes);
            for (int i = 0; i < index; i++) {
                char earlier[NK_MAX_PATH * 2], earlier_tmp[NK_MAX_PATH * 2];
                font_join(earlier, sizeof(earlier), cache_dir, kLatinIndexFile[i]);
                font_staging_name(earlier_tmp, sizeof(earlier_tmp), earlier);
                nk_remove_utf8(earlier_tmp);
            }
            return false;
        }
        font_join(target, sizeof(target), cache_dir, kLatinIndexFile[index]);
        font_staging_name(staged, sizeof(staged), target);
        if (!font_write_bytes(staged, bytes, length)) {
            snprintf(out_error, error_len,
                     "the indexed Latin font %d could not be staged in the cache", index + 1);
            for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
            free(bytes);
            for (int i = 0; i <= index; i++) {
                char earlier[NK_MAX_PATH * 2], earlier_tmp[NK_MAX_PATH * 2];
                font_join(earlier, sizeof(earlier), cache_dir, kLatinIndexFile[i]);
                font_staging_name(earlier_tmp, sizeof(earlier_tmp), earlier);
                nk_remove_utf8(earlier_tmp);
            }
            return false;
        }
        free(bytes);
    }

    /* Merge into the manifest: keep the entries for slots this run does not change. */
    manifest_status = font_load_manifest(cache_dir, &manifest, manifest_message, sizeof(manifest_message));
    if (manifest_status != NK_FONT_STATUS_OK) memset(&manifest, 0, sizeof(manifest));
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        char target[NK_MAX_PATH * 2];
        char staged[NK_MAX_PATH * 2];
        if (selected[slot] < 0) continue;
        font_join(target, sizeof(target), cache_dir, kSlotFile[slot]);
        font_staging_name(staged, sizeof(staged), target);
        if (!font_write_bytes(staged, bytes_for_slot[slot], length_for_slot[slot])) {
            for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) {
                char target_path[NK_MAX_PATH * 2];
                char left[NK_MAX_PATH * 2];
                font_join(target_path, sizeof(target_path), cache_dir, kSlotFile[s]);
                font_staging_name(left, sizeof(left), target_path);
                nk_remove_utf8(left);
            }
            for (int i = 0; i < NK_FONT_LATIN_INDEX_COUNT; i++) {
                char target_path[NK_MAX_PATH * 2];
                char left[NK_MAX_PATH * 2];
                font_join(target_path, sizeof(target_path), cache_dir, kLatinIndexFile[i]);
                font_staging_name(left, sizeof(left), target_path);
                nk_remove_utf8(left);
            }
            font_copy_text(out_error, error_len, "the font could not be written to the cache");
            for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
            return false;
        }
    }
    /* Commit: each staged file is renamed over its target and recorded in the manifest as it
       lands. A rename that fails (a target another program holds open) stops the commit, but the
       files already renamed hold their new bytes, so the manifest is still written for them:
       left as it was, it would name the old bytes and the whole cache would read as invalid.
       Every staged file that was not committed is removed. */
    {
        const char *commit_error = NULL;
        bool manifest_written;
        for (int slot = 0; slot < NK_FONT_SLOT_COUNT && !commit_error; slot++) {
            char target[NK_MAX_PATH * 2];
            char staged[NK_MAX_PATH * 2];
            if (selected[slot] < 0) continue;
            font_join(target, sizeof(target), cache_dir, kSlotFile[slot]);
            font_staging_name(staged, sizeof(staged), target);
            if (nk_rename_utf8(staged, target) != 0) {
                commit_error = "the font could not be committed to the cache";
                break;
            }
            manifest.present[slot] = true;
            manifest.size[slot] = inspections[selected[slot]].size;
            snprintf(manifest.sha256[slot], sizeof(manifest.sha256[slot]), "%s", inspections[selected[slot]].sha256);
            result->slot_imported[slot] = true;
            result->imported_count++;
            result->files[selected[slot]].imported = true;
            snprintf(result->files[selected[slot]].detail, sizeof(result->files[selected[slot]].detail),
                     "imported as the %s font", kSlotRole[slot]);
        }
        for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT && !commit_error; index++) {
            char target[NK_MAX_PATH * 2];
            char staged[NK_MAX_PATH * 2];
            if (latin_selected[index] < 0) continue;
            font_join(target, sizeof(target), cache_dir, kLatinIndexFile[index]);
            font_staging_name(staged, sizeof(staged), target);
            if (nk_rename_utf8(staged, target) != 0) {
                commit_error = "the indexed Latin font could not be committed to the cache";
                break;
            }
            manifest.latin_index_present[index] = true;
            manifest.latin_index_size[index] = inspections[latin_selected[index]].size;
            snprintf(manifest.latin_index_sha256[index], sizeof(manifest.latin_index_sha256[index]),
                     "%s", inspections[latin_selected[index]].sha256);
            result->latin_index_imported[index] = true;
            result->imported_count++;
            result->files[latin_selected[index]].imported = true;
            snprintf(result->files[latin_selected[index]].detail,
                     sizeof(result->files[latin_selected[index]].detail),
                     "imported as Latin index %d", index + 1);
        }
        for (int s = 0; s < NK_FONT_SLOT_COUNT; s++) free(bytes_for_slot[s]);
        if (commit_error) font_remove_staged(cache_dir);
        manifest_written = result->imported_count == 0 || font_write_manifest(cache_dir, &manifest);
        if (commit_error) {
            font_copy_text(out_error, error_len, commit_error);
            if (!manifest_written) {
                font_append_text(out_error, error_len, ", and the font manifest could not be written");
            }
            return false;
        }
        if (!manifest_written) {
            font_copy_text(out_error, error_len, "the font manifest could not be written");
            return false;
        }
    }
    /* A replacement is imported like any font the reader accepts, and its line says what it is. */
    for (int i = 0; i < result->file_count; i++) {
        NkFontFileOutcome *outcome = &result->files[i];
        if (!outcome->replacement || outcome->slot < 0) continue;
        font_append_text(outcome->detail, sizeof(outcome->detail), " (" NK_FONT_REPLACEMENT_NOTE ")");
        if (outcome->imported) result->replacement_count++;
    }
    return true;
}

/* Append text to a bounded message, keeping what is already there. */
static void font_append_text(char *out, size_t out_len, const char *text) {
    size_t used;
    if (!out || out_len == 0) return;
    used = strlen(out);
    if (used >= out_len) return;
    snprintf(out + used, out_len - used, "%s", text);
}

/* Add one problem to a removal's failure text, separated from the problem before it. */
static void font_add_problem(char *problems, size_t problems_len, const char *text) {
    if (problems[0] != '\0') font_append_text(problems, problems_len, "; ");
    font_append_text(problems, problems_len, text);
}

int nk_font_remove_imports(const char *user_data_root,
                           const bool remove[NK_FONT_SLOT_COUNT],
                           char *out_error, size_t error_len) {
    char cache_dir[NK_MAX_PATH];
    char manifest_path[NK_MAX_PATH * 2];
    FontManifest manifest;
    char message[NK_FONT_TEXT_MAX] = "";
    char problems[NK_FONT_TEXT_MAX] = "";
    NkFontStatus status;
    int removed = 0;
    int dropped = 0;
    int failed = 0;
    if (!remove) {
        font_copy_text(out_error, error_len, "no slot was named");
        return -1;
    }
    if (!nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        font_copy_text(out_error, error_len, "PSP font cache location is unavailable");
        return -1;
    }
    font_join(manifest_path, sizeof(manifest_path), cache_dir, NK_FONT_MANIFEST_NAME);
    status = font_load_manifest(cache_dir, &manifest, message, sizeof(message));
    /* A named slot's entry is dropped only once its file is gone: deleted now, or absent
     * already. A file that is still there after the delete failed keeps its entry, so the
     * manifest keeps describing the cache, and the other named slots are still processed. The
     * cache folder holds only this flow's slot files, so a named slot's file is removed even
     * when the manifest is unreadable. */
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        char path[NK_MAX_PATH * 2];
        char line[NK_FONT_DETAIL_MAX];
        if (!remove[slot]) continue;
        font_join(path, sizeof(path), cache_dir, kSlotFile[slot]);
        if (nk_remove_utf8(path) == 0) {
            removed++;
        } else if (!nk_platform_path_missing(path)) {
            failed++;
            snprintf(line, sizeof(line),
                     "could not delete the %s font file %s, which is still in the cache and keeps its manifest entry",
                     kSlotRole[slot], kSlotFile[slot]);
            font_add_problem(problems, sizeof(problems), line);
            continue;
        }
        if (manifest.present[slot]) {
            manifest.present[slot] = false;
            dropped++;
        }
    }
    if (remove[NK_FONT_SLOT_LATIN]) {
        for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) {
            char path[NK_MAX_PATH * 2];
            char line[NK_FONT_DETAIL_MAX];
            font_join(path, sizeof(path), cache_dir, kLatinIndexFile[index]);
            if (nk_remove_utf8(path) == 0) {
                removed++;
            } else if (!nk_platform_path_missing(path)) {
                failed++;
                snprintf(line, sizeof(line),
                         "could not delete Latin index %d file %s, which is still in the cache and keeps its manifest entry",
                         index + 1, kLatinIndexFile[index]);
                font_add_problem(problems, sizeof(problems), line);
                continue;
            }
            if (manifest.latin_index_present[index]) {
                manifest.latin_index_present[index] = false;
                dropped++;
            }
        }
    }
    /* The manifest is rewritten only when an entry was dropped, so a call that changes nothing
     * leaves its bytes as they are. An unreadable manifest is removed, as before, unless a named
     * file stayed; it is then left in place, since its entries cannot be rewritten faithfully. */
    if (status == NK_FONT_STATUS_OK && dropped > 0) {
        bool any_left = false;
        for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) if (manifest.present[slot]) any_left = true;
        for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++)
            if (manifest.latin_index_present[index]) any_left = true;
        if (any_left) {
            if (!font_write_manifest(cache_dir, &manifest)) {
                font_add_problem(problems, sizeof(problems),
                                 "the font manifest could not be rewritten, so it still lists the removed fonts");
            }
        } else if (nk_platform_file_exists(manifest_path) && nk_remove_utf8(manifest_path) != 0) {
            font_add_problem(problems, sizeof(problems), "the font manifest could not be removed");
        }
    } else if (status != NK_FONT_STATUS_OK && failed == 0) {
        if (nk_platform_file_exists(manifest_path) && nk_remove_utf8(manifest_path) != 0) {
            font_add_problem(problems, sizeof(problems), "the font manifest could not be removed");
        }
    }
    if (problems[0] != '\0') {
        char text[NK_FONT_TEXT_MAX + 64];
        if (removed > 0) {
            snprintf(text, sizeof(text), "removed %d font file(s); %s", removed, problems);
        } else {
            snprintf(text, sizeof(text), "%s", problems);
        }
        font_copy_text(out_error, error_len, text);
        return -1;
    }
    return removed;
}

bool nk_font_resolve_directory(const char *user_data_root,
                               const char *fallback_root,
                               char *out_dir,
                               size_t max_len) {
    char cache_dir[NK_MAX_PATH];
    char message[NK_FONT_TEXT_MAX] = "";
    if (!out_dir || max_len == 0) return false;
    if (nk_font_check_cache(user_data_root, message, sizeof(message)) == NK_FONT_STATUS_OK &&
        nk_font_get_cache_dir(user_data_root, cache_dir, sizeof(cache_dir))) {
        snprintf(out_dir, max_len, "%s", cache_dir);
        return true;
    }
    if (fallback_root && *fallback_root) {
        char fpath[NK_MAX_PATH * 2];
        snprintf(fpath, sizeof(fpath), "%s%c%s", fallback_root, nk_platform_path_separator(), NK_FONT_PROJECT_SUBDIR);
        if (nk_platform_dir_exists(fpath)) {
            char abs_font[NK_MAX_PATH];
            if (nk_platform_absolute_path(fpath, abs_font, sizeof(abs_font))) {
                snprintf(out_dir, max_len, "%s", abs_font);
            } else {
                snprintf(out_dir, max_len, "%s", fpath);
            }
            return true;
        }
    }
    return false;
}
