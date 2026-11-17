# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
FROM gcr.io/oss-fuzz-base/base-builder:v1@sha256:1f9cf74bc3d7029b528494542f9d9851b256dceed38d3f420989434dc5ae0760
# The fuzz target's only inputs. Never `COPY .`: Docker ignores .gitignore, so a local
# build from a working checkout would bake build/, logs/, oracle/, place_game_here/ and
# keys/ into an image layer (AGENTS.md section 5).
COPY src/core/nk_json.c src/core/nk_json.h $SRC/nakagawa-recomp/src/core/
COPY tests/fuzzers/test_nk_json_fuzzer.c $SRC/nakagawa-recomp/tests/fuzzers/
WORKDIR $SRC/nakagawa-recomp
COPY .clusterfuzzlite/build.sh $SRC/
