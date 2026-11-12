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


def windows_powershell_exe() -> str:
    """Windows PowerShell 5.1: from PATH, else the in-box System32 location."""
    found = shutil.which("powershell")
    if found:
        return found
    root = os.environ.get("SystemRoot") or os.environ.get("windir") or r"C:\Windows"
    return os.path.join(root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe")


def powershell_exe() -> str:
    """PowerShell 7 when installed, otherwise Windows PowerShell 5.1."""
    override = os.environ.get(ENV_OVERRIDE)
    if override:
        return override
    return shutil.which("pwsh") or windows_powershell_exe()
