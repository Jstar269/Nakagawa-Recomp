# SPDX-License-Identifier: GPL-3.0-or-later

"""Source-shape tripwires for the UMD drive-state wakes in the runtime HLE sources.

Evidence tier: SOURCE_SHAPE only. These checks read handler text; they do not dispatch
anything. The PSP-visible wait results, wakes and callback arguments are asserted by the
native hle_thread_selftest (test_wc_umd_wait_drive_stat and its neighbours).
"""

import re
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_sources  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
HLE_SOURCE = hle_sources.text()


def function_body(name, source=HLE_SOURCE):
    """The brace-delimited body of the C function ``name`` in ``source``.

    The definition and its braces are found on a copy with comments and string and
    character literals blanked (same offsets), so a brace or a name inside one of them
    neither starts nor ends a body; the body is returned from the original text.
    """
    blanked = hle_sources.blank_comments_and_literals(source)
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", blanked)
    if not match:
        raise AssertionError(f"{name} not found in the runtime HLE sources")
    start = match.end() - 1
    depth = 0
    for pos in range(start, len(blanked)):
        char = blanked[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError(f"{name} has no closing brace")


# sceUmdWaitDriveStat's handler is a wrapper; the wait itself and its precheck are the
# functions a drive event would have to come from.
PLAIN_WAIT_FUNCTIONS = ("h_UmdWaitDriveStat", "umd_wait_drive_stat", "umd_wait_precheck")


def plain_wait_drive_events(source=HLE_SOURCE):
    """The plain-wait functions in ``source`` that call sr_umd_signal_ready."""
    return [name for name in PLAIN_WAIT_FUNCTIONS
            if re.search(r"\bsr_umd_signal_ready\s*\(",
                         hle_sources.blank_comments_and_literals(function_body(name, source)))]


class UmdWakeupIsolationTests(unittest.TestCase):
    def test_ready_signal_only_wakes_umd_waiters_and_notifies_registered_callback(self):
        body = function_body("sr_umd_signal_ready")
        self.assertIn("sched_wake(0x554D44u)", body)
        # The ready signal restores the measured activated drive state (0x32) before it
        # notifies the callback. The callback argument follows the compiled SDK version
        # (0x22 with none set, 0x32 otherwise); hle_thread_selftest checks both values.
        self.assertRegex(HLE_SOURCE, r"\bUMD_STAT_ACTIVE\s*=\s*0x32u\b")
        self.assertRegex(HLE_SOURCE, r"\bUMD_STAT_DEACTIVATED\s*=\s*0x12u\b")
        self.assertIn("s_umd_drive_stat = UMD_STAT_ACTIVE;", body)
        self.assertIn("g_sdk_version != 0u", body)
        self.assertIn("sr_callback_notify(s_umd_cb_uid, callback_stat)", body)
        self.assertLess(body.index("s_umd_drive_stat = UMD_STAT_ACTIVE;"),
                        body.index("sr_callback_notify(s_umd_cb_uid, callback_stat)"))

    def test_deactivate_notifies_the_deactivated_drive_state(self):
        body = function_body("h_UmdDeactivate")
        self.assertLess(body.index("s_umd_drive_stat = UMD_STAT_DEACTIVATED;"),
                        body.index("sr_callback_notify(s_umd_cb_uid, s_umd_drive_stat)"))
        self.assertNotRegex(body, r"\bsched_thread_wakeup\s*\(")

    def test_callback_registration_validates_kernel_callback_id(self):
        body = function_body("h_UmdRegisterUMDCallBack")
        self.assertIn("sr_callback_is_valid(A0)", body)
        self.assertIn("0x80010016u", body)
        self.assertIn("s_umd_cb_uid = A0", body)

    def test_plain_wait_does_not_create_a_drive_event(self):
        self.assertIn("umd_wait_drive_stat(A0, 0u)", function_body("h_UmdWaitDriveStat"))
        self.assertEqual(plain_wait_drive_events(), [])

    def test_a_drive_event_inside_the_wait_helper_is_caught(self):
        body = function_body("umd_wait_drive_stat")
        mutated = HLE_SOURCE.replace(body, body.replace("{", "{\n    sr_umd_signal_ready();", 1), 1)
        self.assertNotEqual(mutated, HLE_SOURCE)
        self.assertEqual(plain_wait_drive_events(mutated), ["umd_wait_drive_stat"])

    def test_braces_in_strings_and_comments_do_not_end_a_body(self):
        source = (
            "static int other(void) { return 0; }\n"
            "static int probe(int x) {\n"
            "    log(\"}\"); /* } */ // }\n"
            "    char c = '}';\n"
            "    if (x) { return 1; }\n"
            "    return 2;\n"
            "}\n"
            "static int after(void) { return 3; }\n"
        )
        body = function_body("probe", source)
        self.assertTrue(body.startswith("{\n    log("))
        self.assertTrue(body.endswith("return 2;\n}"))
        self.assertNotIn("after", body)

    def test_cb_wait_consumes_pending_callback(self):
        body = function_body("h_UmdWaitDriveStatCB")
        self.assertIn("sr_thread_has_pending_callbacks", body)
        self.assertIn("sr_thread_dispatch_callbacks", body)


if __name__ == "__main__":
    unittest.main()
