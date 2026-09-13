# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""The format's enumerations.

Mirrored from ``sweeps/sweeps.h``, which mirrors the byte format. These numbers
are on disk and frozen by the specification, so they are transcribed rather than
renumbered into something more Pythonic -- a second numbering would be a second
thing to keep in step.

``IntEnum`` rather than ``Enum`` so a value the format gains before this file
does still compares and prints as a number instead of raising.
"""

from __future__ import annotations

import enum


class Status(enum.IntEnum):
    """``sweeps_status_t``. Surfaced on :class:`~sweepsfile.errors.SweepsError`."""

    OK = 0
    UNKNOWN = -1
    INVALID_ARGUMENT = -2
    NOT_FOUND = -3
    UNSUPPORTED = -4
    UNAVAILABLE = -5
    IO = -6
    PARSE = -7
    OUT_OF_RANGE = -8
    OUT_OF_MEMORY = -9
    TIMED_OUT = -10
    CANCELLED = -11
    PERMISSION_DENIED = -12
    ALREADY_EXISTS = -13
    DEVICE = -14
    PROTOCOL = -15
    CORRUPT = -16
    WRONG_TYPE = -17


class WindowType(enum.IntEnum):
    """The FFT window a recording was taken with. Specification §4.3.1."""

    RECTANGULAR = 0
    HANN = 1
    HAMMING = 2
    BLACKMAN_HARRIS = 3
    FLAT_TOP = 4
    KAISER = 5


class EventKind(enum.IntEnum):
    """Event kinds.

    ``ALERT`` is reserved: it is named, nothing writes it, and its body reads as
    an unknown one. A kind this build does not know keeps its raw number, which
    is why events expose ``kind`` as a plain ``int`` and this enum is only for
    comparing against.
    """

    RETUNE = 1
    PARAMETER_CHANGED = 2
    SWEEP_PASS = 3
    MARKER = 4
    ANNOTATION = 5
    ALERT = 6
    SEGMENT_BOUNDARY = 7
    THROTTLE_CHANGED = 8
    DEVICE_ERROR = 9
    PLUGIN = 10


class RecordType(enum.IntEnum):
    """Record kinds in the chunk stream."""

    MANIFEST = 1
    SEGMENT_OPEN = 2
    SEGMENT_CLOSE = 3
    EVENT = 4
    TILE = 5
    INDEX = 6
    END_OF_STREAM = 7
    TELEMETRY = 8
    PLUGIN_DATA = 9


class ValueType(enum.IntEnum):
    """Metadata value types. Specification §4.2; the numbering is on the wire.

    ``ABSENT`` is not a wire tag -- it is what a lookup reports for a key that is
    not there, which the format keeps distinct from a present-but-empty value.
    """

    ABSENT = 0
    STRING = 1
    INT = 2
    FLOAT = 3
    BOOL = 4
    BYTES = 5
    HASH = 6
    ARRAY = 7


class LogLevel(enum.IntEnum):
    """Levels the library logs at."""

    TRACE = 0
    DEBUG = 1
    INFO = 2
    WARN = 3
    ERROR = 4
