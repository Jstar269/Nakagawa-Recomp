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

The datafile format
===================

Two readers read a Redump datafile: this module (the command-line tools) and
``src/core/nk_disc_verify.c`` (the player). They accept exactly the format below, byte for
byte, and refuse everything else with the same code and the same message, so the player and
the tools never give two verdicts on one disc. Both test suites hold them to it with one
table of cases and expected results (``tests/native/disc_dat_conformance.txt``: hostile
shapes, and a few thousand seeded mutations of the bundled list); a reader that changes
alone fails its suite. The rules are listed in the order the readers apply them; the first
rule an input breaks names the refusal. Offsets in messages count bytes from the start of
the text, after a byte-order mark. ``MAX_*`` below are this module's constants (the C
header has the same values as ``NK_DISC_DAT_MAX_*``).

1. **Size.** A file of more than ``MAX_DAT_BYTES`` (16 MiB) is refused unread
   (``DISC_DAT_TOO_LARGE``).
2. **Zip.** A file that starts with ``PK\\x03\\x04`` is a zip and must be exactly the
   structure Redump serves; anything else is ``DISC_DAT_ZIP_INVALID`` (one message per rule):
   at least 22 bytes; its last 22 bytes are the end record, with no comment; one disk; one
   entry by both counts; the central directory ends where the end record starts; one central
   entry of exactly the size the directory has; no ZIP64 size or offset (0xFFFFFFFF), and an
   extra field made of whole blocks with no ZIP64 block (0x0001); not encrypted (flag bits 0
   and 6); no flag bits but 1, 2 and 11 (so no data descriptor); stored (0) or deflated (8);
   a name with no NUL byte that ends in ``.dat`` (lower case); a declared size of at most
   ``MAX_DAT_BYTES`` (``DISC_DAT_TOO_LARGE`` otherwise, before anything is inflated) and not
   0 (an empty file); its
   local header at offset 0, agreeing with the central entry on flags, method, CRC-32, both
   sizes and the name, its extra field whole blocks with no ZIP64 block; its data ending
   where the central directory starts. A stored entry's two sizes are equal. A deflated
   entry is inflated under zlib's rules (RFC 1951 as zlib 1.2/1.3 decode it raw: an
   incomplete Huffman code only when it is one code of one bit, never for the code-length
   code; at most 286 literal/length and 30 distance codes; no distance before the start)
   into at most its declared size: a stream that would write more is refused there (the
   declared size is never trusted, and never exceeded), as is one that does not inflate or
   whose final block ends before its last byte. The stream is decoded in order and the
   first failure met decides: a malformed code, block header or distance, or the input
   ending, met before the first byte past the declared size is "does not inflate"; that
   byte is "inflates past its declared size" whatever follows it, and when a copy starts
   with no room left, before its distance is checked. The bytes inflated must have the declared
   size and CRC-32. The ``.dat`` inside is then read as text, under rule 3 on.
3. **Encoding.** One UTF-8 byte-order mark (``EF BB BF``) at the start is skipped. The rest
   must be UTF-8 (RFC 3629: no overlong form, no surrogate, nothing past U+10FFFF), else
   ``DISC_DAT_NOT_DATAFILE``. Then no byte 0x00-0x08, 0x0B, 0x0C, 0x0E-0x1F or 0x7F may
   appear anywhere (``DISC_DAT_NOT_DATAFILE``, naming the byte and its offset). Non-ASCII
   bytes are passed through as bytes inside text and attribute values; the markup itself is
   ASCII (rule 5), so a non-ASCII space (U+00A0, U+3000, ...) where markup is expected is a
   malformed tag.
4. **Whitespace** is space, tab, CR and LF (0x20, 0x09, 0x0D, 0x0A) and nothing else.
5. **Markup.** Names (of elements and attributes) are ASCII: a letter, ``_`` or ``:``,
   then letters, digits, ``_``, ``.``, ``:`` or ``-``; they are compared byte for byte,
   case-sensitively (``<ROM>`` is not ``<rom>``). A tag runs from ``<`` to the first ``>``;
   a ``<`` before it is refused, and so is a tag the text ends inside (both
   ``DISC_DAT_NOT_DATAFILE``). Every ``&`` in text and in a tag must start ``&amp;``,
   ``&lt;``, ``&gt;``, ``&quot;``, ``&apos;``, ``&#`` and 1-7 decimal digits and ``;``, or
   ``&#x`` and 1-6 hex digits and ``;`` (``DISC_DAT_ENTITY_REFUSED`` otherwise); only the
   five named ones are ever decoded. Before the root element there may be whitespace,
   ``<?...?>`` declarations and one ``<!DOCTYPE`` (exactly that case) up to its ``>`` with
   no ``[`` or ``<`` in it, which is never read. Any other ``<!`` (a comment, CDATA, an
   entity declaration, a second or late DOCTYPE) and a ``<?`` inside the root are refused
   (``DISC_DAT_ENTITY_REFUSED``). A start tag is ``<name``, then attributes, each one
   whitespace, a name, optional whitespace, ``=``, optional whitespace and a value in double
   quotes (no ``"`` inside; single quotes are not read), then optional whitespace and an
   optional ``/`` just before ``>``. An end tag is ``</name``, optional whitespace and ``>``.
   Anything else between ``<`` and ``>`` is a malformed tag (``DISC_DAT_NOT_DATAFILE``).
   Every ``<`` counts toward ``MAX_DAT_TAGS``, past which the list is refused
   (``DISC_DAT_TOO_LARGE``).
6. **Elements.** Elements must nest: an end tag closes the element its name opened, and
   nothing nests more than ``MAX_DAT_DEPTH`` (8) deep. The first element is ``<datafile>``,
   the root. Text outside it is whitespace only. Its children ``<header>`` (at most one, and
   before any ``<game>``) and ``<game>`` are read; any other element anywhere is skipped,
   whatever its content, except that ``<datafile>``, ``<header>``, ``<game>``, ``<rom>`` and
   ``<serial>`` anywhere but their place are refused, and so is an element inside a
   ``<name>``, ``<version>``, ``<date>`` or ``<serial>`` that is read. After
   ``</datafile>`` only whitespace may follow. A text that ends before ``</datafile>`` (a
   download cut short) is refused, never read as the whole list (all ``DISC_DAT_NOT_DATAFILE``).
7. **Header.** The header's ``<name>``, ``<version>`` and ``<date>`` (each at most once) are
   read as their text with leading and trailing whitespace removed and the five named
   references decoded; the result must be at most 63 bytes of printable ASCII (0x20-0x7E),
   else ``DISC_DAT_NOT_DATAFILE``. A list's date is its ``<date>``, else its ``<version>``.
8. **Games.** A ``<game>``'s ``<rom>`` elements must be empty (``<rom .../>``). A rom is read
   from the first ``sha1``, ``crc`` and ``size`` attribute of each name: exactly 40 and 8
   hex digits of either case, and 1-20 decimal digits below 2**64. A rom without all three,
   or with one malformed, is skipped and counted (``rejected``); more than ``MAX_DAT_ROMS``
   read roms are refused (``DISC_DAT_TOO_LARGE``). A SHA-1 listed twice keeps its first rom.
9. **Disc IDs.** A game has at most one ``<serial>``, whose raw text (references not
   decoded) is at most ``MAX_SERIAL_BYTES`` (256) bytes. It is split at commas; from each
   part space, tab, CR, LF and ``-`` are removed and ``a``-``z`` upper-cased (ASCII only);
   when the part's first nine bytes are four letters ``A``-``Z`` and five digits, they are a
   disc ID ("ULJM 06340" and "ULUS-10041GA" give ULJM06340 and ULUS10041). A ``<serial>``
   naming more than ``MAX_GAME_SERIALS`` (16) different IDs is refused, and so are more
   than ``MAX_DAT_SERIALS`` IDs over all games, each game's counted once
   (``DISC_DAT_TOO_LARGE``). An image's disc ID matches when it is the same nine bytes after
   ASCII upper-casing.
10. A list that reads no rom is refused (``DISC_DAT_EMPTY``).

What the reader holds: the file (at most 16 MiB), the inflated ``.dat`` when it came in a
zip (at most 16 MiB), and the roms and disc IDs it keeps, at most ``MAX_DAT_ROMS`` and
``MAX_DAT_SERIALS``; its work is linear in the input (``_DatScanner.examined`` counts it,
and the tests hold it to a fixed multiple of the input's size, as they hold its memory).
"""

from __future__ import annotations

import codecs
import dataclasses
from dataclasses import dataclass, field
import hashlib
import os
from pathlib import Path
import re
import tempfile
from typing import Callable, Iterable, Mapping, Optional
import zlib

VERIFIED_RETAIL = "VERIFIED_RETAIL"
MODIFIED = "MODIFIED"
UNKNOWN = "UNKNOWN"
NOT_CHECKED = "NOT_CHECKED"
TIERS = (VERIFIED_RETAIL, MODIFIED, UNKNOWN, NOT_CHECKED)

# The format's limits (the module documentation, rules 1 and 5-9; nk_disc_verify.h has the
# same values). Redump's PSP list of 2026-11-12 is 1,475,779 bytes for 3,546 discs (about 416
# bytes and 11 tags a disc, at most 5 disc IDs in a <serial> of at most 74 bytes), so 16 MiB
# and 40,000 discs are ten times the list, more than the PSP library will ever add.
MAX_DAT_BYTES = 16 * 1024 * 1024
MAX_DAT_ROMS = 40_000
MAX_DAT_TAGS = 480_000
MAX_DAT_SERIALS = 80_000
MAX_DAT_DEPTH = 8
MAX_GAME_SERIALS = 16
MAX_SERIAL_BYTES = 256
MAX_HEADER_FIELD = 63
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
_BOM = b"\xef\xbb\xbf"
_WHITESPACE = b" \t\r\n"

# The scanner's patterns. Each is matched at one position, bounded by the end of the tag or
# text it reads, and fails only where the input is then refused: none can backtrack over
# more than the span it was given, so the work stays linear.
_NON_WHITESPACE = re.compile(rb"[^ \t\r\n]")
_CONTROL = re.compile(rb"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")
_NAME = re.compile(rb"[A-Za-z_:][A-Za-z0-9_.:-]*")
_ATTRIBUTE = re.compile(rb'[ \t\r\n]+([A-Za-z_:][A-Za-z0-9_.:-]*)[ \t\r\n]*=[ \t\r\n]*"([^"]*)"')
_START_TAIL = re.compile(rb"[ \t\r\n]*(/?)")
_END_TAIL = re.compile(rb"[ \t\r\n]*")
_REFERENCE = re.compile(rb"&(?:amp|lt|gt|quot|apos|#[0-9]{1,7}|#x[0-9A-Fa-f]{1,6});")
# The length of the longest character reference the reader accepts: an ampersand, a hash,
# seven decimal digits and a semicolon (the hex form, with an "x" and six digits, is as long).
_REFERENCE_WINDOW = 10
_NAMED_REFERENCE = re.compile(rb"&(amp|lt|gt|quot|apos);")
_NAMED = {b"amp": b"&", b"lt": b"<", b"gt": b">", b"quot": b'"', b"apos": b"'"}
_PRINTABLE = re.compile(rb"[\x20-\x7e]*")
_SHA1 = re.compile(rb"[0-9a-fA-F]{40}")
_CRC32 = re.compile(rb"[0-9a-fA-F]{8}")
_SIZE = re.compile(rb"[0-9]{1,20}")
_DISC_ID = re.compile(rb"(?:^|,)([A-Z]{4}[0-9]{5})")
_SERIAL_DROPPED = b" \t\r\n-"
_ROM_ATTRIBUTES = (b"sha1", b"crc", b"size", b"name")

# Element kinds (rule 6): the scanner's stack holds one per open element.
_OTHER, _DATAFILE, _HEADER, _GAME, _NAME_FIELD, _VERSION_FIELD, _DATE_FIELD, _SERIAL, _ROM = range(9)
_HEADER_FIELDS = {b"name": _NAME_FIELD, b"version": _VERSION_FIELD, b"date": _DATE_FIELD}
_TEXT_FIELDS = (_NAME_FIELD, _VERSION_FIELD, _DATE_FIELD, _SERIAL)
_PLACED = (b"datafile", b"header", b"game", b"rom", b"serial")

# Every refusal's message. src/core/nk_disc_verify.c writes the same text for each, and the
# conformance table compares both (code and message).
_TOO_LARGE = f"The datafile is larger than {MAX_DAT_BYTES} bytes."
_ZIP_TOO_LARGE = f"The datafile in the zip is larger than {MAX_DAT_BYTES} bytes."
_TOO_MANY_TAGS = f"The datafile has more than {MAX_DAT_TAGS} tags."
_TOO_MANY_ROMS = f"The datafile lists more than {MAX_DAT_ROMS} discs."
_TOO_MANY_SERIALS = f"The datafile names more than {MAX_DAT_SERIALS} disc IDs."
_NOT_UTF8 = "The datafile isn't UTF-8 text."
_CONTROL_BYTE = "The datafile holds a control character (byte 0x{:02X}) at offset {}."
_TEXT_OUTSIDE = "The datafile has text outside its <datafile> element at offset {}."
_CUT_DECLARATION = "The datafile is cut short inside a <?...?> declaration."
_CUT_DOCTYPE = "The datafile is cut short inside its <!DOCTYPE>."
_CUT_TAG = "The datafile is cut short inside a tag."
_LT_IN_TAG = "The datafile has a '<' inside a tag at offset {}."
_MALFORMED_TAG = "The datafile has a malformed tag at offset {}."
_UNMATCHED = "The datafile has an end tag that does not match its start tag at offset {}."
_NOT_ROOT = "The datafile's first element is not <datafile> (offset {})."
_TOO_DEEP = f"The datafile nests elements more than {MAX_DAT_DEPTH} deep at offset {{}}."
_OUT_OF_PLACE = ("The datafile has a <datafile>, <header>, <game>, <rom> or <serial> element "
                 "out of place at offset {}.")
_SECOND_HEADER = "The datafile has a second <header>, or one after a <game>, at offset {}."
_REPEATED_FIELD = ("The datafile repeats a <name>, <version> or <date> in its header, or a "
                   "<serial> in a game, at offset {}.")
_ELEMENT_IN_FIELD = ("The datafile has an element inside a <name>, <version>, <date> or "
                     "<serial> at offset {}.")
_ROM_NOT_EMPTY = "The datafile has a <rom> that is not empty (<rom .../>) at offset {}."
_BAD_HEADER_FIELD = (f"The datafile's header has a <name>, <version> or <date> that is longer "
                     f"than {MAX_HEADER_FIELD} bytes or not printable ASCII, at offset {{}}.")
_SERIAL_TOO_LONG = f"The datafile has a <serial> longer than {MAX_SERIAL_BYTES} bytes at offset {{}}."
_SERIAL_TOO_MANY = (f"The datafile has a <serial> naming more than {MAX_GAME_SERIALS} disc IDs "
                    f"at offset {{}}.")
_AFTER_ROOT = "The datafile goes on after its </datafile>, at offset {}."
_NO_ROOT = "This file isn't a Redump datafile: it has no <datafile> element."
_CUT_SHORT = "The datafile is cut short: it has no closing </datafile>."
_MARKUP_REFUSED = ("The datafile declares entities, a DTD of its own or markup a Redump list "
                   "does not use, so it was not read.")
_REFERENCE_REFUSED = ("The datafile refers to an entity XML does not predefine; a Redump list "
                      "has none, so it was not read.")
_EMPTY = "The datafile lists no disc with a SHA-1 hash."
_ZIP = "The zip can't be read as a Redump datafile: "


class DiscVerifyError(ValueError):
    """A datafile or image that cannot be used, with a named boundary code."""

    def __init__(self, message: str, *, boundary_code: str) -> None:
        super().__init__(message)
        self.boundary_code = boundary_code
        # LIST_IMPORTED or LIST_BUNDLED when load_verification_dat knows which list failed.
        self.list_source = ""


def _not_datafile(message: str) -> DiscVerifyError:
    return DiscVerifyError(message, boundary_code="DISC_DAT_NOT_DATAFILE")


def _too_large(message: str) -> DiscVerifyError:
    return DiscVerifyError(message, boundary_code="DISC_DAT_TOO_LARGE")


def _entity_refused(message: str) -> DiscVerifyError:
    return DiscVerifyError(message, boundary_code="DISC_DAT_ENTITY_REFUSED")


def _zip_refused(reason: str) -> DiscVerifyError:
    return DiscVerifyError(_ZIP + reason, boundary_code="DISC_DAT_ZIP_INVALID")


@dataclass(frozen=True)
class DiscDigest:
    """The whole-image hashes of one disc image: lowercase hex, and its size in bytes."""

    sha1: str
    crc32: str
    size_bytes: int

    def to_dict(self) -> dict:
        return {"sha1": self.sha1, "crc32": self.crc32, "size_bytes": self.size_bytes}


@dataclass(frozen=True, slots=True)
class DatRom:
    name: str
    size_bytes: int
    crc32: str
    sha1: str
    serials: tuple[str, ...] = ()
    # Its place among the roms read (rule 8), so a SHA-1 listed twice keeps the first.
    listed: int = 0


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
        """The list's date (else its version) for output: printable ASCII of at most 63
        characters, which rule 7 of the format already holds it to."""
        return "".join(ch for ch in (self.date or self.version) if " " <= ch <= "~")[:63]

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


def _ascii_upper(text: str) -> str:
    """``a``-``z`` upper-cased and nothing else (``str.upper`` would also map ``ſ`` to ``S``)."""
    return text.translate(_ASCII_UPPER)


_ASCII_UPPER = {code: code - 32 for code in range(ord("a"), ord("z") + 1)}


def normalize_serials(text: str | bytes, limit: int = MAX_GAME_SERIALS) -> tuple[str, ...]:
    """Disc IDs in a ``<serial>`` value ("TEST-71002, TEST-71002GA") as PARAM.SFO spells them
    (the format's rule 9), at most ``limit`` of them, without repeats, in the order given."""
    data = text.encode("utf-8") if isinstance(text, str) else text
    compact = data.translate(None, _SERIAL_DROPPED).upper()
    found = dict.fromkeys(_DISC_ID.findall(compact))
    return tuple(disc_id.decode("ascii") for disc_id in list(found)[:limit])


def _decode_named(raw: bytes) -> bytes:
    """Text with the five named references decoded and no others (rule 5)."""
    if b"&" not in raw:
        return raw
    return _NAMED_REFERENCE.sub(lambda match: _NAMED[match.group(1)], raw)


class _DatScanner:
    """One forward pass over a datafile's text: the format's rules 4-10 (module documentation),
    as ``src/core/nk_disc_verify.c`` applies them, step for step.

    The scan moves from the start to ``</datafile>`` and never goes back: each search starts
    where the last one stopped and each pattern is matched at one position over a bounded
    span, so the work is linear in the text's length. ``examined`` counts the bytes looked
    at, each time one is looked at (the serial splitting's passes too), so a test can hold
    the cost of any input to a fixed multiple of its length.
    """

    def __init__(self, data: bytes, start: int = 0) -> None:
        self.data = data
        self.start = start
        self.examined: int = 0

    def _offset(self, at: int) -> int:
        return at - self.start

    def _check_references(self, start: int, stop: int) -> None:
        data = self.data
        at = data.find(b"&", start, stop)
        self.examined += (stop if at < 0 else at + 1) - start
        while at >= 0:
            self.examined += _REFERENCE_WINDOW
            if not _REFERENCE.match(data, at, min(at + _REFERENCE_WINDOW, len(data))):
                raise _entity_refused(_REFERENCE_REFUSED)
            following = at + 1
            at = data.find(b"&", following, stop)
            self.examined += (stop if at < 0 else at + 1) - following

    def _header_field(self, raw: bytes, offset: int) -> str:
        value = _decode_named(raw.strip(_WHITESPACE))
        if len(value) > MAX_HEADER_FIELD or not _PRINTABLE.fullmatch(value):
            raise _not_datafile(_BAD_HEADER_FIELD.format(offset))
        return value.decode("ascii")

    def _serial_ids(self, raw: bytes, offset: int) -> list[str]:
        if len(raw) > MAX_SERIAL_BYTES:
            raise _not_datafile(_SERIAL_TOO_LONG.format(offset))
        # Three passes over the value: the bytes dropped, the upper-casing, the ID search.
        self.examined += 3 * len(raw)
        ids = normalize_serials(raw, MAX_GAME_SERIALS + 1)
        if len(ids) > MAX_GAME_SERIALS:
            raise _not_datafile(_SERIAL_TOO_MANY.format(offset))
        return list(ids)

    def parse(self) -> RedumpDat:
        data = self.data
        end = len(data)
        dat = RedumpDat()
        serials: set[str] = set()
        stack: list[tuple[int, int, int]] = []  # (name start, name end, kind)
        tags = roms = serial_ids = 0
        root = closed = doctype = header_seen = game_seen = False
        header_fields: set[int] = set()
        game_serial = False
        game_ids: list[str] = []
        game_roms: list[str] = []  # the SHA-1s this game added, to give them its disc IDs
        field_start = 0
        pos = self.start
        while True:
            lt = data.find(b"<", pos)
            stop = end if lt < 0 else lt
            self.examined += (stop if lt < 0 else lt + 1) - pos
            if not stack:
                text = _NON_WHITESPACE.search(data, pos, stop)
                self.examined += stop - pos
                if text is not None:
                    raise _not_datafile(_TEXT_OUTSIDE.format(self._offset(text.start())))
            else:
                self._check_references(pos, stop)
            if lt < 0:
                break
            tags += 1
            if tags > MAX_DAT_TAGS:
                raise _too_large(_TOO_MANY_TAGS)
            marker = data[lt + 1:lt + 2]
            if marker == b"?":
                if root:
                    raise _entity_refused(_MARKUP_REFUSED)
                close = data.find(b"?>", lt + 2)
                self.examined += (end if close < 0 else close + 2) - (lt + 2)
                if close < 0:
                    raise _not_datafile(_CUT_DECLARATION)
                pos = close + 2
                continue
            if marker == b"!":
                self.examined += len("<!DOCTYPE")
                if doctype or root or not data.startswith(b"<!DOCTYPE", lt):
                    raise _entity_refused(_MARKUP_REFUSED)
                gt = data.find(b">", lt + 2)
                inner = data.find(b"<", lt + 2, end if gt < 0 else gt)
                self.examined += (end if gt < 0 else gt + 1) - (lt + 2)
                if inner < 0 and gt < 0:
                    raise _not_datafile(_CUT_DOCTYPE)
                if inner >= 0 or data.find(b"[", lt, gt) >= 0:
                    raise _entity_refused(_MARKUP_REFUSED)
                self.examined += gt - lt
                doctype = True
                pos = gt + 1
                continue
            gt = data.find(b">", lt + 1)
            inner = data.find(b"<", lt + 1, end if gt < 0 else gt)
            self.examined += (end if gt < 0 else gt + 1) - (lt + 1)
            if inner >= 0:
                raise _not_datafile(_LT_IN_TAG.format(self._offset(lt)))
            if gt < 0:
                raise _not_datafile(_CUT_TAG)
            self._check_references(lt, gt)
            pos = gt + 1
            # Each tag's bytes are read once more by the grammar below.
            self.examined += gt - lt
            if marker == b"/":
                name = _NAME.match(data, lt + 2, gt)
                tail = _END_TAIL.match(data, name.end(), gt) if name is not None else None
                if name is None or tail is None or tail.end() != gt:
                    raise _not_datafile(_MALFORMED_TAG.format(self._offset(lt)))
                if not stack:
                    raise _not_datafile(_UNMATCHED.format(self._offset(lt)))
                open_start, open_end, kind = stack[-1]
                if data[open_start:open_end] != name.group(0):
                    raise _not_datafile(_UNMATCHED.format(self._offset(lt)))
                stack.pop()
            else:
                name = _NAME.match(data, lt + 1, gt)
                if name is None:
                    raise _not_datafile(_MALFORMED_TAG.format(self._offset(lt)))
                element = name.group(0)
                wanted: dict[bytes, bytes] = {}
                at = name.end()
                while True:
                    attribute = _ATTRIBUTE.match(data, at, gt)
                    if attribute is None:
                        break
                    if element == b"rom" and attribute.group(1) in _ROM_ATTRIBUTES:
                        wanted.setdefault(attribute.group(1), attribute.group(2))
                    at = attribute.end()
                tail = _START_TAIL.match(data, at, gt)
                if tail is None or tail.end() != gt:
                    raise _not_datafile(_MALFORMED_TAG.format(self._offset(lt)))
                self_closing = tail.group(1) == b"/"
                if len(stack) >= MAX_DAT_DEPTH:
                    raise _not_datafile(_TOO_DEEP.format(self._offset(lt)))
                parent = stack[-1][2] if stack else None
                if parent in _TEXT_FIELDS:
                    raise _not_datafile(_ELEMENT_IN_FIELD.format(self._offset(lt)))
                if parent is None:
                    if element != b"datafile":
                        raise _not_datafile(_NOT_ROOT.format(self._offset(lt)))
                    kind = _DATAFILE
                    root = True
                elif parent == _DATAFILE and element == b"header":
                    if header_seen or game_seen:
                        raise _not_datafile(_SECOND_HEADER.format(self._offset(lt)))
                    kind = _HEADER
                    header_seen = True
                elif parent == _DATAFILE and element == b"game":
                    kind = _GAME
                    game_seen = True
                    game_serial = False
                    game_ids, game_roms = [], []
                elif parent == _HEADER and element in _HEADER_FIELDS:
                    kind = _HEADER_FIELDS[element]
                    if kind in header_fields:
                        raise _not_datafile(_REPEATED_FIELD.format(self._offset(lt)))
                    header_fields.add(kind)
                elif parent == _GAME and element == b"serial":
                    if game_serial:
                        raise _not_datafile(_REPEATED_FIELD.format(self._offset(lt)))
                    kind = _SERIAL
                    game_serial = True
                elif parent == _GAME and element == b"rom":
                    if not self_closing:
                        raise _not_datafile(_ROM_NOT_EMPTY.format(self._offset(lt)))
                    kind = _ROM
                    sha1 = wanted.get(b"sha1", b"")
                    crc = wanted.get(b"crc", b"")
                    size = wanted.get(b"size", b"")
                    if (_SHA1.fullmatch(sha1) and _CRC32.fullmatch(crc) and _SIZE.fullmatch(size)
                            and int(size) < 2**64):
                        roms += 1
                        if roms > MAX_DAT_ROMS:
                            raise _too_large(_TOO_MANY_ROMS)
                        # Kept as it is read (the first rom of a SHA-1 stays), so a game of
                        # many roms is held once, not also as the bytes it was read from.
                        key = sha1.decode("ascii").lower()
                        if key not in dat.roms_by_sha1:
                            dat.roms_by_sha1[key] = DatRom(
                                name=_decode_named(wanted.get(b"name", b"")).decode("utf-8"),
                                size_bytes=int(size), crc32=crc.decode("ascii").lower(), sha1=key,
                                listed=roms - 1)
                            game_roms.append(key)
                    else:
                        dat.rejected_roms += 1
                elif element in _PLACED:
                    raise _not_datafile(_OUT_OF_PLACE.format(self._offset(lt)))
                else:
                    kind = _OTHER
                field_start = pos
                if not self_closing:
                    stack.append((name.start(), name.end(), kind))
                    continue
            # An element closes: its end tag, or a start tag that ends in "/>".
            if kind in _TEXT_FIELDS:
                raw = data[field_start:lt] if marker == b"/" else b""
                offset = self._offset(lt)
                if kind == _SERIAL:
                    game_ids = self._serial_ids(raw, offset)
                else:
                    attribute_name = {_NAME_FIELD: "name", _VERSION_FIELD: "version",
                                      _DATE_FIELD: "date"}[kind]
                    setattr(dat, attribute_name, self._header_field(raw, offset))
            elif kind == _GAME:
                dat.games += 1
                serial_ids += len(game_ids)
                if serial_ids > MAX_DAT_SERIALS:
                    raise _too_large(_TOO_MANY_SERIALS)
                serials.update(game_ids)
                if game_ids:
                    ids = tuple(game_ids)
                    for key in game_roms:
                        dat.roms_by_sha1[key] = dataclasses.replace(dat.roms_by_sha1[key],
                                                                    serials=ids)
                game_ids, game_roms = [], []
            elif kind == _DATAFILE:
                closed = True
                text = _NON_WHITESPACE.search(data, pos, end)
                self.examined += end - pos
                if text is not None:
                    raise _not_datafile(_AFTER_ROOT.format(self._offset(text.start())))
                break
        if not closed:
            raise _not_datafile(_CUT_SHORT if root else _NO_ROOT)
        dat.serials = frozenset(serials)
        if not dat.roms_by_sha1:
            raise DiscVerifyError(_EMPTY, boundary_code="DISC_DAT_EMPTY")
        return dat


def _check_text(data: bytes, start: int) -> int:
    """Rule 3: UTF-8, then no control byte. Returns the bytes examined (two passes)."""
    if not data.isascii():
        decoder = codecs.getincrementaldecoder("utf-8")("strict")
        try:
            # A chunk at a time, so the text decoded is never held whole.
            for at in range(start, len(data), HASH_CHUNK_BYTES):
                decoder.decode(data[at:at + HASH_CHUNK_BYTES], final=False)
            decoder.decode(b"", final=True)
        except UnicodeDecodeError as exc:
            raise _not_datafile(_NOT_UTF8) from exc
    control = _CONTROL.search(data, start)
    if control is not None:
        raise _not_datafile(_CONTROL_BYTE.format(data[control.start()], control.start() - start))
    return 2 * (len(data) - start)


def parse_dat_bytes(data: bytes) -> RedumpDat:
    """Read a datafile's text (the format's rules 3-10): no zip, no size check."""
    start = len(_BOM) if data.startswith(_BOM) else 0
    _check_text(data, start)
    return _DatScanner(data, start).parse()


def parse_dat_text(text: str | bytes) -> RedumpDat:
    """Read a Logiqx-format datafile (Redump's) from its text: ``parse_dat_bytes`` on its UTF-8."""
    if isinstance(text, bytes):
        return parse_dat_bytes(text)
    try:
        data = text.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise _not_datafile(_NOT_UTF8) from exc
    return parse_dat_bytes(data)


def _read_file(path: Path | str) -> bytes:
    """A file's bytes, read once and refused over ``MAX_DAT_BYTES`` (rule 1)."""
    try:
        with Path(path).open("rb") as handle:
            # read(n) sets n bytes aside before it reads, so ask for the file's own size (plus
            # one, to see a file that grew) rather than the cap; read on only if it grew.
            size = os.fstat(handle.fileno()).st_size
            if size > MAX_DAT_BYTES:
                raise _too_large(_TOO_LARGE)
            data = handle.read(size + 1)
            if len(data) > size:
                data += handle.read(MAX_DAT_BYTES + 1 - len(data))
    except OSError as exc:
        raise DiscVerifyError(f"The datafile can't be read: {exc}",
                              boundary_code="DISC_DAT_READ_FAILED") from exc
    if len(data) > MAX_DAT_BYTES:
        raise _too_large(_TOO_LARGE)
    return data


def _u16(data: bytes, at: int) -> int:
    return int.from_bytes(data[at:at + 2], "little")


def _u32(data: bytes, at: int) -> int:
    return int.from_bytes(data[at:at + 4], "little")


def _extra_is_whole_without_zip64(extra: bytes) -> tuple[bool, bool]:
    """Whether a zip extra field is made of whole blocks, and whether one is ZIP64 (0x0001).
    The walk stops at the first block that is ZIP64 or does not fit."""
    at = 0
    while at < len(extra):
        if at + 4 > len(extra) or at + 4 + _u16(extra, at + 2) > len(extra):
            return False, False
        if _u16(extra, at) == 0x0001:
            return True, True
        at += 4 + _u16(extra, at + 2)
    return True, False


def _inflate(packed: memoryview, size: int) -> bytes:
    """A raw deflate stream inflated into at most ``size`` (at least 1) bytes, under rule 2's
    order, as the C reader's nk_psp_inflate_raw_zlib meets each failure: in stream order,
    the first byte to write past ``size`` is the overflow, and a failure met before it is
    the stream's."""
    inflater = zlib.decompressobj(-15)
    try:
        plain = inflater.decompress(packed, size)
    except zlib.error as exc:
        raise _zip_refused("its file does not inflate.") from exc
    if not inflater.eof:
        # zlib decodes on until it must write or read more, so it stopped either where the
        # next byte goes past ``size`` (and resumes with that write, before any check of
        # what follows, a copy's distance included) or for input there is not.
        try:
            more = inflater.decompress(inflater.unconsumed_tail, 1)
        except zlib.error as exc:
            raise _zip_refused("its file inflates past its declared size.") from exc
        if more:
            raise _zip_refused("its file inflates past its declared size.")
        if not inflater.eof:
            raise _zip_refused("its file does not inflate.")
    if inflater.unused_data:
        raise _zip_refused("its file's compressed data goes on past its end.")
    return plain


def _dat_from_zip(data: bytes) -> bytes:
    """The ``.dat`` inside a zip, under the format's rule 2: the steps of the C reader's
    dat_text_from_zip, in its order."""
    size = len(data)
    if size < 22:
        raise _zip_refused("it is cut short.")
    eocd = size - 22
    if data[eocd:eocd + 4] != b"PK\x05\x06" or _u16(data, eocd + 20) != 0:
        raise _zip_refused("its last 22 bytes are not its end record (it has a comment, or "
                           "data after it).")
    if _u16(data, eocd + 4) != 0 or _u16(data, eocd + 6) != 0:
        raise _zip_refused("it spans several disks.")
    if _u16(data, eocd + 8) != 1 or _u16(data, eocd + 10) != 1:
        raise _zip_refused("it does not hold exactly one file.")
    central_size, central = _u32(data, eocd + 12), _u32(data, eocd + 16)
    if central + central_size != eocd:
        raise _zip_refused("its central directory is not where its end record says.")
    name_length, extra_length = _u16(data, central + 28), _u16(data, central + 30)
    comment_length = _u16(data, central + 32)
    if (central_size < 46 or data[central:central + 4] != b"PK\x01\x02"
            or 46 + name_length + extra_length + comment_length != central_size):
        raise _zip_refused("its central directory is malformed.")
    flags, method = _u16(data, central + 8), _u16(data, central + 10)
    crc, packed_size, plain_size = (_u32(data, central + 16), _u32(data, central + 20),
                                    _u32(data, central + 24))
    local = _u32(data, central + 42)
    if 0xFFFFFFFF in (packed_size, plain_size, local):
        raise _zip_refused("it uses ZIP64.")
    whole, zip64 = _extra_is_whole_without_zip64(
        data[central + 46 + name_length:central + 46 + name_length + extra_length])
    if not whole:
        raise _zip_refused("its central directory is malformed.")
    if zip64:
        raise _zip_refused("it uses ZIP64.")
    if flags & 0x41:
        raise _zip_refused("the file in it is encrypted.")
    if flags & ~0x0806:
        raise _zip_refused(f"the file in it uses a zip feature Redump's zip does not (flags 0x{flags:04X}).")
    if method not in (0, 8):
        raise _zip_refused("the file in it is compressed with a method other than deflate.")
    name = data[central + 46:central + 46 + name_length]
    if b"\0" in name or not name.endswith(b".dat"):
        raise _zip_refused("the file in it is not a .dat file.")
    if plain_size > MAX_DAT_BYTES:
        raise _too_large(_ZIP_TOO_LARGE)
    if plain_size == 0:
        raise _zip_refused("the file in it is empty.")
    if local != 0:
        raise _zip_refused("its file does not start the zip.")
    local_name_length, local_extra_length = _u16(data, 26), _u16(data, 28)
    if (central < 30 or data[:4] != _ZIP_MAGIC or _u16(data, 6) != flags or _u16(data, 8) != method
            or _u32(data, 14) != crc or _u32(data, 18) != packed_size or _u32(data, 22) != plain_size
            or local_name_length != name_length
            or 30 + local_name_length + local_extra_length > central
            or data[30:30 + local_name_length] != name):
        raise _zip_refused("its file's header disagrees with its central directory.")
    whole, zip64 = _extra_is_whole_without_zip64(
        data[30 + local_name_length:30 + local_name_length + local_extra_length])
    if not whole or zip64:
        raise _zip_refused("its file's header disagrees with its central directory.")
    data_start = 30 + local_name_length + local_extra_length
    if data_start + packed_size != central:
        raise _zip_refused("its file's data does not end where its central directory starts.")
    packed = memoryview(data)[data_start:central]
    if method == 0:
        if packed_size != plain_size:
            raise _zip_refused("its stored file's sizes differ.")
        plain = bytes(packed)
    else:
        plain = _inflate(packed, plain_size)
    if len(plain) != plain_size or zlib.crc32(plain) != crc:
        raise _zip_refused("its file's size or CRC-32 does not match its header.")
    return plain


def _dat_text(data: bytes) -> bytes:
    """A datafile's own text (rules 1-2): the bytes given, or the ``.dat`` inside them when they
    are a zip."""
    if len(data) > MAX_DAT_BYTES:
        raise _too_large(_TOO_LARGE)
    return _dat_from_zip(data) if data.startswith(_ZIP_MAGIC) else data


def dat_from_bytes(data: bytes) -> RedumpDat:
    """Read a datafile, or the zip Redump serves, from its bytes: the whole format (rules 1-10).
    ``src/core/nk_disc_verify.c``'s nk_disc_dat_read is the same reader."""
    return parse_dat_bytes(_dat_text(data))


def load_dat(path: Path | str) -> RedumpDat:
    """Read a datafile (or the zip Redump serves) from disk, refusing one over ``MAX_DAT_BYTES``."""
    return dat_from_bytes(_read_file(path))


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
    data = _dat_text(_read_file(source))
    dat = parse_dat_bytes(data)
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
    """The tier of one image; ``known_retail_ids`` are disc IDs of verified images beside it.
    A disc ID is compared as nine bytes after ASCII upper-casing (the format's rule 9), as
    nk_disc_classify compares it."""
    if dat is None or digest is None:
        return NOT_CHECKED
    rom = dat.roms_by_sha1.get(digest.sha1.lower())
    if rom is not None and rom.crc32 == digest.crc32.lower() and rom.size_bytes == digest.size_bytes:
        return VERIFIED_RETAIL
    disc_id = _ascii_upper(disc_id or "")
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
        _ascii_upper(disc_id or "")
        for disc_id, digest in images.values()
        if disc_id and classify(disc_id, digest, dat) == VERIFIED_RETAIL
    }
    return {key: classify(disc_id, digest, dat, verified_ids)
            for key, (disc_id, digest) in images.items()}
