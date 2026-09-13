# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""Metadata, as ordinary Python dictionaries.

There is no ``Metadata`` class here, and that is the design. A manifest is a
handful of typed key/value pairs; a Python caller wants ``manifest["name"]`` and
``json.dumps(manifest)``, not an object with a lifetime rule attached to a
reader that may already be closed. So metadata is read *out* into a ``dict`` at
the boundary and written *in* from any mapping.

The type mapping is exact in both directions, with one asymmetry noted below:

===============  ==================
format           Python
===============  ==================
``string``       ``str``
``int``          ``int``
``float``        ``float``
``bool``         ``bool``
``bytes``        ``bytes``
``hash``         ``dict``
``array``        ``list`` (read-only)
===============  ==================
"""

from __future__ import annotations

import ctypes
import json
from typing import Any, Mapping

from . import _ffi
from .enums import ValueType
from .errors import SweepsError, raise_for_status


def to_json(lib, handle, indent: int = 2) -> str:
    """The whole tree as JSON, through the two-call idiom.

    Sized once with no buffer, then filled. Both calls render the document, so
    this is not free in a loop -- which is exactly why :func:`to_dict` uses the
    typed accessors instead and comes here only for arrays.
    """
    needed = ctypes.c_size_t(0)
    raise_for_status(lib, lib.sweeps_metadata_to_json(handle, indent, None, 0, ctypes.byref(needed)))

    buffer = ctypes.create_string_buffer(needed.value + 1)
    raise_for_status(
        lib,
        lib.sweeps_metadata_to_json(handle, indent, buffer, len(buffer), ctypes.byref(needed)),
    )
    return buffer.value.decode("utf-8", "replace")


def to_dict(lib, handle) -> dict[str, Any]:
    """Materialises a metadata object, nested hashes included.

    Copied rather than viewed: every string here would otherwise point into the
    reader that produced it, and a dict outlives its reader far too easily.
    """
    if not handle:
        return {}

    result: dict[str, Any] = {}
    arrays: list[str] = []

    count = lib.sweeps_metadata_count(handle)
    for index in range(count):
        key_str = _ffi.Str()
        value_type = ctypes.c_int(0)
        raise_for_status(
            lib,
            lib.sweeps_metadata_key_at(
                handle, index, ctypes.byref(key_str), ctypes.byref(value_type)
            ),
        )
        key = key_str.text()
        encoded = key.encode("utf-8")

        kind = value_type.value
        if kind == ValueType.STRING:
            result[key] = lib.sweeps_metadata_get_string(handle, encoded, None).text()
        elif kind == ValueType.INT:
            result[key] = int(lib.sweeps_metadata_get_i64(handle, encoded, 0))
        elif kind == ValueType.FLOAT:
            result[key] = float(lib.sweeps_metadata_get_f64(handle, encoded, 0.0))
        elif kind == ValueType.BOOL:
            result[key] = bool(lib.sweeps_metadata_get_bool(handle, encoded, 0))
        elif kind == ValueType.BYTES:
            payload = _ffi.Bytes()
            raise_for_status(
                lib, lib.sweeps_metadata_get_bytes(handle, encoded, ctypes.byref(payload))
            )
            result[key] = payload.copy()
        elif kind == ValueType.HASH:
            nested = ctypes.c_void_p()
            raise_for_status(
                lib, lib.sweeps_metadata_get_hash(handle, encoded, ctypes.byref(nested))
            )
            result[key] = to_dict(lib, nested)
        elif kind == ValueType.ARRAY:
            # The one type with no typed accessor: the C ABI cut array element
            # access deliberately, because a generic one would have to answer
            # for seven element types. Arrays are homogeneous scalars, so the
            # JSON rendering is lossless for them and is read once, below.
            arrays.append(key)
        else:
            result[key] = None

    if arrays:
        rendered = json.loads(to_json(lib, handle, indent=0))
        for key in arrays:
            result[key] = rendered.get(key)

    return result


class _Builder:
    """Owns a ``sweeps_metadata_t`` for the duration of a writer call."""

    def __init__(self, lib) -> None:
        self._lib = lib
        self.handle = lib.sweeps_metadata_create()
        if not self.handle:
            raise MemoryError("sweeps_metadata_create failed")

    def close(self) -> None:
        if self.handle:
            self._lib.sweeps_metadata_destroy(self.handle)
            self.handle = None

    def __enter__(self) -> "_Builder":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def set(self, key: str, value: Any) -> None:
        lib = self._lib
        encoded = key.encode("utf-8")

        # bool before int, and not merely for tidiness: bool *is* an int in
        # Python, so the obvious ordering would silently record every flag in a
        # manifest as 0 or 1 of type int, and the file would no longer say what
        # it meant.
        if isinstance(value, bool):
            raise_for_status(lib, lib.sweeps_metadata_set_bool(self.handle, encoded, int(value)))
        elif isinstance(value, int):
            raise_for_status(lib, lib.sweeps_metadata_set_i64(self.handle, encoded, value))
        elif isinstance(value, float):
            raise_for_status(lib, lib.sweeps_metadata_set_f64(self.handle, encoded, value))
        elif isinstance(value, str):
            raise_for_status(
                lib, lib.sweeps_metadata_set_string(self.handle, encoded, value.encode("utf-8"))
            )
        elif isinstance(value, (bytes, bytearray, memoryview)):
            raw = bytes(value)
            buffer = ctypes.create_string_buffer(raw, len(raw)) if raw else None
            raise_for_status(
                lib, lib.sweeps_metadata_set_bytes(self.handle, encoded, buffer, len(raw))
            )
        elif isinstance(value, Mapping):
            with _Builder(lib) as nested:
                nested.fill(value)
                raise_for_status(
                    lib, lib.sweeps_metadata_set_hash(self.handle, encoded, nested.handle)
                )
        elif isinstance(value, (list, tuple)):
            # Refused rather than coerced. `Value::ofArray` takes its element
            # type from the first element and replaces anything that does not
            # match with that type's default -- so a mixed list would be written
            # having quietly lost data. Encode it yourself and be explicit.
            raise SweepsError(
                0,
                f"key {key!r}: arrays cannot be written through this API. "
                "The format's arrays are homogeneous and coerce mismatched "
                "elements to a default; encode the list yourself (JSON in a "
                "string, or bytes) so the loss is your decision.",
            )
        else:
            raise TypeError(
                f"key {key!r}: cannot store {type(value).__name__} in .sweeps metadata"
            )

    def fill(self, mapping: Mapping[str, Any]) -> None:
        for key, value in mapping.items():
            if not isinstance(key, str):
                raise TypeError(f"metadata keys must be str, not {type(key).__name__}")
            self.set(key, value)
