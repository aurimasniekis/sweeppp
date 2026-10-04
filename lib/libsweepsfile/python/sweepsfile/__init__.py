# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""Read and write ``.sweeps`` spectrum session files from Python.

A ctypes binding over ``libsweepsfile``'s C ABI. No compiler, no build step, no
extension module, and no dependencies -- numpy is used where it earns its place
and is not required.

    >>> import sweepsfile
    >>> with sweepsfile.open("session.sweeps") as session:
    ...     print(session.name, session.summary.duration_seconds)
    ...     for tile in session.query(from_hz=88e6, to_hz=108e6, max_lines=1024):
    ...         levels = tile.levels_db()          # (lines, bins) float32, numpy

It needs a **shared** ``libsweepsfile`` -- ctypes has nothing to link against in
a static archive. Build the library with ``-DBUILD_SHARED_LIBS=ON`` and, if it
is not on the loader path, point ``SWEEPSFILE_LIBRARY`` at it or call
:func:`load_library`.

The byte format is specified in ``sweeps-format-v1.md``, in enough detail to
write an implementation without consulting any of this.
"""

from __future__ import annotations

import ctypes as _ctypes

from ._ffi import (
    ABI_VERSION,
    DB_PER_STEP,
    LOD_LEVELS,
    TILE_BINS,
    TILE_LINES,
    UNMEASURED_BYTE,
    UNMEASURED_DB,
    LibraryNotFound,
)
from ._library import library, load_library
from ._types import Event, PluginRecord, Segment, Summary, Tile
from .enums import EventKind, LogLevel, RecordType, Status, ValueType, WindowType
from .errors import (
    CorruptError,
    InvalidArgumentError,
    IoError,
    NotFoundError,
    OutOfRangeError,
    ParseError,
    SweepsError,
    UnsupportedError,
    WrongTypeError,
)
from .reader import Reader, open  # noqa: A004 - deliberate, as gzip.open is
from .stream import StreamLine, StreamMirror, StreamReader, StreamRecord
from .writer import DEFAULT_MIN_FREE_BYTES, AcquisitionConfig, FrameOutcome, Writer, create

__version__ = "1.0.0"

__all__ = [
    # entry points
    "open",
    "create",
    "Reader",
    "Writer",
    "StreamReader",
    "StreamMirror",
    # values
    "AcquisitionConfig",
    "Event",
    "FrameOutcome",
    "PluginRecord",
    "Segment",
    "StreamLine",
    "StreamRecord",
    "Summary",
    "Tile",
    # enumerations
    "EventKind",
    "LogLevel",
    "RecordType",
    "Status",
    "ValueType",
    "WindowType",
    # errors
    "SweepsError",
    "CorruptError",
    "InvalidArgumentError",
    "IoError",
    "LibraryNotFound",
    "NotFoundError",
    "OutOfRangeError",
    "ParseError",
    "UnsupportedError",
    "WrongTypeError",
    # library
    "load_library",
    "library",
    "abi_version",
    "library_version",
    "format_version",
    # format constants and helpers
    "ABI_VERSION",
    "DB_PER_STEP",
    "LOD_LEVELS",
    "TILE_BINS",
    "TILE_LINES",
    "UNMEASURED_BYTE",
    "UNMEASURED_DB",
    "DEFAULT_MIN_FREE_BYTES",
    "dequantise_db",
    "quantise_db",
    "event_kind_name",
    "window_type_name",
    "__version__",
]


def abi_version() -> int:
    """The ABI the loaded library implements. Always :data:`ABI_VERSION`, or the
    library would not have loaded."""
    return library().sweeps_abi_version()


def library_version() -> str:
    """The loaded library's own release, e.g. ``"1.0.0"``.

    Not the same number as the ABI version and not derived from it: this moves
    whenever anything ships, the ABI version only when the contract changes.
    """
    return library().sweeps_library_version().text()


def format_version() -> tuple[int, int]:
    """The container version the loaded library writes."""
    major = _ctypes.c_uint32(0)
    minor = _ctypes.c_uint32(0)
    library().sweeps_format_version(_ctypes.byref(major), _ctypes.byref(minor))
    return (major.value, minor.value)


def dequantise_db(value: int, origin_db: float) -> float:
    """A stored byte to dB, relative to a tile's own origin.

    Here rather than left to the caller because getting it wrong is silent: the
    levels come out plausible and forty decibels off.

    ``UNMEASURED_BYTE`` converts to ``UNMEASURED_DB`` whatever the origin.
    """
    return library().sweeps_dequantise_db(value, origin_db)


def quantise_db(db: float, origin_db: float) -> int:
    """dB to a stored byte.

    Saturates at both ends rather than wrapping, and yields ``UNMEASURED_BYTE``
    for anything that is not a reading: the unmeasured sentinel, a value below
    -190 dB, or a non-finite one.
    """
    return library().sweeps_quantise_db(db, origin_db)


def window_type_name(window: WindowType | int) -> str:
    """The canonical spelling -- ``"hann"``, ``"blackman-harris"``.

    This one reaches file bytes, in a segment's reason string.
    """
    return library().sweeps_window_type_name(int(window)).text()


def event_kind_name(kind: EventKind | int) -> str:
    """``"retune"``, ``"plugin"``, or ``"unknown"`` for a kind this build does
    not know."""
    return library().sweeps_event_kind_name(int(kind)).text()
