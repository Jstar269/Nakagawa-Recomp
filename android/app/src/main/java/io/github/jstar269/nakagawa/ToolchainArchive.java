// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * Unpacks the on-device toolchain's archives (#360): sysroot.tar, clang-resource.tar and
 * python-stdlib.tar in assets/toolchain/, which tools/android_sysroot.py write_archive() writes
 * as POSIX tar (PAX) holding only folders and regular files.
 *
 * <p>It reads that form and refuses everything else, so an archive cannot place a file
 * outside the target folder: a link, a device or any other entry type; an absolute path or one
 * with a "." or ".." part; a header whose checksum does not match; or an archive that ends early.
 * A file whose mode is executable stays executable for its owner.
 *
 * <p>It uses only java.io, so the host tests (tools/test_android_toolchain.py) run it with a
 * plain JDK.
 */
final class ToolchainArchive {
    private static final int BLOCK = 512;

    private ToolchainArchive() {}

    /** Unpacks {@code in} under {@code target}; returns the number of files written. */
    static int unpack(InputStream in, File target) throws IOException {
        File root = target.getCanonicalFile();
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("cannot create " + root);
        byte[] header = new byte[BLOCK];
        String paxPath = null;
        int files = 0;
        while (true) {
            if (!readFully(in, header)) throw new IOException("archive ends without its end blocks");
            if (isZero(header)) return files;
            checkSum(header);
            long size = octal(header, 124, 12);
            char type = (char) header[156];
            if (type == 'x') {
                paxPath = paxValue(readBytes(in, size), "path");
                skipPadding(in, size);
                continue;
            }
            String name = paxPath != null ? paxPath : ustarName(header);
            paxPath = null;
            File file = resolve(root, name);
            if (type == '5') {
                if (size != 0) throw new IOException(name + ": a folder with contents");
                if (!file.isDirectory() && !file.mkdirs()) throw new IOException("cannot create " + file);
            } else if (type == '0' || type == '\0') {
                File parent = file.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                    throw new IOException("cannot create " + parent);
                }
                try (OutputStream out = new FileOutputStream(file)) {
                    copy(in, out, size);
                }
                skipPadding(in, size);
                if ((octal(header, 100, 8) & 0100) != 0 && !file.setExecutable(true, true)) {
                    throw new IOException("cannot make " + file + " executable");
                }
                files++;
            } else {
                throw new IOException(name + ": entry type '" + type + "' is not a folder or a file");
            }
        }
    }

    private static File resolve(File root, String name) throws IOException {
        String trimmed = name.endsWith("/") ? name.substring(0, name.length() - 1) : name;
        if (trimmed.isEmpty() || trimmed.startsWith("/")) throw new IOException(name + ": not a relative path");
        for (String part : trimmed.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")) {
                throw new IOException(name + ": not a plain relative path");
            }
        }
        File file = new File(root, trimmed).getCanonicalFile();
        if (!file.getPath().startsWith(root.getPath() + File.separator)) {
            throw new IOException(name + ": outside " + root);
        }
        return file;
    }

    private static String ustarName(byte[] header) throws IOException {
        if (!new String(header, 257, 5, StandardCharsets.US_ASCII).equals("ustar")) {
            throw new IOException("not a POSIX tar header");
        }
        String name = text(header, 0, 100);
        String prefix = text(header, 345, 155);
        return prefix.isEmpty() ? name : prefix + "/" + name;
    }

    /** The value of {@code key} in PAX records ("<length> <key>=<value>\n"), or null. */
    private static String paxValue(byte[] records, String key) throws IOException {
        String value = null;
        int at = 0;
        while (at < records.length) {
            int space = at;
            while (space < records.length && records[space] != ' ') space++;
            if (space == records.length) throw new IOException("bad PAX record");
            int length;
            try {
                length = Integer.parseInt(new String(records, at, space - at, StandardCharsets.US_ASCII));
            } catch (NumberFormatException e) {
                throw new IOException("bad PAX record length");
            }
            // length > records.length - at, not at + length > records.length: the sum can pass
            // Integer.MAX_VALUE and wrap.
            if (length <= space - at + 1 || length > records.length - at || records[at + length - 1] != '\n') {
                throw new IOException("bad PAX record");
            }
            String record = new String(records, space + 1, at + length - space - 2, StandardCharsets.UTF_8);
            int eq = record.indexOf('=');
            if (eq < 0) throw new IOException("bad PAX record");
            if (record.substring(0, eq).equals(key)) value = record.substring(eq + 1);
            at += length;
        }
        return value;
    }

    private static void checkSum(byte[] header) throws IOException {
        long sum = 0;
        for (int i = 0; i < BLOCK; i++) sum += (i >= 148 && i < 156) ? ' ' : (header[i] & 0xff);
        if (sum != octal(header, 148, 8)) throw new IOException("tar header checksum does not match");
    }

    private static long octal(byte[] header, int offset, int length) throws IOException {
        long value = 0;
        int end = offset + length;
        int i = offset;
        while (i < end && header[i] == ' ') i++;
        int digits = 0;
        for (; i < end && header[i] >= '0' && header[i] <= '7'; i++, digits++) {
            value = value * 8 + (header[i] - '0');
        }
        if (digits == 0 || (i < end && header[i] != 0 && header[i] != ' ')) {
            throw new IOException("bad octal field in a tar header");
        }
        return value;
    }

    private static String text(byte[] header, int offset, int length) {
        int end = offset;
        while (end < offset + length && header[end] != 0) end++;
        return new String(header, offset, end - offset, StandardCharsets.UTF_8);
    }

    private static boolean isZero(byte[] block) {
        for (byte b : block) if (b != 0) return false;
        return true;
    }

    private static boolean readFully(InputStream in, byte[] buffer) throws IOException {
        int done = 0;
        while (done < buffer.length) {
            int n = in.read(buffer, done, buffer.length - done);
            if (n < 0) return false;
            done += n;
        }
        return true;
    }

    private static byte[] readBytes(InputStream in, long size) throws IOException {
        if (size > 1 << 20) throw new IOException("PAX header larger than 1 MiB");
        byte[] data = new byte[(int) size];
        if (!readFully(in, data)) throw new IOException("archive ends inside a PAX header");
        return data;
    }

    private static void copy(InputStream in, OutputStream out, long size) throws IOException {
        byte[] buffer = new byte[1 << 16];
        long left = size;
        while (left > 0) {
            int n = in.read(buffer, 0, (int) Math.min(buffer.length, left));
            if (n < 0) throw new IOException("archive ends inside a file");
            out.write(buffer, 0, n);
            left -= n;
        }
    }

    private static void skipPadding(InputStream in, long size) throws IOException {
        int padding = (int) ((BLOCK - size % BLOCK) % BLOCK);
        if (padding > 0 && !readFully(in, new byte[padding])) throw new IOException("archive ends inside padding");
    }
}
