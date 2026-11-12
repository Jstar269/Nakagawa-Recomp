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
  library. So the image's hashes differ from every listed dump of its disc: a damaged or
  incomplete dump does that as well as a changed copy (a fan translation, a patch, a trimmed
  image), and the check can't tell which. Dumping the disc again and checking that is how a
  user tells.
- ``UNKNOWN``: neither. The image is not in the list, which means "not shown to be retail",
  never "bad".
- ``NOT_CHECKED``: no list was found, or the image was not hashed.

The datafile is read in one forward pass (``_DatScanner``), the same rules as
``src/core/nk_disc_verify.c``, so its cost is linear in its size, which ``MAX_DAT_BYTES``
caps. It expands no entity and never reads a DTD: one ``<!DOCTYPE>`` before the root that
only names an external DTD (Logiqx's, as Redump's lists do) is skipped unread, and any other
``<!`` markup (an entity declaration, an internal DTD subset, a second DOCTYPE, a comment) or a
reference to any entity but XML's five predefined ones and character references is refused
(``DISC_DAT_ENTITY_REFUSED``). It reads each ``<game>`` element, its ``<rom>`` elements'
``size``, ``crc`` and ``sha1`` attributes, and any ``<serial>`` text. A rom with a malformed
hash or size is skipped and counted. A datafile may come as the zip Redump serves (exactly one
entry, a ``.dat``, stored or deflated, not ZIP64), its size checked before it is inflated.
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

# The largest datafile read, a file or the .dat a zip holds (checked against the zip's own
# size field before inflating, and again on the bytes inflated). Redump's PSP list of
# 2026-11-12 is 1,475,779 bytes for 3,546 discs, about 416 bytes a disc, so 16 MiB holds about
# 40,000 discs: ten times the list, and more than the PSP library will ever add. A file over it
# is refused (DISC_DAT_TOO_LARGE), so the reader holds at most 16 MiB of bytes, the same text
# decoded (at most 64 MiB in the worst character width) and MAX_DAT_ROMS roms.
MAX_DAT_BYTES = 16 * 1024 * 1024
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

# The datafile scanner's patterns. Each is matched at one position over a bounded window or
# a span the scanner moves past, never searched across the whole text, so no input makes one
# backtrack: no pattern has two unbounded quantifiers over characters they share.
# The end of a tag: its ">", or a "<" that makes it malformed.
_TAG_END = re.compile(r"[<>]")
# An element name, read from at most _TAG_NAME_WINDOW characters after "<" or "</".
_TAG_NAME = re.compile(r"[^ \t\r\n/>]*")
_TAG_NAME_WINDOW = 16
# One attribute up to its value's opening quote: name, "=", '"'. The name, spacing and "="
# classes are disjoint, so a failed match backs off at most once per name character.
_ATTRIBUTE_START = re.compile(r'[ \t\r\n]*([A-Za-z_][A-Za-z0-9_.:-]*)[ \t\r\n]*=[ \t\r\n]*"')
# A reference XML defines without a DTD: the five predefined entities, or a decimal (1-7
# digits) or hex (1-6 digits) character reference, matched in a window of its longest length.
_REFERENCE = re.compile(r"&(?:amp|lt|gt|quot|apos|#[0-9]{1,7}|#x[0-9A-Fa-f]{1,6});")
_REFERENCE_WINDOW = len("&#1114111;")
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
        # LIST_IMPORTED or LIST_BUNDLED when load_verification_dat knows which list failed.
        self.list_source = ""


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
    # For an import: the bundled list's date when the bundled list is newer (load_verification_dat).
    newer_bundled_date: str = ""

    def __len__(self) -> int:
        return len(self.roms_by_sha1)

    def stamp(self) -> str:
        """The list's date (else its version) for output: printable ASCII only, at most 63
        characters, as the C reader's copy is."""
        return "".join(ch for ch in (self.date or self.version)
                       if " " <= ch <= "~")[:63]

    def describe(self) -> str:
        """Which list this is and its date, for output: "the bundled Redump list (... discs)".

        An import older than the bundled list says so, and how to use the bundled one again.
        """
        owner = {LIST_IMPORTED: "your imported Redump list",
                 LIST_BUNDLED: "the Redump list bundled with Nakagawa"}.get(self.source, "a Redump list")
        stamp = self.stamp()
        dated = f"dated {stamp}, " if stamp else ""
        text = f"{owner} ({dated}{len(self)} discs)"
        if self.newer_bundled_date:
            text += (f", older than the list bundled with Nakagawa (dated {self.newer_bundled_date}): "
                     "import a newer list, or delete verification/redump.dat from your data "
                     "folder to use the bundled one")
        return text


def normalize_serials(text: str, limit: int = MAX_GAME_SERIALS) -> tuple[str, ...]:
    """Disc IDs in a ``<serial>`` value ("TEST-71002, TEST-71002GA") as PARAM.SFO spells them.

    At most ``limit`` IDs, as the C reader keeps them: the scan stops there, so a value of
    any length costs one pass and the duplicate check stays within ``limit``.
    """
    serials: list[str] = []
    start = 0
    while start <= len(text) and len(serials) < limit:
        comma = text.find(",", start)
        stop = len(text) if comma < 0 else comma
        compact = re.sub(r"[\s-]", "", text[start:stop]).upper()
        match = _RETAIL_DISC_ID.match(compact)
        if match and match.group(0) not in serials:
            serials.append(match.group(0))
        start = stop + 1
    return tuple(serials)


def _not_datafile(message: str) -> DiscVerifyError:
    return DiscVerifyError(message, boundary_code="DISC_DAT_NOT_DATAFILE")


def _markup_refused() -> DiscVerifyError:
    return DiscVerifyError("The datafile declares entities, a DTD of its own or markup a Redump "
                           "list does not use, so it was not read.",
                           boundary_code="DISC_DAT_ENTITY_REFUSED")


class _DatScanner:
    """One forward pass over a datafile's text, the rules of ``src/core/nk_disc_verify.c``.

    The scan moves from the start to ``</datafile>`` and never goes back. Each search starts
    where the last one stopped, each pattern is matched at one position, and lookahead is
    bounded (a tag name, a reference), so the work is linear in the text's length.
    ``examined`` counts the characters looked at, every one each time it is looked at, so a
    test can hold the cost of any input to a fixed multiple of its length.

    What it reads: the first ``<header>``'s ``<name>``, ``<version>`` and ``<date>``; each
    ``<game>``'s ``<serial>`` text and its ``<rom>`` elements' ``size``, ``crc`` and ``sha1``.
    What it refuses rather than skips: any ``<!`` markup but one ``<!DOCTYPE>`` before the root
    that only names an external DTD (which is never read), so no entity is ever declared; a
    reference to any entity but XML's five and character references; a ``<`` inside a tag.
    """

    def __init__(self, text: str) -> None:
        self.text = text
        self.examined = 0

    def _find(self, needle: str, start: int, stop: int) -> int:
        at = self.text.find(needle, start, stop)
        self.examined += max(0, (stop if at < 0 else at + len(needle)) - start)
        return at

    def _check_references(self, start: int, stop: int) -> None:
        at = self._find("&", start, stop)
        while at >= 0:
            self.examined += _REFERENCE_WINDOW
            if not _REFERENCE.match(self.text, at, min(at + _REFERENCE_WINDOW, len(self.text))):
                raise DiscVerifyError("The datafile refers to an entity XML does not predefine; "
                                      "a Redump list has none, so it was not read.",
                                      boundary_code="DISC_DAT_ENTITY_REFUSED")
            at = self._find("&", at + 1, stop)

    def _attributes(self, start: int, stop: int) -> dict[str, str]:
        """A tag's ``name="value"`` attributes in order, up to the first that is malformed.

        The first of a repeated name is kept, as the C reader keeps it.
        """
        attributes: dict[str, str] = {}
        pos = start
        while pos < stop:
            match = _ATTRIBUTE_START.match(self.text, pos, stop)
            if match is None:
                self.examined += stop - pos
                break
            self.examined += match.end() - pos
            close = self._find('"', match.end(), stop)
            if close < 0:
                break
            attributes.setdefault(match.group(1), self.text[match.end():close])
            pos = close + 1
        return attributes

    def parse(self) -> RedumpDat:
        text = self.text
        length = len(text)
        if self._find("<datafile", 0, length) < 0:
            raise _not_datafile("This file isn't a Redump datafile: it has no <datafile> element.")
        dat = RedumpDat()
        serials: set[str] = set()
        roms = 0
        root = closed = doctype = False
        header = 0  # 0 before the first <header>, 1 inside it, 2 after it
        field = ""  # the element whose text is being read: name, version, date or serial
        field_start = 0
        game_serials: list[str] | None = None  # set while inside a <game>
        game_roms: list[tuple[str, str, str, str]] = []
        pos = 0
        while pos < length:
            lt = self._find("<", pos, length)
            stop = length if lt < 0 else lt
            self._check_references(pos, stop)
            if lt < 0:
                break
            pending, field = field, ""
            if text.startswith("<?", lt):
                end = self._find("?>", lt + 2, length)
                if end < 0:
                    raise _not_datafile("The datafile is cut short inside a <?...?> declaration.")
                pos = end + 2
                continue
            if text.startswith("<!", lt):
                self.examined += len("<!DOCTYPE")
                if doctype or root or text[lt:lt + len("<!DOCTYPE")].upper() != "<!DOCTYPE":
                    raise _markup_refused()
                tag_end = _TAG_END.search(text, lt + 2)
                self.examined += (length if tag_end is None else tag_end.end()) - (lt + 2)
                if tag_end is None:
                    raise _not_datafile("The datafile is cut short inside its <!DOCTYPE>.")
                # Only "<!DOCTYPE name PUBLIC/SYSTEM ...>" naming an external DTD, which is
                # never read: an internal subset ("[") or markup inside it is refused.
                if tag_end.group(0) == "<" or self._find("[", lt, tag_end.start()) >= 0:
                    raise _markup_refused()
                doctype = True
                pos = tag_end.end()
                continue
            tag_end = _TAG_END.search(text, lt + 1)
            self.examined += (length if tag_end is None else tag_end.end()) - (lt + 1)
            if tag_end is None:
                raise _not_datafile("The datafile is cut short inside a tag.")
            if tag_end.group(0) == "<":
                raise _not_datafile("The datafile has a malformed tag: a '<' inside a tag.")
            gt = tag_end.start()
            self._check_references(lt, gt)
            closing = text.startswith("</", lt)
            name_start = lt + (2 if closing else 1)
            window_stop = min(name_start + _TAG_NAME_WINDOW, gt)
            self.examined += window_stop - name_start
            name_match = _TAG_NAME.match(text, name_start, window_stop)
            name = name_match.group(0) if name_match else ""
            name_end = name_start + len(name)
            bare = name_end == gt  # "<name>" with nothing after the name
            pos = gt + 1
            if closing:
                if pending and name == pending:
                    value = _xml_text(text[field_start:lt])
                    self.examined += lt - field_start
                    if pending == "serial" and game_serials is not None:
                        for disc_id in normalize_serials(value):
                            if disc_id not in game_serials and len(game_serials) < MAX_GAME_SERIALS:
                                game_serials.append(disc_id)
                    elif pending in ("name", "version", "date") and not getattr(dat, pending):
                        setattr(dat, pending, value.strip())
                elif name == "header" and header == 1:
                    header = 2
                elif name == "game" and game_serials is not None:
                    dat.games += 1
                    serials.update(game_serials)
                    for rom_name, size, crc, sha1 in game_roms:
                        entry = DatRom(name=_xml_text(rom_name), size_bytes=int(size),
                                       crc32=crc.lower(), sha1=sha1.lower(),
                                       serials=tuple(game_serials))
                        dat.roms_by_sha1.setdefault(entry.sha1, entry)
                    game_serials, game_roms = None, []
                elif name == "datafile" and not text[name_end:gt].strip():
                    self.examined += gt - name_end
                    closed = True
                    break
                continue
            self_closing = text[gt - 1] == "/"
            if name == "datafile":
                root = True
            elif name == "header" and header == 0 and game_serials is None:
                header = 1
            elif name in ("name", "version", "date") and header == 1 and bare:
                field, field_start = name, pos
            elif name == "game":
                if game_serials is not None:
                    raise _not_datafile("The datafile is cut short inside a <game> element.")
                if self_closing:
                    dat.games += 1
                else:
                    game_serials, game_roms = [], []
            elif name == "serial" and game_serials is not None and bare:
                field, field_start = name, pos
            elif name == "rom" and game_serials is not None:
                attributes = self._attributes(name_end, gt - 1 if self_closing else gt)
                sha1 = attributes.get("sha1", "")
                crc = attributes.get("crc", "")
                size = attributes.get("size", "")
                if not (_SHA1.fullmatch(sha1) and _CRC32.fullmatch(crc) and _SIZE.fullmatch(size)
                        and int(size) < 2**64):
                    dat.rejected_roms += 1
                    continue
                roms += 1
                if roms > MAX_DAT_ROMS:
                    raise DiscVerifyError(f"The datafile lists more than {MAX_DAT_ROMS} discs.",
                                          boundary_code="DISC_DAT_TOO_LARGE")
                game_roms.append((attributes.get("name", ""), size, crc, sha1))
        # A list cut short (a download that stopped) is refused, never read as the whole list:
        # its discs past the cut would all read as unknown.
        if not closed:
            raise _not_datafile("The datafile is cut short: it has no closing </datafile>.")
        if game_serials is not None:
            raise _not_datafile("The datafile is cut short inside a <game> element.")
        dat.serials = frozenset(serials)
        if not dat.roms_by_sha1:
            raise DiscVerifyError("The datafile lists no disc with a SHA-1 hash.",
                                  boundary_code="DISC_DAT_EMPTY")
        return dat


def parse_dat_text(text: str) -> RedumpDat:
    """Read a Logiqx-format datafile (Redump's) from its text, in one pass (``_DatScanner``)."""
    return _DatScanner(text).parse()


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


# The end record and its comment are in the last 22 + 65,535 bytes of a zip.
_ZIP_END_SEARCH = 22 + 0xFFFF


def _has_zip64_extra(extra: bytes) -> bool:
    """Whether a zip entry's extra field holds a ZIP64 block (header ID 0x0001)."""
    at = 0
    while at + 4 <= len(extra):
        block_id = int.from_bytes(extra[at:at + 2], "little")
        if block_id == 0x0001:
            return True
        at += 4 + int.from_bytes(extra[at + 2:at + 4], "little")
    return False


def _zip_refused(reason: str) -> DiscVerifyError:
    return DiscVerifyError(f"The zip can't be read as a Redump datafile: {reason}",
                           boundary_code="DISC_DAT_ZIP_INVALID")


def _dat_from_zip(data: bytes) -> bytes:
    """The one ``.dat`` inside a zip as Redump serves it, read under ``MAX_DAT_BYTES``.

    The zip must hold exactly one file, named ``*.dat``, stored or deflated and not encrypted.
    Its size is never taken on trust: at most ``MAX_DAT_BYTES + 1`` bytes are inflated, and
    the CRC-32 is checked by reading it whole.
    """
    # ZIP64 is refused, as the C reader refuses it: a ZIP64 end record (its locator sits just
    # before the end record) or a ZIP64 extra field on the entry.
    end_record = data.rfind(b"PK\x05\x06", max(0, len(data) - _ZIP_END_SEARCH))
    if end_record >= 20 and data[end_record - 20:end_record - 16] == b"PK\x06\x07":
        raise _zip_refused("it uses ZIP64.")
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            # Every entry counts, a folder entry too: Redump's zip holds the one .dat alone.
            files = archive.infolist()
            if len(files) != 1:
                raise _zip_refused(f"it holds {len(files)} entries, not one file.")
            info = files[0]
            if info.is_dir():
                raise _zip_refused("the one entry in it is a folder.")
            if _has_zip64_extra(info.extra):
                raise _zip_refused("it uses ZIP64.")
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
    stands in for it silently, so the user learns their list is unusable. The error's
    ``list_source`` says which list failed. An import older than the bundled list is still
    used (the user chose it), with ``newer_bundled_date`` set so the output says so. None only
    when neither list exists.
    """
    path = Path(bundled) if bundled is not None else bundled_dat_path()
    try:
        imported = load_imported_dat(user_data_root)
    except DiscVerifyError as exc:
        exc.list_source = LIST_IMPORTED
        raise
    if imported is not None:
        # The user's choice stands, but an import older than the bundled list is named as such.
        try:
            bundled_dat = load_dat(path) if path.is_file() else None
        except DiscVerifyError:
            bundled_dat = None
        if bundled_dat is not None and list_date_is_older(imported.stamp(), bundled_dat.stamp()):
            imported.newer_bundled_date = bundled_dat.stamp()
        return imported
    if not path.is_file():
        return None
    try:
        dat = load_dat(path)
    except DiscVerifyError as exc:
        exc.list_source = LIST_BUNDLED
        raise
    dat.source = LIST_BUNDLED
    return dat


_LIST_DATE = re.compile(r"([0-9]{4}-[0-9]{2}-[0-9]{2})(?: ([0-9]{2}-[0-9]{2}-[0-9]{2}))?")


def list_date_is_older(date: str, other: str) -> bool:
    """Whether a list's date sorts before another's, both in Redump's ``YYYY-MM-DD HH-MM-SS``
    form (the time is optional). A date in any other form is never called older."""
    mine, theirs = _LIST_DATE.fullmatch(date), _LIST_DATE.fullmatch(other)
    if mine is None or theirs is None:
        return False
    if mine.group(1) != theirs.group(1):
        return mine.group(1) < theirs.group(1)
    return bool(mine.group(2) and theirs.group(2) and mine.group(2) < theirs.group(2))


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
