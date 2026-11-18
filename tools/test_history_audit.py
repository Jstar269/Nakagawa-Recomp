# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from pathlib import Path
import tempfile
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import history_audit
from nk_core.git_isolation import run_git
from ci_test_shards import nightly_tier
from typing import Any


class TestHistoryAudit(unittest.TestCase):
    def test_repository_baseline(self):
        baseline = history_audit.get_repository_baseline(history_audit.ROOT)
        self.assertIn("git_commit_main", baseline)
        self.assertGreater(baseline["total_commits"], 0)
        self.assertGreater(baseline["total_objects"], 0)
        self.assertGreater(baseline["total_refs"], 0)

    def test_code_capture_is_a_forbidden_extension(self):
        # A .nkcap holds bytes of the user's own game RAM (#1933); history must flag it too.
        self.assertIn(".nkcap", history_audit.FORBIDDEN_EXTENSIONS)

    def test_redaction_and_finding_classification(self):
        token = "ghp_" + "123456789012345678901234567890123456"
        token_literal = "ghp_" + "123456789012345678901234567890123456"
        f = history_audit.HistoryFinding(
            category="DEFINITE_SECRET",
            code="TEST_SECRET",
            commit="abc12345",
            path="config/keys.py",
            detail="Found key: 0123456789abcdef0123456789abcdef and token " + token,
        )
        d = f.to_dict(redact=True)
        self.assertEqual(d["category"], "DEFINITE_SECRET")
        self.assertNotIn("0123456789abcdef0123456789abcdef", d["detail"])
        self.assertNotIn(token_literal, d["detail"])
        self.assertIn("[REDACTED_HEX_KEY]", d["detail"])
        self.assertIn("[REDACTED_API_TOKEN]", d["detail"])

    @nightly_tier
    def test_full_history_audit_report_generation(self):
        report = history_audit.generate_full_history_audit_report(history_audit.ROOT)
        self.assertIn("status", report)
        self.assertIn("baseline", report)
        self.assertIn("summary", report)
        self.assertIn("large_blobs", report)
        self.assertIn("findings", report)

    def _blob_findings(self, content: str) -> list:
        """Commit ``content`` into a throwaway repository and scan it."""
        return self._blob_findings_bytes(content.encode("utf-8"))

    def _blob_findings_bytes(self, data: bytes, name: str = "note.md",
                             private_roots: tuple[str, ...] | None = None) -> list:
        """Commit ``data`` byte for byte into a throwaway repository and scan it."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for argv in (("init", "-q"),):
                run_git(argv, cwd=root, check=True, capture_output=True)
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_bytes(data)
            run_git(["-c", "core.autocrlf=false", "add", name], cwd=root, check=True)
            run_git(["commit", "-qm", "fixture"], cwd=root, check=True)
            if private_roots is None:
                returned: list[Any] = history_audit.audit_history_blob_contents(root)
            else:
                returned = history_audit.audit_history_blob_contents(root, private_roots=private_roots)
            return returned

    def test_quoted_armour_delimiter_without_a_body_is_not_a_secret(self):
        """Prose naming the PEM format is not a key.

        docs/PROVENANCE_MERGE_GATE.md told a maintainer which value to paste into
        a CI secret store and quoted both delimiters on one line to identify the
        format. That failed this gate as a DEFINITE_SECRET while containing no
        key material at all, and a gate that is red over prose is a gate people
        learn to skip past.
        """
        begin = "-----BE" + "GIN RSA PRIVATE KEY-----"
        end = "-----E" + "ND RSA PRIVATE KEY-----"
        prose = (
            "* `PROVENANCE_APP_PRIVATE_KEY` -- the full PEM text "
            "(`" + begin + "` ... `" + end + "`), newlines preserved.\n"
        )
        secrets = [f for f in self._blob_findings(prose) if f.category == "DEFINITE_SECRET"]
        self.assertEqual(
            secrets, [],
            "an armour delimiter with no key body between it and the footer is "
            "documentation, not a secret",
        )

    def test_armour_delimiter_with_a_body_is_still_a_secret(self):
        """The narrowing must not cost a single real key."""
        bodies = {
            "RSA": "MIIEowIBAAKCAQEAx7Vq9kZ3mQ8Jf2pL0sYtWn4cB6dHgR1uEvXaTzKmNpQrSuVw",
            "OPENSSH": "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAABlwAAAAdzc2gtcn",
            "ENCRYPTED": "MIIFDjBABgkqhkiG9w0BBQ0wMzAbBgkqhkiG9w0BBQwwDgQI5yNCu9T5SnsCAggA",
        }
        for label, body in bodies.items():
            with self.subTest(key=label):
                begin = "-----BE" + "GIN " + label + " PRIVATE KEY-----"
                end = "-----E" + "ND " + label + " PRIVATE KEY-----"
                pem = begin + "\n" + body + "\n" + end + "\n"
                self.assertTrue(
                    any(f.code == "HISTORICAL_BLOB_SECRET" for f in self._blob_findings(pem)),
                    "a " + label + " key body must still be caught",
                )

    def test_control_character_or_stray_byte_does_not_hide_a_secret_or_path(self):
        # A vertical tab, an ANSI colour code or one cp1252 byte made the whole blob
        # "binary", and every text signature was skipped: a coloured private log naming
        # a user's folder, or a key followed by a form feed, passed the history gate.
        begin = "-----BE" + "GIN RSA PRIVATE KEY-----"
        end = "-----E" + "ND RSA PRIVATE KEY-----"
        body = "MIIEowIBAAKCAQEAx7Vq9kZ3mQ8Jf2pL0sYtWn4cB6dHgR1uEvXaTzKmNpQrSuVw"
        pem = begin + "\n" + body + "\n" + end + "\n"

        def codes(data):
            return [f.code for f in self._blob_findings_bytes(data)]

        self.assertIn("HISTORICAL_BLOB_SECRET", codes((pem + "\t\r\n").encode()),
                      "tab, CR and LF are ordinary text")
        self.assertIn("HISTORICAL_BLOB_SECRET", codes((pem + "\x0b").encode()))
        self.assertIn("HISTORICAL_BLOB_SECRET", codes((pem + "\x0c").encode()))
        user_path = "C:" + chr(92) + "Us" + "ers" + chr(92) + "someone" + chr(92) + "logs"
        colour_log = "\x1b[31mERROR\x1b[0m opened " + user_path + "\n"
        self.assertIn("HISTORICAL_BLOB_LOCAL_PATH", codes(colour_log.encode()))
        cp1252_note = "caf\xe9 notes in ".encode("cp1252") + user_path.encode() + b"\n"
        self.assertIn("HISTORICAL_BLOB_LOCAL_PATH", codes(cp1252_note))
        self.assertIn("HISTORICAL_BLOB_PRIVATE_VOCABULARY",
                      codes(b"\x1b[1m" + ("HST" + "_PGD_VKEY").encode() + b"\n"))
        # A token inside a binary blob (a NUL byte) is still a secret.
        token = "gh" + "p_" + "7" * 36
        self.assertIn("HISTORICAL_BLOB_SECRET", codes(b"\0\x01\x02" + token.encode() + b"\0"))
        # A binary blob still runs no path signature over replacement-decoded bytes:
        # neither one with a NUL byte nor a NUL-free lookup table (one public table in
        # history is 2 MiB of such bytes) whose random bytes happen to spell a path.
        self.assertNotIn("HISTORICAL_BLOB_LOCAL_PATH", codes(b"\0" + user_path.encode() + b"\0"))
        import random
        table = bytes(b or 1 for b in random.Random(20261117).randbytes(64 * 1024))
        self.assertNotIn(b"\0", table)
        self.assertNotIn("HISTORICAL_BLOB_LOCAL_PATH",
                         codes(table[:4096] + b" /tm" + b"p/abc " + table[4096:]))
        self.assertIn("HISTORICAL_BLOB_SECRET", codes(table + b" " + token.encode() + b" " + table))

    def test_one_object_listing_serves_every_pass_and_changes_no_finding(self):
        # Fast path: the report lists history once and shares that listing with
        # every pass. Slow path: each pass lists history itself (raw_objects=None).
        # Both must agree on every pass, and the fixture must produce findings on
        # both paths so the comparison is not vacuous.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run_git(("init", "-q"), cwd=root, check=True, capture_output=True)
            begin = "-----BE" + "GIN RSA PRIVATE KEY-----"
            end = "-----E" + "ND RSA PRIVATE KEY-----"
            body = "MIIEowIBAAKCAQEAx7Vq9kZ3mQ8Jf2pL0sYtWn4cB6dHgR1uEvXaTzKmNpQrSuVw"
            (root / "notes.md").write_text(begin + "\n" + body + "\n" + end + "\n", encoding="utf-8")
            (root / "oracle").mkdir()
            (root / "oracle" / "dump.txt").write_text("synthetic\n", encoding="utf-8")
            run_git(["add", "-A"], cwd=root, check=True)
            run_git(["commit", "-qm", "fixture"], cwd=root, check=True)

            real_git = history_audit._git
            issued: list[list[str]] = []

            def recording(cmd, repo_root=history_audit.ROOT):
                issued.append(list(cmd))
                return real_git(cmd, repo_root=repo_root)

            with mock.patch.object(history_audit, "_git", side_effect=recording):
                shared = history_audit.generate_full_history_audit_report(root, root / "absent.json")
            listings = [cmd for cmd in issued if cmd[:2] == ["rev-list", "--objects"]]
            self.assertEqual(len(listings), 1, "the report must list history exactly once")
            self.assertGreater(shared["summary"]["total_findings"], 0)
            # The oracle/ path finding takes its identity from the tree entry's mode, not
            # from one `git cat-file -t` process per finding.
            self.assertEqual([cmd for cmd in issued if cmd[:1] == ["cat-file"]], [])
            self.assertIn(("HISTORICAL_PATH_PREFIX", "oracle/dump.txt"),
                          [(f["code"], f["path"]) for f in shared["findings"]])

            listing = history_audit._object_listing(root)
            info = history_audit._object_info(root, list(history_audit._named_objects(listing)))
            paths = history_audit._path_listing(root)
            self.assertEqual(
                [f.to_dict() for f in history_audit.audit_history_tree_paths(root, paths)],
                [f.to_dict() for f in history_audit.audit_history_tree_paths(root)],
            )
            self.assertEqual(
                [f.to_dict() for f in history_audit.audit_history_blob_contents(root, listing, object_info=info)],
                [f.to_dict() for f in history_audit.audit_history_blob_contents(root)],
            )
            self.assertEqual(
                history_audit.audit_large_blobs(root, size_threshold=1, raw_objects=listing, object_info=info),
                history_audit.audit_large_blobs(root, size_threshold=1),
            )
            self.assertTrue(history_audit.audit_large_blobs(root, size_threshold=1))
            self.assertEqual(
                history_audit.get_repository_baseline(root, listing),
                history_audit.get_repository_baseline(root),
            )

    def test_literal_prefilters_agree_with_their_regexes(self):
        # Fast path: each _has_* prefilter, which skips a regex unless a literal every
        # match contains is present. Slow path: the named regex alone. Seeds match each
        # signature in several letter cases, and seeded random edits add near-misses and
        # the characters whose case folding is special (dotless i, long s, Kelvin sign).
        # Every verdict must agree, and both outcomes must occur for every signature.
        import random
        bs = chr(92)
        seeds = [
            "ghp_" + "A" * 36, "github_pat_" + "B" * 80, "sk_live_" + "C" * 24, "AKIA" + "D" * 16,
            "gho_" + "E" * 36, "ghs_" + "F" * 36, "github_pat_" + "B" * 82, "sk_live_" + "C" * 99,
            "C:" + bs + "Users" + bs + "someone" + bs + "notes",
            "C:" + bs * 2 + "Users" + bs * 2 + "someone" + bs * 2 + "notes",
            "C:" + bs * 2 + "Windows" + bs * 2 + "Temp" + bs * 2 + "x",
            "C:" + bs + "Documents and Settings" + bs + "someone" + bs + "x",
            "/ho" + "me/someone/x", "/Us" + "ers/someone/x", "/mnt/c/Us" + "ers/someone/x",
            bs + bs + "server" + bs + "share",
            "~/One" + "Drive - Contoso", "/tm" + "p/build", "/var/tm" + "p/build",
            "C:" + bs + "AppData" + bs + "Local" + bs + "Temp" + bs + "x",
            "github.com/Jstar269/nakagawa-recomp-history-private", "HST_PGD_VKEY_HEX",
            "private save baseline", "private trace capture", "private dump evidence",
        ]
        checks = {
            "api token": (lambda t, low: history_audit._has_api_token(t),
                          history_audit.API_TOKEN_PATTERN),
            "windows path": (lambda t, low: history_audit._has_windows_user_path(t),
                             history_audit.WINDOWS_USER_PATH),
            "cloud folder": (lambda t, low: history_audit._has_onedrive_path(t, low),
                         history_audit.ONEDRIVE_PATH),
            "temp path": (lambda t, low: history_audit._has_temp_path(t, low),
                          history_audit.TEMP_PATH),
            "private repo": (lambda t, low: history_audit._has_private_repo_url(t),
                             history_audit.PRIVATE_REPO_URL),
            "vocabulary": (lambda t, low: history_audit._has_private_operational_vocabulary(t, low),
                           history_audit.PRIVATE_OPERATIONAL_VOCABULARY),
        }
        rng = random.Random(20261009)
        alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_./:" + bs + chr(10)
        special = "\u0131\u0130\u017f\u212a\u212b\u00df\u1e9e"
        corpus = []
        for seed in seeds:
            corpus += [seed, seed.upper(), seed.lower()]
            for _ in range(150):
                text = list(seed)
                for _ in range(rng.randint(1, 3)):
                    op = rng.random()
                    if op < 0.4 and text:
                        index = rng.randrange(len(text))
                        if rng.random() < 0.3:
                            text[index] = rng.choice(special + alphabet)
                        else:
                            text[index] = text[index].swapcase()
                    elif op < 0.7 and text:
                        del text[rng.randrange(len(text))]
                    else:
                        text.insert(rng.randint(0, len(text)), rng.choice(special + alphabet))
                corpus.append("".join(text))
        for name, (prefilter, pattern) in checks.items():
            verdicts = []
            for text in corpus:
                lowered = text.lower()
                fast = prefilter(text, lowered)
                slow = pattern.search(text) is not None
                verdicts.append(slow)
                self.assertEqual(fast, slow, f"{name}: {text!r}")
            self.assertIn(True, verdicts, f"{name}: the corpus must contain matches")
            self.assertIn(False, verdicts, f"{name}: the corpus must contain near-misses")

    def test_each_content_signature_is_still_found_through_the_prefilters(self):
        # End to end, the audit must still report each signature from a committed blob.
        bs = chr(92)
        token = "gh" + "p_" + "1" * 36
        tests = {
            "HISTORICAL_BLOB_SECRET": "key " + token + chr(10),
            "HISTORICAL_BLOB_LOCAL_PATH": "see C:" + bs + "Users" + bs + "someone" + bs + "notes.txt" + chr(10),
            "HISTORICAL_BLOB_PRIVATE_REPO": "clone github.com/Jstar269/nakagawa-recomp-history-private" + chr(10),
            "HISTORICAL_BLOB_PRIVATE_VOCABULARY": "run HST_PGD_VKEY_HEX now" + chr(10),
        }
        for code, content in tests.items():
            with self.subTest(code=code):
                self.assertTrue(
                    any(f.code == code for f in self._blob_findings(content)),
                    f"{code} must still be reported",
                )

    def test_ancestor_only_sensitive_blob_is_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for argv in (("init", "-q"),):
                run_git(argv, cwd=root, check=True, capture_output=True)
            (root / "safe.txt").write_text("safe\n", encoding="utf-8")
            run_git(["add", "safe.txt"], cwd=root, check=True)
            run_git(["commit", "-qm", "safe"], cwd=root, check=True)
            sensitive_fixture = " ".join(("private", "save", "baseline", "capture")) + "\n"
            (root / "private.txt").write_text(sensitive_fixture, encoding="utf-8")
            run_git(["add", "private.txt"], cwd=root, check=True)
            run_git(["commit", "-qm", "temporary"], cwd=root, check=True)
            run_git(["rm", "-q", "private.txt"], cwd=root, check=True)
            run_git(["commit", "-qm", "remove"], cwd=root, check=True)

            findings = history_audit.audit_history_blob_contents(root)
            self.assertTrue(
                any(f.code == "HISTORICAL_BLOB_PRIVATE_VOCABULARY" for f in findings),
                "sensitive content existing only in an ancestor must still fail",
            )


    def _repo_with_removed_blob(self, root, name, text):
        run_git(("init", "-q"), cwd=root, check=True, capture_output=True)
        (root / "safe.txt").write_text("safe\n", encoding="utf-8")
        run_git(["add", "safe.txt"], cwd=root, check=True)
        run_git(["commit", "-qm", "safe"], cwd=root, check=True)
        (root / name).write_text(text, encoding="utf-8")
        run_git(["add", name], cwd=root, check=True)
        run_git(["commit", "-qm", "temporary"], cwd=root, check=True)
        blob = run_git(["rev-parse", "HEAD:" + name], cwd=root, check=True,
                       capture_output=True, text=True).stdout.strip()
        run_git(["rm", "-q", name], cwd=root, check=True)
        run_git(["commit", "-qm", "remove"], cwd=root, check=True)
        return blob

    def _write_reviewed(self, path, entries):
        import json
        path.write_text(json.dumps({"schema_version": 1, "reviewed": entries}), encoding="utf-8")

    def test_reviewed_blob_is_reported_but_not_a_finding(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            root.mkdir()
            fixture = " ".join(("private", "trace", "path")) + "\n"
            blob = self._repo_with_removed_blob(root, "notes.txt", fixture)
            reviewed = Path(tmp) / "reviewed.json"
            self._write_reviewed(reviewed, [{
                "blob": blob, "code": "HISTORICAL_BLOB_PRIVATE_VOCABULARY",
                "path": "notes.txt", "reason": "synthetic fixture"}])
            report = history_audit.generate_full_history_audit_report(root, reviewed)
            self.assertEqual(report["status"], "OK")
            self.assertEqual(report["summary"]["reviewed_findings"], 1)
            self.assertEqual(report["reviewed_findings"][0]["reason"], "synthetic fixture")

    def test_reviewed_blob_accepts_its_path_findings(self):
        """A prohibited-extension finding names its blob like a content finding does, so the
        exact-blob review entry accepts it; another blob at the same path stays a finding."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            root.mkdir()
            blob = self._repo_with_removed_blob(root, "guest.prx", "synthetic fixture bytes\n")
            findings = [f for f in history_audit.audit_history_tree_paths(root)
                        if f.code == "HISTORICAL_PROHIBITED_EXTENSION"]
            self.assertEqual([f.commit for f in findings], ["blob:" + blob[:12]])
            reviewed = Path(tmp) / "reviewed.json"
            self._write_reviewed(reviewed, [{
                "blob": blob, "code": "HISTORICAL_PROHIBITED_EXTENSION",
                "path": "guest.prx", "reason": "synthetic fixture"}])
            report = history_audit.generate_full_history_audit_report(root, reviewed)
            self.assertEqual(report["status"], "OK")
            self.assertEqual(report["summary"]["reviewed_findings"], 1)
            self._write_reviewed(reviewed, [{
                "blob": "0" * 40, "code": "HISTORICAL_PROHIBITED_EXTENSION",
                "path": "guest.prx", "reason": "different content"}])
            report = history_audit.generate_full_history_audit_report(root, reviewed)
            self.assertEqual(report["status"], "FAIL")

    def test_reviewed_commit_accepts_a_gitlink_path_finding(self):
        """A gitlink under a forbidden prefix took an 8-hex id that no review entry (a
        40-hex commit compared on 12 hex) could ever equal, so it stayed a finding."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            root.mkdir()
            run_git(("init", "-q"), cwd=root, check=True, capture_output=True)
            (root / "safe.txt").write_text("safe\n", encoding="utf-8")
            run_git(["add", "safe.txt"], cwd=root, check=True)
            run_git(["commit", "-qm", "safe"], cwd=root, check=True)
            linked = run_git(["rev-parse", "HEAD"], cwd=root, check=True,
                             capture_output=True, text=True).stdout.strip()
            run_git(["update-index", "--add", "--cacheinfo", f"160000,{linked},build/sub"],
                    cwd=root, check=True)
            run_git(["commit", "-qm", "gitlink"], cwd=root, check=True)
            findings = [f for f in history_audit.audit_history_tree_paths(root)
                        if f.code == "HISTORICAL_PATH_PREFIX"]
            self.assertEqual([(f.commit, f.path) for f in findings], [(linked[:12], "build/sub")])
            reviewed = Path(tmp) / "reviewed.json"
            self._write_reviewed(reviewed, [{
                "commit": linked, "code": "HISTORICAL_PATH_PREFIX",
                "path": "build/sub", "reason": "synthetic fixture"}])
            report = history_audit.generate_full_history_audit_report(root, reviewed)
            self.assertEqual(report["status"], "OK")
            self.assertEqual(report["summary"]["reviewed_findings"], 1)
            self._write_reviewed(reviewed, [{
                "commit": "0" * 40, "code": "HISTORICAL_PATH_PREFIX",
                "path": "build/sub", "reason": "different commit"}])
            report = history_audit.generate_full_history_audit_report(root, reviewed)
            self.assertEqual(report["status"], "FAIL")

    def test_review_of_one_blob_never_excuses_other_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            root.mkdir()
            fixture = " ".join(("private", "trace", "path")) + "\n"
            self._repo_with_removed_blob(root, "notes.txt", fixture)
            reviewed = Path(tmp) / "reviewed.json"
            self._write_reviewed(reviewed, [{
                "blob": "0" * 40, "code": "HISTORICAL_BLOB_PRIVATE_VOCABULARY",
                "path": "notes.txt", "reason": "different content"}])
            report = history_audit.generate_full_history_audit_report(root, reviewed)
            self.assertEqual(report["status"], "FAIL")
            self.assertEqual(report["summary"]["reviewed_findings"], 0)

    def _repo_with_message(self, root, subject, body):
        run_git(("init", "-q"), cwd=root, check=True, capture_output=True)
        (root / "safe.txt").write_text("safe\n", encoding="utf-8")
        run_git(["add", "safe.txt"], cwd=root, check=True)
        run_git(["commit", "-q", "-m", subject, "-m", body], cwd=root, check=True)
        return run_git(["rev-parse", "HEAD"], cwd=root, check=True,
                       capture_output=True, text=True).stdout.strip()

    def test_commit_body_is_scanned_not_only_the_subject(self):
        """A squash merge copies the PR body into the message; the body reaches history too."""
        root_literal = "Q:/" + "synthetic-private-root"
        user_path = "C:" + chr(92) + "Us" + "ers" + chr(92) + "alice" + chr(92) + "notes"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sha = self._repo_with_message(
                root, "clean subject",
                f"Landed from {root_literal}/landings/landing.json and {user_path}.")
            findings = history_audit.audit_history_commit_metadata(
                root, private_roots=(root_literal.upper(),))
        codes = sorted((f.code, f.commit, f.path) for f in findings)
        self.assertEqual(codes, [
            ("COMMIT_LOG_LOCAL_PATH", sha[:12], "<commit_message>"),
            ("COMMIT_LOG_PRIVATE_ROOT", sha[:12], "<commit_message>"),
        ])

    def test_clean_commit_message_has_no_findings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._repo_with_message(root, "clean subject", "A body that names build/ and docs/ only.")
            self.assertEqual(
                history_audit.audit_history_commit_metadata(root, private_roots=("Q:/nowhere",)), [])

    def test_canonical_policy_supplies_private_roots(self):
        roots = history_audit.policy_private_roots(history_audit.ROOT)
        self.assertTrue(roots, "assets/public_source_profile.json must declare private_roots")
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(history_audit.policy_private_roots(Path(tmp)), ())

    def test_reviewed_commit_message_is_reported_but_not_a_finding(self):
        root_literal = "Q:/" + "synthetic-private-root"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            root.mkdir()
            sha = self._repo_with_message(root, "clean subject", f"see {root_literal}/x")
            reviewed = Path(tmp) / "reviewed.json"
            self._write_reviewed(reviewed, [{
                "commit": sha, "code": "COMMIT_LOG_PRIVATE_ROOT",
                "path": "<commit_message>", "reason": "synthetic fixture"}])
            with mock.patch.object(history_audit, "policy_private_roots", return_value=(root_literal,)):
                report = history_audit.generate_full_history_audit_report(root, reviewed)
                self.assertEqual(report["status"], "OK", report["findings"])
                self.assertEqual(report["summary"]["reviewed_findings"], 1)
                self._write_reviewed(reviewed, [{
                    "commit": "0" * 40, "code": "COMMIT_LOG_PRIVATE_ROOT",
                    "path": "<commit_message>", "reason": "a different commit"}])
                report = history_audit.generate_full_history_audit_report(root, reviewed)
                self.assertEqual(report["status"], "FAIL")

    def test_reviewed_entry_names_exactly_one_object(self):
        with tempfile.TemporaryDirectory() as tmp:
            reviewed = Path(tmp) / "reviewed.json"
            for entry in (
                {"code": "X", "path": "p", "reason": "r"},
                {"blob": "a" * 40, "commit": "b" * 40, "code": "X", "path": "p", "reason": "r"},
                {"commit": "abc", "code": "X", "path": "p", "reason": "r"},
            ):
                with self.subTest(entry=entry):
                    self._write_reviewed(reviewed, [entry])
                    with self.assertRaises(ValueError):
                        history_audit.load_reviewed_findings(reviewed)

    def test_malformed_reviewed_file_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            reviewed = Path(tmp) / "reviewed.json"
            self._write_reviewed(reviewed, [{"blob": "abc", "code": "X", "path": "p", "reason": "r"}])
            with self.assertRaises(ValueError):
                history_audit.load_reviewed_findings(reviewed)
            self._write_reviewed(reviewed, [{"blob": "a" * 40, "code": "X", "path": "p", "reason": " "}])
            with self.assertRaises(ValueError):
                history_audit.load_reviewed_findings(reviewed)

    def test_plain_title_file_name_is_not_private_vocabulary(self):
        self.assertIsNone(history_audit.PRIVATE_OPERATIONAL_VOCABULARY.search(
            "archive = " + "GAMEDATA" + ".BDL"))
        self.assertIsNotNone(history_audit.PRIVATE_OPERATIONAL_VOCABULARY.search(
            "HST" + "_PGD_VKEY_HEX"))

    def test_real_length_tokens_are_secrets_in_blobs_and_messages(self):
        # A real fine-grained PAT body is 22 characters, an underscore and 59 more; a
        # current Stripe live key runs far past 24. The fixed {80} and {24} counts, each
        # followed by \b, could match neither, so the history gate passed over both.
        tokens = {
            "fine-grained PAT": "github" + "_pat_" + "Q7" * 11 + "_" + "w3" * 29 + "x",
            "long live key": "sk" + "_live_" + "Zt9" * 33,
            "OAuth token": "gh" + "o_" + "N4" * 18,
            "server token": "gh" + "s_" + "k8" * 18,
        }
        for label, token in tokens.items():
            with self.subTest(token=label):
                self.assertTrue(history_audit._has_api_token("value " + token + "\n"))
                self.assertIn("HISTORICAL_BLOB_SECRET",
                              [f.code for f in self._blob_findings("key " + token + "\n")])
                finding = history_audit.HistoryFinding("DEFINITE_SECRET", "X", "c", "p", "saw " + token)
                self.assertNotIn(token[-8:], finding.to_dict()["detail"], "the whole token is redacted")
                with tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    self._repo_with_message(root, "clean subject", "body names " + token)
                    codes = [f.code for f in history_audit.audit_history_commit_metadata(
                        root, private_roots=("Q:/nowhere",))]
                self.assertIn("COMMIT_LOG_SECRET", codes)

    def test_escaped_windows_and_unc_paths_are_local_paths(self):
        # JSON, C and Python literals double every backslash. The single-backslash
        # patterns matched none of these, so a committed manifest naming a user's
        # folder passed the gate.
        bs = chr(92)
        escaped = bs * 2
        positives = {
            "json user path": '{"p": "C:' + escaped + "Us" + "ers" + escaped + "someone" + escaped + 'Desktop"}',
            "json legacy profile": '"D:' + escaped + "Documents and Settings" + escaped + 'someone"',
            "json unc share": '{"p": "' + escaped * 2 + "fileserver" + escaped + 'share"}',
            "json temp dir": '"C:' + escaped + "Windows" + escaped + "Te" + "mp" + escaped + 'build1"',
        }
        for label, text in positives.items():
            with self.subTest(text=label):
                self.assertIn("HISTORICAL_BLOB_LOCAL_PATH",
                              [f.code for f in self._blob_findings(text + "\n")])
        # An escape sequence in source is not a share: a mixed backslash count is refused.
        for text in ('printf("' + escaped + "n" + escaped + 't");',
                     "r'" + escaped + "w+" + bs + "d'"):
            with self.subTest(text=text):
                self.assertIsNone(history_audit.UNC_PATH.search(text))
        message_path = "C:" + escaped + "Us" + "ers" + escaped + "someone" + escaped + "x"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._repo_with_message(root, "clean subject", "copied " + message_path)
            codes = [f.code for f in history_audit.audit_history_commit_metadata(
                root, private_roots=("Q:/nowhere",))]
        self.assertIn("COMMIT_LOG_LOCAL_PATH", codes)

    def test_keys_directory_and_upper_case_key_names_are_forbidden_paths(self):
        # keys/ holds a user's local key constants (AGENTS.md section 5); no prefix,
        # name or extension rule covered it, and a forbidden name in another letter
        # case slipped past the exact comparison.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run_git(("init", "-q"), cwd=root, check=True, capture_output=True)
            (root / "keys").mkdir()
            (root / "keys" / "console_values.txt").write_text("synthetic\n", encoding="utf-8")
            # Distinct bytes: rev-list names each object once, under one of its paths.
            (root / "PGD_KEYS.TXT").write_text("another synthetic\n", encoding="utf-8")
            run_git(["add", "-A"], cwd=root, check=True)
            run_git(["commit", "-qm", "fixture"], cwd=root, check=True)
            found = sorted((f.code, f.path) for f in history_audit.audit_history_tree_paths(root))
        self.assertIn(("HISTORICAL_PATH_PREFIX", "keys/console_values.txt"), found)
        self.assertIn(("HISTORICAL_GAME_ARTIFACT", "PGD_KEYS.TXT"), found)

    def test_forbidden_path_whose_bytes_sit_elsewhere_is_still_found(self):
        # rev-list --objects names each object once, under the first path it meets. An
        # empty build/x.txt, a .iso copy of an allowed file, or a forbidden name in a
        # later commit with old bytes never showed its forbidden path.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run_git(("init", "-q"), cwd=root, check=True, capture_output=True)
            (root / "a.txt").write_bytes(b"")
            (root / "build").mkdir()
            (root / "build" / "x.txt").write_bytes(b"")
            (root / "readme.txt").write_bytes(b"same bytes\n")
            (root / "disc.iso").write_bytes(b"same bytes\n")
            run_git(["add", "-A"], cwd=root, check=True)
            run_git(["commit", "-qm", "fixture"], cwd=root, check=True)
            (root / "notes.txt").write_bytes(b"older bytes\n")
            run_git(["add", "-A"], cwd=root, check=True)
            run_git(["commit", "-qm", "second"], cwd=root, check=True)
            run_git(["mv", "notes.txt", "nidseq_mine.txt"], cwd=root, check=True)
            run_git(["commit", "-qm", "rename"], cwd=root, check=True)
            found = {(f.code, f.path) for f in history_audit.audit_history_tree_paths(root)}
            report = history_audit.generate_full_history_audit_report(root, root / "absent.json")
        self.assertIn(("HISTORICAL_PATH_PREFIX", "build/x.txt"), found)
        self.assertIn(("HISTORICAL_PROHIBITED_EXTENSION", "disc.iso"), found)
        self.assertIn(("HISTORICAL_GAME_ARTIFACT", "nidseq_mine.txt"), found)
        self.assertEqual(report["status"], "FAIL")

    def _corrupt_loose_blob(self, root, name):
        blob = run_git(["rev-parse", "HEAD:" + name], cwd=root, check=True,
                       capture_output=True, text=True).stdout.strip()
        loose = root / ".git" / "objects" / blob[:2] / blob[2:]
        loose.chmod(0o644)
        loose.write_bytes(b"not a zlib stream")

    def test_unreadable_blob_fails_the_audit_instead_of_being_skipped(self):
        # cat-file --batch-check reports a corrupt object as "missing" with exit status 0;
        # the scan dropped it and the gate said OK over a blob it never read.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run_git(("init", "-q"), cwd=root, check=True, capture_output=True)
            for name in ("a.txt", "b.txt", "c.txt"):
                (root / name).write_text(name + " synthetic\n", encoding="utf-8")
            run_git(["add", "-A"], cwd=root, check=True)
            run_git(["commit", "-qm", "fixture"], cwd=root, check=True)
            self.assertEqual(history_audit.audit_history_blob_contents(root), [])
            self._corrupt_loose_blob(root, "b.txt")
            with self.assertRaisesRegex(RuntimeError, "cannot be read"):
                history_audit.audit_history_blob_contents(root)
            with self.assertRaises(RuntimeError):
                history_audit.generate_full_history_audit_report(root, root / "absent.json")

    def test_large_blob_inventory_raises_instead_of_reporting_none(self):
        # Asked without the report's batch-check answer, the size pass swallowed every
        # error and an unreadable object, and reported no large blobs.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run_git(("init", "-q"), cwd=root, check=True, capture_output=True)
            for name in ("a.txt", "b.txt"):
                (root / name).write_text(name + " synthetic\n", encoding="utf-8")
            run_git(["add", "-A"], cwd=root, check=True)
            run_git(["commit", "-qm", "fixture"], cwd=root, check=True)
            self.assertEqual(len(history_audit.audit_large_blobs(root, size_threshold=1)), 2)
            self._corrupt_loose_blob(root, "b.txt")
            with self.assertRaisesRegex(RuntimeError, "cannot be read"):
                history_audit.audit_large_blobs(root, size_threshold=1)
            listing = history_audit._object_listing(root)
            with mock.patch.object(history_audit.subprocess, "run",
                                   return_value=mock.Mock(returncode=128, stdout=b"", stderr=b"fatal")):
                with self.assertRaisesRegex(RuntimeError, "batch-check failed"):
                    history_audit.audit_large_blobs(root, size_threshold=1, raw_objects=listing)

    def test_short_cat_file_output_fails_the_audit(self):
        # A cat-file --batch that dies partway leaves short output; the scan stopped
        # there and reported what it had read as the whole history.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run_git(("init", "-q"), cwd=root, check=True, capture_output=True)
            for name in ("a.txt", "b.txt"):
                (root / name).write_text(name + " synthetic\n", encoding="utf-8")
            run_git(["add", "-A"], cwd=root, check=True)
            run_git(["commit", "-qm", "fixture"], cwd=root, check=True)
            real_popen = history_audit.subprocess.Popen

            def truncating_popen(argv, *args, **kwargs):
                proc = real_popen(argv, *args, **kwargs)
                if list(argv[-2:]) != ["cat-file", "--batch"]:
                    return proc
                real_communicate = proc.communicate

                def communicate(data=None):
                    out, err = real_communicate(data)
                    return out[: len(out) // 2], err

                proc.communicate = communicate  # type: ignore[method-assign]
                return proc

            with mock.patch.object(history_audit.subprocess, "Popen", side_effect=truncating_popen):
                with self.assertRaisesRegex(RuntimeError, "cat-file --batch"):
                    history_audit.audit_history_blob_contents(root)

    def test_blob_naming_a_private_root_is_a_finding(self):
        # private_roots were checked in commit messages only, so a historical blob naming
        # the private workspace passed the Full-History Audit gate.
        bs = chr(92)
        roots = ("Q:/" + "nsynth", "Q:" + bs + "nsynth")
        positives = {
            "slash": "see Q:/nsynth/private/x\n",
            "backslash": "see Q:" + bs + "NSynth" + bs + "oracle\n",
            "json escaped": '{"p": "q:' + bs * 2 + "nsynth" + bs * 2 + 'logs"}\n',
            "bare": "root is Q:/nsynth.\n",
        }
        for label, text in positives.items():
            with self.subTest(text=label):
                self.assertIn("HISTORICAL_BLOB_PRIVATE_ROOT",
                              [f.code for f in self._blob_findings_bytes(text.encode(), private_roots=roots)])
        negatives = {
            "longer name": "see Q:/nsynthx/y\n",
            "escaped newline": 'printf("eq:' + bs + 'nsynthetic");\n',
        }
        for label, text in negatives.items():
            with self.subTest(text=label):
                self.assertNotIn("HISTORICAL_BLOB_PRIVATE_ROOT",
                                 [f.code for f in self._blob_findings_bytes(text.encode(), private_roots=roots)])
        # The policy file declares the roots; it is the one blob allowed to name them.
        self.assertNotIn("HISTORICAL_BLOB_PRIVATE_ROOT", [
            f.code for f in self._blob_findings_bytes(
                b'{"private_roots": ["Q:/nsynth"]}\n',
                name=history_audit.POLICY_PATH.as_posix(), private_roots=roots)])
        patterns = history_audit.private_root_patterns(roots)
        self.assertEqual(len(patterns), 1, "both spellings of one root are one pattern")

    def test_canonical_private_roots_reach_the_blob_scan(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run_git(("init", "-q"), cwd=root, check=True, capture_output=True)
            (root / "notes.txt").write_text("see Q:/synthroot/a\n", encoding="utf-8")
            run_git(["add", "-A"], cwd=root, check=True)
            run_git(["commit", "-qm", "fixture"], cwd=root, check=True)
            with mock.patch.object(history_audit, "policy_private_roots", return_value=("Q:/synthroot",)):
                report = history_audit.generate_full_history_audit_report(root, root / "absent.json")
        self.assertIn("HISTORICAL_BLOB_PRIVATE_ROOT", [f["code"] for f in report["findings"]])

    def test_wrapped_and_single_line_base64_payloads_are_flagged(self):
        # Standard base64 wraps at 76 columns; the payload check matched only one line.
        import base64
        import random
        payload = random.Random(20261117).randbytes(800 * 1024)
        forms = {
            "wrapped": base64.encodebytes(payload),
            "single line": base64.b64encode(payload) + b"\n",
            "wrapped CRLF": base64.encodebytes(payload).replace(b"\n", b"\r\n"),
        }
        for label, data in forms.items():
            with self.subTest(form=label):
                self.assertGreaterEqual(len(data), 1024 * 1024)
                self.assertIn("HISTORICAL_BLOB_ENCODED_PAYLOAD",
                              [f.code for f in self._blob_findings_bytes(data, name="dump.txt")])
        prose = (b"Ordinary documentation, with punctuation and words.\n" * 21000)
        self.assertNotIn("HISTORICAL_BLOB_ENCODED_PAYLOAD",
                         [f.code for f in self._blob_findings_bytes(prose, name="notes.md")])

    def test_commit_message_runs_the_blob_text_signatures(self):
        # The message pass ran only the user-path, cloud-folder, root and secret checks;
        # a squash-merge body naming a share, a temp folder, the private repository or
        # private vocabulary passed while the same text in a blob was a finding.
        bs = chr(92)
        cases = {
            "COMMIT_LOG_LOCAL_PATH": [
                "copied from " + bs * 2 + "nas" + bs + "captures",
                "staged in C:" + bs + "Windows" + bs + "Te" + "mp" + bs + "run1",
                "wrote /tm" + "p/run1",
            ],
            "COMMIT_LOG_PRIVATE_REPO": ["see github.com/" + "Jstar269/nakagawa-recomp-" + "history-private"],
            "COMMIT_LOG_PRIVATE_VOCABULARY": ["set HST" + "_PGD_VKEY_HEX first"],
        }
        for code, bodies in cases.items():
            for body in bodies:
                with self.subTest(body=body), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    sha = self._repo_with_message(root, "clean subject", body)
                    found = [(f.code, f.commit) for f in history_audit.audit_history_commit_metadata(
                        root, private_roots=("Q:/nowhere",))]
                    self.assertIn((code, sha[:12]), found)

if __name__ == "__main__":
    unittest.main()
