# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Pick the PowerShell host for the project's Python tools.

PowerShell 7 (``pwsh``) is used when it is installed. Windows PowerShell 5.1
(``powershell``) is the fallback, because end users may not have PowerShell 7.
Scripts passed through here must therefore parse and behave under both.

``NK_POWERSHELL`` overrides the choice (tests, unusual installs).
"""

from __future__ import annotations

import os
import shutil

ENV_OVERRIDE = "NK_POWERSHELL"


def _which(name: str) -> str | None:
    """Find ``name`` through PATH only, never in the working directory.

    On Windows, ``shutil.which`` looks in the current directory before PATH unless
    ``NoDefaultCurrentDirectoryInExePath`` is set, so a ``pwsh.exe`` left in the folder a tool
    was started from would run instead of the installed one. Setting it here protects every
    tool that starts PowerShell, not only the ``nk`` CLI (which sets it at import), and the
    children those tools start inherit it.
    """
    if os.name == "nt":
        os.environ["NoDefaultCurrentDirectoryInExePath"] = "1"
    return shutil.which(name)


def windows_powershell_exe() -> str:
    """Windows PowerShell 5.1: from PATH, else the in-box System32 location."""
    found = _which("powershell")
    if found:
        return found
    root = os.environ.get("SystemRoot") or os.environ.get("windir") or r"C:\Windows"
    return os.path.join(root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe")


def powershell_exe() -> str:
    """PowerShell 7 when installed, otherwise Windows PowerShell 5.1."""
    override = os.environ.get(ENV_OVERRIDE)
    if override:
        return override
    return _which("pwsh") or windows_powershell_exe()
