# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""Reading ``.sweeps`` files."""

from __future__ import annotations

import ctypes
import os
from typing import Any, Callable, Iterator, Sequence

from . import _ffi, _metadata
from ._library import library
from ._types import Event, PluginRecord, Segment, Summary, Tile
from .enums import EventKind, LogLevel, WindowType
from .errors import SweepsError, WrongTypeError, raise_for_status

try:
    import numpy as _np
except ImportError:  # pragma: no cover
    _np = None

#: ``(level, category, message) -> None``
LogSink = Callable[[LogLevel, str, str], None]


class _LazySequence(Sequence):
    """A read-only sequence that builds its items on access.

    A session can carry a hundred thousand events. Decoding all of them to
    answer ``len()`` would make opening a file cost what reading it should, so
    items are built when they are asked for and the count comes from the
    library.
    """

    __slots__ = ("_count", "_build", "_name")

    def __init__(self, count: int, build: Callable[[int], Any], name: str) -> None:
        self._count = count
        self._build = build
        self._name = name

    def __len__(self) -> int:
        return self._count

    def __getitem__(self, index):
        if isinstance(index, slice):
            return [self._build(i) for i in range(*index.indices(self._count))]
        if index < 0:
            index += self._count
        if not 0 <= index < self._count:
            raise IndexError(f"{self._name} index out of range")
        return self._build(index)

    def __iter__(self) -> Iterator[Any]:
        for index in range(self._count):
            yield self._build(index)

    def __repr__(self) -> str:
        return f"<{self._name}: {self._count}>"


class Reader:
    """An open ``.sweeps`` session.

    The file is memory-mapped, so scrollback into a multi-hour session never
    loads the whole thing: a tile query touches only the pages it needs.

    Use it as a context manager, or call :meth:`close`. Every value this class
    returns is a copy, so results stay valid after closing -- but the reader
    itself does not, and using a closed one raises rather than crashing.

    Safe to use from several threads at once: every operation on it is
    read-only, and the library holds no global state.
    """

    def __init__(self, path: str | os.PathLike[str], *, log: LogSink | None = None) -> None:
        self._lib = library()
        self._handle: ctypes.c_void_p | None = None
        self._log_thunk = None

        encoded = os.fsencode(os.fspath(path))
        handle = ctypes.c_void_p()

        if log is None:
            status = self._lib.sweeps_reader_open(encoded, ctypes.byref(handle))
        else:
            # The thunk must outlive the reader: ctypes does not keep a
            # reference for us, and a collected callback is a jump into freed
            # memory the next time the library logs.
            self._log_thunk = _ffi.LOG_FN(self._make_sink(log))
            status = self._lib.sweeps_reader_open_ex(
                encoded, self._log_thunk, None, ctypes.byref(handle)
            )

        raise_for_status(self._lib, status, f"opening {os.fspath(path)}")
        self._handle = handle

    @staticmethod
    def _make_sink(log: LogSink):
        def thunk(_user, level, category, message):
            # A Python exception cannot cross back into C -- ctypes would print
            # it and return, mid-parse -- so a broken sink is contained here
            # rather than left to corrupt a read in progress.
            try:
                log(LogLevel(level), category.text(), message.text())
            except Exception:  # noqa: BLE001
                pass

        return thunk

    # -- lifecycle ---------------------------------------------------------

    def close(self) -> None:
        """Closes the file. Idempotent."""
        if self._handle is not None:
            self._lib.sweeps_reader_close(self._handle)
            self._handle = None
            self._log_thunk = None

    def __enter__(self) -> "Reader":
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
            raise SweepsError(0, "this reader is closed")
        return self._handle

    def __repr__(self) -> str:
        if self._handle is None:
            return "<Reader closed>"
        return f"<Reader {self.name!r}, {len(self.segments)} segment(s)>"

    # -- summary -----------------------------------------------------------

    @property
    def name(self) -> str:
        return self._lib.sweeps_reader_name(self._live).text()

    @property
    def app_version(self) -> str:
        """The version of the application that wrote the session."""
        return self._lib.sweeps_reader_app_version(self._live).text()

    @property
    def summary(self) -> Summary:
        raw = _ffi.Summary()
        raw.struct_size = ctypes.sizeof(raw)
        raise_for_status(self._lib, self._lib.sweeps_reader_summary(self._live, ctypes.byref(raw)))
        return Summary(
            major_version=raw.major_version,
            minor_version=raw.minor_version,
            incompatible_features=raw.incompatible_features,
            created_wall_ns=raw.created_wall_ns,
            total_lines=raw.total_lines,
            total_tiles=raw.total_tiles,
            file_bytes=raw.file_bytes,
            first_line_ns=raw.first_line_ns,
            last_line_ns=raw.last_line_ns,
            truncated_bytes=raw.truncated_bytes,
            lowest_hz=raw.lowest_hz,
            highest_hz=raw.highest_hz,
            recovered_by_scan=bool(raw.recovered_by_scan),
            newer_minor_version=bool(raw.newer_minor_version),
        )

    @property
    def manifest(self) -> dict[str, Any]:
        """The session manifest as a dict.

        Empty when the file carries no manifest or the one it carries could not
        be decoded -- which the ``log`` sink passed to the constructor says, and
        nothing else does.
        """
        return _metadata.to_dict(self._lib, self._lib.sweeps_reader_manifest(self._live))

    # -- segments ----------------------------------------------------------

    @property
    def segments(self) -> tuple[Segment, ...]:
        handle = self._live
        return tuple(
            self._segment(i) for i in range(self._lib.sweeps_reader_segment_count(handle))
        )

    def segment_by_id(self, segment_id: int) -> Segment:
        """The segment with this id.

        By id, never by position: an extracted file contains segments whose ids
        neither start at zero nor run contiguously.
        """
        index = ctypes.c_size_t(0)
        raise_for_status(
            self._lib,
            self._lib.sweeps_reader_segment_index_of(self._live, segment_id, ctypes.byref(index)),
            f"segment {segment_id}",
        )
        return self._segment(index.value)

    def _segment(self, index: int) -> Segment:
        lib = self._lib
        handle = self._live

        raw = _ffi.Segment()
        raw.struct_size = ctypes.sizeof(raw)
        raise_for_status(lib, lib.sweeps_reader_segment_at(handle, index, ctypes.byref(raw)))

        gains: dict[str, float] = {}
        for gain_index in range(lib.sweeps_reader_segment_gain_count(handle, index)):
            gain_name = _ffi.Str()
            gain_value = ctypes.c_double(0.0)
            raise_for_status(
                lib,
                lib.sweeps_reader_segment_gain(
                    handle, index, gain_index, ctypes.byref(gain_name), ctypes.byref(gain_value)
                ),
            )
            gains[gain_name.text()] = gain_value.value

        try:
            window = WindowType(raw.window)
        except ValueError:
            window = raw.window  # a window this binding does not know yet

        return Segment(
            id=raw.id,
            index=index,
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
            reason=lib.sweeps_reader_segment_reason(handle, index).text(),
            device_id=lib.sweeps_reader_segment_device_id(handle, index).text(),
            device_label=lib.sweeps_reader_segment_device_label(handle, index).text(),
            gains=gains,
        )

    # -- events ------------------------------------------------------------

    @property
    def events(self) -> Sequence[Event]:
        """The event stream, decoded on access.

        Indexable and iterable. Decoding is deferred because a long session
        carries a great many events and most callers want a handful of them.
        """
        return _LazySequence(
            self._lib.sweeps_reader_event_count(self._live), self._event, "events"
        )

    def _event(self, index: int) -> Event:
        lib = self._lib
        handle = lib.sweeps_reader_event_at(self._live, index)
        if not handle:
            raise IndexError("event index out of range")

        kind = lib.sweeps_event_kind(handle)
        return Event(
            kind=kind,
            kind_name=lib.sweeps_event_kind_name(kind).text(),
            monotonic_ns=lib.sweeps_event_monotonic_ns(handle),
            wall_ns=lib.sweeps_event_wall_ns(handle),
            segment_id=lib.sweeps_event_segment_id(handle),
            body=self._event_body(handle, kind),
        )

    def _event_body(self, handle, kind: int) -> dict[str, Any]:
        """Decodes a body through the typed accessors.

        Keyed on the body rather than on the kind, and the fall-through matters:
        ``ALERT`` is a reserved kind with no defined body, and a kind from a
        newer producer has one this build cannot name. Both read as ``raw``
        bytes rather than as nothing.
        """
        lib = self._lib

        if kind == EventKind.RETUNE:
            center_hz = ctypes.c_double(0.0)
            step = ctypes.c_uint32(0)
            raise_for_status(
                lib, lib.sweeps_event_retune(handle, ctypes.byref(center_hz), ctypes.byref(step))
            )
            return {"center_hz": center_hz.value, "step_index": step.value}

        if kind == EventKind.PARAMETER_CHANGED:
            key, value = _ffi.Str(), _ffi.Str()
            grid, calibration = ctypes.c_int(0), ctypes.c_int(0)
            raise_for_status(
                lib,
                lib.sweeps_event_parameter_changed(
                    handle,
                    ctypes.byref(key),
                    ctypes.byref(value),
                    ctypes.byref(grid),
                    ctypes.byref(calibration),
                ),
            )
            return {
                "key": key.text(),
                "value": value.text(),
                "grid_affecting": bool(grid.value),
                "calibration_affecting": bool(calibration.value),
            }

        if kind == EventKind.SWEEP_PASS:
            pass_id = ctypes.c_uint64(0)
            start_hz, stop_hz, duration = (ctypes.c_double(0.0) for _ in range(3))
            raise_for_status(
                lib,
                lib.sweeps_event_sweep_pass(
                    handle,
                    ctypes.byref(pass_id),
                    ctypes.byref(start_hz),
                    ctypes.byref(stop_hz),
                    ctypes.byref(duration),
                ),
            )
            return {
                "pass_id": pass_id.value,
                "start_hz": start_hz.value,
                "stop_hz": stop_hz.value,
                "duration_seconds": duration.value,
            }

        if kind == EventKind.MARKER:
            label = _ffi.Str()
            frequency, level = ctypes.c_double(0.0), ctypes.c_double(0.0)
            raise_for_status(
                lib,
                lib.sweeps_event_marker(
                    handle, ctypes.byref(label), ctypes.byref(frequency), ctypes.byref(level)
                ),
            )
            return {
                "label": label.text(),
                "frequency_hz": frequency.value,
                "level_dbm": level.value,
            }

        if kind == EventKind.ANNOTATION:
            text = _ffi.Str()
            start_hz, stop_hz = ctypes.c_double(0.0), ctypes.c_double(0.0)
            raise_for_status(
                lib,
                lib.sweeps_event_annotation(
                    handle, ctypes.byref(text), ctypes.byref(start_hz), ctypes.byref(stop_hz)
                ),
            )
            return {"text": text.text(), "start_hz": start_hz.value, "stop_hz": stop_hz.value}

        if kind == EventKind.SEGMENT_BOUNDARY:
            reason = _ffi.Str()
            raise_for_status(lib, lib.sweeps_event_segment_boundary(handle, ctypes.byref(reason)))
            return {"reason": reason.text()}

        if kind == EventKind.THROTTLE_CHANGED:
            reason = _ffi.Str()
            fraction = ctypes.c_double(0.0)
            raise_for_status(
                lib,
                lib.sweeps_event_throttle_changed(
                    handle, ctypes.byref(reason), ctypes.byref(fraction)
                ),
            )
            return {"reason": reason.text(), "processed_fraction": fraction.value}

        if kind == EventKind.DEVICE_ERROR:
            device_id, message = _ffi.Str(), _ffi.Str()
            raise_for_status(
                lib,
                lib.sweeps_event_device_error(
                    handle, ctypes.byref(device_id), ctypes.byref(message)
                ),
            )
            return {"device_id": device_id.text(), "message": message.text()}

        if kind == EventKind.PLUGIN:
            plugin_id, event_name = _ffi.Str(), _ffi.Str()
            fields = ctypes.c_void_p()
            raise_for_status(
                lib,
                lib.sweeps_event_plugin(
                    handle,
                    ctypes.byref(plugin_id),
                    ctypes.byref(event_name),
                    ctypes.byref(fields),
                ),
            )
            return {
                "plugin_id": plugin_id.text(),
                "event": event_name.text(),
                "fields": _metadata.to_dict(lib, fields),
            }

        payload = _ffi.Bytes()
        try:
            raise_for_status(lib, lib.sweeps_event_unknown_body(handle, ctypes.byref(payload)))
        except WrongTypeError:
            # A kind this binding has not been taught, whose body the library
            # nonetheless decoded. Nothing useful to say about it.
            return {}
        return {"raw": payload.copy()}

    # -- plugin records ----------------------------------------------------

    @property
    def plugin_records(self) -> Sequence[PluginRecord]:
        return _LazySequence(
            self._lib.sweeps_reader_plugin_count(self._live), self._plugin, "plugin_records"
        )

    def _plugin(self, index: int) -> PluginRecord:
        lib = self._lib
        handle = lib.sweeps_reader_plugin_at(self._live, index)
        if not handle:
            raise IndexError("plugin record index out of range")

        body = _ffi.Bytes()
        raise_for_status(lib, lib.sweeps_plugin_body(handle, ctypes.byref(body)))
        return PluginRecord(
            plugin_id=lib.sweeps_plugin_id(handle).text(),
            name=lib.sweeps_plugin_name(handle).text(),
            schema_version=lib.sweeps_plugin_schema_version(handle),
            monotonic_ns=lib.sweeps_plugin_monotonic_ns(handle),
            body=body.copy(),
        )

    # -- tiles -------------------------------------------------------------

    def _make_query(
        self,
        *,
        from_ns: int | None = None,
        to_ns: int | None = None,
        from_hz: float | None = None,
        to_hz: float | None = None,
        max_lines: int | None = None,
        max_bins: int | None = None,
        segment_id: int | None = None,
        lod: int | None = None,
    ) -> _ffi.Query:
        # Always from the library's own defaults, never from zeros. A zeroed
        # query asks for a time range ending at nanosecond zero and gets
        # nothing, successfully -- which is the trap the C API's _init function
        # exists to prevent, and which keyword arguments prevent by
        # construction here.
        query = _ffi.Query()
        self._lib.sweeps_query_init(ctypes.byref(query))

        if from_ns is not None:
            query.from_ns = from_ns
        if to_ns is not None:
            query.to_ns = to_ns
        if from_hz is not None:
            query.from_hz = from_hz
        if to_hz is not None:
            query.to_hz = to_hz
        if max_lines is not None:
            query.max_lines = max_lines
        if max_bins is not None:
            query.max_bins = max_bins
        if segment_id is not None:
            query.segment_id = segment_id
            query.has_segment_id = 1
        if lod is not None:
            query.lod = lod
            query.has_lod = 1
        return query

    def query(self, **bounds: Any) -> list[Tile]:
        """Fetches the tiles covering a time and frequency range.

        Accepts ``from_ns``, ``to_ns``, ``from_hz``, ``to_hz``, ``max_lines``,
        ``max_bins``, ``segment_id`` and ``lod``; anything omitted keeps the
        library's default, which is the whole session at 2048 lines by 4096
        bins.

        The reader picks the coarsest pyramid level that satisfies ``max_lines``
        -- which is what keeps "draw three hours" from meaning "read three hours
        of lines". Pass ``lod`` to serve exactly one level instead; a level with
        no tiles returns nothing rather than silently falling back.
        """
        query = self._make_query(**bounds)

        tiles = ctypes.c_void_p()
        raise_for_status(
            self._lib,
            self._lib.sweeps_reader_query(self._live, ctypes.byref(query), ctypes.byref(tiles)),
        )

        try:
            return [self._tile(tiles, i) for i in range(self._lib.sweeps_tiles_count(tiles))]
        finally:
            # The tile list is owned rather than borrowed: the reader assembles
            # it per call. Freed here because every Tile above copied its bytes.
            self._lib.sweeps_tiles_free(tiles)

    def _tile(self, tiles, index: int) -> Tile:
        lib = self._lib
        handle = lib.sweeps_tiles_at(tiles, index)
        if not handle:
            raise IndexError("tile index out of range")

        info = _ffi.TileInfo()
        info.struct_size = ctypes.sizeof(info)
        raise_for_status(lib, lib.sweeps_tile_info(handle, ctypes.byref(info)))

        size = ctypes.c_size_t(0)
        data = lib.sweeps_tile_data(handle, ctypes.byref(size))
        payload = ctypes.string_at(data, size.value) if data and size.value else b""

        return Tile(
            segment_id=info.segment_id,
            lod=info.lod,
            time_block=info.time_block,
            freq_block=info.freq_block,
            lines=info.lines,
            bins=info.bins,
            origin_db=info.origin_db,
            first_line_ns=info.first_line_ns,
            last_line_ns=info.last_line_ns,
            start_hz=info.start_hz,
            bin_width_hz=info.bin_width_hz,
            data=payload,
        )

    def choose_lod(self, from_ns: int, to_ns: int, max_lines: int, segment_id: int) -> int:
        """The pyramid level whose line spacing best fits ``max_lines``.

        Never a level with no tiles: short sessions never fill a coarse tile,
        and a truncated one loses the coarse levels first.
        """
        return self._lib.sweeps_reader_choose_lod(
            self._live, from_ns, to_ns, max_lines, segment_id
        )

    def has_tiles_at_lod(self, segment_id: int, lod: int) -> bool:
        return bool(self._lib.sweeps_reader_has_tiles_at_lod(self._live, segment_id, lod))

    def spectrum_at(self, monotonic_ns: int, segment_id: int):
        """The spectrum at one instant, in dB.

        A numpy float32 array when numpy is available, a ``list[float]``
        otherwise. Raises :class:`~sweepsfile.errors.NotFoundError` when the
        segment has no data at that moment.
        """
        lib = self._lib
        handle = self._live

        count = ctypes.c_size_t(0)
        raise_for_status(
            lib,
            lib.sweeps_reader_spectrum_at(
                handle, monotonic_ns, segment_id, None, 0, ctypes.byref(count)
            ),
        )

        buffer = (ctypes.c_float * count.value)()
        raise_for_status(
            lib,
            lib.sweeps_reader_spectrum_at(
                handle, monotonic_ns, segment_id, buffer, count.value, ctypes.byref(count)
            ),
        )

        if _np is not None:
            return _np.frombuffer(buffer, dtype=_np.float32, count=count.value).copy()
        return list(buffer[: count.value])

    # -- integrity and extraction -----------------------------------------

    def verify(self) -> int:
        """Walks every record and verifies every checksum.

        Returns the number of records checked, or raises at the first failure --
        a stronger statement than "it parsed".
        """
        records = ctypes.c_uint64(0)
        raise_for_status(self._lib, self._lib.sweeps_reader_verify(self._live, ctypes.byref(records)))
        return records.value

    def extract(
        self,
        destination: str | os.PathLike[str],
        *,
        application_version: str | None = None,
        created_wall_ns: int = 0,
        **bounds: Any,
    ) -> None:
        """Copies a time and frequency range into a new standalone ``.sweeps``.

        A tile copy plus a new index and manifest -- no re-encoding and no
        re-quantisation, so the extracted tiles are bit-identical to the source.

        Only ``from_ns``/``to_ns`` and ``from_hz``/``to_hz`` are consulted: an
        extraction copies whole tiles or none, so ``segment_id``, ``lod`` and
        the caps -- which shape a *read* -- have nothing to say here. To extract
        one segment, pass its time span. Segment ids are preserved rather than
        renumbered.
        """
        for ignored in ("segment_id", "lod", "max_lines", "max_bins"):
            if ignored in bounds:
                raise TypeError(
                    f"extract() does not accept {ignored!r}: an extraction is a range, "
                    "not a query. Pass from_ns/to_ns or from_hz/to_hz."
                )

        query = self._make_query(**bounds)

        options = _ffi.ExtractOptions()
        self._lib.sweeps_extract_options_init(ctypes.byref(options))
        encoded_version = application_version.encode("utf-8") if application_version else None
        options.application_version = encoded_version
        options.created_wall_ns = created_wall_ns

        raise_for_status(
            self._lib,
            self._lib.sweeps_reader_extract(
                self._live,
                os.fsencode(os.fspath(destination)),
                ctypes.byref(query),
                ctypes.byref(options),
            ),
            f"extracting to {os.fspath(destination)}",
        )


def open(  # noqa: A001 - deliberately shadows the builtin, as gzip.open does
    path: str | os.PathLike[str], *, log: LogSink | None = None
) -> Reader:
    """Opens a ``.sweeps`` file for reading.

    ``log`` receives the library's diagnostics -- a damaged index, a truncated
    tail, a tile that failed its checksum. Worth passing: all three are
    recovered from rather than refused, and this callback is the only way to
    learn any of them happened.
    """
    return Reader(path, log=log)
