# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""What a reader hands back.

All frozen dataclasses holding plain Python values, copied out of the library at
the moment they are read. Nothing here points into a reader, so nothing here
stops being true when one is closed -- which matters far more in Python than in
C, where objects escape their creating scope by default rather than by effort.
"""

from __future__ import annotations

import datetime as _datetime
from dataclasses import dataclass, field
from typing import Any

from .enums import WindowType

try:  # numpy is optional, and used only where it earns its place
    import numpy as _np
except ImportError:  # pragma: no cover - exercised by environments without numpy
    _np = None

#: dB per stored quantisation step. Fixed by the format.
DB_PER_STEP = 0.5

#: The stored byte meaning nothing was ever measured in that bin, and the dB
#: value it converts to. Measured levels are 1..255; coverage is not a level,
#: so a gap between two swept spans must not read as a quiet one.
UNMEASURED_BYTE = 0
UNMEASURED_DB = -200.0


def _utc(ns: int) -> _datetime.datetime | None:
    """Wall-clock nanoseconds to an aware UTC datetime, or None for "unset".

    Split into seconds and microseconds rather than dividing: a float seconds
    value loses the low digits of a nanosecond timestamp somewhere around 2001,
    and these are file timestamps people compare for equality.
    """
    if ns == 0:
        return None
    seconds, remainder = divmod(int(ns), 1_000_000_000)
    return _datetime.datetime.fromtimestamp(
        seconds, tz=_datetime.timezone.utc
    ) + _datetime.timedelta(microseconds=remainder // 1000)


@dataclass(frozen=True)
class Summary:
    """How a session file was opened."""

    major_version: int
    minor_version: int
    incompatible_features: int
    created_wall_ns: int
    total_lines: int
    total_tiles: int
    file_bytes: int
    first_line_ns: int
    last_line_ns: int
    #: Bytes past the last intact record. Non-zero for a session that ended
    #: abruptly; everything before that point was still read.
    truncated_bytes: int
    lowest_hz: float
    highest_hz: float
    #: The index was missing or unusable and the file was recovered by scanning.
    #: Surfaced rather than hidden: an operator should know a session ended
    #: abruptly.
    recovered_by_scan: bool
    #: The file's minor version is newer than this build knows. Not an error --
    #: the file was read and additive content was skipped.
    newer_minor_version: bool

    @property
    def format_version(self) -> tuple[int, int]:
        return (self.major_version, self.minor_version)

    @property
    def created(self) -> _datetime.datetime | None:
        return _utc(self.created_wall_ns)

    @property
    def duration_seconds(self) -> float:
        if self.last_line_ns <= self.first_line_ns:
            return 0.0
        return (self.last_line_ns - self.first_line_ns) * 1e-9


@dataclass(frozen=True)
class Segment:
    """One acquisition configuration's worth of session.

    Everything needed to interpret the tiles written under it, which is what
    makes a mid-session parameter change harmless: bins are just numbers unless
    you know the centre, span, sample rate, FFT size, window and gain that
    produced them.
    """

    #: The segment's identity, **not its position**. An extracted file contains
    #: segments whose ids neither start at zero nor run contiguously, which is
    #: why :meth:`Reader.segment_by_id` exists.
    id: int
    index: int

    bin_count: int
    start_hz: float
    bin_width_hz: float

    start_wall_ns: int
    start_monotonic_ns: int
    #: 0 while the segment is still open.
    end_monotonic_ns: int
    line_count: int

    fft_size: int
    window: WindowType
    center_hz: float
    span_hz: float
    sample_rate: float
    window_beta: float
    #: Equivalent noise bandwidth in bins, carried explicitly so a reader need
    #: not know how to regenerate the window to interpret the RBW.
    window_enbw: float
    overlap: float
    rbw_hz: float
    reference_level_dbm: float
    #: Correction from dBFS to dBm. Applied at display time rather than baked
    #: into the stored data, so a later calibration fix applies retroactively.
    dbfs_to_dbm_offset: float

    #: Why this segment was opened -- "session start", "sample rate changed".
    reason: str
    device_id: str
    device_label: str
    gains: dict[str, float] = field(default_factory=dict)

    @property
    def stop_hz(self) -> float:
        return self.start_hz + self.bin_width_hz * self.bin_count

    @property
    def start_wall(self) -> _datetime.datetime | None:
        return _utc(self.start_wall_ns)


@dataclass(frozen=True)
class Event:
    """One entry in the event stream.

    The events are what make replay *faithful*: frames alone reproduce the
    waterfall, the events reproduce everything around it, so the RBW, FFT size
    and gain readouts change at the same moments they did live.

    ``body`` is a dict whose keys are the same ones ``sweeps events --json``
    emits, decoded through the typed accessors rather than through JSON. A kind
    this build does not know keeps its bytes under ``"raw"``.
    """

    kind: int
    kind_name: str
    monotonic_ns: int
    wall_ns: int
    segment_id: int
    body: dict[str, Any]

    @property
    def wall(self) -> _datetime.datetime | None:
        return _utc(self.wall_ns)

    def __repr__(self) -> str:
        return f"Event({self.kind_name}, {self.monotonic_ns} ns, {self.body!r})"


@dataclass(frozen=True)
class Tile:
    """One returned tile, and its true extents.

    The extents are carried rather than assumed: the reader may have served a
    coarser pyramid level than asked for, and a query spanning a parameter
    change returns tiles from segments with different grids.
    """

    segment_id: int
    lod: int
    #: The tile's position in the segment's grid, carried rather than
    #: reconstructed -- reassembling full waterfall rows out of several
    #: frequency blocks needs both, and deriving them from floating-point
    #: frequencies is a rounding bug waiting to happen.
    time_block: int
    freq_block: int
    lines: int
    bins: int
    #: The dB value stored byte 0 represents. Per tile rather than per file, so
    #: a quiet band and a loud one each get the full 255-step range.
    origin_db: float
    first_line_ns: int
    last_line_ns: int
    start_hz: float
    bin_width_hz: float
    #: Quantised levels, row-major: ``lines`` rows of ``bins`` bytes.
    #: Byte-identical to a waterfall texture, so an upload is a memcpy.
    data: bytes

    @property
    def stop_hz(self) -> float:
        return self.start_hz + self.bin_width_hz * self.bins

    def db_at(self, line: int, bin_index: int) -> float:
        """The dB level of one cell, or ``UNMEASURED_DB`` where nothing was
        measured. Always available, numpy or not."""
        if not 0 <= line < self.lines or not 0 <= bin_index < self.bins:
            raise IndexError(f"({line}, {bin_index}) outside {self.lines}x{self.bins}")
        value = self.data[line * self.bins + bin_index]
        if value == UNMEASURED_BYTE:
            return UNMEASURED_DB
        return self.origin_db + value * DB_PER_STEP

    def levels(self):
        """The raw quantised bytes as a ``(lines, bins)`` uint8 array.

        Requires numpy. There is no list-of-lists fallback on purpose: a tile is
        up to a quarter of a million cells, and materialising that as Python
        ints would be slower and larger than the file it came from. Use
        :attr:`data` and :meth:`db_at` when numpy is not available.
        """
        if _np is None:
            raise RuntimeError("numpy is required for Tile.levels(); use .data or .db_at()")
        return _np.frombuffer(self.data, dtype=_np.uint8).reshape(self.lines, self.bins)

    def levels_db(self):
        """Dequantised levels as a ``(lines, bins)`` float32 array, with
        ``UNMEASURED_DB`` wherever nothing was measured. Requires numpy."""
        if _np is None:
            raise RuntimeError("numpy is required for Tile.levels_db(); use .db_at()")
        raw = self.levels()
        db = raw.astype(_np.float32) * DB_PER_STEP + _np.float32(self.origin_db)
        # Masked rather than left at the bottom of the scale: an unmeasured bin
        # plotted as a level invents a noise floor the radio never looked at.
        return _np.where(raw == UNMEASURED_BYTE, _np.float32(UNMEASURED_DB), db)


@dataclass(frozen=True)
class PluginRecord:
    """A producer's own record, opaque to the container.

    One record type serves every producer: ``plugin_id`` is reverse-DNS, which
    is what keeps two plugins from colliding without a registry.
    """

    plugin_id: str
    name: str
    schema_version: int
    #: 0 when the record is not tied to a moment, which is what keeps it in an
    #: extraction whatever range was asked for.
    monotonic_ns: int
    body: bytes
