# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The one JSON Schema subset validator the repository's tools share.

``tools/flight_diff.py`` (the flight-recorder bundle schema) and ``tools/nk_cli.py`` (the bring-up report
schema) each carried its own hand-written validator with a different keyword set and a different reading
of ``pattern``. This module replaces both. It is not a general JSON Schema implementation: it checks the
keywords listed below with the semantics of JSON Schema 2020-12, and it refuses any other keyword by name
when the validator is built, so a schema never claims a constraint this module does not enforce.

Supported keywords

* Type and value: ``type`` (one name or a list), ``const``, ``enum``.
* Strings: ``minLength``, ``maxLength``, ``pattern``.
* Numbers: ``minimum``, ``maximum``.
* Arrays: ``items`` (one schema), ``minItems``, ``maxItems``, ``uniqueItems``.
* Objects: ``properties``, ``required``, ``additionalProperties`` (a boolean or a schema),
  ``propertyNames``, ``minProperties``.
* Combinators: ``allOf``, ``not``, ``if`` with ``then`` and ``else``.
* References: ``$ref`` to a same-document pointer (``#`` or ``#/...``), and ``$defs`` / ``definitions`` as
  its targets. A ``$ref`` applies together with its sibling keywords (2020-12).
* Boolean schemas: ``true`` accepts everything, ``false`` accepts nothing.
* Annotations, ignored on purpose because they assert nothing: ``$schema``, ``$id``, ``$comment``,
  ``title``, ``description``, ``default``, ``examples``.

Any other keyword (``oneOf``, ``anyOf``, ``patternProperties``, ``format``, ``exclusiveMinimum``,
``multipleOf``, a tuple-form ``items`` and so on) raises ``UnsupportedKeywordError`` when the validator is
built, before any value is checked.

Semantics worth knowing

* ``pattern`` is an unanchored search, as in the specification: ``"^[a-z]+"`` accepts ``"abc!"`` and a
  pattern that must cover the whole value anchors itself with ``^`` and ``$``. The pattern dialect is
  Python ``re`` with two adjustments that bring it to the ECMA-262 reading the specification names:
  ``$`` matches only at the end of the string (Python's ``$`` also matches before a trailing newline, so
  ``"^[a-f0-9]+$"`` would accept ``"abc\\n"``), and ``\\d``, ``\\w`` and ``\\b`` are ASCII only. Other
  dialect differences are not translated; write patterns in the common subset.
* ``integer`` accepts only a Python ``int`` (a JSON ``1.0`` is refused: this validator fails closed where
  the specification would accept it), and ``true`` / ``false`` are never numbers.
* ``const``, ``enum`` and ``uniqueItems`` compare JSON values: ``true`` is not ``1``, ``1`` equals ``1.0``,
  and objects and arrays compare by content.
* ``additionalProperties`` and ``propertyNames`` look at this schema's ``properties`` only
  (``patternProperties`` is not supported).
* ``then`` and ``else`` without ``if`` are ignored, as the specification says.
"""

from __future__ import annotations

import re
from typing import Any
from urllib.parse import unquote

#: Keywords that assert nothing; accepted and ignored.
ANNOTATION_KEYWORDS = frozenset({
    "$schema", "$id", "$comment", "title", "description", "default", "examples",
})
#: Keywords this module enforces (``$defs`` / ``definitions`` only hold ``$ref`` targets).
ASSERTION_KEYWORDS = frozenset({
    "type", "const", "enum",
    "minLength", "maxLength", "pattern",
    "minimum", "maximum",
    "items", "minItems", "maxItems", "uniqueItems",
    "properties", "required", "additionalProperties", "propertyNames", "minProperties",
    "allOf", "not", "if", "then", "else",
    "$ref", "$defs", "definitions",
})
SUPPORTED_KEYWORDS = ANNOTATION_KEYWORDS | ASSERTION_KEYWORDS

_TYPE_NAMES = frozenset({"object", "array", "string", "boolean", "integer", "number", "null"})
_SCHEMA_MAP_KEYWORDS = ("properties", "$defs", "definitions")
_SCHEMA_LIST_KEYWORDS = ("allOf",)
_SCHEMA_KEYWORDS = ("items", "additionalProperties", "propertyNames", "not", "if", "then", "else")
_COUNT_KEYWORDS = ("minLength", "maxLength", "minItems", "maxItems", "minProperties")


class SchemaDefinitionError(Exception):
    """The schema itself is unusable. Deliberately not a ``ValueError``: a caller that treats a
    ``ValueError`` as "this value is invalid" must never swallow a broken schema."""


class UnsupportedKeywordError(SchemaDefinitionError):
    """The schema uses a keyword this module does not enforce."""

    def __init__(self, pointer: str, keyword: str) -> None:
        super().__init__(f"{pointer}: unsupported JSON Schema keyword {keyword!r}; "
                         "tools/json_schema_subset.py lists the keywords it enforces")
        self.pointer = pointer
        self.keyword = keyword


class SchemaViolation(ValueError):
    """A value does not match the schema; ``path`` is where, ``reason`` is why."""

    def __init__(self, path: str, reason: str) -> None:
        super().__init__(f"{path}: {reason}")
        self.path = path
        self.reason = reason


def _fail(path: str, reason: str) -> SchemaViolation:
    return SchemaViolation(path, reason)


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _canonical(value: Any) -> Any:
    """A hashable form in which two JSON values are equal exactly when JSON says they are."""
    if isinstance(value, bool):
        return ("boolean", value)
    if isinstance(value, (int, float)):
        return ("number", value)  # 1 == 1.0 and hash(1) == hash(1.0)
    if isinstance(value, list):
        return ("array", tuple(_canonical(item) for item in value))
    if isinstance(value, dict):
        return ("object", frozenset((key, _canonical(item)) for key, item in value.items()))
    return ("scalar", value)


def _json_equal(left: Any, right: Any) -> bool:
    return bool(_canonical(left) == _canonical(right))


def compile_pattern(pattern: str) -> re.Pattern[str]:
    """Compile ``pattern`` with the dialect adjustments the module docstring describes."""
    translated: list[str] = []
    in_class = False
    index = 0
    while index < len(pattern):
        char = pattern[index]
        if char == "\\":
            translated.append(pattern[index:index + 2])
            index += 2
            continue
        if in_class:
            if char == "]":
                in_class = False
        elif char == "[":
            in_class = True
        elif char == "$":
            translated.append(r"\Z")
            index += 1
            continue
        translated.append(char)
        index += 1
    try:
        return re.compile("".join(translated), re.ASCII)
    except re.error as exc:
        raise SchemaDefinitionError(f"pattern {pattern!r} does not compile: {exc}") from exc


def _is_schema(node: Any) -> bool:
    return isinstance(node, (dict, bool))


def _resolve_pointer(root: Any, reference: str, where: str) -> Any:
    if reference == "#":
        return root
    if not reference.startswith("#/"):
        raise SchemaDefinitionError(f"{where}: only same-document references are supported, got {reference!r}")
    node = root
    for part in reference[2:].split("/"):
        part = unquote(part).replace("~1", "/").replace("~0", "~")
        if isinstance(node, dict) and part in node:
            node = node[part]
        elif isinstance(node, list) and part.isdecimal() and int(part) < len(node):
            node = node[int(part)]
        else:
            raise SchemaDefinitionError(f"{where}: reference {reference!r} does not resolve")
    return node


class SchemaValidator:
    """A schema checked once, then used for many values.

    Building the validator walks the whole schema, so an unsupported keyword is refused even in a branch
    that no value reaches. ``validate`` raises ``SchemaViolation`` (a ``ValueError``) for a bad value.
    """

    def __init__(self, schema: Any) -> None:
        if not _is_schema(schema):
            raise SchemaDefinitionError("a schema is a JSON object or a boolean")
        self.root = schema
        self._patterns: dict[str, re.Pattern[str]] = {}
        self._check(schema, "#")

    # -- schema checking --------------------------------------------------------------------------------
    def _check(self, schema: Any, pointer: str) -> None:
        if isinstance(schema, bool):
            return
        if not isinstance(schema, dict):
            raise SchemaDefinitionError(f"{pointer}: a schema is a JSON object or a boolean")
        for keyword in schema:
            if keyword not in SUPPORTED_KEYWORDS:
                raise UnsupportedKeywordError(pointer, keyword)
        self._check_keyword_values(schema, pointer)
        for keyword in _SCHEMA_MAP_KEYWORDS:
            if keyword in schema:
                children = schema[keyword]
                if not isinstance(children, dict):
                    raise SchemaDefinitionError(f"{pointer}/{keyword}: expected an object of schemas")
                for name, child in children.items():
                    self._check(child, f"{pointer}/{keyword}/{name}")
        for keyword in _SCHEMA_LIST_KEYWORDS:
            if keyword in schema:
                children = schema[keyword]
                if not isinstance(children, list) or not children:
                    raise SchemaDefinitionError(f"{pointer}/{keyword}: expected a non-empty list of schemas")
                for position, child in enumerate(children):
                    self._check(child, f"{pointer}/{keyword}/{position}")
        for keyword in _SCHEMA_KEYWORDS:
            if keyword in schema:
                self._check(schema[keyword], f"{pointer}/{keyword}")
        if "$ref" in schema:
            self._check_reference(schema["$ref"], pointer)

    def _check_reference(self, reference: Any, pointer: str) -> None:
        if not isinstance(reference, str):
            raise SchemaDefinitionError(f"{pointer}/$ref: expected a string")
        seen: set[str] = set()
        while True:
            if reference in seen:
                raise SchemaDefinitionError(f"{pointer}/$ref: reference cycle through {reference!r}")
            seen.add(reference)
            target = _resolve_pointer(self.root, reference, f"{pointer}/$ref")
            if not (isinstance(target, dict) and isinstance(target.get("$ref"), str)):
                if not _is_schema(target):
                    raise SchemaDefinitionError(f"{pointer}/$ref: {reference!r} is not a schema")
                return
            reference = target["$ref"]

    def _check_keyword_values(self, schema: dict[str, Any], pointer: str) -> None:
        def bad(keyword: str, expected: str) -> SchemaDefinitionError:
            return SchemaDefinitionError(f"{pointer}/{keyword}: expected {expected}")

        if "type" in schema:
            names = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
            if not names or any(not isinstance(name, str) or name not in _TYPE_NAMES for name in names):
                raise bad("type", "a JSON type name or a list of them")
        if "enum" in schema and not isinstance(schema["enum"], list):
            raise bad("enum", "a list")
        for keyword in _COUNT_KEYWORDS:
            if keyword in schema and not (_is_int(schema[keyword]) and schema[keyword] >= 0):
                raise bad(keyword, "a non-negative integer")
        for keyword in ("minimum", "maximum"):
            if keyword in schema and not _is_number(schema[keyword]):
                raise bad(keyword, "a number")
        if "pattern" in schema:
            if not isinstance(schema["pattern"], str):
                raise bad("pattern", "a string")
            self._pattern(schema["pattern"])
        if "required" in schema and not (
                isinstance(schema["required"], list) and all(isinstance(name, str) for name in schema["required"])):
            raise bad("required", "a list of property names")
        if "uniqueItems" in schema and not isinstance(schema["uniqueItems"], bool):
            raise bad("uniqueItems", "a boolean")
        if "items" in schema and not _is_schema(schema["items"]):
            raise bad("items", "one schema (tuple-form items is not supported)")

    def _pattern(self, pattern: str) -> re.Pattern[str]:
        compiled = self._patterns.get(pattern)
        if compiled is None:
            compiled = self._patterns[pattern] = compile_pattern(pattern)
        return compiled

    # -- validation -------------------------------------------------------------------------------------
    def validate(self, value: Any, path: str = "$", schema: Any = None) -> None:
        """Check ``value`` against the root schema, or against ``schema``, a sub-schema of the root."""
        self._validate(value, self.root if schema is None else schema, path)

    def _matches(self, value: Any, schema: Any, path: str) -> bool:
        try:
            self._validate(value, schema, path)
        except SchemaViolation:
            return False
        return True

    def _validate(self, value: Any, schema: Any, path: str) -> None:
        if schema is True:
            return
        if schema is False:
            raise _fail(path, "no value is allowed here")

        if "$ref" in schema:
            target = _resolve_pointer(self.root, schema["$ref"], path)
            self._validate(value, target, path)
        if "const" in schema and not _json_equal(value, schema["const"]):
            raise _fail(path, f"expected {schema['const']!r}")
        if "enum" in schema and not any(_json_equal(value, member) for member in schema["enum"]):
            raise _fail(path, f"expected one of {schema['enum']!r}")
        if "type" in schema:
            expected = schema["type"]
            if not any(self._has_type(value, name) for name in (expected if isinstance(expected, list) else [expected])):
                raise _fail(path, f"expected {expected!r}")

        if isinstance(value, str):
            self._validate_string(value, schema, path)
        elif _is_number(value):
            if "minimum" in schema and value < schema["minimum"]:
                raise _fail(path, "number is below its minimum")
            if "maximum" in schema and value > schema["maximum"]:
                raise _fail(path, "number is above its maximum")
        elif isinstance(value, list):
            self._validate_array(value, schema, path)
        elif isinstance(value, dict):
            self._validate_object(value, schema, path)

        for child in schema.get("allOf", ()):
            self._validate(value, child, path)
        if "not" in schema and self._matches(value, schema["not"], path):
            raise _fail(path, "value matches a forbidden schema")
        if "if" in schema:
            branch = schema.get("then") if self._matches(value, schema["if"], path) else schema.get("else")
            if branch is not None:
                self._validate(value, branch, path)

    @staticmethod
    def _has_type(value: Any, name: str) -> bool:
        if name == "object":
            return isinstance(value, dict)
        if name == "array":
            return isinstance(value, list)
        if name == "string":
            return isinstance(value, str)
        if name == "boolean":
            return isinstance(value, bool)
        if name == "integer":
            return _is_int(value)
        if name == "number":
            return _is_number(value)
        return value is None  # "null"; every other name was refused when the validator was built

    def _validate_string(self, value: str, schema: dict[str, Any], path: str) -> None:
        if "minLength" in schema and len(value) < schema["minLength"]:
            raise _fail(path, "string is too short")
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            raise _fail(path, "string is too long")
        if "pattern" in schema and self._pattern(schema["pattern"]).search(value) is None:
            raise _fail(path, "string does not match its schema pattern")

    def _validate_array(self, value: list[Any], schema: dict[str, Any], path: str) -> None:
        if "minItems" in schema and len(value) < schema["minItems"]:
            raise _fail(path, "array is too short")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            raise _fail(path, "array is too long")
        if schema.get("uniqueItems"):
            canonical = [_canonical(item) for item in value]
            if len(canonical) != len(set(canonical)):
                raise _fail(path, "array items are not unique")
        if "items" in schema:
            for index, item in enumerate(value):
                self._validate(item, schema["items"], f"{path}[{index}]")

    def _validate_object(self, value: dict[str, Any], schema: dict[str, Any], path: str) -> None:
        if len(value) < schema.get("minProperties", 0):
            raise _fail(path, f"fewer than {schema['minProperties']} properties")
        for key in schema.get("required", ()):
            if key not in value:
                raise _fail(path, f"missing required property {key!r}")
        properties = schema.get("properties", {})
        additional = schema.get("additionalProperties", True)
        for key, item in value.items():
            if "propertyNames" in schema:
                self._validate(key, schema["propertyNames"], f"{path} property name {key!r}")
            if key in properties:
                self._validate(item, properties[key], f"{path}.{key}")
            elif additional is False:
                raise _fail(path, f"property {key!r} is not allowed")
            elif additional is not True:
                self._validate(item, additional, f"{path}.{key}")


def validate(value: Any, schema: Any, path: str = "$") -> None:
    """Build a validator for ``schema`` and check ``value``; use ``SchemaValidator`` to reuse the build."""
    SchemaValidator(schema).validate(value, path)
