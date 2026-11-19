# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Game library manager and persistent storage for Nakagawa Recomp."""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import asdict, dataclass, field
import json
import os
from pathlib import Path
import sys
import tempfile
import time
from typing import Any, Dict, List, Optional

from .package_cache import (
    BoundedJsonError,
    MAX_CACHE_JSON_DEPTH,
    MAX_CACHE_JSON_ITEMS,
    MAX_CACHE_JSON_MEMBERS,
    MAX_CACHE_JSON_NODES,
    bounded_echo,
    bounded_echo_fields,
    read_bounded_json,
)


LIBRARY_SCHEMA_VERSION = 1

# Externally supplied library.json is one bounded JSON artifact (#319): the
# byte ceiling is enforced with a bounded read (never a whole-file read), the
# structural ceilings run before json.loads, and record-level limits bound the
# decoded list. Byte, structure, and record limits apply independently;
# nothing is ever truncated silently.
# Room for MAX_LIBRARY_GAMES records as the player writes them (about 1 KiB each
# with typical paths); the player's reader has the same limit, NK_LIBRARY_MAX_BYTES
# in src/core/nk_library.h.
MAX_LIBRARY_JSON_BYTES = 16 * 1024 * 1024
MAX_LIBRARY_JSON_DEPTH = MAX_CACHE_JSON_DEPTH
MAX_LIBRARY_GAMES = 4096

_GAME_RECORD_FIELDS = frozenset({
    "disc_id", "title_name", "iso_path", "prepared_root", "is_prepared",
    "assets_staged", "extracted_asset_count", "extracted_audio_count",
    "extracted_visual_count", "extracted_layout_count", "disc_version",
    "last_played", "play_count", "settings_override",
})
_LIBRARY_ROOT_FIELDS = frozenset({"schema_version", "updated_at", "games"})

# A saved record emits every dataclass field, even defaults. The package-cache
# structural budgets would reject an ordinary saved library around 1,170 games.
# Give this route budgets derived from the record schemas that write
# library.json: this module's, and the player's, whose records carry up to 26
# fields (nk_library_save in src/core/nk_library.c: 22, plus the optional
# source_image_path and the disc check's content_sha1, content_crc32 and
# disc_verification). The independent byte limit still applies, including to
# large settings overrides.
_PLAYER_RECORD_FIELD_COUNT = 26
_RECORD_FIELD_BUDGET = max(len(_GAME_RECORD_FIELDS), _PLAYER_RECORD_FIELD_COUNT)
MAX_LIBRARY_JSON_MEMBERS = max(
    MAX_CACHE_JSON_MEMBERS,
    MAX_LIBRARY_GAMES * _RECORD_FIELD_BUDGET + len(_LIBRARY_ROOT_FIELDS),
)
MAX_LIBRARY_JSON_ITEMS = max(
    MAX_CACHE_JSON_ITEMS, MAX_LIBRARY_JSON_MEMBERS + MAX_LIBRARY_GAMES,
)
MAX_LIBRARY_JSON_NODES = max(
    MAX_CACHE_JSON_NODES,
    MAX_LIBRARY_GAMES * (2 * _RECORD_FIELD_BUDGET + 1)
    + 2 * len(_LIBRARY_ROOT_FIELDS) + 2,
)


def _existing_schema_version(path: Path) -> Optional[int]:
    """The integer schema_version of the library file at ``path``, or None.

    None when there is no file or it cannot be read as a library object: the
    loader has already reported such a file, and replacing it loses nothing a
    reader could use.
    """
    if not path.is_file():
        return None
    try:
        data = read_bounded_json(
            path,
            max_bytes=MAX_LIBRARY_JSON_BYTES,
            max_depth=MAX_LIBRARY_JSON_DEPTH,
            max_members=MAX_LIBRARY_JSON_MEMBERS,
            max_items=MAX_LIBRARY_JSON_ITEMS,
            max_nodes=MAX_LIBRARY_JSON_NODES,
        )
    except (OSError, BoundedJsonError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    version = data.get("schema_version")
    return version if type(version) is int else None


@contextmanager
def _library_file_lock(user_data_root: Path):
    """Hold the same per-user library lock used by the player's library writer."""
    root = user_data_root.resolve(strict=False)
    lock_dir = root / "locks"
    if lock_dir.is_symlink() or getattr(lock_dir, "is_junction", lambda: False)():
        raise ValueError("The library lock folder is a link; refusing to write outside user data.")
    lock_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        lock_dir.resolve(strict=True).relative_to(root)
    except (OSError, RuntimeError, ValueError) as exc:
        raise ValueError("The library lock folder is outside user data.") from exc
    lock_path = lock_dir / "library.lock"
    if lock_path.is_symlink() or getattr(lock_path, "is_junction", lambda: False)():
        raise ValueError("The library lock file is a link; refusing to write outside user data.")
    with lock_path.open("a+b") as handle:
        if sys.platform == "win32":
            import msvcrt

            handle.seek(0, os.SEEK_END)
            if handle.tell() == 0:
                handle.write(b"\0")
                handle.flush()
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                handle.seek(0)
                try:
                    msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
                except OSError:
                    pass
        else:
            import fcntl

            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


@dataclass
class LibraryGameRecord:
    disc_id: str
    title_name: str
    iso_path: str
    prepared_root: str = ""
    is_prepared: bool = False
    assets_staged: bool = False
    extracted_asset_count: int = 0
    extracted_audio_count: int = 0
    extracted_visual_count: int = 0
    extracted_layout_count: int = 0
    disc_version: str = "1.00"
    last_played: Optional[float | str] = None
    play_count: int = 0
    settings_override: Dict[str, Any] = field(default_factory=dict)
    extra_fields: Dict[str, Any] = field(default_factory=dict, repr=False)

    def to_dict(self) -> Dict[str, Any]:
        record = asdict(self)
        extra_fields = record.pop("extra_fields")
        record.update(extra_fields)
        return record

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "LibraryGameRecord":
        """Validate one externally supplied game record before field access.

        Every malformed shape raises ``ValueError`` with a named diagnosis;
        substitution of a scalar, list, or null for a required record object
        never escapes as ``AttributeError``/``TypeError``.
        """
        if not isinstance(data, dict):
            raise ValueError("game record must be a JSON object")
        extra_fields = {key: value for key, value in data.items() if key not in _GAME_RECORD_FIELDS}

        disc_id = data.get("disc_id")
        if not isinstance(disc_id, str) or not disc_id.strip():
            raise ValueError("game record disc_id must be a non-empty string")
        title_name = data.get("title_name", "Unknown Title")
        if not isinstance(title_name, str) or len(title_name) > 256:
            raise ValueError("game record title_name must be a string of at most 256 characters")
        iso_path = data.get("iso_path", "")
        if not isinstance(iso_path, str):
            raise ValueError("game record iso_path must be a string")
        prepared_root = data.get("prepared_root", "")
        if not isinstance(prepared_root, str):
            raise ValueError("game record prepared_root must be a string")
        is_prepared = data.get("is_prepared", False)
        if not isinstance(is_prepared, bool):
            raise ValueError("game record is_prepared must be a boolean")
        assets_staged = data.get("assets_staged", False)
        if not isinstance(assets_staged, bool):
            raise ValueError("game record assets_staged must be a boolean")
        counts: Dict[str, int] = {}
        for field_name in (
            "extracted_asset_count", "extracted_audio_count",
            "extracted_visual_count", "extracted_layout_count", "play_count",
        ):
            value = data.get(field_name, 0)
            # type() rejects bool (an int subclass) and float on purpose.
            if type(value) is not int or value < 0:
                raise ValueError(f"game record {field_name} must be a non-negative integer")
            counts[field_name] = value
        disc_version = data.get("disc_version", "1.00")
        if not isinstance(disc_version, str) or len(disc_version) > 32:
            raise ValueError("game record disc_version must be a string of at most 32 characters")
        last_played = data.get("last_played")
        if last_played is not None and type(last_played) not in (int, float) and not isinstance(last_played, str):
            raise ValueError("game record last_played must be a timestamp, string, or null")
        if isinstance(last_played, str) and len(last_played) > 64:
            raise ValueError("game record last_played string exceeds 64 characters")
        settings_override = data.get("settings_override", {})
        if not isinstance(settings_override, dict) or any(
            not isinstance(key, str) for key in settings_override
        ):
            raise ValueError("game record settings_override must be an object with string keys")

        return cls(
            disc_id=disc_id.strip(),
            title_name=title_name,
            iso_path=iso_path,
            prepared_root=prepared_root,
            is_prepared=is_prepared,
            assets_staged=assets_staged,
            extracted_asset_count=counts["extracted_asset_count"],
            extracted_audio_count=counts["extracted_audio_count"],
            extracted_visual_count=counts["extracted_visual_count"],
            extracted_layout_count=counts["extracted_layout_count"],
            disc_version=disc_version,
            last_played=last_played,
            play_count=counts["play_count"],
            settings_override=dict(settings_override),
            extra_fields=extra_fields,
        )

    def is_source_available(self) -> bool:
        """Check whether the backing ISO file still exists on disk."""
        if not self.iso_path:
            return False
        return Path(self.iso_path).is_file()


def _library_key(disc_id: str) -> str:
    """The key a disc id is stored and looked up under: case- and edge-whitespace-insensitive."""
    return disc_id.upper().strip()


class GameLibrary:
    """Persistent storage for registered and installed PSP games."""

    def __init__(self, storage_file: Optional[Path | str] = None) -> None:
        self.storage_file = Path(storage_file).resolve() if storage_file else None
        self._games: Dict[str, LibraryGameRecord] = {}
        self._removed: set[str] = set()

    def add_or_update_game(self, record: LibraryGameRecord) -> None:
        key = _library_key(record.disc_id)
        self._removed.discard(key)
        self._games[key] = record

    def get_game(self, disc_id: str) -> Optional[LibraryGameRecord]:
        return self._games.get(_library_key(disc_id))

    def remove_game(self, disc_id: str) -> bool:
        key = _library_key(disc_id)
        if key in self._games:
            del self._games[key]
            self._removed.add(key)
            return True
        return False

    def list_games(self) -> List[LibraryGameRecord]:
        return list(self._games.values())

    def count(self) -> int:
        return len(self._games)

    def verify_sources(self) -> Dict[str, bool]:
        """Verify availability of source ISO files for all registered games."""
        return {disc_id: record.is_source_available() for disc_id, record in self._games.items()}

    def save(self, target_file: Optional[Path | str] = None) -> None:
        out_file = Path(target_file).resolve() if target_file else self.storage_file
        if not out_file:
            raise ValueError("No storage file path specified for GameLibrary.")
        out_file.parent.mkdir(parents=True, exist_ok=True)
        with _library_file_lock(out_file.parent):
            # A library a newer player wrote is never replaced, as the native writer
            # refuses it (src/core/nk_user_data_schema.h).
            existing_version = _existing_schema_version(out_file)
            if existing_version is not None and existing_version > LIBRARY_SCHEMA_VERSION:
                raise ValueError(
                    f"{out_file}: library schema_version {existing_version} was written by a newer "
                    f"Nakagawa; this build writes version {LIBRARY_SCHEMA_VERSION} and left "
                    "the file as it is"
                )

            merged = GameLibrary()
            if existing_version == LIBRARY_SCHEMA_VERSION:
                merged = GameLibrary.load(out_file)
            for key in self._removed:
                merged._games.pop(key, None)
            merged._games.update(self._games)
            if len(merged._games) > MAX_LIBRARY_GAMES:
                raise ValueError(
                    f"library holds {len(merged._games)} games, over the {MAX_LIBRARY_GAMES}-record limit"
                )

            payload = {
                "schema_version": LIBRARY_SCHEMA_VERSION,
                "updated_at": time.time(),
                "games": [record.to_dict() for record in merged._games.values()],
            }
            tmp_file: Optional[Path] = None
            try:
                # Each writer owns its temporary file; merge under the same OS lock
                # as the player's library writer to preserve concurrent additions.
                with tempfile.NamedTemporaryFile(
                    mode="w", encoding="utf-8", dir=out_file.parent,
                    prefix=f".{out_file.name}.", suffix=".tmp", delete=False,
                ) as handle:
                    tmp_file = Path(handle.name)
                    json.dump(payload, handle, indent=2, ensure_ascii=False)
                GameLibrary.load(tmp_file)
                tmp_file.replace(out_file)
            finally:
                if tmp_file is not None:
                    tmp_file.unlink(missing_ok=True)

    @classmethod
    def load(cls, file_path: Path | str) -> "GameLibrary":
        """Load one externally supplied library.json under explicit ceilings.

        The byte ceiling is enforced with a bounded read before any wholesale
        read, UTF-8 decoding and structural ceilings run through the shared
        bounded-JSON helper, and every malformed shape raises ``ValueError``
        with a named diagnosis rather than a parser-implementation exception.
        """
        path = Path(file_path).resolve()
        library = cls(storage_file=path)
        if not path.is_file():
            return library

        try:
            data = read_bounded_json(
                path,
                max_bytes=MAX_LIBRARY_JSON_BYTES,
                max_depth=MAX_LIBRARY_JSON_DEPTH,
                max_members=MAX_LIBRARY_JSON_MEMBERS,
                max_items=MAX_LIBRARY_JSON_ITEMS,
                max_nodes=MAX_LIBRARY_JSON_NODES,
            )
        except (OSError, BoundedJsonError) as exc:
            raise ValueError(
                f"{path}: library JSON is unreadable or exceeds a bounded-input limit: {exc}"
            ) from exc

        # Fail closed on a schema this loader does not understand, the way the
        # native loader in src/core/nk_library.c already does. Interpreting the
        # familiar-looking fields of a newer file and then rewriting it as
        # schema 1 on the next save silently discards whatever the newer schema
        # added -- a data-losing "success" rather than a refusal.
        if not isinstance(data, dict):
            raise ValueError(f"{path}: library root must be a JSON object")
        unknown = set(data) - _LIBRARY_ROOT_FIELDS
        if unknown:
            raise ValueError(
                f"{path}: library root has unsupported fields: " + bounded_echo_fields(unknown)
            )
        schema = data.get("schema_version")
        if type(schema) is not int or schema != LIBRARY_SCHEMA_VERSION:
            raise ValueError(
                f"{path}: unsupported library schema_version {bounded_echo(schema)}; "
                f"this build reads version {LIBRARY_SCHEMA_VERSION}"
            )
        updated_at = data.get("updated_at")
        if updated_at is not None and type(updated_at) not in (int, float):
            raise ValueError(f"{path}: library updated_at must be a timestamp or null")
        if "games" not in data:
            raise ValueError(f"{path}: library is missing the games field")
        games_list = data["games"]
        if not isinstance(games_list, list):
            raise ValueError(f"{path}: 'games' must be a list")
        if len(games_list) > MAX_LIBRARY_GAMES:
            raise ValueError(
                f"{path}: 'games' holds {len(games_list)} records, "
                f"over the {MAX_LIBRARY_GAMES}-record limit"
            )
        for index, record_dict in enumerate(games_list):
            try:
                record = LibraryGameRecord.from_dict(record_dict)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"{path}: game record {index} is invalid: {exc}") from exc
            if _library_key(record.disc_id) in library._games:
                raise ValueError(
                    f"{path}: game record {index} repeats disc_id {bounded_echo(record.disc_id)}"
                )
            library.add_or_update_game(record)

        return library
