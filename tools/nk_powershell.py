# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Pick the PowerShell host for the project's Python tools.

PowerShell 7 (``pwsh``) is used when it is installed. Windows PowerShell 5.1
(``powershell``) is the fallback, because end users may not have PowerShell 7.
Scripts passed through here must therefore parse and behave under both.

``NK_POWERSHELL`` overrides the choice (tests, unusual installs).
"""

from __future__ import annotations

from collections.abc import Mapping
import os
import shutil
import tempfile

ENV_OVERRIDE = "NK_POWERSHELL"
CACHE_PATH_ENV = "PSModuleAnalysisCachePath"


def _env_get_ci(env: Mapping[str, str], name: str) -> str:
    """Case-insensitive lookup for Windows environment dictionaries."""
    target = name.casefold()
    for key, value in env.items():
        if key.casefold() == target:
            return value
    return ""


def _env_set_ci(env: dict[str, str], name: str, value: str) -> None:
    """Set ``name`` in ``env``, removing any differently-cased duplicate on Windows."""
    target = name.casefold()
    for existing in [key for key in env if key.casefold() == target and key != name]:
        del env[existing]
    env[name] = value


def powershell_env(
    env: Mapping[str, str] | None = None,
    *,
    cache_dir: str | os.PathLike[str] | None = None,
) -> dict[str, str]:
    """Return a child environment whose PowerShell module-analysis cache is absolute.

    Windows PowerShell 5.1 resolves its cache file from ``PSModuleAnalysisCachePath``
    and otherwise from ``Path.Combine(GetFolderPath(LocalApplicationData),
    'Microsoft\\\\Windows\\\\PowerShell\\\\ModuleAnalysisCache')``. When a caller or
    test overrides ``USERPROFILE`` without an existing ``AppData\\\\Local`` folder (or
    points ``USERPROFILE`` at an uncreatable path), ``SHGetFolderPathW`` returns an
    empty string and PowerShell writes ``Microsoft\\\\Windows\\\\PowerShell\\\\ModuleAnalysisCache``
    relative to the current working directory. Setting an absolute
    ``PSModuleAnalysisCachePath`` anchors the cache for both Windows PowerShell 5.1
    and PowerShell 7.
    """
    result = dict(os.environ if env is None else env)
    if os.name != "nt":
        return result

    if cache_dir is not None:
        cache_file = os.path.join(os.path.abspath(os.fspath(cache_dir)), "ModuleAnalysisCache")
        _env_set_ci(result, CACHE_PATH_ENV, cache_file)
        return result

    existing = _env_get_ci(result, CACHE_PATH_ENV).strip()
    if existing and os.path.isabs(existing):
        return result

    local_appdata = _env_get_ci(result, "LOCALAPPDATA").strip()
    if local_appdata and os.path.isabs(local_appdata):
        cache_file = os.path.join(
            local_appdata, "Microsoft", "Windows", "PowerShell", "ModuleAnalysisCache"
        )
    else:
        temp_base = (
            _env_get_ci(result, "TEMP").strip()
            or _env_get_ci(result, "TMP").strip()
            or tempfile.gettempdir()
        )
        if not os.path.isabs(temp_base):
            temp_base = tempfile.gettempdir()
        cache_file = os.path.join(temp_base, "nk-powershell-cache", "ModuleAnalysisCache")

    _env_set_ci(result, CACHE_PATH_ENV, cache_file)
    return result


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
