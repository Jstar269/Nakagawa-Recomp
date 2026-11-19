// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.regex.Pattern;

/**
 * Which runtime library TitleActivity may load (#360). A title is one the APK ships, named by a
 * bare name (nk_title_showcase-scene-v1) or by its path in the native library folder, or one the
 * player built on this device, named by its absolute path in the app's files folder, where
 * package builds write. Any other name is refused, so a launch never loads a library from
 * anywhere else. Paths are compared after symbolic links and ".." are resolved. It also names
 * the launcher's status files a launch may write, and writes one as the title shim does.
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

    /**
     * {@code path} when it names one of the launcher's status files for a title launch,
     * title-launch-[pid]-[serial].[suffix] (android_launch_file in src/core/nk_platform_posix.c),
     * else null. A debug build lets any app start TitleActivity, and the title process records
     * these files by renaming over them, so a launch never names any other file.
     */
    static String launchFile(String path, String suffix) {
        if (path == null || path.isEmpty() || path.indexOf('\0') >= 0) return null;
        String name = new File(path).getName();
        return name.matches("title-launch-[0-9]+-[0-9]+\\." + Pattern.quote(suffix)) ? path : null;
    }

    /**
     * Records {@code status} in {@code path} as nk_android_title.c does: written to a temporary
     * name, then renamed, so the launcher never reads a partial number. Returns whether it is
     * recorded.
     */
    static boolean writeStatus(String path, int status) {
        File temp = new File(path + ".tmp");
        try (FileOutputStream out = new FileOutputStream(temp)) {
            out.write((status + "\n").getBytes(StandardCharsets.US_ASCII));
        } catch (IOException | SecurityException e) {
            return false;   // nothing is renamed into place: the launcher never reads it as a status
        }
        return temp.renameTo(new File(path));
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
