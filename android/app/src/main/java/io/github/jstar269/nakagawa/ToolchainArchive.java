// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.Executor;

/**
 * Unpacks the on-device toolchain's archives (#360): sysroot.tar, clang-resource.tar and
 * python-stdlib.tar in assets/toolchain/, which tools/android_sysroot.py write_archive() writes
 * as POSIX tar (PAX) holding only folders and regular files.
 *
 * <p>It reads that form and refuses everything else, so an archive cannot place a file
 * outside the target folder: a link, a device or any other entry type; an absolute path or one
 * with a "." or ".." part; a header whose checksum does not match; or an archive that ends early.
 * A file whose mode is executable stays executable for its owner. Each file is synced to storage
 * before the next entry, so the caller's completion stamp, written after, never outlives its data.
 *
 * <p>{@link Install} runs the whole unpack once per process on a worker thread, with progress, a
 * stamp written last and a failure the caller can show and retry.
 *
 * <p>It uses only java.io, java.nio and java.util.concurrent, so the host tests
 * (tools/test_android_toolchain.py) run it with a plain JDK.
 */
final class ToolchainArchive {
    private static final int BLOCK = 512;

    private ToolchainArchive() {}

    /** Told after each file {@link #unpack} writes. */
    interface Progress {
        void file();
    }

    /** Unpacks {@code in} under {@code target}; returns the number of files written. */
    static int unpack(InputStream in, File target) throws IOException {
        return unpack(in, target, null);
    }

    /** As {@link #unpack(InputStream, File)}, telling {@code progress} (if not null) after each file. */
    static int unpack(InputStream in, File target, Progress progress) throws IOException {
        File root = target.getCanonicalFile();
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("cannot create " + root);
        return walk(in, root, progress);
    }

    /** The number of files {@code in} holds, read and checked as {@link #unpack} does, writing nothing. */
    static int count(InputStream in) throws IOException {
        return walk(in, null, null);
    }

    /** Reads the archive; with a null {@code root}, only checks it and counts its files. */
    private static int walk(InputStream in, File root, Progress progress) throws IOException {
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
            String relative = relativePath(name);
            File file = root != null ? resolve(root, name, relative) : null;
            if (type == '5') {
                if (size != 0) throw new IOException(name + ": a folder with contents");
                if (file != null && !file.isDirectory() && !file.mkdirs()) throw new IOException("cannot create " + file);
            } else if (type == '0' || type == '\0') {
                boolean executable = (octal(header, 100, 8) & 0100) != 0;
                if (file == null) {
                    copy(in, null, size);
                } else {
                    File parent = file.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                        throw new IOException("cannot create " + parent);
                    }
                    try (FileOutputStream out = new FileOutputStream(file)) {
                        copy(in, out, size);
                        out.getFD().sync();
                    }
                }
                skipPadding(in, size);
                if (file != null && executable && !file.setExecutable(true, true)) {
                    throw new IOException("cannot make " + file + " executable");
                }
                files++;
                if (progress != null) progress.file();
            } else {
                throw new IOException(name + ": entry type '" + type + "' is not a folder or a file");
            }
        }
    }

    /** {@code name} without a trailing "/", refused unless it is a plain relative path. */
    private static String relativePath(String name) throws IOException {
        String trimmed = name.endsWith("/") ? name.substring(0, name.length() - 1) : name;
        if (trimmed.isEmpty() || trimmed.startsWith("/")) throw new IOException(name + ": not a relative path");
        for (String part : trimmed.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")) {
                throw new IOException(name + ": not a plain relative path");
            }
        }
        return trimmed;
    }

    private static File resolve(File root, String name, String relative) throws IOException {
        File file = new File(root, relative).getCanonicalFile();
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

    /** Copies {@code size} bytes to {@code out}, or reads past them when {@code out} is null. */
    private static void copy(InputStream in, OutputStream out, long size) throws IOException {
        byte[] buffer = new byte[1 << 16];
        long left = size;
        while (left > 0) {
            int n = in.read(buffer, 0, (int) Math.min(buffer.length, left));
            if (n < 0) throw new IOException("archive ends inside a file");
            if (out != null) out.write(buffer, 0, n);
            left -= n;
        }
    }

    private static void skipPadding(InputStream in, long size) throws IOException {
        int padding = (int) ((BLOCK - size % BLOCK) % BLOCK);
        if (padding > 0 && !readFully(in, new byte[padding])) throw new IOException("archive ends inside padding");
    }

    /**
     * Deletes {@code file} and everything under it; a file that stays is a failure. A symbolic
     * link is deleted itself, never followed: File.listFiles() would list its target's folder.
     */
    static void deleteTree(File file) throws IOException {
        boolean link = Files.isSymbolicLink(file.toPath());
        if (!link) {
            File[] children = file.listFiles();
            if (children != null) for (File child : children) deleteTree(child);
        }
        // exists() follows a link, so a dangling one would look absent.
        if ((link || file.exists()) && !file.delete()) throw new IOException("cannot delete " + file);
    }

    /** The first 256 bytes of {@code file} as text; "" when it is missing or unreadable. */
    static String readText(File file) {
        try (InputStream in = new FileInputStream(file)) {
            byte[] bytes = new byte[256];
            int n = in.read(bytes);
            return n > 0 ? new String(bytes, 0, n, StandardCharsets.UTF_8) : "";
        } catch (IOException e) {
            return "";
        }
    }

    /** Writes {@code text} to {@code file} and syncs it to storage. */
    static void writeText(File file, String text) throws IOException {
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        }
    }

    /** Opens one of the toolchain's archives by name (assets/toolchain/{@code name} in the APK). */
    interface Source {
        InputStream open(String name) throws IOException;
    }

    /** Told when an {@link Install}'s status changes, on the thread that changed it. */
    interface Listener {
        void changed(Status status);
    }

    /** One consistent view of an {@link Install}. */
    static final class Status {
        enum State { IDLE, RUNNING, DONE, FAILED }

        final State state;
        /** Files written so far, of {@link #total}; total is 0 until the archives are counted. */
        final int done;
        final int total;
        /** Why the unpack failed, when {@link #state} is FAILED; null otherwise. */
        final String error;

        Status(State state, int done, int total, String error) {
            this.state = state;
            this.done = done;
            this.total = total;
            this.error = error;
        }

        @Override
        public String toString() {
            return state + " " + done + "/" + total + (error != null ? " " + error : "");
        }
    }

    /**
     * The whole toolchain's unpack, run on an executor and at most once at a time per process: a
     * second {@link #start} while it runs, or after it is done, starts nothing, so an activity
     * created again finds the same run and its result.
     *
     * <p>The stamp file says the unpack is complete for one APK (its version and install time).
     * A run checks it under an exclusive lock on the lock file, which keeps another process from
     * unpacking at once; when it does not match, the run deletes it first, then the toolchain
     * folder, counts the archives' files, unpacks each, and writes the stamp last, synced. A run
     * that fails, or a process that dies part way, therefore leaves no matching stamp, and the
     * next start unpacks again. A failure becomes FAILED with its reason, and {@link #start}
     * tries again.
     */
    static final class Install {
        private final Source source;
        private final String[][] archives;
        private final File folder;
        private final File stampFile;
        private final File lockFile;
        private final String stamp;
        private Status.State state = Status.State.IDLE;
        private int done;
        private int total;
        private String error;
        private Listener listener;

        /**
         * @param archives pairs of an archive's name and the folder under {@code folder} it unpacks to
         * @param stamp the APK's stamp; null when it is unknown, which fails every run
         */
        Install(Source source, String[][] archives, File folder, File stampFile, File lockFile, String stamp) {
            this.source = source;
            this.archives = archives;
            this.folder = folder;
            this.stampFile = stampFile;
            this.lockFile = lockFile;
            this.stamp = stamp;
        }

        /** Whether an unpack for this APK finished, in this process or an earlier one. */
        boolean isComplete() {
            return stamp != null && stamp.equals(readText(stampFile));
        }

        /**
         * Starts the unpack on {@code executor} unless it is running or done; returns whether it
         * started one.
         */
        boolean start(Executor executor) {
            synchronized (this) {
                if (state == Status.State.RUNNING || state == Status.State.DONE) return false;
                state = Status.State.RUNNING;
                done = 0;
                total = 0;
                error = null;
            }
            changed();
            try {
                executor.execute(this::run);
            } catch (RuntimeException e) {
                finish(Status.State.FAILED, "cannot start the unpack: " + describe(e));
            }
            return true;
        }

        synchronized Status status() {
            return new Status(state, done, total, error);
        }

        synchronized void setListener(Listener listener) {
            this.listener = listener;
        }

        /** Waits while a run is going on, and returns the status it ended with. */
        synchronized Status await() throws InterruptedException {
            while (state == Status.State.RUNNING) wait();
            return status();
        }

        /** Waits until a run has finished the unpack, through any failures and retries. */
        synchronized void awaitDone() throws InterruptedException {
            while (state != Status.State.DONE) wait();
        }

        private void run() {
            try {
                unpackAll();
                finish(Status.State.DONE, null);
            } catch (IOException | RuntimeException e) {
                finish(Status.State.FAILED, describe(e));
            } catch (Error e) {
                finish(Status.State.FAILED, describe(e));  // never left RUNNING for a waiter
                throw e;
            }
        }

        private void unpackAll() throws IOException {
            if (stamp == null) throw new IOException("the app's version cannot be read");
            try (RandomAccessFile lock = new RandomAccessFile(lockFile, "rw");
                 FileChannel channel = lock.getChannel()) {
                channel.lock();  // released when the channel closes
                if (isComplete()) return;  // another process finished it while this one waited
                if (stampFile.exists() && !stampFile.delete()) throw new IOException("cannot delete " + stampFile);
                deleteTree(folder);
                int files = 0;
                for (String[] archive : archives) {
                    try (InputStream in = source.open(archive[0])) {
                        files += count(in);
                    }
                }
                synchronized (this) {
                    total = files;
                }
                changed();
                for (String[] archive : archives) {
                    try (InputStream in = source.open(archive[0])) {
                        unpack(in, new File(folder, archive[1]), this::fileDone);
                    }
                }
                // Written last, after every file is synced: without it, the next start unpacks again.
                writeText(stampFile, stamp);
            }
        }

        private void fileDone() {
            synchronized (this) {
                done++;
            }
            changed();
        }

        private void finish(Status.State end, String why) {
            synchronized (this) {
                state = end;
                error = why;
                notifyAll();
            }
            changed();
        }

        /** Tells the listener, outside the lock so it may call {@link #status}. */
        private void changed() {
            Listener told;
            Status now;
            synchronized (this) {
                told = listener;
                now = status();
            }
            if (told != null) told.changed(now);
        }

        private static String describe(Throwable e) {
            String message = e.getMessage();
            return message != null && !message.isEmpty() ? message : e.getClass().getSimpleName();
        }
    }
}
