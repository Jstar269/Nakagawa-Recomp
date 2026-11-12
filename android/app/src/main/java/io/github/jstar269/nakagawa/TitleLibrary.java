// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import java.io.File;
import java.io.IOException;

/**
 * Which runtime library TitleActivity may load (#360). A title is one the APK ships, named by a
 * bare name (nk_title_showcase-scene-v1) or by its path in the native library folder, or one the
 * player built on this device, named by its absolute path in the app's files folder, where
 * package builds write. Any other name is refused, so a launch never loads a library from
 * anywhere else. Paths are compared after symbolic links and ".." are resolved.
 *
 * <p>Plain Java with no Android classes, so tools/test_android_build.py runs it on the host.
 */
final class TitleLibrary {
    /** A library TitleActivity may load. */
    static final class Choice {
        /** Its canonical path, which SDL passes to dlopen(). */
        final String path;
        /** Its System.loadLibrary() name when the APK ships it, else "". */
        final String bundledName;

        Choice(String path, String bundledName) {
            this.path = path;
            this.bundledName = bundledName;
        }
    }

    private TitleLibrary() {
    }

    /** The library named by {@code library}, or null when it may not be loaded. */
    static Choice resolve(String library, String nativeLibraryDir, String filesDir) {
        if (library == null || library.isEmpty() || library.indexOf('\0') >= 0) return null;
        try {
            File libDir = new File(nativeLibraryDir).getCanonicalFile();
            if (library.indexOf('/') < 0 && library.indexOf(File.separatorChar) < 0) {
                String name = library;
                if (name.startsWith("lib")) name = name.substring(3);
                if (name.endsWith(".so")) name = name.substring(0, name.length() - 3);
                if (name.isEmpty()) return null;
                return bundled(new File(libDir, "lib" + name + ".so").getCanonicalFile(), libDir);
            }
            if (!new File(library).isAbsolute()) return null;
            File file = new File(library).getCanonicalFile();
            if (!file.isFile() || !file.getName().endsWith(".so")) return null;
            if (libDir.equals(file.getParentFile())) return bundled(file, libDir);
            File files = new File(filesDir).getCanonicalFile();
            for (File parent = file.getParentFile(); parent != null; parent = parent.getParentFile()) {
                if (parent.equals(files)) return new Choice(file.getPath(), "");
            }
            return null;
        } catch (IOException | SecurityException e) {
            return null;
        }
    }

    /** A library directly in the native library folder, named lib[name].so. */
    private static Choice bundled(File file, File libDir) {
        String fileName = file.getName();
        if (!libDir.equals(file.getParentFile()) || !file.isFile()
                || !fileName.startsWith("lib") || !fileName.endsWith(".so")
                || fileName.length() <= "lib.so".length()) {
            return null;
        }
        return new Choice(file.getPath(), fileName.substring(3, fileName.length() - 3));
    }
}
