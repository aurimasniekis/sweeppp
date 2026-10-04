# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""Reading a live ``.sweeps`` stream (Appendix C of the specification).

    >>> reader, mirror = sweepsfile.StreamReader(), sweepsfile.StreamMirror()
    >>> for chunk in iter(lambda: sys.stdin.buffer.read1(65536), b""):
    ...     reader.feed(chunk)
    ...     for record in reader.records():
    ...         mirror.apply(record)
    ...         if record.type == sweepsfile.RecordType.PLUGIN_DATA:
    ...             line = mirror.line()             # the producer's commit

An encrypted link is out of scope: decrypt it first and feed the plaintext.
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
from typing import Any, Iterator

from . import _ffi
from ._library import library
from ._types import Segment
from .enums import RecordType, Status, WindowType
from .errors import SweepsError, raise_for_status

try:
    import numpy as _np
except ImportError:  # pragma: no cover
    _np = None


@dataclass(frozen=True)
class StreamRecord:
    """One record off a stream, its payload copied out."""

    #: A :class:`RecordType`, or a plain int for a type this binding does not
    #: know -- skip those.
    type: RecordType | int
    payload: bytes


@dataclass(frozen=True)
class StreamLine:
    """The current line, copied out of the mirror."""

    segment_id: int
    #: The sender's line number: the ``timeBlock`` of the latest tile applied.
    line: int
    #: Tiles applied since the segment opened.
    tiles_applied: int
    start_hz: float
    bin_width_hz: float
    #: dB per bin, ``UNMEASURED_DB`` where no tile has carried it yet. A float32
    #: numpy array when numpy is available, a list of floats otherwise.
    levels: Any

    @property
    def bin_count(self) -> int:
        return len(self.levels)


class StreamReader:
    """Splits the bytes of a stream into records, however they arrive.

    The 16-byte stream header is checked as it arrives. A bad header, a
    checksum mismatch or an oversized record breaks the stream for good, and
    every later call raises the same error.
    """

    def __init__(self, max_payload_bytes: int = 0) -> None:
        """``max_payload_bytes`` bounds every record; 0 means 16 MiB."""
        self._lib = library()
        handle = ctypes.c_void_p()
        raise_for_status(
            self._lib, self._lib.sweeps_stream_reader_create(max_payload_bytes, ctypes.byref(handle))
        )
        self._handle: ctypes.c_void_p | None = handle

    def feed(self, data: bytes | bytearray | memoryview) -> None:
        view = bytes(data)
        raise_for_status(
            self._lib, self._lib.sweeps_stream_reader_feed(self._live, view, len(view))
        )

    def records(self) -> Iterator[StreamRecord]:
        """Every complete record fed so far, then stops until more is fed."""
        raw = _ffi.StreamRecord()
        has_record = ctypes.c_int(0)
        while True:
            raw.struct_size = ctypes.sizeof(raw)
            raise_for_status(
                self._lib,
                self._lib.sweeps_stream_reader_next_record(
                    self._live, ctypes.byref(raw), ctypes.byref(has_record)
                ),
            )
            if not has_record.value:
                return
            try:
                kind: RecordType | int = RecordType(raw.type)
            except ValueError:
                kind = raw.type
            yield StreamRecord(type=kind, payload=raw.payload.copy())

    @property
    def buffered(self) -> int:
        """Bytes fed and not yet returned as a record. Non-zero once the input
        has ended means it ended mid-record."""
        return self._lib.sweeps_stream_reader_buffered(self._live)

    def close(self) -> None:
        if self._handle is not None:
            self._lib.sweeps_stream_reader_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "StreamReader":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:  # pragma: no cover - interpreter teardown
        try:
            self.close()
        except Exception:  # noqa: BLE001
            pass

    @property
    def _live(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise SweepsError(0, "this stream reader is closed")
        return self._handle


class StreamMirror:
    """The current line of a stream, rebuilt from its SegmentOpen, Tile and
    SegmentClose records. Every other record is ignored."""

    def __init__(self, max_bins: int = 0) -> None:
        """A segment wider than ``max_bins`` is refused; 0 means 16 Mi bins."""
        self._lib = library()
        handle = ctypes.c_void_p()
        raise_for_status(
            self._lib, self._lib.sweeps_stream_mirror_create(max_bins, ctypes.byref(handle))
        )
        self._handle: ctypes.c_void_p | None = handle

    def apply(self, record: StreamRecord) -> None:
        """Raises for a segment or tile that does not fit, and changes nothing."""
        payload = bytes(record.payload)
        buffer = ctypes.create_string_buffer(payload, max(len(payload), 1))
        raw = _ffi.StreamRecord()
        raw.struct_size = ctypes.sizeof(raw)
        raw.type = int(record.type)
        raw.payload.data = ctypes.cast(buffer, ctypes.POINTER(ctypes.c_uint8))
        raw.payload.len = len(payload)
        raise_for_status(
            self._lib, self._lib.sweeps_stream_mirror_apply(self._live, ctypes.byref(raw))
        )

    def line(self) -> StreamLine | None:
        """The current line, or None before the first segment."""
        raw = _ffi.StreamLine()
        raw.struct_size = ctypes.sizeof(raw)
        status = self._lib.sweeps_stream_mirror_line(self._live, ctypes.byref(raw))
        if status == Status.NOT_FOUND:
            return None
        raise_for_status(self._lib, status)

        count = raw.bin_count
        copied = (ctypes.c_float * count)()
        ctypes.memmove(copied, raw.levels, count * ctypes.sizeof(ctypes.c_float))
        levels = _np.frombuffer(copied, dtype=_np.float32).copy() if _np is not None else list(copied)
        return StreamLine(
            segment_id=raw.segment_id,
            line=raw.line,
            tiles_applied=raw.tiles_applied,
            start_hz=raw.start_hz,
            bin_width_hz=raw.bin_width_hz,
            levels=levels,
        )

    def segment(self) -> Segment | None:
        """The open segment, or None before the first. ``index`` is 0: a
        stream has one segment at a time."""
        lib = self._lib
        handle = self._live
        raw = _ffi.Segment()
        raw.struct_size = ctypes.sizeof(raw)
        status = lib.sweeps_stream_mirror_segment(handle, ctypes.byref(raw))
        if status == Status.NOT_FOUND:
            return None
        raise_for_status(lib, status)

        gains: dict[str, float] = {}
        for index in range(raw.gain_count):
            name = _ffi.Str()
            value = ctypes.c_double(0.0)
            raise_for_status(
                lib,
                lib.sweeps_stream_mirror_segment_gain(
                    handle, index, ctypes.byref(name), ctypes.byref(value)
                ),
            )
            gains[name.text()] = value.value

        try:
            window: WindowType | int = WindowType(raw.window)
        except ValueError:
            window = raw.window

        return Segment(
            id=raw.id,
            index=0,
            bin_count=raw.bin_count,
            start_hz=raw.start_hz,
            bin_width_hz=raw.bin_width_hz,
            start_wall_ns=raw.start_wall_ns,
            start_monotonic_ns=raw.start_monotonic_ns,
            end_monotonic_ns=raw.end_monotonic_ns,
            line_count=raw.line_count,
            fft_size=raw.fft_size,
            window=window,
            center_hz=raw.center_hz,
            span_hz=raw.span_hz,
            sample_rate=raw.sample_rate,
            window_beta=raw.window_beta,
            window_enbw=raw.window_enbw,
            overlap=raw.overlap,
            rbw_hz=raw.rbw_hz,
            reference_level_dbm=raw.reference_level_dbm,
            dbfs_to_dbm_offset=raw.dbfs_to_dbm_offset,
            reason=lib.sweeps_stream_mirror_segment_reason(handle).text(),
            device_id=lib.sweeps_stream_mirror_segment_device_id(handle).text(),
            device_label=lib.sweeps_stream_mirror_segment_device_label(handle).text(),
            gains=gains,
        )

    def close(self) -> None:
        if self._handle is not None:
            self._lib.sweeps_stream_mirror_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "StreamMirror":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:  # pragma: no cover - interpreter teardown
        try:
            self.close()
        except Exception:  # noqa: BLE001
            pass

    @property
    def _live(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise SweepsError(0, "this stream mirror is closed")
        return self._handle
