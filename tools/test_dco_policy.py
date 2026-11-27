# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent.parent


class RuleStatementAssertions(unittest.TestCase):
    def assertRuleStated(self, blocks, terms, rule):
        matches = [block for block in blocks if all(term.lower() in block.lower() for term in terms)]
        self.assertTrue(matches, f"the documentation no longer states {rule}: no block has all of {terms}")


class TestDCOPolicy(RuleStatementAssertions):
    def test_dco_policy_document_exists(self):
        doc_file = ROOT / "docs" / "DCO_POLICY.md"
        self.assertTrue(doc_file.is_file(), "docs/DCO_POLICY.md file missing")

        content = doc_file.read_text(encoding="utf-8")
        self.assertRuleStated(
            _blocks(_section(content, 1)),
            ("DCO 1.1", "Signed-off-by:"),
            "the DCO 1.1 commit-attestation requirement",
        )
        self.assertRuleStated(
            _blocks(_section(content, 2)),
            ("Developer Certificate of Origin", "Version 1.1"),
            "the DCO 1.1 legal text",
        )
        self.assertIn("Signed-off-by:", content)
        self.assertRuleStated(
            _blocks(_section(content, 3)),
            ("Signed-off-by:", "every commit"),
            "the commit sign-off procedure",
        )
        self.assertRuleStated(
            _blocks(_section(content, 3)),
            ("automatically add", "-s", "--signoff"),
            "the command-line option for adding a sign-off trailer",
        )
        self.assertRegex(_section(content, 3), r"git commit -s\b")
        self.assertRuleStated(
            _blocks(_section(content, 4)),
            ("missing `Signed-off-by:` line", "not** a merge blocker", "may merge"),
            "that a pull request without a sign-off can still be merged",
        )
        self.assertRuleStated(
            _blocks(_section(content, 5)),
            ("Automated Bots", "Dependency Updates"),
            "the bot and dependency-update policy section",
        )

    def test_contributing_dco_reference(self):
        contrib_file = ROOT / "CONTRIBUTING.md"
        self.assertTrue(contrib_file.is_file(), "CONTRIBUTING.md file missing")

        content = contrib_file.read_text(encoding="utf-8")
        self.assertRegex(content, r"\[[^\]]+\]\(docs/DCO_POLICY\.md\)")
        blocks = _blocks(content)
        self.assertRuleStated(
            blocks,
            ("DCO 1.1", "sign-off", "optional"),
            "that a DCO sign-off is optional",
        )
        self.assertRuleStated(
            blocks,
            ("right to submit", "GPL-3.0-or-later", "template"),
            "the right-to-submit confirmation",
        )
        self.assertIn("Signed-off-by:", content)

    def test_pr_template_dco_checkbox(self):
        pr_tmpl = ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md"
        self.assertTrue(pr_tmpl.is_file(), "PULL_REQUEST_TEMPLATE.md file missing")

        content = pr_tmpl.read_text(encoding="utf-8")
        # These are legal attestations: preserve the right-to-submit, optional sign-off,
        # third-party-source and AI-assistance statements verbatim.
        for legal_line in (
            "- [ ] I wrote this change, or have the right to submit it, under the project's license (GPL-3.0-or-later). See [contribution rights](https://github.com/Jstar269/nakagawa-recomp/blob/main/docs/DCO_POLICY.md).",
            "- [ ] Optional: every commit has a `Signed-off-by:` line from its human author, added with `git commit -s` ([Developer Certificate of Origin 1.1](https://github.com/Jstar269/nakagawa-recomp/blob/main/docs/DCO_POLICY.md#3-how-to-sign-off-commits-optional)).",
            "- [ ] Or: new third-party material is identified below, with its exact source, revision and license, and its notices are kept.",
            "- [ ] Or: material AI assistance is disclosed below, with the source needed for review.",
        ):
            with self.subTest(legal_line=legal_line):
                self.assertIn(legal_line, content)

    def test_maintainer_waiver_is_standing_and_survives_public_launch(self):
        # The waiver's whole purpose is that it does NOT lapse at public launch.
        # An edit that re-scopes it to "prior to public launch" would silently
        # reintroduce a merge blocker on a single-maintainer public repository.
        content = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        blocks = _blocks(_section(content, 5))
        self.assertIn("Maintainer Standing Waiver", content)
        self.assertRuleStated(
            blocks,
            ("waiver", "until", "explicitly revokes"),
            "a revocation-scoped maintainer waiver",
        )
        self.assertRuleStated(
            blocks,
            ("public launch", "not time-limited", "commit-range-limited", "repository-visibility-limited"),
            "a waiver that continues after public launch",
        )
        self.assertTrue(
            any(re.search(r"does \*\*not\*\* expire", block) for block in blocks),
            "the waiver must state that public launch does not end it",
        )
        self.assertFalse(
            any("prior to public launch" in block.lower() for block in blocks),
            "the superseded launch-scoped waiver wording must not return",
        )

    def test_waived_missing_signoff_is_not_a_merge_blocker(self):
        content = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        blocks = _blocks(_section(content, 5))
        self.assertRuleStated(
            blocks,
            ("missing `Signed-off-by:` trailer", "not** a merge blocker", "not a defect"),
            "the waived sign-off's non-blocking status",
        )
        self.assertRuleStated(
            blocks,
            ("history", "not rewritten", "sign-off checker"),
            "the ban on retroactively rewriting history for sign-offs",
        )

    def test_dco_is_not_a_required_status_check(self):
        content = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        blocks = _blocks(_section(content, 6))
        self.assertRuleStated(
            blocks,
            ("DCO is not a required status check", "public repository", "optional"),
            "that DCO is not a required status check",
        )
        self.assertRuleStated(
            blocks,
            ("automated DCO check", "advisory", "non-blocking"),
            "the non-blocking DCO status-check mode",
        )
        self.assertFalse(
            any(
                all(term in block.lower() for term in ("dco check workflow", "upon public launch"))
                for block in _blocks(content)
            ),
            "the superseded launch-triggered DCO check must not return",
        )

    def test_agents_may_never_fabricate_a_signoff(self):
        # The waiver must not read as agent authority to sign on someone's
        # behalf. This rule is stated in three places; all must hold.
        policy = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        self.assertRuleStated(
            _blocks(_section(policy, 5)),
            ("agent", "no", "authority", "sign-off"),
            "the ban on agents fabricating sign-offs",
        )
        for rel in ("AGENTS.md", "docs/AI_USAGE.md"):
            text = (ROOT / rel).read_text(encoding="utf-8")
            if rel == "AGENTS.md":
                self.assertRuleStated(
                    _blocks(_section(text, 3)),
                    ("contributor identity", "Signed-off-by:", "DCO", "source lineage"),
                    f"{rel}'s ban on fabricated sign-off identities",
                )
            else:
                blocks = _blocks(text)
                self.assertRuleStated(
                    blocks,
                    ("must not invent", "identity", "DCO sign-off"),
                    f"{rel}'s ban on invented sign-off identities",
                )
                self.assertRuleStated(
                    blocks,
                    ("must not add", "sign-off", "maintainer's behalf"),
                    f"{rel}'s ban on adding sign-offs for the maintainer",
                )

    def test_outside_contributor_rules_are_forgiving_but_keep_the_license(self):
        # Maintainer decision 2026-10-30: sign-off is optional and a missed box never closes a pull
        # request, but borrowed code must still name its source and carry a GPL-compatible license.
        policy = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        self.assertRuleStated(
            _blocks(_section(policy, 5)),
            ("outside contributors", "right to submit", "third-party source", "AI-assisted work disclosure"),
            "outside contributors' right-to-submit, third-party and AI disclosure",
        )
        self.assertRuleStated(
            _blocks(_section(policy, 4)),
            ("never by itself a reason to close", "asks", "GPL-compatible license"),
            "that a missed box is asked about, not rejected, while the license rule stays",
        )
        self.assertRuleStated(
            _blocks(policy),
            ("GitHub's Terms of Service", "inbound = outbound", "GPL-3.0-or-later"),
            "how a contribution is licensed",
        )

        contrib = (ROOT / "CONTRIBUTING.md").read_text(encoding="utf-8")
        self.assertRuleStated(
            _blocks(contrib),
            ("not blocked", "asks in review", "GPL-compatible license"),
            "the forgiving review rule in CONTRIBUTING.md",
        )

        pr_tmpl = (ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md").read_text(encoding="utf-8")
        # These disclosure lines are legal attestations and remain verbatim.
        self.assertIn(
            "- [ ] Or: new third-party material is identified below, with its exact source, revision and license, and its notices are kept.",
            pr_tmpl,
        )
        self.assertIn(
            "- [ ] Or: material AI assistance is disclosed below, with the source needed for review.",
            pr_tmpl,
        )
        # A pull request body resolves relative links against the repository root URL, not the
        # template's folder, so "../docs/X.md" opens a 404 page; template links must be absolute.
        relative = re.findall(r"\]\((?!https?://|#|mailto:)([^)]+)\)", pr_tmpl)
        self.assertEqual(relative, [], "PR template links must be absolute GitHub URLs")

    def test_dco_signoff_regex(self):
        pattern = re.compile(r"^Signed-off-by:\s+([^<]+)\s+<([^>]+)>$", re.MULTILINE)
        msg_valid = "feat(tooling): add feature\n\nSigned-off-by: Jane Doe <jane@example.com>"
        msg_invalid = "feat(tooling): add feature without signoff"

        self.assertTrue(pattern.search(msg_valid))
        self.assertFalse(pattern.search(msg_invalid))


def _section(content: str, number: int) -> str:
    """Return numbered section ``number`` (heading to the next ``## `` heading)."""
    start = re.search(rf"(?m)^## {number}\.\s", content)
    if start is None:
        raise AssertionError(f"AGENTS.md has no section {number}")
    end = re.search(r"(?m)^## ", content[start.end():])
    return content[start.start(): start.end() + end.start()] if end else content[start.start():]


def _heading_section(content: str, heading: str) -> str:
    """Return the section under the exact Markdown ``heading`` line, up to the next heading of its level or higher."""
    start = re.search(rf"(?m)^{re.escape(heading)}\s*$", content)
    if start is None:
        raise AssertionError(f"no section headed {heading!r}")
    level = len(heading) - len(heading.lstrip("#"))
    end = re.search(rf"(?m)^#{{1,{level}}} ", content[start.end():])
    return content[start.start(): start.end() + end.start()] if end else content[start.start():]


def _execution_contract() -> str:
    """The execution contract AGENTS.md section 7 points to, in docs/ARCHITECTURE.md."""
    return _heading_section((ROOT / "docs" / "ARCHITECTURE.md").read_text(encoding="utf-8"),
                            "## Current execution contract")


def _blocks(section: str) -> list[str]:
    """Split a section into bullets and paragraphs, each with its wrapping collapsed.

    The checks below look for a rule's key terms inside one block, so rewording,
    reflowing or reordering a rule never fails them; dropping a term does.
    """
    blocks: list[str] = []
    for line in section.splitlines():
        stripped = line.strip()
        if not stripped:
            blocks.append("")
        elif stripped.startswith(("- ", "* ", "|", "```", "#")) or not blocks or not blocks[-1]:
            blocks.append(stripped)
        else:
            blocks[-1] += " " + stripped
    return [block for block in blocks if block]


class TestGovernanceSurfaces(RuleStatementAssertions):
    """Small fail-closed checks for the public agent/configuration contract.

    These pin the rules AGENTS.md must state, as the key terms each rule needs,
    not its sentences: a maintainer can reword or reflow the contract, and the
    test fails only when a rule loses a term that carries its meaning.
    """

    def test_agents_contract_pins_required_content(self):
        content = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
        self.assertRegex(content, r"(?m)^## Where to start\s*$")
        for section in range(1, 12):
            with self.subTest(section=section):
                self.assertRegex(content, rf"(?m)^## {section}\.\s")

        self.assertRuleStated(
            _blocks(_section(content, 4)),
            ("Assisted-by:", "never", "Co-Authored-By:", "docs/AI_USAGE.md"),
            "the agent commit trailer rule",
        )

        section_3 = _section(content, 3)
        section_3_blocks = _blocks(section_3)
        self.assertRuleStated(
            section_3_blocks,
            ("explicit maintainer authorization", "current turn"),
            "that the hard stops need explicit maintainer authorization in the current turn",
        )
        hard_stop_bullets = [block for block in section_3_blocks if block.startswith("- ")]
        hard_stops = {
            "attestation on a maintainer's behalf": ("provenance attestation", "legal clearance", "behalf"),
            "no invented sign-off or lineage": ("contributor identity", "Signed-off-by:", "DCO", "source lineage"),
            "no history rewriting": ("history rewriting", "force-push", "another agent's work"),
            "no settings changes": ("security", "settings", "exact setting"),
            "no device writes": ("firmware", "flash", "NAND", "idStorage", "damage"),
            "no private disclosure": ("disclosing private inputs", "keys", "derived bytes", "Section 5"),
        }
        for rule, terms in hard_stops.items():
            with self.subTest(hard_stop=rule):
                self.assertRuleStated(hard_stop_bullets, terms, f"the hard stop: {rule}")

        hardware = [block for block in section_3_blocks if block.startswith("**PSP hardware.**")]
        self.assertEqual(len(hardware), 1, "section 3 needs one PSP hardware paragraph")
        self.assertRuleStated(
            hardware,
            ("only as its user directs", "physical steps", "never simulated", "device writes", "forbidden"),
            "the bounds of a contributor's PSP use",
        )

        execution_contracts = (
            (118, ("fail-closed", "analyzer-owned", "fail closed", "not an all-Allegrex interpreter")),
            (126, ("`jr`/`jalr`", "latched at transfer", "before link writes", "delay slot")),
            (127, ("`target`", "`resume_pc`", "`dispatch_call`", "CALL and TAIL", "not a resume descriptor")),
            (128, ("cosimulation", "CALL/TAIL", "negative corpus", "build-only mutant", "not a valid semantic kill")),
        )
        # Section 7 keeps a pointer and its binding rules; the contract itself lives in docs/ARCHITECTURE.md.
        self.assertRuleStated(
            _blocks(_section(content, 7)),
            ("docs/ARCHITECTURE.md#current-execution-contract", "re-check", "live source", "never a resume descriptor"),
            "section 7's pointer to the execution contract",
        )
        section_7_blocks = _blocks(_execution_contract())
        last_position = -1
        for pr, terms in execution_contracts:
            with self.subTest(contract=pr):
                bullets = [i for i, block in enumerate(section_7_blocks) if block.startswith(f"- **PR #{pr}:**")]
                self.assertEqual(len(bullets), 1, f"section 7 needs one PR #{pr} contract bullet")
                self.assertRuleStated([section_7_blocks[bullets[0]]], terms, f"the PR #{pr} execution contract")
                self.assertGreater(bullets[0], last_position, "the execution contracts stay in PR order")
                last_position = bullets[0]

        self.assertRuleStated(
            _blocks(_section(content, 11)),
            ("docs/AI_USAGE.md#the-final-report", "exact facts", "`NOT_PERFORMED`", "`none`"),
            "section 11's pointer to the report template",
        )
        final_report = _heading_section((ROOT / "docs" / "AI_USAGE.md").read_text(encoding="utf-8"),
                                        "### The final report")
        report = re.search(r"```text\n(.*?)```", final_report, re.S)
        self.assertIsNotNone(report, "docs/AI_USAGE.md's final report section needs its report template block")
        fields = [line.split(":", 1)[0] for line in report.group(1).splitlines() if line.strip()]
        self.assertEqual(
            fields,
            [
                "LANE", "MISSION", "BASE_SHA", "HEAD_SHA", "PR", "STATUS", "RESULT", "ROOT_CAUSE",
                "CHANGED_FILES", "NEW_PATH_PROVENANCE", "FAILING_BEFORE", "TESTS", "PRIVATE_ACCEPTANCE",
                "HARDWARE", "UNRESOLVED", "DISCOVERED_FOLLOWUPS", "NOT_PERFORMED", "READY_QUALITY",
            ],
            "the section 11 report template must keep its fields",
        )

    def test_agents_marks_upstream_behavior_as_a_bounded_evidence_tier(self):
        content = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
        section_6 = " ".join(_blocks(_section(content, 6))).lower()
        section_7 = " ".join(_blocks(_section(content, 7))).lower()
        contract = " ".join(_blocks(_execution_contract())).lower()
        for required in (
            "unknown nids and unowned dispatches stop visibly",
            "a modeled psp error may return to the guest",
            "do not deliberately crash the player",
            "reported visibly",
            "stays a named stop",
        ):
            with self.subTest(section=6, required=required):
                self.assertIn(required, section_6)
        for required in (
            "ppsspp-derived, not psp-measured",
            "jpcsp-derived, not psp-measured",
            "exact upstream revision and symbol",
            "log first use once per run",
            "a psp measurement for the same scope supersedes it",
            "no public reference",
            "save-data and flash writes",
            "never fills an unknown nid",
            "production-path regression",
        ):
            with self.subTest(section="docs/ARCHITECTURE.md execution contract", required=required):
                self.assertIn(required, contract)
        # The second tier's limits stay resident in AGENTS.md section 7 as well.
        for required in ("never fills an unknown nid", "save-data and flash writes", "no public reference"):
            with self.subTest(section=7, required=required):
                self.assertIn(required, section_7)

    def test_hard_limits_and_review_separate_guest_errors_from_hidden_failures(self):
        content = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
        start = content.index("### Hard limits")
        hard_limits = " ".join(_blocks(content[start:content.index("\n### ", start + 1)])).lower()
        for required in ("no hidden failures", "fake success", "deliberate crashes", "a modeled psp error"):
            with self.subTest(surface="AGENTS.md hard limits", required=required):
                self.assertIn(required, hard_limits)
        review = " ".join((ROOT / ".github/REVIEW.md").read_text(encoding="utf-8").lower().split())
        for required in ("modeled psp error", "whole-run stop", "host crash"):
            with self.subTest(surface=".github/REVIEW.md", required=required):
                self.assertIn(required, review)

    def test_agents_keeps_the_maintainer_workspace_process_out_of_the_public_contract(self):
        # The maintainer's own workspace, hardware and landing procedure lives outside the
        # repository; the public file says so, states that it can only add rules, and keeps
        # the integration and merge boundaries every contributor relies on.
        content = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
        self.assertNotRegex(content, r"(?m)^## Maintainer process")
        self.assertNotIn("Standing hardware authorization", content)
        intro = " ".join(_blocks(content[: content.index("## Where to start")]))
        self.assertRuleStated([intro], ("outside this repository", "only add", "never relax"), "the split")
        section_10 = _blocks(_section(content, 10))
        self.assertRuleStated(section_10, ("**Integration**", "full required hosted CI is green", "goes back to its author"),
                              "the public integration boundary")
        self.assertRuleStated(section_10, ("maintainer authorizes", "merges a PR", "nobody bypasses", "required checks"),
                              "the merge boundary")
        self.assertRuleStated(_blocks(_section(content, 8)), ("before the pull request opens", "Nobody pushes to `main`"),
                              "the early push of a planned topic branch")

    def test_agents_scopes_opt_in_network_approval(self):
        content = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
        section_5 = " ".join(_blocks(_section(content, 5))).lower()
        for required in (
            "offline by default",
            "approves opt-in network features in principle",
            "does not authorize an upload, or any connection before the player has opted in to the feature",
            "docs/setup.md#adding-an-opt-in-host-or-feature",
            "no setting makes a nid miss permissive",
        ):
            with self.subTest(surface="AGENTS.md section 5", required=required):
                self.assertIn(required, section_5)
        # The opt-in checklist section 5 points to lives in docs/SETUP.md Network use.
        checklist = " ".join(_blocks(_heading_section(
            (ROOT / "docs" / "SETUP.md").read_text(encoding="utf-8"), "#### Adding an opt-in host or feature"))).lower()
        for required in (
            "approves opt-in network features in principle",
            "does not authorize an upload, or any connection before the player has opted in to the feature",
            "it is contacted only after that feature's opt-in",
            "receives nothing beyond what the feature's consent text names",
            "the pull request names the host and lists it in the feature's public issue, its consent text",
            "nothing connects before the player answers, and no answer means off",
            "each feature's public issue must name its hosts, payload, consent and data boundaries",
            "before it ships",
            "off by default",
            "listed in `docs/setup.md` network use",
            "tools/test_prereq_fetcher.py",
        ):
            with self.subTest(surface="docs/SETUP.md opt-in checklist", required=required):
                self.assertIn(required, checklist)

    def test_release_safety_rule_is_present_and_not_contradicted(self):
        policy_text = "\n".join(
            (ROOT / rel).read_text(encoding="utf-8")
            for rel in (
                "AGENTS.md",
                "CLAUDE.md",
                ".github/copilot-instructions.md",
                ".github/PULL_REQUEST_TEMPLATE.md",
            )
        )
        self.assertRuleStated(
            _blocks(_section((ROOT / "AGENTS.md").read_text(encoding="utf-8"), 3)),
            (
                "must not", "Git tags", "GitHub Releases", "release assets", "published version",
                "explicit maintainer authorization in the current turn", "do not authorize",
            ),
            "the tag and release rule",
        )
        self.assertNotRegex(
            policy_text,
            r"(?im)^[ \t-]*agents?\s+(?:may|can)\s+(?:create|edit|publish|delete).*\b(?:tag|release)",
        )

    def test_ai_and_dco_guidance_never_instruct_a_history_rewrite(self):
        # AGENTS.md Section 10 forbids stacked pull requests and rebasing or force-pushing a
        # pushed branch. The AI and DCO guidance must never offer either as a route.
        ai = (ROOT / "docs" / "AI_USAGE.md").read_text(encoding="utf-8")
        dco = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        self.assertRuleStated(
            _blocks(ai),
            ("never stack", "origin/main", "cherry-pick"),
            "the no-stacked-PR rule in AI_USAGE.md",
        )
        for rel, text in (("docs/AI_USAGE.md", ai), ("docs/DCO_POLICY.md", dco)):
            with self.subTest(document=rel):
                self.assertRuleStated(
                    _blocks(text),
                    ("never rebase", "force-push", "pushed branch"),
                    f"the no-history-rewrite rule in {rel}",
                )
                self.assertNotRegex(text, r"--force-with-lease|git rebase\b")
        self.assertNotIn("make the stack explicit", ai)

    def test_documented_gate_paths_exist(self):
        for rel in (
            "tools/codegen.py",
            "tools/policy_sync.py",
            "tools/publish_audit.py",
            "tools/modified_file_notice_audit.py",
            "tools/lint_docs.py",
            "tools/ci_paths.py",
            "tools/test_ci_paths.py",
            "tools/test_codegen_transfer_target_timing.py",
            "tools/test_codegen_continuations.py",
            "tools/test_dispatch_c.py",
            "tools/test_dispatch_call_boundary.py",
            "tools/test_cosim_fixture.py",
            "tools/test_sched_invariants.py",
            "src/rt/guest_interp.c",
            "src/rt/recomp.c",
            "src/rt/sched.c",
        ):
            with self.subTest(path=rel):
                self.assertTrue((ROOT / rel).is_file(), f"documented gate path missing: {rel}")

    def test_review_cites_the_documentation_gates_where_they_are_listed(self):
        # AGENTS.md section 9 is a bullet list that points onward, with no table and so no
        # "docs row"; the documentation gates are the CONTRIBUTING.md full-gates row.
        agents_section_9 = _section((ROOT / "AGENTS.md").read_text(encoding="utf-8"), 9)
        self.assertNotRegex(agents_section_9, r"(?m)^\|")
        review = (ROOT / ".github" / "REVIEW.md").read_text(encoding="utf-8")
        docs_bullet = next((block for block in _blocks(review) if block.startswith("- Docs:")), "")
        self.assertNotIn("row of `AGENTS.md` section 9", docs_bullet)
        self.assertIn("(../CONTRIBUTING.md#the-full-gates)", docs_bullet)
        self.assertIn("docs and agent policy", docs_bullet)
        contributing = (ROOT / "CONTRIBUTING.md").read_text(encoding="utf-8")
        self.assertRegex(contributing, r"(?m)^### The full gates$")
        self.assertRegex(contributing, r"(?m)^\| docs and agent policy \|")

    def test_pull_request_template_asks_for_what_agents_md_says_it_carries(self):
        # "What a good pull request looks like": the filled-in template carries the exact base
        # and head SHAs, scope, uncertainty, private-input and hardware status, and blockers.
        agents = " ".join((ROOT / "AGENTS.md").read_text(encoding="utf-8").split())
        self.assertIn(
            "the exact base and head SHAs, scope, uncertainty, private-input and hardware status, blockers",
            agents,
        )
        template = (ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md").read_text(encoding="utf-8")
        for field in ("- Base and head SHAs:", "- Scope and uncertainty:",
                      "- Private inputs and PSP hardware:", "- Blockers:"):
            with self.subTest(field=field):
                self.assertRegex(template, rf"(?m)^{re.escape(field)}")

    def test_single_pull_request_template_is_canonical(self):
        template = ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md"
        template_dir = ROOT / ".github" / "PULL_REQUEST_TEMPLATE"
        self.assertTrue(template.is_file())
        self.assertFalse((template_dir / "default.md").exists())
        self.assertEqual(list(template_dir.glob("*.md")), [])

    def test_tracked_claude_guidance_is_public_and_small(self):
        # Every tool-specific router only points at AGENTS.md's quick start: it
        # stays small, public, and free of tracker numbers that go stale.
        windows_path = r"[A-Z]" + re.escape(":") + r"[\\/]"
        unix_user_path = "/" + "Users" + "/"
        for rel in ("CLAUDE.md", ".github/copilot-instructions.md"):
            with self.subTest(router=rel):
                content = (ROOT / rel).read_text(encoding="utf-8")
                self.assertLessEqual(len(content.splitlines()), 50)
                self.assertIn("AGENTS.md#where-to-start", content)
                self.assertNotRegex(
                    content,
                    rf"(?i)(place_game_here|game\.iso|EBOOT|SaveBase|memstick/|{unix_user_path}|{windows_path}|session[-_ ]?[0-9a-f]{{8,}})",
                )
                self.assertNotRegex(content, r"(?i)\b(?:issue|pr)\s*#\d+")

    def test_every_issue_form_applies_a_label(self):
        """F-41f91e2c: CONTRIBUTING.md says every issue carries at least one label, so each issue
        form names one (config.yml is the chooser's settings, not a form)."""
        forms = sorted((ROOT / ".github" / "ISSUE_TEMPLATE").glob("*.yml"))
        self.assertIn("smoke_test_report.yml", [form.name for form in forms])
        for form in forms:
            if form.name == "config.yml":
                continue
            with self.subTest(form=form.name):
                text = form.read_text(encoding="utf-8")
                match = re.search(r"(?m)^labels:[ \t]*(.*)$", text)
                self.assertIsNotNone(match, f"{form.name} has no top-level labels key")
                assert match is not None
                inline = match.group(1).strip()
                if inline:
                    labels = re.findall(r"""["']?([^"',\[\]\s][^"',\[\]]*?)["']?\s*(?:,|\])""", inline)
                else:
                    block = text[match.end():].split("\n")
                    labels = []
                    for line in block[1:]:
                        item = re.match(r"^\s+-\s+(.+?)\s*$", line)
                        if not item:
                            break
                        labels.append(item.group(1).strip("\"'"))
                self.assertTrue([label for label in labels if label.strip()], f"{form.name} names no label")

    def test_claude_is_no_longer_ignored(self):
        ignore = (ROOT / ".gitignore").read_text(encoding="utf-8")
        self.assertNotIn("/CLAUDE.md", ignore.splitlines())


if __name__ == "__main__":
    unittest.main()
