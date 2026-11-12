# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Disc verification: a whole-image hash compared with Redump's PSP list.

A disc image is hashed whole (SHA-1 and CRC32, the two hashes Redump lists) on the user's
machine. A compressed image (.cso, .zso, .chd) is hashed after the one-time expansion
(``disc_expand_tool.ensure_plain_iso``), so it hashes the same as the .iso it holds.

The hashes are compared with one Redump PSP datafile:

- the list the user imported (``nk_cli.py redump-import``), when there is one: it takes
  precedence, so a user can move to a newer list than the one the project ships;
- otherwise the list bundled with the project, ``assets/redump/redump-psp.zip``, Redump's own
  download kept byte for byte (``assets/redump/PROVENANCE.md``).

Nothing downloads a list and nothing is uploaded: the bundled list is a file in the checkout
or the player's folder, and an import copies a file the user chose.

Each image gets one tier:

- ``VERIFIED_RETAIL``: its SHA-1 matches a disc in the list, and the CRC32 and size agree.
- ``MODIFIED``: no match, but its disc ID is a known retail disc ID: one the list names (a
  datafile with ``<serial>`` entries), or the disc ID of a verified image in the same
  library. Fan translations, patched and trimmed images land here.
- ``UNKNOWN``: neither. The image is not in the list, which means "not shown to be retail",
  never "bad".
- ``NOT_CHECKED``: no list was found, or the image was not hashed.

The datafile is read by a small bounded scanner, the same rules as ``src/core/nk_disc_verify.c``.
It expands no entity and never reads a DTD: a ``<!DOCTYPE>`` that only names Logiqx's public
DTD (as Redump's lists do) is skipped unread, and a list that declares entities, carries an
internal DTD subset or refers to any entity but XML's five predefined ones and character
references is refused (``DISC_DAT_ENTITY_REFUSED``). It reads each ``<game>`` element, its
``<rom>`` elements' ``size``, ``crc`` and ``sha1`` attributes, and any ``<serial>`` text. A rom
with a malformed hash or size is skipped and counted. A datafile may come as the zip Redump
serves (one ``.dat`` inside, stored or deflated), read under the same size cap.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import io
import os
from pathlib import Path
import re
import tempfile
from typing import Callable, Iterable, Mapping, Optional
import zipfile
import zlib

VERIFIED_RETAIL = "VERIFIED_RETAIL"
MODIFIED = "MODIFIED"
UNKNOWN = "UNKNOWN"
NOT_CHECKED = "NOT_CHECKED"
TIERS = (VERIFIED_RETAIL, MODIFIED, UNKNOWN, NOT_CHECKED)

# The largest datafile read: Redump's biggest lists are a few MiB.
MAX_DAT_BYTES = 64 * 1024 * 1024
MAX_DAT_ROMS = 200_000
# Serials kept per <game>, as the C reader keeps them.
MAX_GAME_SERIALS = 16
_XML_ENTITIES = {"&amp;": "&", "&lt;": "<", "&gt;": ">", "&quot;": '"', "&apos;": "'"}
HASH_CHUNK_BYTES = 1024 * 1024
# The imported list, relative to the user's data folder (nk_disc_verify.h has the same path).
DAT_RELATIVE_PATH = Path("verification") / "redump.dat"
# The bundled list, relative to the checkout and to the player's folder (staged there by
# tools/stage_runtime_dlls.py; nk_disc_verify.h has the same path).
BUNDLED_DAT_RELATIVE_PATH = Path("assets") / "redump" / "redump-psp.zip"
PROJECT_ROOT = Path(__file__).resolve().parents[2]
# Which list a check used.
LIST_IMPORTED = "imported"
LIST_BUNDLED = "bundled"
_ZIP_MAGIC = b"PK\x03\x04"

_GAME = re.compile(r"<game\b[^>]*>(.*?)</game\s*>", re.DOTALL)
_GAME_OPEN = re.compile(r"<game\b")
_DATAFILE_END = re.compile(r"</datafile\s*>")
_ROM = re.compile(r"<rom\b([^>]*?)/?>")
_ATTRIBUTE = re.compile(r'([A-Za-z_][A-Za-z0-9_.-]*)\s*=\s*"([^"]*)"')
_SERIAL = re.compile(r"<serial>([^<]*)</serial\s*>")
_HEADER_NAME = re.compile(r"<header\b.*?<name>([^<]*)</name\s*>.*?</header\s*>", re.DOTALL)
_HEADER_VERSION = re.compile(r"<header\b.*?<version>([^<]*)</version\s*>.*?</header\s*>",
                             re.DOTALL)
_HEADER_DATE = re.compile(r"<header\b.*?<date>([^<]*)</date\s*>.*?</header\s*>", re.DOTALL)
# What the reader refuses rather than skips: an entity declaration anywhere, a DOCTYPE with an
# internal subset ("[" before its ">"), and a reference to any entity but the five predefined
# ones and character references. The external DTD a DOCTYPE names is never read.
_ENTITY_DECLARATION = re.compile(r"<!ENTITY", re.IGNORECASE)
_DOCTYPE_SUBSET = re.compile(r"<!DOCTYPE\b[^>\[]*\[", re.IGNORECASE)
_UNKNOWN_REFERENCE = re.compile(r"&(?!(?:amp|lt|gt|quot|apos);|#[0-9]{1,7};|#x[0-9A-Fa-f]{1,6};)")
_GAME_NAME = re.compile(r'\bname\s*=\s*"([^"]*)"')
_SHA1 = re.compile(r"[0-9a-fA-F]{40}")
_CRC32 = re.compile(r"[0-9a-fA-F]{8}")
_SIZE = re.compile(r"[0-9]{1,20}")
_RETAIL_DISC_ID = re.compile(r"[A-Z]{4}[0-9]{5}")
_ENTITY = re.compile("|".join(re.escape(entity) for entity in _XML_ENTITIES))


def _xml_text(text: str) -> str:
    """Element or attribute text with XML's five predefined entities decoded, and no others."""
    return _ENTITY.sub(lambda match: _XML_ENTITIES[match.group(0)], text)


class DiscVerifyError(ValueError):
    """A datafile or image that cannot be used, with a named boundary code."""

    def __init__(self, message: str, *, boundary_code: str) -> None:
        super().__init__(message)
        self.boundary_code = boundary_code


@dataclass(frozen=True)
class DiscDigest:
    """The whole-image hashes of one disc image: lowercase hex, and its size in bytes."""

    sha1: str
    crc32: str
    size_bytes: int

    def to_dict(self) -> dict:
        return {"sha1": self.sha1, "crc32": self.crc32, "size_bytes": self.size_bytes}


@dataclass(frozen=True)
class DatRom:
    name: str
    size_bytes: int
    crc32: str
    sha1: str
    serials: tuple[str, ...] = ()


@dataclass
class RedumpDat:
    """The parts of one datafile verification uses."""

    name: str = ""
    version: str = ""
    date: str = ""
    roms_by_sha1: dict[str, DatRom] = field(default_factory=dict)
    serials: frozenset[str] = frozenset()
    games: int = 0
    rejected_roms: int = 0
    # LIST_IMPORTED or LIST_BUNDLED once a caller picked the list (load_verification_dat).
    source: str = ""

    def __len__(self) -> int:
        return len(self.roms_by_sha1)

    def describe(self) -> str:
        """Which list this is and its date, for output: "the bundled Redump list (... discs)"."""
        owner = {LIST_IMPORTED: "your imported Redump list",
                 LIST_BUNDLED: "the Redump list bundled with Nakagawa"}.get(self.source, "a Redump list")
        # The date is the datafile's own text: shown without control characters.
        stamp = "".join(ch for ch in (self.date or self.version) if ch.isprintable())[:64]
        dated = f"dated {stamp}, " if stamp else ""
        return f"{owner} ({dated}{len(self)} discs)"


def normalize_serials(text: str) -> tuple[str, ...]:
    """Disc IDs in a ``<serial>`` value ("TEST-71002, TEST-71002GA") as PARAM.SFO spells them."""
    serials = []
    for part in text.split(","):
        compact = re.sub(r"[\s-]", "", part).upper()
        match = _RETAIL_DISC_ID.match(compact)
        if match and match.group(0) not in serials:
            serials.append(match.group(0))
    return tuple(serials)


def parse_dat_text(text: str) -> RedumpDat:
    """Read a Logiqx-format datafile (Redump's) from its text."""
    if "<datafile" not in text:
        raise DiscVerifyError("This file isn't a Redump datafile: it has no <datafile> element.",
                              boundary_code="DISC_DAT_NOT_DATAFILE")
    # A list cut short (a download that stopped) is refused, never read as the whole list:
    # its discs past the cut would all read as unknown.
    if not _DATAFILE_END.search(text):
        raise DiscVerifyError("The datafile is cut short: it has no closing </datafile>.",
                              boundary_code="DISC_DAT_NOT_DATAFILE")
    if _ENTITY_DECLARATION.search(text) or _DOCTYPE_SUBSET.search(text):
        raise DiscVerifyError("The datafile declares its own entities or DTD; a Redump list has "
                              "none, so it was not read.", boundary_code="DISC_DAT_ENTITY_REFUSED")
    if _UNKNOWN_REFERENCE.search(text):
        raise DiscVerifyError("The datafile refers to an entity XML does not predefine; a Redump "
                              "list has none, so it was not read.",
                              boundary_code="DISC_DAT_ENTITY_REFUSED")
    dat = RedumpDat()
    header_name = _HEADER_NAME.search(text)
    header_version = _HEADER_VERSION.search(text)
    header_date = _HEADER_DATE.search(text)
    dat.name = _xml_text(header_name.group(1)).strip() if header_name else ""
    dat.version = _xml_text(header_version.group(1)).strip() if header_version else ""
    dat.date = _xml_text(header_date.group(1)).strip() if header_date else ""
    serials: set[str] = set()
    if len(_GAME_OPEN.findall(text)) != len(_GAME.findall(text)):
        raise DiscVerifyError("The datafile is cut short inside a <game> element.",
                              boundary_code="DISC_DAT_NOT_DATAFILE")
    for game in _GAME.finditer(text):
        dat.games += 1
        body = game.group(1)
        game_serials: list[str] = []
        for serial in _SERIAL.finditer(body):
            for disc_id in normalize_serials(_xml_text(serial.group(1))):
                if disc_id not in game_serials and len(game_serials) < MAX_GAME_SERIALS:
                    game_serials.append(disc_id)
        serials.update(game_serials)
        for rom in _ROM.finditer(body):
            attributes = dict(_ATTRIBUTE.findall(rom.group(1)))
            sha1 = attributes.get("sha1", "")
            crc = attributes.get("crc", "")
            size = attributes.get("size", "")
            if not (_SHA1.fullmatch(sha1) and _CRC32.fullmatch(crc) and _SIZE.fullmatch(size)
                    and int(size) < 2**64):
                dat.rejected_roms += 1
                continue
            if len(dat.roms_by_sha1) >= MAX_DAT_ROMS:
                raise DiscVerifyError(f"The datafile lists more than {MAX_DAT_ROMS} discs.",
                                      boundary_code="DISC_DAT_TOO_LARGE")
            entry = DatRom(name=_xml_text(attributes.get("name", "")), size_bytes=int(size),
                           crc32=crc.lower(), sha1=sha1.lower(), serials=tuple(game_serials))
            dat.roms_by_sha1.setdefault(entry.sha1, entry)
    dat.serials = frozenset(serials)
    if not dat.roms_by_sha1:
        raise DiscVerifyError("The datafile lists no disc with a SHA-1 hash.",
                              boundary_code="DISC_DAT_EMPTY")
    return dat


def _read_dat_bytes(path: Path | str) -> bytes:
    """A datafile's bytes, read once and refused over ``MAX_DAT_BYTES``."""
    try:
        with Path(path).open("rb") as handle:
            data = handle.read(MAX_DAT_BYTES + 1)
    except OSError as exc:
        raise DiscVerifyError(f"The datafile can't be read: {exc}",
                              boundary_code="DISC_DAT_READ_FAILED") from exc
    if len(data) > MAX_DAT_BYTES:
        raise DiscVerifyError(f"The datafile is larger than {MAX_DAT_BYTES} bytes.",
                              boundary_code="DISC_DAT_TOO_LARGE")
    return data


def _zip_refused(reason: str) -> DiscVerifyError:
    return DiscVerifyError(f"The zip can't be read as a Redump datafile: {reason}",
                           boundary_code="DISC_DAT_ZIP_INVALID")


def _dat_from_zip(data: bytes) -> bytes:
    """The one ``.dat`` inside a zip as Redump serves it, read under ``MAX_DAT_BYTES``.

    The zip must hold exactly one file, named ``*.dat``, stored or deflated and not encrypted.
    Its size is never taken on trust: at most ``MAX_DAT_BYTES + 1`` bytes are inflated, and
    the CRC-32 is checked by reading it whole.
    """
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            files = [info for info in archive.infolist() if not info.is_dir()]
            if len(files) != 1:
                raise _zip_refused(f"it holds {len(files)} files, not one.")
            info = files[0]
            if not info.filename.lower().endswith(".dat"):
                raise _zip_refused("the file in it is not a .dat file.")
            if info.flag_bits & 0x1:
                raise _zip_refused("the file in it is encrypted.")
            if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                raise _zip_refused("the file in it is compressed with a method other than deflate.")
            if info.file_size > MAX_DAT_BYTES:
                raise DiscVerifyError(f"The datafile in the zip is larger than {MAX_DAT_BYTES} bytes.",
                                      boundary_code="DISC_DAT_TOO_LARGE")
            with archive.open(info) as stream:
                text = stream.read(MAX_DAT_BYTES + 1)
                if len(text) > MAX_DAT_BYTES:
                    raise DiscVerifyError(
                        f"The datafile in the zip is larger than {MAX_DAT_BYTES} bytes.",
                        boundary_code="DISC_DAT_TOO_LARGE")
    except DiscVerifyError:
        raise
    except (zipfile.BadZipFile, zipfile.LargeZipFile, NotImplementedError, RuntimeError, EOFError,
            ValueError, OSError, zlib.error) as exc:
        raise _zip_refused(f"{exc}") from exc
    return text


def _dat_bytes(data: bytes) -> bytes:
    """A datafile's own text: the bytes given, or the ``.dat`` inside them when they are a zip."""
    return _dat_from_zip(data) if data.startswith(_ZIP_MAGIC) else data


def _parse_dat_bytes(data: bytes) -> RedumpDat:
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise DiscVerifyError("The datafile isn't UTF-8 text.",
                              boundary_code="DISC_DAT_NOT_DATAFILE") from exc
    return parse_dat_text(text)


def load_dat(path: Path | str) -> RedumpDat:
    """Read a datafile (or the zip Redump serves) from disk, refusing one over ``MAX_DAT_BYTES``."""
    return _parse_dat_bytes(_dat_bytes(_read_dat_bytes(path)))


def imported_dat_path(user_data_root: Path | str) -> Path:
    """Where the player and the tools keep the user's imported datafile."""
    return Path(user_data_root) / DAT_RELATIVE_PATH


def bundled_dat_path(project_root: Path | str = PROJECT_ROOT) -> Path:
    """The list bundled with the project, in a checkout or a player folder."""
    return Path(project_root) / BUNDLED_DAT_RELATIVE_PATH


def import_dat(source: Path | str, user_data_root: Path | str) -> RedumpDat:
    """Check a datafile the user chose and keep a copy in their data folder.

    ``source`` is a ``.dat`` or the zip Redump serves; from a zip, the ``.dat`` inside is what
    is checked and kept. The copy replaces any earlier import whole (written beside it, then
    renamed), so a reader never sees half a list. The file it is written to is created new
    under a random name, so a link already in the folder is never written through. The bytes
    checked are the bytes saved: the file is read once. Nothing leaves the machine. From then
    on the import takes precedence over the bundled list.
    """
    data = _dat_bytes(_read_dat_bytes(source))
    dat = _parse_dat_bytes(data)
    dat.source = LIST_IMPORTED
    target = imported_dat_path(user_data_root)
    temporary: Path | None = None
    folder = target.parent
    root = Path(user_data_root).resolve(strict=False)
    if (folder.is_symlink() or folder.is_junction() or target.is_symlink()
            or not folder.resolve(strict=False).is_relative_to(root)):
        raise DiscVerifyError("The data folder's verification folder is a link; the list was not "
                              "saved outside the data folder.",
                              boundary_code="DISC_DAT_WRITE_FAILED")
    try:
        folder.mkdir(parents=True, exist_ok=True)
        handle, name = tempfile.mkstemp(dir=folder, prefix=f".{target.name}.", suffix=".tmp")
        temporary = Path(name)
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
        os.replace(temporary, target)
    except OSError as exc:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
        raise DiscVerifyError(f"The datafile can't be saved in the data folder: {exc}",
                              boundary_code="DISC_DAT_WRITE_FAILED") from exc
    return dat


def load_imported_dat(user_data_root: Path | str) -> Optional[RedumpDat]:
    """The datafile the user imported into this data folder, or None when there is none."""
    path = imported_dat_path(user_data_root)
    if not path.is_file():
        return None
    dat = load_dat(path)
    dat.source = LIST_IMPORTED
    return dat


def load_verification_dat(user_data_root: Path | str, *,
                          bundled: Path | str | None = None) -> Optional[RedumpDat]:
    """The list a disc check uses: the user's import when there is one, else the bundled list.

    ``bundled`` defaults to :func:`bundled_dat_path`. ``RedumpDat.source`` says which list it
    is. An import that can't be read raises its ``DiscVerifyError``: the bundled list never
    stands in for it silently, so the user learns their list is unusable. None only when
    neither list exists.
    """
    imported = load_imported_dat(user_data_root)
    if imported is not None:
        return imported
    path = Path(bundled) if bundled is not None else bundled_dat_path()
    if not path.is_file():
        return None
    dat = load_dat(path)
    dat.source = LIST_BUNDLED
    return dat


def hash_image(path: Path | str, *,
               on_progress: Optional[Callable[[int, int], None]] = None) -> DiscDigest:
    """SHA-1 and CRC32 of a whole plain disc image, read once in 1 MiB chunks."""
    path = Path(path)
    sha1 = hashlib.sha1(usedforsecurity=False)
    crc = 0
    done = 0
    try:
        total = path.stat().st_size
        with path.open("rb") as handle:
            while chunk := handle.read(HASH_CHUNK_BYTES):
                sha1.update(chunk)
                crc = zlib.crc32(chunk, crc)
                done += len(chunk)
                if on_progress is not None:
                    on_progress(done, total)
    except OSError as exc:
        raise DiscVerifyError(f"The disc image can't be read: {exc}",
                              boundary_code="DISC_IMAGE_READ_FAILED") from exc
    return DiscDigest(sha1=sha1.hexdigest(), crc32=f"{crc & 0xFFFFFFFF:08x}", size_bytes=done)


def classify(disc_id: Optional[str], digest: Optional[DiscDigest], dat: Optional[RedumpDat],
             known_retail_ids: Iterable[str] = ()) -> str:
    """The tier of one image; ``known_retail_ids`` are disc IDs of verified images beside it."""
    if dat is None or digest is None:
        return NOT_CHECKED
    rom = dat.roms_by_sha1.get(digest.sha1.lower())
    if rom is not None and rom.crc32 == digest.crc32.lower() and rom.size_bytes == digest.size_bytes:
        return VERIFIED_RETAIL
    disc_id = (disc_id or "").upper()
    if disc_id and (disc_id in dat.serials or disc_id in set(known_retail_ids)):
        return MODIFIED
    return UNKNOWN


def classify_all(images: Mapping[str, tuple[Optional[str], Optional[DiscDigest]]],
                 dat: Optional[RedumpDat]) -> dict[str, str]:
    """Tiers for a whole library at once, so the order images were added never changes one.

    ``images`` maps a caller's key to (disc ID, digest). A disc ID is known retail when any
    image in the set with that disc ID is verified.
    """
    verified_ids = {
        (disc_id or "").upper()
        for disc_id, digest in images.values()
        if disc_id and classify(disc_id, digest, dat) == VERIFIED_RETAIL
    }
    return {key: classify(disc_id, digest, dat, verified_ids)
            for key, (disc_id, digest) in images.items()}
