# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Nakagawa Core: Title-agnostic preparation, inspection, and execution library.

The exported names are resolved on first use (PEP 562). Importing one submodule must not run
the others: the publication and provenance gates import ``nk_core.git_isolation``, and an
eager package would make the preparation engine, the launcher and the code generator they
import part of every gate run. This file is a protected path for that reason
(``PROTECTED_EXTRA`` in ``tools/ci_trust_verdict.py``); keep it free of imports that run at
load time.
"""

from __future__ import annotations

import importlib
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .iso_inspect import IsoInspectionError, inspect_iso
    from .launcher import RuntimeLauncher, RuntimeLaunchError
    from .library import GameLibrary, LibraryGameRecord
    from .prep_engine import PreparationEngine, PreparationOutputError
    from .title_registry import TitleRegistry, get_default_registry
    from .types import (
        CancellationToken,
        EventSeverity,
        IsoMetadata,
        PreparationResult,
        PrepStage,
        ProgressEvent,
        TitleProfile,
    )

#: Exported name -> the submodule that defines it.
_EXPORTS = {
    "CancellationToken": "types",
    "EventSeverity": "types",
    "GameLibrary": "library",
    "IsoInspectionError": "iso_inspect",
    "IsoMetadata": "types",
    "LibraryGameRecord": "library",
    "PrepStage": "types",
    "PreparationEngine": "prep_engine",
    "PreparationOutputError": "prep_engine",
    "PreparationResult": "types",
    "ProgressEvent": "types",
    "RuntimeLaunchError": "launcher",
    "RuntimeLauncher": "launcher",
    "TitleProfile": "types",
    "TitleRegistry": "title_registry",
    "get_default_registry": "title_registry",
    "inspect_iso": "iso_inspect",
}

__all__ = [
    "CancellationToken",
    "EventSeverity",
    "GameLibrary",
    "IsoMetadata",
    "IsoInspectionError",
    "LibraryGameRecord",
    "PreparationEngine",
    "PreparationOutputError",
    "PreparationResult",
    "PrepStage",
    "ProgressEvent",
    "RuntimeLauncher",
    "RuntimeLaunchError",
    "TitleProfile",
    "TitleRegistry",
    "get_default_registry",
    "inspect_iso",
]


def __getattr__(name: str) -> object:
    module = _EXPORTS.get(name)
    if module is None:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    value = getattr(importlib.import_module(f".{module}", __name__), name)
    globals()[name] = value
    return value


def __dir__() -> list[str]:
    return sorted(set(globals()) | set(_EXPORTS))
