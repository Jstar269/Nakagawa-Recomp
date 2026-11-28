# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Per-user PSP system-font import: PGF checks, slot classification, manifest v2, removal.

The checks in :func:`pgf_verdict` mirror the project-authored native reader
(``src/rt/pgf_public.c``, ``pgf_validate_memory``) rule for rule, in the same order.
``tests/native`` and ``tools/test_pgf_validate_parity.py`` compare the two on the same
files. The slot constants mirror ``src/core/nk_font_slots.h``, and a test pins them.

Nothing here reads keys, decrypts, or uploads. The user brings their own font files.
"""

from __future__ import annotations

from datetime import datetime, timezone
import functools
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import threading
from typing import Any, Dict, Iterable, List, Optional, Tuple

from .prereq_fetcher import default_data_root as default_user_data_root

# --- Layout, mirrored from src/core/nk_font_slots.h -------------------------------------
CACHE_PARENT = "fonts"
CACHE_SUBDIR = "v2"
MANIFEST_NAME = "manifest.json"
MANIFEST_SCHEMA_VERSION = 2
MANIFEST_SOURCE_USER = "user"
READER_VERSION = 1
PROJECT_SUBDIR = "font"
PGF_MAX_BYTES = 16 * 1024 * 1024
IMPORT_MAX_FILES = 64
_FONT_IMPORT_LOCK = threading.RLock()

#: Slot ids in NkFontSlot order. Each slot's cache file is its served project name.
SLOTS: Tuple[str, ...] = ("japanese", "latin", "korean")
SLOT_ROLES: Dict[str, str] = {"japanese": "Japanese", "latin": "Latin", "korean": "Korean"}
SLOT_FILES: Dict[str, str] = {
    "japanese": "nkjpn.pgf",
    "latin": "nkltn.pgf",
    "korean": "nkkr.pgf",
}
LATIN_INDEX_FILES: Tuple[str, ...] = tuple(f"ltn{index}.pgf" for index in range(16))
LATIN_SIZE_GROUP_COUNT = 8
#: The project's Latin font for the second size group; a project folder without it serves the first.
PROJECT_LATIN_SMALL_FILE = "nkltns.pgf"
#: The project face of each Latin index's own family and style (src/core/nk_font.c
#: kProjectLatinIndexFile): the two regular sans-serif faces keep the names above.
PROJECT_LATIN_INDEX_FILES: Tuple[str, ...] = tuple(
    "nkltn.pgf" if index == 0 else PROJECT_LATIN_SMALL_FILE if index == 8 else f"nkltn{index}.pgf"
    for index in range(16)
)


def latin_index_from_filename(name: str) -> Optional[int]:
    """Return a firmware Latin file ordinal for ltn0.pgf..ltn15.pgf, case-insensitively."""
    try:
        return LATIN_INDEX_FILES.index(name.casefold())
    except ValueError:
        return None

# --- Reader constants, mirrored from src/rt/pgf_public.c and docs/cleanroom/PGF_SPEC.md ----
BASE_HEADER_SIZE = 392
REV3_HEADER_SIZE = 412
MAX_COUNT = 1 << 20
TABLE_COUNT = 4
COMPOSITE_PARTS = 3
METRIC_MISSING_INDEX = 0xFF
PROBE_LATIN = (0x41, 0x61)
PROBE_KANA = (0x3042, 0x30A2)
PROBE_HANGUL = (0xAC00, 0xD55C)

#: Refusal names in PgfRefusal order (src/rt/pgf_api.h). Index 0 is accepted.
REFUSAL_NAMES: Tuple[str, ...] = (
    "NONE",
    "TOO_LARGE",
    "TRUNCATED",
    "HEADER_OFFSET",
    "MAGIC",
    "REVISION",
    "HEADER_SIZE",
    "GLYPH_RANGE",
    "COUNTS",
    "NO_GLYPHS",
    "GLYPH",
)
REFUSAL_TEXT: Dict[str, str] = {
    "TOO_LARGE": "larger than the 16 MiB reader ceiling",
    "TRUNCATED": "truncated",
    "HEADER_OFFSET": "header offset is not zero",
    "MAGIC": "invalid PGF magic (expected PGF0)",
    "REVISION": "unsupported revision or version",
    "HEADER_SIZE": "header size does not match the revision",
    "GLYPH_RANGE": "corrupt glyph indices (first exceeds last)",
    "COUNTS": "count or bit width out of range",
    "NO_GLYPHS": "no glyph records",
    "GLYPH": "a glyph record fails the reader's checks",
}


# --- Replacement fonts: valid PGFs that are not a PSP's own fonts ----------------------------
# A replacement is accepted like any font the reader accepts, but no status calls it the user's
# own PSP font. src/core/nk_font.c (nk_font_is_replacement) applies the same rule to the same
# bytes, and tools/test_font_provisioning.py compares the two.

#: Where a PGF header holds its font name, and its width (docs/cleanroom/PGF_SPEC.md, section 3.2).
FONT_NAME_OFFSET = 0x35
FONT_NAME_SIZE = 64
#: The font name of every stand-in this project converts from an open font starts with these
#: bytes ("Nakagawa Open Latin", "Nakagawa Open Japanese", "Nakagawa Open Korean": mk/housekeeping.mk,
#: and tools/ttf2pgf.py's default name).
PROJECT_STANDIN_NAME_PREFIX = b"Nakagawa Open "

#: SHA-256 of each version of the open replacement fonts other programs ship in place of a PSP's
#: own (jpn0.pgf, kr0.pgf and ltn0.pgf to ltn15.pgf). Their headers carry the console fonts' family
#: names, so only the exact files are recognised. Each group names the public repository, the
#: revision that first held those bytes and the folder; the list covers every version in those
#: folders' history up to hrydgard/ppsspp@5193e380a57bf2401f00cf60c9c382cd26bdcb4d and
#: jpcsp/jpcsp@00ac4d6daedd89c0492093ae83fb0d3d819af414, except one jpn0.pgf
#: (hrydgard/ppsspp@25aebba067a85fd949563099a015e7522442c83b) whose origin its history does not
#: establish. It names replacement files only, never a PSP's own font.
KNOWN_REPLACEMENT_SHA256: frozenset[str] = frozenset({
    # hrydgard/ppsspp@1429d0206ac152fe4c77936f57016c9279467b12 (2013-02-27), flash0/font/
    "8e2cd9c996e7cd5d229612cadd7bb03a82ccc8728b198568df8b4f585786cb5a",  # kr0.pgf
    "e59233e26b58a7ffb4a0eead399116c92dd30dd13ef888e46ebde3bc08045a18",  # jpn0.pgf
    "fc6c6a61f741d806102eb9d4b53e8661e44a886a824518c347b52ea492100cc3",  # ltn0.pgf
    "37a8261dd27b7fbc5f1b273f393bce956fbe9cabcbb9978cfc7acb06fd03ceea",  # ltn1.pgf
    "96ca233797aae69ae06473aef930b78ed0b003555394b127bda5c049eef32954",  # ltn2.pgf
    "8c6e59844c0cf3274c92b71fb74616d3512200075761386a045fb6dbffb39f02",  # ltn3.pgf
    "ab09ddda69d8141ecb6738f0267cd086b1cf3eeadb8bef93c129e86f2261b06e",  # ltn4.pgf
    "c8b5fbdd4eca5eba3e6e5d993bc5879fd2208f94669b98620e56f691989eda40",  # ltn5.pgf
    "633d44c2af710235404c70cab4bc2a0b2dff8efcf7eb0dcb874f8d24da5d032d",  # ltn6.pgf
    "6ae771b2c07773f2a1435d8af652477fb2ec86075b89c6917150ba1e8d7af75d",  # ltn7.pgf
    "3ce9134e64ee9af66bd177d9821213cbe7e7e7e4a05697e61fe8afaf88ceb3c1",  # ltn8.pgf
    "f5f036a9e7063a6cfe64b1cdbe9164d0fe8f946c3fb4caf0a610b882764dcee9",  # ltn9.pgf
    "7459613b939ae8adc7942f0b6a4c0141e5a5efcc00871755c5e46209dd44ff1d",  # ltn10.pgf
    "b5cb9aa151075e0d70419b935012ae052de122c672b9581f686528bef1284c62",  # ltn11.pgf
    "b57089eca4c0f1645aac463ee91bb7404150901dd6b0f45c67be805b8b262fdd",  # ltn12.pgf
    "45e6187a5de1f03fe9faa4f4e5dfabbe707389b275851fda47af2c4bfa39bf22",  # ltn13.pgf
    "cb7456d9ad273e33b79351bbeff6439d2735f470c7bfe9b46ca262ebbd73365d",  # ltn14.pgf
    "ebcdde936664990ff4d447988bb50d39b7d42a1d242909c8430368044b73a0d6",  # ltn15.pgf
    # hrydgard/ppsspp@69fe13e6c30b616a28d4f083f52c11b22ae36a25 (2013-03-17), flash0/font/
    "0095817395400b683ad5d79c107aba6dde5d6bdcb7d4f5c0e36cd996d3fb32b4",  # jpn0.pgf
    # hrydgard/ppsspp@9a114587a3eb378351448422390aff10ab2602e4 (2013-03-17), flash0/font/
    "fa843d54502597b11a393226d8c3cb3360230feb8cd053dff29a0b0afc960635",  # jpn0.pgf
    # hrydgard/ppsspp@0436b751a70c4a6b374f3dca777f13e0d42f8855 (2013-08-24), flash0/font/
    "b4ee2e944bfe59ef0b8e40f54f92a0954373b95636e7f1519128809f406a2a41",  # jpn0.pgf
    # hrydgard/ppsspp@77c1e96588718b83527fac9dfe395c2e5eddf4f7 (2020-10-09), assets/flash0/font/
    "521e8c40955da80e28354ac2adff243f9dff071b177cdc72461376a837a92639",  # jpn0.pgf
    # hrydgard/ppsspp@f68e0fecd4f52b9836b8aa031248f2a1d3489bce (2020-10-26), assets/flash0/font/
    "d283601e6c410c9be0dbb279b76bd87cb1d9ef6a2600cf04fe54f7054ea85a8d",  # jpn0.pgf
    # hrydgard/ppsspp@3831d2346d08ac7d7a8d64c39cb38593e2df3be4 (2020-10-27), assets/flash0/font/
    "ede37dd375560fcf34d27da61078d38319a484030d684eda905df3f77ac8ddee",  # ltn1.pgf
    "79c5fa821dad8413bc5a0c6243a69cf89fb94fbe30579fffba6f980e2e201bea",  # ltn3.pgf
    "0061fabc877438ae7554e704e93e9e767905b15155a9db2e6a17339a7ceb8338",  # ltn5.pgf
    "7a05364683f6ed03a87f7a578046306a6c31c2db4f9153d9c1ae6792dc95c973",  # ltn7.pgf
    # hrydgard/ppsspp@41ee5fbd931c796d188d35e6df43156dea25985f (2020-10-28), assets/flash0/font/
    "b2cd3644892a4792f85d28a7005ecb9a240546671b3d1fca58f7d6552a6c94d9",  # ltn1.pgf
    "e6683d4e13ab7b5c1601e09b42ae20fddb99bdacb58839f949ffd7615b101347",  # ltn3.pgf
    "d575de373d3184170cbd10b370b7fde9ad60012e7afeb1c6c058d2f13f0dc7fa",  # ltn5.pgf
    "4c5a11c72446b4ce4495fd8825f4aaca46b2874f651def4c53ab8bb8ebbce71e",  # ltn7.pgf
    # hrydgard/ppsspp@06b0e33839b4260caa7f950df7e7a19961395e70 (2020-10-31), assets/flash0/font/
    "59ee75b2be664f68730e312a98ce3bb03fb2177ba24aab73d321ed79bed27332",  # ltn0.pgf
    "d8fcf2a699adfdd4beca3dcc52c73d93817c4c9445f356ee34ffd2a22298258d",  # ltn2.pgf
    "a3357b82525953a8eec36ae9cc3bd8c256fae7cc1aa379fa798ba0cb979b9f44",  # ltn4.pgf
    # hrydgard/ppsspp@737e0d55d4a06cb026ec453010e1860ecc23df97 (2020-11-22), assets/flash0/font/
    "c6f58823cd316dd1840a4dc5b1aaded4ce50a02d3afc7859af96e287b4c5c67c",  # ltn0.pgf
    "076ef1e86b7d090cc8b5064cda6eb8f4d7889967b23e7379fce0ef2cfa76d94a",  # ltn1.pgf
    "a7369bc141f1d808c300258130fa231e26d007a33ac03eb345a2220a14c60f45",  # ltn2.pgf
    "252e3c4dd7844aeb48e183539ade79c83388126402959b7fa7aee9206404bca1",  # ltn3.pgf
    "2136dc41c671e4b3c9cdbd6323f3b21ca4b7dfc83bd0575e7f19b0cf21e94c20",  # ltn4.pgf
    "24ca2b1fb1d4078104fcb6c34c7e5bc60fae6f06e38c442d8089d7c97bc023b0",  # ltn5.pgf
    "22e0036e0cdcfc4ef5ac27d289804d06b91efd2e3b549983efa0e6c32967e978",  # ltn6.pgf
    "c3c8cc74d74efdc0523670d1a08b0ae523977a154f7bd8d870fd284e71579746",  # ltn7.pgf
    # hrydgard/ppsspp@f1a3ba7f5669204a964e4a49c0354d8ab64e6dbc (2020-11-22), assets/flash0/font/
    "9a0424fa5050317ef1a15601ef3a26a33ac99ade3adaf6f31dda056113660ca5",  # jpn0.pgf
    # hrydgard/ppsspp@388ac3c9aeb69c86e269eb1ad16fb8d1ef2a5fcd (2020-11-23), assets/flash0/font/
    "5a612817cc78058603d90e349ba80347ad53e85b251b2b7046e207d7e5e7383f",  # jpn0.pgf
    # hrydgard/ppsspp@b5e23007700fc026c4fddea45e87f345d2a41d32 (2020-11-29), assets/flash0/font/
    "115d7de21446aa7b7122ab750f3b6d68e265cb5b62f3c8a36c8766272d8331ae",  # ltn0.pgf
    "eff4a780810c8caee666723e12291b750af2348706aaf81a3767b709956a29fb",  # ltn2.pgf
    "6058470e62f172ff6c87633cbd02fe9b9f05a9e9f8cddbf2f48b94aeb944e876",  # ltn8.pgf
    "7c010ab6616417a7a60f81afd6163849fd9c68e753148a9909f907e03b6d077d",  # ltn9.pgf
    "d4d6f486ba26f3eca714bbb6def86e48b5eee4b6f1ece4ec2076aec7e1bffb0e",  # ltn10.pgf
    "9a6733f492460b9f75f25a1187408b94e57415bb3c6fa8d36241312f2168c2cd",  # ltn11.pgf
    "cefc0b3b6e48d8bd0df51acaf8771ece87336af3aaa930019d9b0fe29d5ee1a6",  # ltn12.pgf
    "ddf07858d55ea1fae19ba26410d5aa7da6028e8004c7abbc23d06371cb081af7",  # ltn13.pgf
    "d246772639659f028737eb30263b4a4185e0035849ce73996abcac1fc54ad5a4",  # ltn14.pgf
    "35e699c79f38b685293f792f35e9e7aae8b8033fd77779db47d3d76378b052db",  # ltn15.pgf
    # hrydgard/ppsspp@75bdb5ff1723bd39574baf11336c5c8a3173deec (2020-11-30), assets/flash0/font/
    "7147db48e15e2f41c4da5214c4c14b4252c71fb164a9b407e6f7df5c4f5ccf21",  # kr0.pgf
    # hrydgard/ppsspp@0c243ee13909a1256de4be2b9d374aa6f7b46d38 (2020-12-08), assets/flash0/font/
    "bb414a4fac24c503ff07d78aa6c8c08958ba7c518440450da4c6ac44523c2921",  # kr0.pgf
    "59254bec7f0370424a904469e84b1068f9fa6cea355f201696e798e5888de34f",  # jpn0.pgf
    "e4f98d7f221eb0121d385844555450224aaf89d347458df94f9ef2a529212107",  # ltn0.pgf
    "3ae0732b667b60aaa7b3f204c6ba42ec3955330cc9fc48831579cf8630f50eb4",  # ltn1.pgf
    "eafc02b3f0e2025463f7a32e62367945eb40bc33a43a16998ca4b2d7b694f218",  # ltn2.pgf
    "c33820c387b0c99e6409128b45e53015b62200fd4b862b0f1173b015be4f597d",  # ltn3.pgf
    "4ae00e18621e20af98601813d1ac8d20c481ca1df97b11f38ef5251bd621a30c",  # ltn4.pgf
    "7ba3a04d602bf57df9c1c40ec03dc5d902a7225d3d5efa3a1fda47503e3e588b",  # ltn5.pgf
    "79188057278018b192a3a9ba69f230ba5234665eb66c8b4985cb2ee31138c5db",  # ltn6.pgf
    "ee009e747e6d1fc41852607cf2ebb12967bb43f2a8de98be6ee70ea6a55236de",  # ltn7.pgf
    "87e75290cace40b36fc6ee8942dc282544699b04ce58309067f1f20a65a7cd7f",  # ltn8.pgf
    "bf67c7bd1b2cec1d7735de774afd70400d2ad58d12297f1e0c999fc6cab84b8d",  # ltn9.pgf
    "50594f5eba9b1951109826d0016b801badcc7c3421084d8e096168234aa2a610",  # ltn10.pgf
    "ff3cd634fbb9e3a5e426dc7d415a6308c23943718d8984f860c71fe1bbfad13f",  # ltn11.pgf
    "5d1962bcf461dbf586e3657d738823203462d9feb1c67f6191ed6eac58bbc49b",  # ltn12.pgf
    "f742eb3d1305efa0f33b3817ae3c164250b23c6283b654223eab60788739bb50",  # ltn13.pgf
    "07781f2e3f3d278a033289c77b781b007d8e34ef0ac710e64a4282cc7e4205c8",  # ltn14.pgf
    "0aefe7c513ea24dccdcd03d1119b08cb1175b5135df2a87458a4404295fe83db",  # ltn15.pgf
    # hrydgard/ppsspp@b4847976d35c33ea3daa57ef040ea3da2b939964 (2020-12-17), assets/flash0/font/
    "3cbede6c4147e05e9a1dcb24d34c4967119238b6111bb259e97c5e74e46de787",  # kr0.pgf
    "378d4466b61f362621ef7d5df5ad96e23c0c1104f57bcbed07a6a490325d23d2",  # jpn0.pgf
    "1c98682617515e60f2e8c7a0ef98cccc7bb1bb9ef6f98634daa40e255a418f69",  # ltn0.pgf
    "697f4848f6506490e808520a0ca80eb4479d18efc151d8061626b369a88ecd38",  # ltn2.pgf
    "207ff74d27d4dca556ae03d2d29741a2de30d649b3180e0d78487e68816d21c7",  # ltn4.pgf
    "95f9e2ea3c7b6841d09140a378467312c14350b45b90e5c43e4651f91df1960b",  # ltn6.pgf
    # hrydgard/ppsspp@f001ee284e418c53fa59ce9a7583e75afb4f839a (2020-12-17), assets/flash0/font/
    "f66da94ce73b699ca54a82677cebcf537a74b904c7a93d2ec224d0a78393b166",  # ltn0.pgf
    "d498b083046a23e91d31af5e20c87a48ec80cb46ba6a87f3706e6d597a7fdd33",  # ltn2.pgf
    "d420943080c1a86731b0f1110d4aafddae962f2d76bdc52e3c24eb92015359a4",  # ltn4.pgf
    "2158626b212831feb66cfea8cfc1be243803014c2a9ca1649b3ebf60bb8448c9",  # ltn6.pgf
    # hrydgard/ppsspp@9fe10351c43e4ee21e02db54caaacf257ddd1fa6 (2020-12-31), assets/flash0/font/
    "1c797e9c36b5637db7d855f31d6a70109d7cac823b98cce82863cd9b1c9f1cf0",  # jpn0.pgf
    # hrydgard/ppsspp@95e6e6c83944149dc2346851fb2fe25afae91406 (2021-01-10), assets/flash0/font/
    "e22b1d1f81b7239483ab00f7d84bfce118b71df84f1ae779509d90fbae419df5",  # jpn0.pgf
    "4621ff6a474a0cd1f36dc6555759ded517a5937d6b97f09d77957f9fd293d845",  # ltn0.pgf
    "d428bd0794086ea00156433bedc6fa2c28be83785b9209f6b3b01190446365d6",  # ltn1.pgf
    "ac4041e5c9c9bb87929c4fa1da58eb53a9cc68f0c2a91c10969ab0c0daf9eb42",  # ltn2.pgf
    "d8ad51e08bd754936d2285300e179d4fb92750e1714ce29c452ebb74a2669e40",  # ltn3.pgf
    "2f5024dcef4009bdefe915840f21b1df46c6a4bd67d473649af99dc420b45160",  # ltn4.pgf
    "c194fd0a25da1d85bad01b180bd70704e3958e485e82f1246699d93a75bac7fc",  # ltn5.pgf
    "601f745eff6a0d8117ee90d39b2d7bfb38f02a6d0ea6d66746f9908d6687f046",  # ltn6.pgf
    "7a39f9b2cc9cd4c57cd8324b280cf0c74e7e82ac36e4a88e3fdea256a3a0eecc",  # ltn7.pgf
    "859935a6ff9cb264c5e4bdcedf41cde8c69c35629aa9dc15a5d8d74d5922d2e2",  # ltn8.pgf
    "9ab574ca7ff8a4960678b546b7c55e4831816777d69d8078a8490fd21bc1f9b3",  # ltn9.pgf
    "64e174205e27b9f6ec3d68dad93bb1f0e8c1bb6ecdaf1f33b675046f3889a67d",  # ltn10.pgf
    "ee8df0e6e9927496126e0a72c3804c770f598f6429081d150002d94138f8c451",  # ltn11.pgf
    "763e7ebbc4d2992fb6f7e5fca3968d43b7f16bc48e24c4367e9efa6d4bb412cf",  # ltn12.pgf
    "730d7be0c7aee29fbd3f7da75cb28822cc7c7c50b3dba37efe9e1b7e1dea5eb5",  # ltn13.pgf
    "cb527e38be7f633ecb52ca2e0c19cf5dfa85b9bf799d060f0de81beb4b9b9f0a",  # ltn14.pgf
    "3b91c9d4175feaeeff2f9b7a427011c8456921e4b40ce1ac0e0797c809364362",  # ltn15.pgf
    # hrydgard/ppsspp@3540d5c5aaf7728800b14b21fb4193dbdc0c69d4 (2021-01-11), assets/flash0/font/
    "55d4454f5002c2aeb35b5845c3cfe320f6a2c42d376efde717794a1a65c2d8d5",  # jpn0.pgf
    "b496d93c68194b086a66617936b800f21423f2b095a22f723fca7b47e0ba3b9c",  # ltn0.pgf
    "e0208dc5f6f6d1c987f8b18eb1a9a51fb1c5625b4d282832f9e300222849da63",  # ltn1.pgf
    "64d71054a2bf82e14343b357009339f4b10bca4f16a54b439d8cf15679986165",  # ltn2.pgf
    "99c0b3ed0e720b69364377ae8168f137ed644781c57a8d5eebdb9124244224a3",  # ltn3.pgf
    "2ea31544bd466b837c08b57758f4a5149c774c5f2a6f9474ede3672f66b1d4ae",  # ltn4.pgf
    "bcb9ae29ca5b76b60dd755cd5ac197b7c7276cf6b13809c0a1b9561f757b3d24",  # ltn5.pgf
    "3ca71b8d7108c68727dcd30f0a59d322ff94a3d0515ed40c6d554bedcfe82951",  # ltn6.pgf
    "0b4e52dd08d7906f6129f1fd36a9d4f783f6f831011193ccbde2c16a7b6475f1",  # ltn7.pgf
    "45e57df9359fc18cc6afb3dac9226f9e6fb44fd779dc1685606c5a7bff930716",  # ltn8.pgf
    "236100fdc101f544f67f88a13ab99c7a109de16366c08ee42840803ad0ba3be3",  # ltn9.pgf
    "7e9fdb33f0090762b074ec837a7c78c4eb2ec8721c28af9f4d8850b2b2b6fb99",  # ltn10.pgf
    "efa2f7bb60ccebd6399caa48c988945f639351b9f563429667be6bce17c50c4d",  # ltn11.pgf
    "5a8ba0844769b3385ca393f18d9c20ac626afed006663005714f27fae9853b7b",  # ltn12.pgf
    "78fc49f8a6cb71d6582babe8a5dc24504ab02b111a6363b7c41e710a6fc8b254",  # ltn13.pgf
    "e881bfce6405f017f6e189bb73b22c52654e90d0f37e692e89371de660a863c9",  # ltn14.pgf
    "24d770ee71c3dfaa153955871d341e508a517f5ac0208626c13ae6e5635ae016",  # ltn15.pgf
    # hrydgard/ppsspp@dc34bea8d4dee77629da3cbaedc83fc8c05af159 (2021-01-19), assets/flash0/font/
    "6b0b3a8be9e709d6af02223d077675e1530f9879f42656377a9d8d42e2614333",  # kr0.pgf
    "f91a8071bef137b43bebdae6ea72eb3fbf19a4ce5fa5a0860b20c5498b624b62",  # jpn0.pgf
    "fe9ed5904dffacdeccfcb1f0a71ecdbbcda9eddf15f6e3e31f7397cf7a9b8a34",  # ltn0.pgf
    "5e731e3815bea7264b4e71f476232821e9c70f2afc7a3d36d6d166700a29c658",  # ltn1.pgf
    "750b03759e941747821942c24bdcd77104cfc444b587478afef1d3b6576ae2ec",  # ltn2.pgf
    "1ca894be5f14557ec37688067d250c8e2aab1e739a623cb941a3a100e82a4148",  # ltn3.pgf
    "d62b340de7e296376753a37c58740d6a2e0ce9f0526139455225e40e1e64cd9e",  # ltn4.pgf
    "30ddcf60ebf0cb09f45bac4ad54c355198b758430811ca24bcc29d34f5c7ecee",  # ltn5.pgf
    "abdd38713bd7265201099560b7f19ed3db219be1dee1844980f535129b0ab097",  # ltn6.pgf
    "680fad898cf0e3ddd780f212f04dad5a195db63d4c7fc1ea95c01756f7887c84",  # ltn7.pgf
    "519df2fc812cffa7128e5f97e88adffb3ea215f0cb6d272031e2637e084a92d7",  # ltn8.pgf
    "32d9b38b1df172db43bc71d0bd647c916d3a43f9dea6a6b0f1ce74fd658473aa",  # ltn9.pgf
    "f02240d67dcaacb02926beca7c64c50d29c0c9adb79c676ebdd3ffaebfa3b16b",  # ltn10.pgf
    "a84d95ba545bdf8d000eb53b8ce792c84cedd3d4e335e88d7578d42be6d41644",  # ltn11.pgf
    "02edcd49609c847cead4a0f89517709d5b55eb26e49bd72a873a763004bf2e02",  # ltn12.pgf
    "3df05e0f06d39f94dee6054037aa35986246277752fcf1d7c193018b189e4e8b",  # ltn13.pgf
    "fbf4442a023b1bc83a5a00086b16dfb402f364654ab809e1f6913da877cb1453",  # ltn14.pgf
    "7e54faefa7fa575dcc8c5242b40b51f52db764b73ef65efdd559cff58ddb7c3e",  # ltn15.pgf
    # hrydgard/ppsspp@62047545f04cc1852908506695ff657a4cd9d50b (2021-01-20), assets/flash0/font/
    "0c9a7b89c178a98984ed3b8ea95f0f3edbae1d851a9ca79361ac859d5a7c784b",  # jpn0.pgf
    # hrydgard/ppsspp@f1ccccdc9a1d6dfef32597b4e7932059231be3b6 (2021-01-22), assets/flash0/font/
    "83f901f65a74c8a51de0e56b414e0bfe6d7861334d19bf57b4e0b6d5397a86ca",  # jpn0.pgf
    # hrydgard/ppsspp@8c4f11cb6afa6bdc81ae202ee5237801f2443a90 (2021-01-22), assets/flash0/font/
    "9dabcc129bfee943f0e0cad99cc5bcb7b8f6e6d0fc39531b45b7c7be57fa9f29",  # jpn0.pgf
    # hrydgard/ppsspp@6e2430fbdbb6716b0965d837330666f89a27be85 (2021-01-24), assets/flash0/font/
    "dc9f7408cb80f4b66f5a0b40dafe239122f06f71fc86c27dffea332ee26a8813",  # jpn0.pgf
    # hrydgard/ppsspp@5742f2f389f754313161a6f2ac523d9e16a33bd8 (2021-01-28), assets/flash0/font/
    "9f318d25a1c2a7e572985c511a6e3f982041cffd0faf8e6ff64da8a87a675a43",  # ltn0.pgf
    "0cb72bdce33780b78596001fdc6842c244aac06e813fddf28e3437e4db45dd5b",  # ltn1.pgf
    "7ba261c4c756ea62e831cff12fcbe67d56a1c2a15e45d48696d982dc914f4a5b",  # ltn2.pgf
    "82bf8f0b60fd7e513560b29c52f6c721ade05910a144704c17f78aba298d9b08",  # ltn3.pgf
    "203885da7342bb5d77072f5a3732a199046f37b3b2d26abbecb0b76aa03db1a0",  # ltn4.pgf
    "e30b45662d37516c590ca070c831b44a6883587e223abd28082cb017623474f7",  # ltn5.pgf
    "5294d702a2d1cadfbeb8b8a1e2d8f7704ad073104a67b938eb03e215e431a438",  # ltn6.pgf
    "9a4d35eeeee93fe8b5434e8dff79dabf7bbd914ad0d6461fb60d36eb178bf2c8",  # ltn7.pgf
    # hrydgard/ppsspp@da40ed6e81b19b562fe9a58c19df5c039e59db0d (2021-01-30), assets/flash0/font/
    "c8620bdd244585bbf993a82903168f6ec8464d4cbcad238d4e0ca5623c1ba3c1",  # ltn8.pgf
    "e250f2782e0c72707ce16f73331e2604daa84c488c8b9c959e2296867c1710bf",  # ltn9.pgf
    "ff8fcdebca739eaa81c07caabd78b3e3eafb89cac0e68caf7aaf7a2b25514e85",  # ltn10.pgf
    "ebb94efd7d18143d6c6d54ae7aa5cc801d8c56dc7ca613c779cc60dc8b640a16",  # ltn11.pgf
    "c71f3dee57b8a105a88791dd041a008adb918b14a3892a1b18ed6a63f721201a",  # ltn12.pgf
    "25b0efa79cf1b6656b35356483ad2b1191626550d59b5943ed681e82b945bd73",  # ltn13.pgf
    "bc2c9389ac33f120b4be4fd3efc467de12fd886ec147969a4309ea270ccc1da3",  # ltn14.pgf
    "4ab43137ebed97d8bbec829b32d4b343f9895e0b14ec1c884c1b44dfa35d5204",  # ltn15.pgf
    # hrydgard/ppsspp@42f28ce41ef9964d616cdc09379dd982e1e33943 (2021-02-06), assets/flash0/font/
    "ee27c33b91430335d50c5ae4de1eb5bba84893802ea38727d153d2a59a7e16d2",  # jpn0.pgf
    # hrydgard/ppsspp@41927a5c39951cb068805b86d3dcd74417e40845 (2021-02-15), assets/flash0/font/
    "a59fca642fb5d501e67ee57d95407415d7cd9e8fad2e2fadfe868d713912c628",  # jpn0.pgf
    # hrydgard/ppsspp@848142d2aca94eb9d7178ee28f17aa0c4ea1a94f (2021-02-23), assets/flash0/font/
    "fe976fafcf4078ca30b9ec0ffa1146030db4c890ec4b8bfe002127fbb1136c1a",  # jpn0.pgf
    "9d31c490fc956621b1707562d99cecbdb4fd5995e81fe9bc2d47a7d277207040",  # ltn0.pgf
    "ecfdfc8c7f1ee367dda81ff3177af626b5bf64014315ce0c389903c40964b4bf",  # ltn2.pgf
    "5cf844980333075ecdab91c9ed0f9200a7343bbfea46b9205b02f97b99f73d4b",  # ltn4.pgf
    "a0fb14123ceb1ef81cb230cbf0688de594d0725de234a2ad710cd9b89391c7e9",  # ltn6.pgf
    # hrydgard/ppsspp@a384b62247f1971555fb68f796c01df8fb8f984a (2021-03-14), assets/flash0/font/
    "1184f16bc89bad1fea49093806784470281af54f279b57ef0378ce33e6603870",  # ltn0.pgf
    "250babd8a242d7eaaa84284c78d5e2273b29e61d0d8c01b65414e3248d7c1164",  # ltn4.pgf
    # hrydgard/ppsspp@09f90969ecf0c02271149062e6167fbc13cd4f26 (2021-06-18), assets/flash0/font/
    "75b0c9e8fc6aee7945c818e68315afacc61e479e37adebe32dae21cf0ed8d5a9",  # jpn0.pgf
    # hrydgard/ppsspp@082bf0d67cbdf7d359fa12c24ca4600819d00ad2 (2021-07-04), assets/flash0/font/
    "3a2c2a331ad2e84da8866aa37a305217cc47d884cc7a58a70ce4fce7f3e6e07a",  # jpn0.pgf
    # hrydgard/ppsspp@b51451d6720e2cabfba2709dc3c58941e987a3bb (2022-11-30), assets/flash0/font/
    "63267092a2079f386b56ac7bf99ca4334362e23fe0ae0d010833e866c22d7356",  # jpn0.pgf
    # hrydgard/ppsspp@eb65f586ec3203250dfd8ce63c536cb1bdb48a4b (2022-12-12), assets/flash0/font/
    "4051120c55c7e1cebeee62f823ebb37a71d79a2a8024b5a84cdb06594b9bf463",  # jpn0.pgf
    # hrydgard/ppsspp@e4aa9e517f3cee51081b474906bc3f0c53bd31de (2023-01-29), assets/flash0/font/
    "d363cec75fecfafd1511e370e48c748c30199c694b83a4026be34d799b192917",  # jpn0.pgf
    # jpcsp/jpcsp@8da2d2271d2b68e89abf3ee8b4bee444e1d5dafb (2011-12-25), flash0/font/
    "6bfe027829042be4188378eb49408c5eb59c8ad19bff0128da1ef1f6f56e8977",  # kr0.pgf
    "2e86bcaff9832579c9fc1e260edeb70370927f36eeb710aa73c9839df9dbb505",  # jpn0.pgf
    # jpcsp/jpcsp@ed2de4afb7e449b0bf0ebbc6da3e1dd9d6f473a3 (2014-10-26), flash0/font/
    "90ba7e28b905ac5604511270ad10043253f0c6cf998950499e2b72fc81d465ae",  # jpn0.pgf
    # jpcsp/jpcsp@506827cfca8b745fae5ade39da8961446cb8a84e (2020-11-16), flash0/font/
    "5fc7dfe74fc519b63954e03bba2ee122808420ad131ddff1d1557ee3bcda915e",  # jpn0.pgf
})


def is_replacement_font(data: bytes, sha256: Optional[str] = None) -> bool:
    """True when the bytes are a replacement for a PSP font rather than the user's own: a known
    replacement file (by its SHA-256) or one of this project's stand-ins (by its font name).

    A pure function of the bytes; ``sha256`` is their digest when the caller has it already.
    """
    name = bytes(data[FONT_NAME_OFFSET:FONT_NAME_OFFSET + FONT_NAME_SIZE])
    if len(name) >= len(PROJECT_STANDIN_NAME_PREFIX) and name.startswith(PROJECT_STANDIN_NAME_PREFIX):
        return True
    digest = sha256 if sha256 is not None else hashlib.sha256(data).hexdigest()
    return digest in KNOWN_REPLACEMENT_SHA256


#: What a status or outcome line adds for a replacement font.
REPLACEMENT_NOTE = "a replacement font, not your PSP's own"


class FontValidationError(ValueError):
    """Raised when a PGF font file fails the reader's checks."""


class FontImportError(RuntimeError):
    """Raised when font import cannot find, read, or stage font files."""


class _Refused(Exception):
    """Internal: the reader refuses the image by the named rule."""

    def __init__(self, refusal: str) -> None:
        super().__init__(refusal)
        self.refusal = refusal


# --- The reader's rules --------------------------------------------------------------------


def _u16(data: bytes, offset: int) -> int:
    returned: int = struct.unpack_from("<H", data, offset)[0]
    return returned


def _u32(data: bytes, offset: int) -> int:
    returned: int = struct.unpack_from("<I", data, offset)[0]
    return returned


def _i32(data: bytes, offset: int) -> int:
    returned: int = struct.unpack_from("<i", data, offset)[0]
    return returned


def _packed_size(count: int, bits: int) -> int:
    words = (count * bits + 31) // 32
    return words * 4


def _get_bits(data: bytes, base: int, span: int, bit_offset: int, bit_count: int) -> Optional[int]:
    """Read bit_count bits at bit_offset inside the region data[base:base+span]; None if invalid."""
    end_bit = bit_offset + bit_count
    if bit_count == 0 or bit_count > 32 or end_bit > span * 8:
        return None
    first_byte = bit_offset // 8
    shift = bit_offset % 8
    bytes_needed = (shift + bit_count + 7) // 8
    packed = 0
    for i in range(bytes_needed):
        packed |= data[base + first_byte + i] << (8 * i)
    packed >>= shift
    if bit_count == 32:
        return packed & 0xFFFFFFFF
    return packed & ((1 << bit_count) - 1)


class _Image:
    """The reader's state for one image: directory fields, sections, and glyph checks."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.size = len(data)
        self.first_glyph = 0
        self.glyph_count = 0
        self.char_map_count = 0
        self.char_map_bits = 0
        self.char_pointer_bits = 0
        self.shadow_count = 0
        self.metric_counts = [0] * TABLE_COUNT
        self.metric_offsets = [0] * TABLE_COUNT
        self.shadow_map_offset = 0
        self.subcharmap_offset = 0
        self.subcharmap_count = 0
        self.char_map_offset = 0
        self.char_pointer_offset = 0
        self.glyph_data_offset = 0
        self.revision = 0

    # Directory -------------------------------------------------------------------------
    def parse_directory(self) -> None:
        data = self.data
        size = self.size
        if size > 16 * 1024 * 1024:
            raise _Refused("TOO_LARGE")
        if size < BASE_HEADER_SIZE:
            raise _Refused("TRUNCATED")
        if _u16(data, 0) != 0:
            raise _Refused("HEADER_OFFSET")
        if data[4:8] != b"PGF0":
            raise _Refused("MAGIC")
        header_size = _u16(data, 2)
        revision = _i32(data, 8)
        version = _i32(data, 12)
        if revision < 0 or revision > 3 or version < 0:
            raise _Refused("REVISION")
        if (header_size not in (BASE_HEADER_SIZE, REV3_HEADER_SIZE)) or (
            header_size == REV3_HEADER_SIZE and revision != 3
        ):
            raise _Refused("HEADER_SIZE")
        if size < header_size:
            raise _Refused("TRUNCATED")

        self.revision = revision
        self.char_map_count = _u32(data, 0x10)
        pointer_count = _u32(data, 0x14)
        char_map_bits = _u32(data, 0x18)
        pointer_bits = _u32(data, 0x1C)
        first = _u16(data, 0xB6)
        last = _u16(data, 0xB8)
        shadow_count = _u32(data, 0x16C)
        shadow_bits = _u32(data, 0x170)
        if first > last:
            raise _Refused("GLYPH_RANGE")
        if self.char_map_count > last - first + 1:
            raise _Refused("COUNTS")
        if (
            char_map_bits == 0
            or char_map_bits > 32
            or pointer_bits == 0
            or pointer_bits > 32
            or self.char_map_count > MAX_COUNT
            or pointer_count > MAX_COUNT
            or shadow_count > MAX_COUNT
        ):
            raise _Refused("COUNTS")
        glyph_count = pointer_count
        if glyph_count == 0:
            raise _Refused("NO_GLYPHS")
        if glyph_count > MAX_COUNT:
            raise _Refused("COUNTS")
        if shadow_count == 0:
            shadow_ok = shadow_bits == 0 or shadow_bits == 16
        else:
            shadow_ok = shadow_bits == 16
        if not shadow_ok:
            raise _Refused("COUNTS")

        self.first_glyph = first
        self.glyph_count = glyph_count
        self.char_map_bits = char_map_bits
        self.char_pointer_bits = pointer_bits
        self.shadow_count = shadow_count
        self.metric_counts = [data[0x102 + i] for i in range(TABLE_COUNT)]

        rev3_a = _u16(data, 0x18C) if header_size == REV3_HEADER_SIZE else 0
        rev3_b = _u16(data, 0x194) if header_size == REV3_HEADER_SIZE else 0

        cursor = header_size

        def add_section(length: int) -> int:
            nonlocal cursor
            if length > size or cursor > size or length > size - cursor:
                raise _Refused("TRUNCATED")
            offset = cursor
            cursor += length
            return offset

        for i in range(TABLE_COUNT):
            self.metric_offsets[i] = add_section(self.metric_counts[i] * 8)
        self.shadow_map_offset = add_section(_packed_size(shadow_count, shadow_bits))
        if header_size == REV3_HEADER_SIZE:
            self.subcharmap_offset = add_section(rev3_a * 4)
            add_section(rev3_b * 4)
            self.subcharmap_count = rev3_a
        self.char_map_offset = add_section(_packed_size(self.char_map_count, char_map_bits))
        self.char_pointer_offset = add_section(_packed_size(pointer_count, pointer_bits))
        self.glyph_data_offset = cursor

    # Map, pointers, metrics -----------------------------------------------------------
    def map_index(self, code: int) -> Optional[int]:
        """The character-map index of a code, as pgf_map_index computes it (PGF_SPEC.md 3.2)."""
        if self.subcharmap_count == 0:
            return code - self.first_glyph if code >= self.first_glyph else None
        base = 0
        for i in range(self.subcharmap_count):
            start = _u16(self.data, self.subcharmap_offset + i * 4)
            length = _u16(self.data, self.subcharmap_offset + i * 4 + 2)
            if start <= code < start + length:
                return base + code - start
            base += length
        return None

    def map_entry(self, code: int) -> Optional[int]:
        index = self.map_index(code)
        if index is None or index >= self.char_map_count:
            return None
        span = self.size - self.char_map_offset
        value = _get_bits(self.data, self.char_map_offset, span, index * self.char_map_bits, self.char_map_bits)
        if value is None:
            return None
        if value == (1 << self.char_map_bits) - 1 or value >= self.glyph_count:
            return None
        return value

    def pointer_entry(self, glyph_id: int) -> Optional[int]:
        if glyph_id >= self.glyph_count:
            return None
        span = self.size - self.char_pointer_offset
        units = _get_bits(
            self.data, self.char_pointer_offset, span, glyph_id * self.char_pointer_bits, self.char_pointer_bits
        )
        if units is None:
            return None
        return units * 4

    def table_pair(self, table: int, index: int) -> Optional[Tuple[int, int]]:
        if table >= TABLE_COUNT:
            return None
        if index == METRIC_MISSING_INDEX:
            # Index 255 is "no entry" whatever the table's count; console font files use it in
            # shorter tables. The zero pair is a project decision, unmeasured on PSP.
            return (0, 0)
        if index >= self.metric_counts[table]:
            return None
        entry = self.metric_offsets[table] + index * 8
        return _i32(self.data, entry), _i32(self.data, entry + 4)

    # Glyphs ---------------------------------------------------------------------------
    def primary_glyph(self, glyph_id: int) -> Optional[Dict[str, Any]]:
        relative = self.pointer_entry(glyph_id)
        if relative is None or relative > self.size - self.glyph_data_offset:
            return None
        absolute64 = self.glyph_data_offset + relative
        if absolute64 > self.size:
            return None
        absolute = absolute64
        available = self.size - absolute
        if available < 12:
            return None
        base = absolute

        def bits(bit_offset: int, bit_count: int) -> Optional[int]:
            return _get_bits(self.data, base, available, bit_offset, bit_count)

        shadow_offset = bits(0, 14)
        width = bits(14, 7)
        height = bits(21, 7)
        adjust_x = bits(28, 7)
        adjust_y = bits(35, 7)
        row_order = bits(42, 2)
        # Bits 44-47: dimension, X, Y and advance groups, 1 = table index, 0 = inline pair; an
        # inline advance (bit 47 clear) is unmeasured and refused (pgf_primary_glyph).
        flags = bits(44, 4)
        shadow_id = bits(55, 9)
        if (shadow_offset is None or width is None or height is None or adjust_x is None
                or adjust_y is None or row_order is None or flags is None or shadow_id is None):
            return None
        if not flags & 8:
            return None

        cursor = absolute + 8
        dimension = y_metrics = None
        for group in range(3):
            if flags & (1 << group):
                if cursor >= self.size:
                    return None
                field = self.data[cursor]
                cursor += 1
                pair = self.table_pair(group, field)
                if pair is None:
                    return None
            else:
                if cursor > self.size or self.size - cursor < 8:
                    return None
                pair = (_i32(self.data, cursor), _i32(self.data, cursor + 4))
                cursor += 8
            # The X-adjustment pair is read and bounds-checked above; only the other two are kept.
            if group == 0:
                dimension = pair
            elif group == 2:
                y_metrics = pair
        if dimension is None or y_metrics is None or cursor >= self.size:
            return None
        field = self.data[cursor]
        cursor += 1
        advance = self.table_pair(3, field)
        if advance is None:
            return None
        record_size = cursor - absolute
        descender = y_metrics[0] - dimension[1]
        if descender < -(1 << 31) or descender > (1 << 31) - 1:
            return None
        return {
            "record_offset": absolute,
            "record_size": record_size,
            "bitmap_offset": cursor,
            "shadow_offset": shadow_offset,
            "width": width,
            "height": height,
            "row_order": row_order,
            "shadow_id": shadow_id,
            "composite": 0,
            "shadow_row_order": 0,
        }

    def checked_glyph(self, glyph_id: int, allow_composite: bool) -> Optional[Dict[str, Any]]:
        glyph = self.primary_glyph(glyph_id)
        if glyph is None:
            return None
        if glyph["shadow_id"] != 0:
            if glyph["shadow_id"] > self.shadow_count:
                return None
            pos = self.shadow_map_offset + (glyph["shadow_id"] - 1) * 2
            shadow_code = _u16(self.data, pos)
            if self.map_entry(shadow_code) is None:
                # A shadow entry naming character 0, absent from the map, names no shadow glyph;
                # any other unmapped code is refused (pgf_checked_glyph).
                if shadow_code != 0:
                    return None
                glyph["shadow_row_order"] = 0
                return self._finish(glyph, allow_composite)
            if glyph["shadow_offset"] < glyph["record_size"]:
                return None
            target = glyph["record_offset"] + glyph["shadow_offset"]
            if target > self.size:
                return None
            if target == self.size:
                # Project decision, unmeasured on PSP: treat an EOF target as an absent optional
                # prefix so the primary glyph stays usable without reading past the image.
                shadow_row = 0
            else:
                if self.size - target < 6:
                    return None
                row = _get_bits(self.data, target, self.size - target, 42, 2)
                if row is None:
                    return None
                shadow_row = row
            glyph["shadow_row_order"] = shadow_row
        return self._finish(glyph, allow_composite)

    def _finish(self, glyph: Dict[str, Any], allow_composite: bool) -> Optional[Dict[str, Any]]:
        if glyph["row_order"] != 3:
            return glyph
        if not allow_composite:
            return None
        return self.resolve_composite(glyph)

    def resolve_composite(self, glyph: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        if glyph["bitmap_offset"] > self.size or self.size - glyph["bitmap_offset"] < COMPOSITE_PARTS * 2:
            return None
        present = 0
        for part in range(COMPOSITE_PARTS):
            code = _u16(self.data, glyph["bitmap_offset"] + part * 2)
            component_id = self.map_entry(code)
            if component_id is None:
                # A code 0 the map does not hold names no component (pgf_resolve_composite).
                if code != 0:
                    return None
                continue
            component = self.checked_glyph(component_id, False)
            if component is None:
                return None
            if component["row_order"] not in (1, 2) or component["width"] == 0 or component["height"] == 0:
                return None
            present += 1
        if present == 0:
            return None
        glyph["composite"] = 1
        return glyph

    def has_char(self, code: int) -> bool:
        glyph_id = self.map_entry(code)
        if glyph_id is None:
            return False
        glyph = self.checked_glyph(glyph_id, True)
        if glyph is None:
            return False
        returned: bool = glyph["width"] != 0 and glyph["height"] != 0
        return returned


def pgf_verdict(data: bytes) -> Dict[str, Any]:
    """Run the reader's checks on an in-memory image, mirroring pgf_validate_memory.

    Returns a dict with ``refusal`` (a name from REFUSAL_NAMES; "NONE" when accepted), and,
    when accepted, the header facts and coverage flags. Never raises for a bad image.
    """
    image = _Image(bytes(data))
    try:
        image.parse_directory()
        for glyph_id in range(image.glyph_count):
            if image.checked_glyph(glyph_id, True) is None:
                raise _Refused("GLYPH")
    except _Refused as refused:
        return {"refusal": refused.refusal, "ok": False}
    raw = image.data
    return {
        "refusal": "NONE",
        "ok": True,
        "revision": image.revision,
        "header_size": _u16(raw, 2),
        "first_glyph": image.first_glyph,
        "last_glyph": _u16(raw, 0xB8),
        "glyph_count": image.glyph_count,
        "char_map_count": image.char_map_count,
        "nominal_h": _i32(raw, 0x24),
        "nominal_v": _i32(raw, 0x28),
        "has_latin": int(all(image.has_char(code) for code in PROBE_LATIN)),
        "has_kana": int(any(image.has_char(code) for code in PROBE_KANA)),
        "has_hangul": int(any(image.has_char(code) for code in PROBE_HANGUL)),
    }


def classify_verdict(verdict: Dict[str, Any]) -> Tuple[Optional[str], str]:
    """Name the slot for an accepted verdict, as nk_font_classify_verdict does."""
    if not verdict.get("ok"):
        return None, "the reader refused the file"
    if verdict["has_hangul"] and verdict["has_kana"]:
        return None, "covers both Hangul and kana, so it names no slot"
    if verdict["has_hangul"]:
        return "korean", ""
    if verdict["has_kana"]:
        return "japanese", ""
    if verdict["has_latin"]:
        return "latin", ""
    return None, "covers none of the slot probe characters"


def _inspect(data: bytes) -> Dict[str, Any]:
    verdict = pgf_verdict(data)
    sha256 = hashlib.sha256(data).hexdigest()
    result: Dict[str, Any] = {
        "size": len(data),
        "sha256": sha256,
        "replacement": is_replacement_font(data, sha256),
        "accepted": bool(verdict["ok"]),
        "refusal": "" if verdict["ok"] else REFUSAL_TEXT[verdict["refusal"]],
        "verdict": verdict,
        "slot": None,
        "slot_reason": "",
    }
    if verdict["ok"]:
        result["slot"], result["slot_reason"] = classify_verdict(verdict)
    else:
        result["slot_reason"] = result["refusal"]
    return result


@functools.lru_cache(maxsize=256)
def _inspect_file_cached(path: str, size: int, modified_ns: int) -> Dict[str, Any]:
    """Reuse a PGF verdict while its resolved path, size, and modification time are unchanged."""
    del size, modified_ns
    return _inspect(Path(path).read_bytes())


def _inspect_path(path: Path) -> Dict[str, Any]:
    resolved = path.resolve(strict=True)
    info = resolved.stat()
    return _inspect_file_cached(str(resolved), info.st_size, info.st_mtime_ns)


# --- Public validation -----------------------------------------------------------------


def validate_pgf_data(data: bytes, filename: str = "PGF") -> Dict[str, Any]:
    """Validate PGF bytes with the reader's rules and report size, SHA-256, and slot.

    Raises FontValidationError naming the refusal. Accepted images return the header
    facts, the coverage flags, and the slot the image names (None when it names none).
    """
    info = _inspect(data)
    if not info["accepted"]:
        raise FontValidationError(f"Font file '{filename}' is refused by the PGF reader: {info['refusal']}.")
    verdict = info["verdict"]
    return {
        "size": info["size"],
        "sha256": info["sha256"],
        "revision": verdict["revision"],
        "header_size": verdict["header_size"],
        "first_glyph": verdict["first_glyph"],
        "last_glyph": verdict["last_glyph"],
        "glyph_count": verdict["glyph_count"],
        "char_map_count": verdict["char_map_count"],
        "sparse": verdict["char_map_count"] > verdict["glyph_count"],
        "nominal_h": verdict["nominal_h"],
        "nominal_v": verdict["nominal_v"],
        "has_latin": bool(verdict["has_latin"]),
        "has_kana": bool(verdict["has_kana"]),
        "has_hangul": bool(verdict["has_hangul"]),
        "slot": info["slot"],
        "slot_reason": info["slot_reason"],
        "replacement": info["replacement"],
    }


def validate_pgf_file(file_path: Path) -> Dict[str, Any]:
    """Read and validate a PGF font file from disk, refusing a file above the ceiling unread."""
    if not file_path.is_file():
        raise FontValidationError(f"Font file '{file_path.name}' does not exist or is not a file.")
    try:
        if file_path.stat().st_size > PGF_MAX_BYTES:
            raise FontValidationError(
                f"Font file '{file_path.name}' is refused by the PGF reader: {REFUSAL_TEXT['TOO_LARGE']}."
            )
        data = file_path.read_bytes()
    except OSError as exc:
        raise FontValidationError(f"Font file '{file_path.name}' could not be read: {exc}") from exc
    return validate_pgf_data(data, filename=file_path.name)


# --- Cache layout and manifest v2 -----------------------------------------------------


def get_font_cache_dir(user_data_root: Optional[Path] = None) -> Path:
    """Return the versioned per-user font cache directory: <user data>/fonts/v2."""
    root = (user_data_root or default_user_data_root()).expanduser().resolve(strict=False)
    return root / CACHE_PARENT / CACHE_SUBDIR


def _reject_duplicate_keys(pairs: List[Tuple[str, Any]]) -> Dict[str, Any]:
    """object_pairs_hook: refuse a repeated key, as the C manifest reader does (nk_json.c)."""
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON object key: {key!r}")
        result[key] = value
    return result


def _load_manifest_uncached(path: Path) -> Tuple[str, str, Dict[str, Dict[str, Any]]]:
    """Return (status, message, entries) with entries keyed by slot id.

    Status is MISSING (no manifest), INVALID (malformed, or an entry this build does not
    accept), or OK. The entries are the manifest's own values; file checks are separate.
    """
    if not path.is_file():
        return "MISSING", "No imported PSP font.", {}
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except (OSError, ValueError):
        return "INVALID", "PSP font manifest is malformed; run fonts import <folder>.", {}
    if not isinstance(manifest, dict):
        return "INVALID", "PSP font manifest is malformed; run fonts import <folder>.", {}
    schema = manifest.get("schema_version")
    if type(schema) is not int or schema != MANIFEST_SCHEMA_VERSION:
        return "INVALID", "PSP font manifest schema is not version 2; run fonts import <folder>.", {}
    files = manifest.get("files")
    if not isinstance(files, dict):
        return "INVALID", "PSP font manifest lists no files; run fonts import <folder>.", {}
    entries: Dict[str, Dict[str, Any]] = {}
    for name, entry in files.items():
        slot = next((s for s in SLOTS if SLOT_FILES[s] == name), None)
        size = entry.get("size") if isinstance(entry, dict) else None
        sha = entry.get("sha256") if isinstance(entry, dict) else None
        ok = (
            slot is not None
            and slot not in entries
            and isinstance(entry, dict)
            and entry.get("slot") == slot
            and isinstance(size, int)
            and not isinstance(size, bool)
            and size > 0
            and isinstance(sha, str)
            and len(sha) == 64
            and entry.get("source") == MANIFEST_SOURCE_USER
        )
        if not ok or slot is None:
            return "INVALID", (
                "PSP font manifest has an entry this build does not accept; run fonts import <folder>."
            ), {}
        entries[slot] = {"size": size, "sha256": sha}
    latin_indices = manifest.get("latin_indices", {})
    if not isinstance(latin_indices, dict):
        return "INVALID", "PSP font manifest has invalid Latin index entries; run fonts import <folder>.", {}
    for name, entry in latin_indices.items():
        index = latin_index_from_filename(name)
        size = entry.get("size") if isinstance(entry, dict) else None
        sha = entry.get("sha256") if isinstance(entry, dict) else None
        ok = (
            index is not None
            and name == LATIN_INDEX_FILES[index]
            and f"latin_index_{index}" not in entries
            and isinstance(entry, dict)
            and entry.get("slot") == "latin"
            and type(entry.get("index")) is int
            and entry.get("index") == index
            and isinstance(size, int)
            and not isinstance(size, bool)
            and size > 0
            and isinstance(sha, str)
            and len(sha) == 64
            and entry.get("source") == MANIFEST_SOURCE_USER
        )
        if not ok:
            return "INVALID", (
                "PSP font manifest has a Latin index entry this build does not accept; run fonts import <folder>."
            ), {}
        entries[f"latin_index_{index}"] = {"index": index, "size": size, "sha256": sha}
    if not entries:
        return "INVALID", "PSP font manifest lists no files; run fonts import <folder>.", {}
    return "OK", "Imported PSP fonts are listed in the manifest.", entries


@functools.lru_cache(maxsize=64)
def _load_manifest_cached(path: str, size: int, modified_ns: int
                          ) -> Tuple[str, str, Dict[str, Dict[str, Any]]]:
    del size, modified_ns
    return _load_manifest_uncached(Path(path))


def _load_manifest(cache_dir: Path) -> Tuple[str, str, Dict[str, Dict[str, Any]]]:
    """Load one manifest version, reusing parsing until its file identity changes."""
    path = cache_dir / MANIFEST_NAME
    if not path.is_file():
        return "MISSING", "No imported PSP font.", {}
    try:
        resolved = path.resolve(strict=True)
        info = resolved.stat()
        status, message, entries = _load_manifest_cached(
            str(resolved), info.st_size, info.st_mtime_ns
        )
    except (OSError, RuntimeError):
        return "INVALID", "PSP font manifest is malformed; run fonts import <folder>.", {}
    return status, message, {key: dict(entry) for key, entry in entries.items()}


def _check_slot_file(cache_dir: Path, slot: str, entry: Dict[str, Any]) -> Tuple[bool, str]:
    """Check a manifest entry against its file: present, size and SHA-256, accepted, right slot."""
    path = cache_dir / SLOT_FILES[slot]
    if not path.is_file():
        return False, "its file is missing"
    try:
        info = _inspect_path(path)
    except (OSError, RuntimeError) as exc:
        return False, f"its file cannot be read ({getattr(exc, 'strerror', None) or exc})"
    if not info["accepted"]:
        return False, f"the reader refused it: {info['refusal']}"
    if info["slot"] != slot:
        return False, f"it does not name the {SLOT_ROLES[slot]} slot"
    if info["size"] != entry["size"] or info["sha256"] != entry["sha256"]:
        return False, "its size or SHA-256 differs from the manifest"
    return True, ""


def _check_latin_index_file(cache_dir: Path, index: int, entry: Dict[str, Any]) -> Tuple[bool, str]:
    """Check one indexed Latin cache file against its manifest and the PGF reader."""
    path = cache_dir / LATIN_INDEX_FILES[index]
    if not path.is_file():
        return False, "its file is missing"
    try:
        info = _inspect_path(path)
    except (OSError, RuntimeError) as exc:
        return False, f"its file cannot be read ({getattr(exc, 'strerror', None) or exc})"
    if not info["accepted"]:
        return False, f"the reader refused it: {info['refusal']}"
    if info["slot"] != "latin":
        return False, "it does not name the Latin slot"
    if info["size"] != entry["size"] or info["sha256"] != entry["sha256"]:
        return False, "its size or SHA-256 differs from the manifest"
    return True, ""


def inspect_font_cache(
    user_data_root: Optional[Path] = None,
    fallback_root: Optional[Path] = None,
    cache_dir: Optional[Path] = None,
) -> Tuple[str, str]:
    """Check the imported-font manifest and each file: ('OK', 'MISSING', or 'INVALID').

    ``fallback_root`` is accepted for the existing call sites and is not a font source.
    """
    del fallback_root
    cdir = cache_dir or get_font_cache_dir(user_data_root)
    status, message, entries = _load_manifest(cdir)
    if status != "OK":
        return status, message
    for slot in SLOTS:
        if slot not in entries:
            continue
        ok, reason = _check_slot_file(cdir, slot, entries[slot])
        if not ok:
            return "INVALID", (
                f"Imported {SLOT_ROLES[slot]} font is invalid: {reason}; run fonts import <folder>."
            )
    for index in range(len(LATIN_INDEX_FILES)):
        key = f"latin_index_{index}"
        if key not in entries:
            continue
        ok, reason = _check_latin_index_file(cdir, index, entries[key])
        if not ok:
            return "INVALID", (
                f"Imported Latin index {index + 1} font is invalid: {reason}; run fonts import <folder>."
            )
    return "OK", "Imported PSP fonts are listed in the manifest."


def slot_states(
    user_data_root: Optional[Path] = None,
    project_root: Optional[Path] = None,
) -> Dict[str, Dict[str, Any]]:
    """Per-slot preflight: the imported font when it checks, else the project font, else none.

    ``replacement`` is true when the imported font is a replacement (is_replacement_font), which
    is served but is not the user's own PSP font.
    """
    cdir = get_font_cache_dir(user_data_root)
    status, message, entries = _load_manifest(cdir)
    states: Dict[str, Dict[str, Any]] = {}
    for slot in SLOTS:
        role = SLOT_ROLES[slot]
        reason = ""
        if status == "OK" and slot in entries:
            ok, why = _check_slot_file(cdir, slot, entries[slot])
            if ok:
                if _inspect_path(cdir / SLOT_FILES[slot])["replacement"]:
                    states[slot] = {"source": "user", "replacement": True,
                                    "detail": f"{role}: a replacement font."}
                else:
                    states[slot] = {"source": "user", "replacement": False,
                                    "detail": f"{role}: your imported font."}
                continue
            reason = why
        elif status == "INVALID":
            reason = message
        if project_root is not None:
            project_path = Path(project_root) / PROJECT_SUBDIR / SLOT_FILES[slot]
            if project_path.is_file():
                try:
                    info = _inspect_path(project_path)
                except (OSError, RuntimeError):
                    info = {"accepted": False}
                if info.get("accepted") and info.get("slot") == slot:
                    states[slot] = {"source": "project", "replacement": False,
                                    "detail": f"{role}: project font."}
                    continue
        if reason:
            detail = f"{role}: missing ({reason})."
        else:
            detail = f"{role}: missing. Import a font from your PSP or install the project font."
        states[slot] = {"source": "none", "replacement": False, "detail": detail}
    return states


def resolve_latin_index(
    user_data_root: Optional[Path], project_font_dir: Optional[Path], index: int
) -> Dict[str, Any]:
    """Resolve a Latin registry index by exact file, same-group file, legacy-only cache, project,
    then legacy as a last fallback.

    ``served_index`` is 0..15 for an indexed user file, -1 for the project font, and -2 for a
    legacy unindexed user import. An old cache containing no indexed files keeps serving its
    legacy file for every index; mixed/newer caches use it only when the project font is absent.
    A project resolution also names its ``project_file`` and ``project_style_index``, the Latin
    index whose family and style that file carries: the index's own face first, then the regular
    face of its size group, then the larger size's regular face.
    """
    if index < 0 or index >= len(LATIN_INDEX_FILES):
        raise ValueError(f"Latin index must be between 0 and {len(LATIN_INDEX_FILES) - 1}.")
    cdir = get_font_cache_dir(user_data_root)
    status, _, entries = _load_manifest(cdir)
    if status == "OK":
        exact_key = f"latin_index_{index}"
        if exact_key in entries and _check_latin_index_file(cdir, index, entries[exact_key])[0]:
            return {"source": "user", "served_index": index}
        group_start = (index // LATIN_SIZE_GROUP_COUNT) * LATIN_SIZE_GROUP_COUNT
        group_end = group_start + LATIN_SIZE_GROUP_COUNT
        for distance in range(1, LATIN_SIZE_GROUP_COUNT):
            for candidate in (index - distance, index + distance):
                key = f"latin_index_{candidate}"
                if group_start <= candidate < group_end and key in entries and \
                        _check_latin_index_file(cdir, candidate, entries[key])[0]:
                    return {"source": "user", "served_index": candidate}
        has_latin_indices = any(
            f"latin_index_{candidate}" in entries for candidate in range(len(LATIN_INDEX_FILES))
        )
        if not has_latin_indices and "latin" in entries and \
                _check_slot_file(cdir, "latin", entries["latin"])[0]:
            return {"source": "user", "served_index": -2}

    if project_font_dir is not None:
        group_regular = LATIN_SIZE_GROUP_COUNT if index >= LATIN_SIZE_GROUP_COUNT else 0
        for name, style in ((PROJECT_LATIN_INDEX_FILES[index], index),
                            (PROJECT_LATIN_INDEX_FILES[group_regular], group_regular),
                            (SLOT_FILES["latin"], 0)):
            project_path = Path(project_font_dir) / name
            try:
                info = _inspect_path(project_path) if project_path.is_file() else {"accepted": False}
            except (OSError, RuntimeError):
                info = {"accepted": False}
            if info.get("accepted") and info.get("slot") == "latin":
                return {"source": "project", "served_index": -1, "project_file": name,
                        "project_style_index": style}

    if status == "OK" and "latin" in entries and _check_slot_file(cdir, "latin", entries["latin"])[0]:
        return {"source": "user", "served_index": -2}
    return {"source": "none", "served_index": -3}


def latin_index_states(
    user_data_root: Optional[Path] = None,
    project_root: Optional[Path] = None,
) -> List[Dict[str, Any]]:
    """Describe every Latin registry index and identify any exact-file fallback.

    ``replacement`` is true when an imported file serves the index and that file is a
    replacement (is_replacement_font); an exact one says "replacement" instead of "exact".
    """
    project_font_dir = Path(project_root) / PROJECT_SUBDIR if project_root is not None else None
    cdir = get_font_cache_dir(user_data_root)
    states: List[Dict[str, Any]] = []
    for index, requested in enumerate(LATIN_INDEX_FILES):
        resolution = resolve_latin_index(user_data_root, project_font_dir, index)
        served_index = resolution["served_index"]
        replacement = False
        if resolution["source"] == "user":
            served = LATIN_INDEX_FILES[served_index] if served_index >= 0 else SLOT_FILES["latin"]
            try:
                replacement = bool(_inspect_path(cdir / served)["replacement"])
            except (OSError, RuntimeError):
                replacement = False
        if served_index == index:
            detail = f"L{index + 1} {'replacement' if replacement else 'exact'} {requested}"
        elif served_index >= 0:
            detail = f"L{index + 1} missing; fallback {LATIN_INDEX_FILES[served_index]}"
        elif served_index == -1:
            detail = f"L{index + 1} missing; project {resolution['project_file']}"
        elif served_index == -2:
            detail = f"L{index + 1} missing; legacy nkltn.pgf"
        else:
            detail = f"L{index + 1} missing {requested}"
        states.append({"source": resolution["source"], "served_index": served_index,
                       "replacement": replacement, "detail": detail})
    return states


def own_font_count(slots: Dict[str, Dict[str, Any]], latin: List[Dict[str, Any]]) -> int:
    """How many of the 18 PSP fonts are served by the user's own import of exactly that font:
    not a stand-in, not a replacement (nk_font_own_font_count_from_states)."""
    own = sum(1 for slot in ("japanese", "korean")
              if slots[slot]["source"] == "user" and not slots[slot].get("replacement"))
    own += sum(1 for index, state in enumerate(latin)
               if state["source"] == "user" and state["served_index"] == index
               and not state.get("replacement"))
    return own


def resolve_font_directory(
    user_data_root: Optional[Path] = None,
    fallback_root: Optional[Path] = None,
) -> Optional[Path]:
    """Resolve the cache-or-project font directory: the cache when its manifest checks.

    The runtime launcher does not pass this to the child as SR_FONTDIR. The cache travels as
    SR_USER_DATA_ROOT, and SR_FONTDIR names only the project fonts (build_launch_plan).
    """
    cdir = get_font_cache_dir(user_data_root)
    status, _ = inspect_font_cache(cache_dir=cdir)
    if status == "OK":
        return cdir
    if fallback_root:
        fallback = fallback_root / PROJECT_SUBDIR
        if fallback.is_dir():
            return fallback
    return None


# --- Import and removal ---------------------------------------------------------------


def scan_source_folder(folder: Path) -> List[Path]:
    """List the .pgf files directly inside folder (not subfolders), whatever their names."""
    if not folder.is_dir():
        raise FontImportError(f"Font source path is not a directory: '{folder}'")
    try:
        names = sorted(
            entry.name for entry in folder.iterdir() if entry.is_file() and entry.name.lower().endswith(".pgf")
        )
    except OSError as exc:
        raise FontImportError(f"Error reading directory '{folder}': {exc}") from exc
    return [folder / name for name in names]


def _write_manifest(cache_dir: Path, entries: Dict[str, Dict[str, Any]]) -> None:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    files = {}
    for slot in SLOTS:
        if slot not in entries:
            continue
        files[SLOT_FILES[slot]] = {
            "slot": slot,
            "size": entries[slot]["size"],
            "sha256": entries[slot]["sha256"],
            "source": MANIFEST_SOURCE_USER,
            "reader_version": READER_VERSION,
        }
    latin_indices = {}
    for index, name in enumerate(LATIN_INDEX_FILES):
        entry = entries.get(f"latin_index_{index}")
        if entry is None:
            continue
        latin_indices[name] = {
            "slot": "latin",
            "index": index,
            "size": entry["size"],
            "sha256": entry["sha256"],
            "source": MANIFEST_SOURCE_USER,
            "reader_version": READER_VERSION,
        }
    manifest = {
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "import_time": now,
        "files": files,
        "latin_indices": latin_indices,
    }
    target = cache_dir / MANIFEST_NAME
    staged: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=cache_dir,
            prefix=f".{MANIFEST_NAME}.", suffix=".tmp", delete=False,
        ) as stream:
            staged = Path(stream.name)
            json.dump(manifest, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(staged, target)
        staged = None
    finally:
        if staged is not None:
            staged.unlink(missing_ok=True)
    _load_manifest_cached.cache_clear()


def _stage_font_bytes(cache_dir: Path, target: Path, data: bytes) -> Path:
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=cache_dir, prefix=f".{target.name}.",
            suffix=".tmp", delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        return temporary
    except OSError:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
        raise


def _backup_font_file(target: Path, backup_dir: Path) -> Path | None:
    if not target.exists():
        return None
    if not target.is_file():
        raise FontImportError(f"Cached font path '{target.name}' is not a file.")
    backup = backup_dir / target.name
    with target.open("rb") as source, backup.open("wb") as destination:
        shutil.copyfileobj(source, destination)
        destination.flush()
        os.fsync(destination.fileno())
    return backup


def _restore_font_file(target: Path, backup: Path | None) -> None:
    if backup is None:
        target.unlink(missing_ok=True)
        return
    staged = _stage_font_bytes(target.parent, target, backup.read_bytes())
    temporary: Path | None = staged
    try:
        os.replace(staged, target)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _import_fonts_unlocked(
    source_folder: Path,
    user_data_root: Optional[Path] = None,
    choose: Optional[Dict[str, str]] = None,
) -> Dict[str, Any]:
    """Import user fonts from one folder and write manifest v2.

    Each file is read once and checked by the reader. A file the reader refuses, or that
    names no slot, is reported and skipped. A slot that several files name is imported only
    when ``choose`` names one of them for that slot; otherwise it is left alone. Originals are
    not touched. Slots this run does not change keep their entries.
    """
    source_folder = Path(source_folder)
    paths = scan_source_folder(source_folder)
    if not paths:
        raise FontImportError(f"No .pgf files are in '{source_folder}'.")
    if len(paths) > IMPORT_MAX_FILES:
        raise FontImportError(f"The folder holds more than {IMPORT_MAX_FILES} .pgf files.")
    choose = dict(choose or {})
    for slot in choose:
        if slot not in SLOTS:
            raise FontImportError(f"Unknown font slot '{slot}'; use one of {', '.join(SLOTS)}.")

    files: List[Dict[str, Any]] = []
    checked: Dict[str, Dict[str, Any]] = {}
    candidates: Dict[str, List[int]] = {slot: [] for slot in SLOTS}
    latin_candidates: Dict[int, List[int]] = {index: [] for index in range(len(LATIN_INDEX_FILES))}
    for path in paths:
        outcome: Dict[str, Any] = {"name": path.name, "slot": None, "imported": False, "detail": ""}
        try:
            size_on_disk = path.stat().st_size
            if size_on_disk > PGF_MAX_BYTES:
                outcome["detail"] = f"refused: {REFUSAL_TEXT['TOO_LARGE']}"
                files.append(outcome)
                continue
            data = path.read_bytes()
        except OSError as exc:
            outcome["detail"] = f"refused: cannot be read ({exc.strerror or exc})"
            files.append(outcome)
            continue
        info = _inspect(data)
        index = len(files)
        checked[path.name] = info
        if not info["accepted"]:
            outcome["detail"] = f"refused: {info['refusal']}"
        elif info["slot"] is None:
            outcome["detail"] = f"refused: {info['slot_reason']}"
        else:
            latin_index = latin_index_from_filename(path.name)
            if latin_index is not None:
                if info["slot"] != "latin":
                    outcome["detail"] = f"refused: {path.name} does not name a Latin font"
                else:
                    outcome["slot"] = "latin"
                    outcome["latin_index"] = latin_index
                    outcome["detail"] = f"names Latin index {latin_index + 1}"
                    latin_candidates[latin_index].append(index)
            else:
                outcome["slot"] = info["slot"]
                outcome["latin_index"] = None
                outcome["detail"] = f"names the {SLOT_ROLES[info['slot']]} slot"
                candidates[info["slot"]].append(index)
        files.append(outcome)

    selected: Dict[str, int] = {}
    for slot in SLOTS:
        options = candidates[slot]
        want = choose.get(slot)
        if want:
            match = [i for i in options if files[i]["name"] == want]
            if not match:
                raise FontImportError(f"The chosen {SLOT_ROLES[slot]} file does not name that slot.")
            selected[slot] = match[0]
        elif len(options) == 1:
            selected[slot] = options[0]
        elif len(options) > 1:
            for i in options:
                files[i]["detail"] = (
                    f"not imported: {len(options)} files name the {SLOT_ROLES[slot]} slot; choose one"
                )
        for i in options:
            if slot in selected and i != selected[slot]:
                files[i]["detail"] = f"not chosen for the {SLOT_ROLES[slot]} slot"

    selected_latin: Dict[int, int] = {}
    for latin_index, options in latin_candidates.items():
        if len(options) == 1:
            selected_latin[latin_index] = options[0]
        elif len(options) > 1:
            for i in options:
                files[i]["detail"] = f"not imported: multiple files name Latin index {latin_index + 1}"

    cache_dir = get_font_cache_dir(user_data_root)
    cache_dir.mkdir(parents=True, exist_ok=True)
    _, _, previous = _load_manifest(cache_dir)

    staged: List[Tuple[Path, Path]] = []
    try:
        for slot, index in selected.items():
            source_path = source_folder / files[index]["name"]
            data = source_path.read_bytes()
            info = _inspect(data)
            if not info["accepted"] or info["slot"] != slot or info["sha256"] != checked[files[index]["name"]]["sha256"]:
                raise FontImportError(
                    f"The chosen {SLOT_ROLES[slot]} file changed or failed its check; try again."
                )
            target = cache_dir / SLOT_FILES[slot]
            tmp = _stage_font_bytes(cache_dir, target, data)
            staged.append((tmp, target))
        for latin_index, index in selected_latin.items():
            source_path = source_folder / files[index]["name"]
            data = source_path.read_bytes()
            info = _inspect(data)
            if (not info["accepted"] or info["slot"] != "latin" or
                    info["sha256"] != checked[files[index]["name"]]["sha256"]):
                raise FontImportError(
                    f"The Latin index {latin_index + 1} file changed or failed its check; try again."
                )
            target = cache_dir / LATIN_INDEX_FILES[latin_index]
            tmp = _stage_font_bytes(cache_dir, target, data)
            staged.append((tmp, target))
    except OSError as exc:
        for tmp, _ in staged:
            tmp.unlink(missing_ok=True)
        raise FontImportError(f"Failed to stage imported fonts into cache: {exc}") from exc
    except FontImportError:
        for tmp, _ in staged:
            tmp.unlink(missing_ok=True)
        raise

    entries = {slot: dict(entry) for slot, entry in previous.items()}
    backups: Dict[Path, Path | None] = {}
    replaced: List[Path] = []
    try:
        with tempfile.TemporaryDirectory(
            prefix=".font-import-backup-", dir=cache_dir
        ) as backup_name:
            backup_dir = Path(backup_name)
            for _tmp, target in staged:
                backups[target] = _backup_font_file(target, backup_dir)
            try:
                for tmp, target in staged:
                    os.replace(tmp, target)
                    replaced.append(target)
                for slot, index in selected.items():
                    info = checked[files[index]["name"]]
                    entries[slot] = {"size": info["size"], "sha256": info["sha256"]}
                    files[index]["imported"] = True
                    files[index]["detail"] = f"imported as the {SLOT_ROLES[slot]} font"
                for latin_index, index in selected_latin.items():
                    info = checked[files[index]["name"]]
                    entries[f"latin_index_{latin_index}"] = {
                        "index": latin_index, "size": info["size"], "sha256": info["sha256"]
                    }
                    files[index]["imported"] = True
                    files[index]["detail"] = f"imported as Latin index {latin_index + 1}"
                if selected or selected_latin:
                    _write_manifest(cache_dir, entries)
            except OSError as exc:
                rollback_error: OSError | None = None
                for target in reversed(replaced):
                    try:
                        _restore_font_file(target, backups[target])
                    except OSError as restore_exc:
                        rollback_error = restore_exc
                _inspect_file_cached.cache_clear()
                if rollback_error is not None:
                    raise FontImportError(
                        "Font import failed and the previous cache could not be fully restored."
                    ) from rollback_error
                raise FontImportError(
                    f"Failed to stage imported fonts into cache: {exc}"
                ) from exc
    except OSError as exc:
        for tmp, _target in staged:
            tmp.unlink(missing_ok=True)
        _inspect_file_cached.cache_clear()
        raise FontImportError(f"Failed to stage imported fonts into cache: {exc}") from exc
    except FontImportError:
        for tmp, _target in staged:
            tmp.unlink(missing_ok=True)
        raise

    _inspect_file_cached.cache_clear()

    # A replacement is imported like any font the reader accepts, and its line says what it is.
    replacement_count = 0
    for outcome in files:
        inspected = checked.get(outcome["name"])
        outcome["replacement"] = bool(inspected and inspected["accepted"] and inspected["replacement"])
        if outcome["replacement"] and outcome["slot"] is not None:
            outcome["detail"] = f"{outcome['detail']} ({REPLACEMENT_NOTE})"
            if outcome["imported"]:
                replacement_count += 1

    return {
        "status": "OK" if selected or selected_latin else "NONE",
        "cache_dir": str(cache_dir),
        "imported_count": len(selected) + len(selected_latin),
        "replacement_count": replacement_count,
        "slots": {slot: SLOT_FILES[slot] for slot in selected},
        "latin_indices": {str(index): LATIN_INDEX_FILES[index] for index in selected_latin},
        "files": {
            **{SLOT_FILES[slot]: {"size": entries[slot]["size"], "sha256": entries[slot]["sha256"]}
               for slot in SLOTS if slot in entries},
            **{LATIN_INDEX_FILES[index]: {
                "size": entries[f"latin_index_{index}"]["size"],
                "sha256": entries[f"latin_index_{index}"]["sha256"],
            } for index in range(len(LATIN_INDEX_FILES)) if f"latin_index_{index}" in entries},
        },
        "outcomes": files,
    }


def import_fonts(
    source_folder: Path,
    user_data_root: Optional[Path] = None,
    choose: Optional[Dict[str, str]] = None,
) -> Dict[str, Any]:
    """Import fonts serially in this process so manifests and selected slots commit together."""
    with _FONT_IMPORT_LOCK:
        return _import_fonts_unlocked(source_folder, user_data_root, choose)


def remove_imports(user_data_root: Optional[Path] = None, slots: Optional[Iterable[str]] = None) -> int:
    """Remove the imported font for each named slot (every slot when None).

    Deletes only the cache's slot files. A slot's manifest entry is dropped once its file
    is gone: deleted here, or already absent. A delete that fails for any other reason,
    such as a directory at the slot's file name, keeps that slot's entry, and the other
    named slots are still processed. The manifest is rewritten only when an entry was
    dropped, and removed when no entry is left, or when it is not usable and no named file
    stayed (an unusable manifest whose file could not be removed is left in place, since
    its entries cannot be rewritten faithfully; nk_font_remove_imports does the same).
    A slot named twice is processed once. Returns the number
    of slot files removed; raises FontImportError naming each slot whose file could not be
    removed, after the manifest is updated.
    """
    cache_dir = get_font_cache_dir(user_data_root)
    wanted = list(SLOTS) if slots is None else list(dict.fromkeys(slots))
    for slot in wanted:
        if slot not in SLOTS:
            raise FontImportError(f"Unknown font slot '{slot}'; use one of {', '.join(SLOTS)}.")
    status, _, entries = _load_manifest(cache_dir)
    removed = 0
    dropped = False
    failures: List[str] = []
    for slot in wanted:
        path = cache_dir / SLOT_FILES[slot]
        base_delete_failed = False
        try:
            path.unlink()
            removed += 1
        except (FileNotFoundError, NotADirectoryError):
            # Already absent. A cache path through a non-directory is ENOTDIR on POSIX and
            # not-found on Windows; either way no slot file is there.
            pass
        except OSError as exc:
            kept = "; its manifest entry is kept" if slot in entries else ""
            failures.append(
                f"The {SLOT_ROLES[slot]} font file {SLOT_FILES[slot]} could not be removed "
                f"({exc.strerror or exc}){kept}."
            )
            base_delete_failed = True
        if not base_delete_failed and entries.pop(slot, None) is not None:
            dropped = True
        if slot == "latin":
            for index, name in enumerate(LATIN_INDEX_FILES):
                path = cache_dir / name
                try:
                    path.unlink()
                    removed += 1
                except (FileNotFoundError, NotADirectoryError):
                    pass
                except OSError as exc:
                    kept = "; its manifest entry is kept" if f"latin_index_{index}" in entries else ""
                    failures.append(
                        f"The Latin index {index + 1} font file {name} could not be removed "
                        f"({exc.strerror or exc}){kept}."
                    )
                    continue
                if entries.pop(f"latin_index_{index}", None) is not None:
                    dropped = True
    manifest_path = cache_dir / MANIFEST_NAME
    if status != "OK":
        if not failures and manifest_path.is_file():
            manifest_path.unlink()
            _load_manifest_cached.cache_clear()
    elif dropped:
        if entries:
            _write_manifest(cache_dir, entries)
        else:
            manifest_path.unlink(missing_ok=True)
            _load_manifest_cached.cache_clear()
    if removed:
        _inspect_file_cached.cache_clear()
    if failures:
        if removed:
            failures.append(f"Removed {removed} other imported font file(s).")
        raise FontImportError(" ".join(failures))
    return removed
