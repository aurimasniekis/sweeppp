# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""Writing ``.sweeps`` files.

The writer is **synchronous** and owns no thread, queue or lock: every call
writes on the calling thread. A recorder that must not stall acquisition needs a
queue, but the shape of that queue is an application's decision, and building
one in would put threading in the one library that has no business containing
any. The same applies here.
"""

from __future__ import annotations

import ctypes
import os
from dataclasses import dataclass, field
from typing import Any, Mapping, Sequence

from . import _ffi, _metadata
from ._library import library
from .enums import LogLevel, WindowType
from .errors import SweepsError, raise_for_status
from .reader import LogSink

try:
    import numpy as _np
except ImportError:  # pragma: no cover
    _np = None

#: The C++ default: stop if the filesystem drops below half a gigabyte.
DEFAULT_MIN_FREE_BYTES = 512 * 1024 * 1024


@dataclass
class AcquisitionConfig:
    """Everything that produced a measurement.

    **This is the structure the format's forward compatibility rests on.**
    Without it a recording is uninterpretable: bins are just numbers unless you
    know the centre, span, sample rate, FFT size, window and gain that produced
    them. Every segment records one in full, so tiles written under any past
    configuration stay interpretable without reference to anything outside the
    file.

    The defaults below are the library's own, not zeros. That distinction is
    load-bearing in C, where zeroing this struct claims a rectangular window and
    an ENBW of zero and writes both into the file as fact; here the dataclass
    makes it impossible to do by accident.
    """

    center_hz: float = 0.0
    span_hz: float = 0.0
    sample_rate: float = 0.0
    fft_size: int = 0
    window: WindowType = WindowType.HANN
    window_beta: float = 8.6
    #: Equivalent noise bandwidth in bins, carried explicitly so a reader need
    #: not know how to regenerate the window to interpret the RBW.
    window_enbw: float = 1.5
    overlap: float = 0.0
    #: ``sample_rate * window_enbw / fft_size``.
    rbw_hz: float = 0.0
    reference_level_dbm: float = 0.0
    dbfs_to_dbm_offset: float = 0.0
    device_id: str = ""
    device_label: str = ""
    #: Gain stages by parameter key. Free-form, because it must describe any
    #: device without the format knowing which radios exist.
    gains: dict[str, float] = field(default_factory=dict)


@dataclass(frozen=True)
class FrameOutcome:
    """What one frame did to the file."""

    segment_id: int
    #: This frame's configuration or grid differed from the open segment's, so
    #: a new one was opened.
    segment_opened: bool
    #: A retention cap has stopped the writer. Further frames are accepted and
    #: ignored.
    retention_stopped: bool
    #: Why the segment opened, in the words written into the file. Empty unless
    #: ``segment_opened``.
    reason: str


class _Held:
    """Keeps the Python objects a C struct points into alive for a call.

    ctypes does not retain what a ``c_char_p`` field was assigned from, so
    without this the encoded strings would be collected while the library still
    held pointers to them -- intermittently, and only under memory pressure.
    """

    __slots__ = ("_refs",)

    def __init__(self) -> None:
        self._refs: list[Any] = []

    def text(self, value: str | None) -> bytes | None:
        if value is None or value == "":
            return None
        encoded = value.encode("utf-8")
        self._refs.append(encoded)
        return encoded

    def keep(self, obj: Any) -> Any:
        self._refs.append(obj)
        return obj


def _to_acq_config(lib, config: AcquisitionConfig, held: _Held) -> _ffi.AcqConfig:
    raw = _ffi.AcqConfig()
    lib.sweeps_acq_config_init(ctypes.byref(raw))

    raw.center_hz = config.center_hz
    raw.span_hz = config.span_hz
    raw.sample_rate = config.sample_rate
    raw.fft_size = config.fft_size
    raw.window = int(config.window)
    raw.window_beta = config.window_beta
    raw.window_enbw = config.window_enbw
    raw.overlap = config.overlap
    raw.rbw_hz = config.rbw_hz
    raw.reference_level_dbm = config.reference_level_dbm
    raw.dbfs_to_dbm_offset = config.dbfs_to_dbm_offset
    raw.device_id = held.text(config.device_id)
    raw.device_label = held.text(config.device_label)

    if config.gains:
        gains = (_ffi.Gain * len(config.gains))()
        for index, (name, value) in enumerate(config.gains.items()):
            gains[index].name = held.text(name)
            gains[index].value = float(value)
        held.keep(gains)
        raw.gains = ctypes.cast(gains, ctypes.POINTER(_ffi.Gain))
        raw.gain_count = len(config.gains)

    return raw


def _as_float_buffer(bins: Any, held: _Held):
    """A contiguous ``float32`` buffer and its length, from whatever was passed.

    numpy arrays take the fast path -- a cast, no copy, when they are already
    ``float32`` and contiguous. Everything else is copied, because the library
    reads ``float*`` and nothing else will do.
    """
    if _np is not None and isinstance(bins, _np.ndarray):
        prepared = _np.ascontiguousarray(bins, dtype=_np.float32).ravel()
        held.keep(prepared)
        return prepared.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), prepared.size

    if isinstance(bins, (bytes, bytearray, memoryview)):
        raise TypeError(
            "write_frame() takes a sequence of levels in dB, not raw bytes; "
            "pass a list, array('f') or numpy array of floats"
        )

    values = list(bins)
    buffer = (ctypes.c_float * len(values))(*values)
    held.keep(buffer)
    return ctypes.cast(buffer, ctypes.POINTER(ctypes.c_float)), len(values)


class Writer:
    """Writes a ``.sweeps`` file.

    Use it as a context manager. **Exiting the block closes and checks**, which
    matters: closing is what flushes the pending tiles, writes the index and
    patches the header, and a failure there is how you learn the disk filled.
    A session ended by power loss is still readable by design; one ended by an
    ignored error need not be.

    Single-threaded, unlike :class:`~sweepsfile.reader.Reader`.
    """

    def __init__(
        self,
        path: str | os.PathLike[str],
        *,
        bins_per_line: int = 2048,
        session_name: str = "",
        notes: str = "",
        application_version: str = "",
        created_wall_ns: int = 0,
        max_bytes: int = 0,
        max_seconds: float = 0.0,
        min_free_bytes: int = DEFAULT_MIN_FREE_BYTES,
        log: LogSink | None = None,
    ) -> None:
        self._lib = library()
        self._handle: ctypes.c_void_p | None = None
        self._closed = False
        self._log_thunk = None
        self._held = _Held()

        config = _ffi.WriterConfig()
        self._lib.sweeps_writer_config_init(ctypes.byref(config))
        config.bins_per_line = bins_per_line
        config.max_bytes = max_bytes
        config.max_seconds = max_seconds
        config.min_free_bytes = min_free_bytes
        config.session_name = self._held.text(session_name)
        config.notes = self._held.text(notes)
        config.application_version = self._held.text(application_version)
        config.created_wall_ns = created_wall_ns

        if log is not None:
            self._log_thunk = _ffi.LOG_FN(self._make_sink(log))
            config.log = ctypes.cast(self._log_thunk, ctypes.c_void_p)

        handle = ctypes.c_void_p()
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_create(
                os.fsencode(os.fspath(path)), ctypes.byref(config), ctypes.byref(handle)
            ),
            f"creating {os.fspath(path)}",
        )
        self._handle = handle

    @staticmethod
    def _make_sink(log: LogSink):
        def thunk(_user, level, category, message):
            try:
                log(LogLevel(level), category.text(), message.text())
            except Exception:  # noqa: BLE001
                pass

        return thunk

    # -- lifecycle ---------------------------------------------------------

    @property
    def _live(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise SweepsError(0, "this writer has been destroyed")
        return self._handle

    def close(self) -> None:
        """Flushes tiles, writes the index and patches the header. Idempotent.

        Raises if any of that failed. The destructor closes too, but it has
        nowhere to report a failure -- so a caller who relies on it learns about
        a full disk from a truncated file, weeks later.
        """
        if self._handle is None or self._closed:
            return
        status = self._lib.sweeps_writer_close(self._handle)
        self._closed = True
        raise_for_status(self._lib, status, "closing the session")

    def destroy(self) -> None:
        """Releases the writer, discarding any error from closing. Idempotent."""
        if self._handle is not None:
            self._lib.sweeps_writer_destroy(self._handle)
            self._handle = None
            self._log_thunk = None

    def __enter__(self) -> "Writer":
        return self

    def __exit__(self, exc_type, *_exc: object) -> None:
        try:
            # On the way out of a failing block, do not raise a second error
            # over the first: the original exception is the more useful one.
            if exc_type is None:
                self.close()
        finally:
            self.destroy()

    def __del__(self) -> None:  # pragma: no cover - interpreter teardown
        try:
            self.destroy()
        except Exception:  # noqa: BLE001
            pass

    def __repr__(self) -> str:
        if self._handle is None:
            return "<Writer destroyed>"
        return f"<Writer {self.path!r}, {self.lines_written} line(s)>"

    # -- frames ------------------------------------------------------------

    def write_frame(
        self,
        bins: Sequence[float] | Any,
        *,
        config: AcquisitionConfig,
        start_hz: float,
        bin_width_hz: float,
        monotonic_ns: int,
        wall_ns: int = 0,
    ) -> FrameOutcome:
        """Stores one spectrum, opening a new segment if the grid changed.

        ``bins`` is levels in dB -- a numpy array, an ``array('f')``, or any
        sequence of floats. ``start_hz`` and ``bin_width_hz`` describe *this
        frame*, which for a partial sweep covers only its step's slice and is
        therefore not derivable from ``config`` alone.
        """
        held = _Held()
        acquisition = _to_acq_config(self._lib, config, held)

        frame = _ffi.Frame()
        self._lib.sweeps_frame_init(ctypes.byref(frame))
        frame.bins, frame.count = _as_float_buffer(bins, held)
        frame.start_hz = start_hz
        frame.bin_width_hz = bin_width_hz
        frame.monotonic_ns = monotonic_ns
        frame.wall_ns = wall_ns
        frame.config = ctypes.pointer(acquisition)

        outcome = _ffi.FrameOutcome()
        outcome.struct_size = ctypes.sizeof(outcome)
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_write_frame(
                self._live, ctypes.byref(frame), ctypes.byref(outcome)
            ),
        )

        # Folded back in here, though the C ABI has to keep it separate: the
        # reason lives in a scratch buffer good only until the next call, and
        # copying it now is what makes this dataclass safe to keep.
        return FrameOutcome(
            segment_id=outcome.segment_id,
            segment_opened=bool(outcome.segment_opened),
            retention_stopped=bool(outcome.retention_stopped),
            reason=self._lib.sweeps_writer_last_segment_reason(self._live).text(),
        )

    # -- events ------------------------------------------------------------
    #
    # One method per kind. The record's segment id is the segment open at that
    # moment, not anything the caller supplies: a live event is stamped where it
    # actually landed.

    def record_retune(
        self, monotonic_ns: int, wall_ns: int, center_hz: float, step_index: int = 0
    ) -> None:
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_record_retune(
                self._live, monotonic_ns, wall_ns, center_hz, step_index
            ),
        )

    def record_parameter_changed(
        self,
        monotonic_ns: int,
        wall_ns: int,
        key: str,
        value: str,
        *,
        grid_affecting: bool = False,
        calibration_affecting: bool = False,
    ) -> None:
        """A parameter moved.

        ``grid_affecting`` when the change redefines the frequency grid and
        therefore closes the current segment; ``calibration_affecting`` when the
        grid is intact but the noise floor moved, so later analysis of these
        tiles must know it happened.
        """
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_record_parameter_changed(
                self._live,
                monotonic_ns,
                wall_ns,
                key.encode("utf-8"),
                value.encode("utf-8"),
                int(grid_affecting),
                int(calibration_affecting),
            ),
        )

    def record_sweep_pass(
        self,
        monotonic_ns: int,
        wall_ns: int,
        pass_id: int,
        start_hz: float,
        stop_hz: float,
        duration_seconds: float,
    ) -> None:
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_record_sweep_pass(
                self._live, monotonic_ns, wall_ns, pass_id, start_hz, stop_hz, duration_seconds
            ),
        )

    def record_marker(
        self, monotonic_ns: int, wall_ns: int, label: str, frequency_hz: float, level_dbm: float
    ) -> None:
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_record_marker(
                self._live, monotonic_ns, wall_ns, label.encode("utf-8"), frequency_hz, level_dbm
            ),
        )

    def record_annotation(
        self, monotonic_ns: int, wall_ns: int, text: str, start_hz: float, stop_hz: float
    ) -> None:
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_record_annotation(
                self._live, monotonic_ns, wall_ns, text.encode("utf-8"), start_hz, stop_hz
            ),
        )

    def record_segment_boundary(self, monotonic_ns: int, wall_ns: int, reason: str) -> None:
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_record_segment_boundary(
                self._live, monotonic_ns, wall_ns, reason.encode("utf-8")
            ),
        )

    def record_throttle_changed(
        self, monotonic_ns: int, wall_ns: int, reason: str, processed_fraction: float
    ) -> None:
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_record_throttle_changed(
                self._live, monotonic_ns, wall_ns, reason.encode("utf-8"), processed_fraction
            ),
        )

    def record_device_error(
        self, monotonic_ns: int, wall_ns: int, device_id: str, message: str
    ) -> None:
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_record_device_error(
                self._live,
                monotonic_ns,
                wall_ns,
                device_id.encode("utf-8"),
                message.encode("utf-8"),
            ),
        )

    def record_plugin_event(
        self,
        monotonic_ns: int,
        wall_ns: int,
        plugin_id: str,
        event_name: str,
        fields: Mapping[str, Any] | None = None,
    ) -> None:
        """A plugin's own event.

        ``plugin_id`` is reverse-DNS -- ``org.sweeppp.bandplan`` -- which is what
        keeps two plugins from colliding without a registry. ``fields`` is any
        mapping; see :mod:`sweepsfile._metadata` for the type mapping.
        """
        with _metadata._Builder(self._lib) as builder:
            if fields:
                builder.fill(fields)
            raise_for_status(
                self._lib,
                self._lib.sweeps_writer_record_plugin_event(
                    self._live,
                    monotonic_ns,
                    wall_ns,
                    plugin_id.encode("utf-8"),
                    event_name.encode("utf-8"),
                    builder.handle,
                ),
            )

    def write_plugin_data(
        self,
        plugin_id: str,
        record_name: str,
        body: bytes,
        *,
        schema_version: int = 1,
        monotonic_ns: int = 0,
    ) -> None:
        """A producer's own record, opaque to the container.

        ``monotonic_ns`` of 0 means the record is not tied to a moment, which is
        what keeps it in an extraction whatever range was asked for.
        """
        raw = bytes(body)
        buffer = ctypes.create_string_buffer(raw, len(raw)) if raw else None
        raise_for_status(
            self._lib,
            self._lib.sweeps_writer_plugin_data(
                self._live,
                plugin_id.encode("utf-8"),
                record_name.encode("utf-8"),
                schema_version,
                monotonic_ns,
                buffer,
                len(raw),
            ),
        )

    # -- what has been written so far -------------------------------------

    @property
    def path(self) -> str:
        return self._lib.sweeps_writer_path(self._live).text()

    @property
    def bytes_written(self) -> int:
        return self._lib.sweeps_writer_bytes_written(self._live)

    @property
    def lines_written(self) -> int:
        return self._lib.sweeps_writer_lines_written(self._live)

    @property
    def segment_count(self) -> int:
        return self._lib.sweeps_writer_segment_count(self._live)

    @property
    def last_frame_ns(self) -> int:
        """Timestamp of the most recent frame, which defines a segment's end."""
        return self._lib.sweeps_writer_last_frame_ns(self._live)

    @property
    def retention_reached(self) -> bool:
        """A retention cap has stopped the writer."""
        return bool(self._lib.sweeps_writer_retention_reached(self._live))

    @property
    def retention_reason(self) -> str:
        return self._lib.sweeps_writer_retention_reason(self._live).text()

    @property
    def last_segment_reason(self) -> str:
        return self._lib.sweeps_writer_last_segment_reason(self._live).text()


def create(path: str | os.PathLike[str], **options: Any) -> Writer:
    """Creates a ``.sweeps`` file for writing. See :class:`Writer`."""
    return Writer(path, **options)
