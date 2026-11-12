/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * libnk_python.so: the Python interpreter the on-device toolchain runs (#360).
 *
 * The official CPython release for Android ships libpython3.15.so and its standard library
 * but no python program, because an app embeds Python instead of executing it. The build
 * tools the device runs are Python scripts started as commands, so this is that program:
 * Python's own command line, Py_BytesMain(), as the python executable of a CPython build
 * runs it. Named lib<name>.so, it can live in the app's native library folder, the only
 * place an app may execute files from, beside libpython3.15.so.
 *
 * The caller sets PYTHONHOME to the folder that holds lib/python3.15, which the app
 * unpacks from its assets; Python reports a missing standard library itself.
 *
 * It is built only for Android, against the headers of the pinned CPython release; any
 * other compile sees an empty unit.
 */

#if defined(__ANDROID__)

#include <Python.h>

int main(int argc, char **argv)
{
    return Py_BytesMain(argc, argv);
}

#endif /* __ANDROID__ */
