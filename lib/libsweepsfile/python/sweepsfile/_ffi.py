# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""The raw C ABI, as ctypes sees it.

A mechanical mirror of ``sweeps/sweeps.h`` and nothing else: no convenience, no
interpretation, no Python types. Everything friendly lives a layer up, so that
when the header gains a function there is exactly one place here to add it and
one place there to make it pleasant.

ctypes rather than a compiled extension, deliberately. The whole argument for
the C ABI is that reaching ``.sweeps`` should not require a toolchain, and a
binding that needs a compiler to install would hand that back. This module needs
Python and a shared ``libsweepsfile``.

**The struct layouts below are an ABI contract.** Field order and type must
match the header exactly; ctypes cannot check that for us, and a mismatch is
silent garbage rather than an error. ``sweeps_abi_version`` is checked on load,
and every struct carries ``struct_size``, which is what makes a library newer
than this file safe rather than merely lucky.
"""

from __future__ import annotations

import ctypes
import os
import sys
from ctypes import (
    POINTER,
    c_char_p,
    c_double,
    c_float,
    c_int,
    c_int32,
    c_size_t,
    c_uint8,
    c_uint16,
    c_uint32,
    c_uint64,
    c_void_p,
)

#: The ABI this module was written against. Checked against the loaded library.
ABI_VERSION = 1

#: Fixed by the format, and mirrored so callers need not import the C header.
TILE_LINES = 256
TILE_BINS = 1024
LOD_LEVELS = 3
DB_PER_STEP = 0.5
UNMEASURED_BYTE = 0
UNMEASURED_DB = -200.0
PRIVATE_USE_FIRST = 0xFF00


# ---------------------------------------------------------------------------
# Structs
# ---------------------------------------------------------------------------


class Str(ctypes.Structure):
    """``sweeps_str_t``. Borrowed, NUL-terminated UTF-8.

    Every use is copied into a Python ``str`` at the boundary. A view outlives
    nothing here on purpose: Python objects escape their creating scope freely,
    and a ``bytes`` that pointed into a closed reader would be a use-after-free
    with no traceback to explain it.
    """

    _fields_ = [("data", c_char_p), ("len", c_size_t)]

    def text(self) -> str:
        if not self.data or self.len == 0:
            return ""
        return ctypes.string_at(self.data, self.len).decode("utf-8", "replace")


class Bytes(ctypes.Structure):
    """``sweeps_bytes_t``. Borrowed, not text, not NUL-terminated."""

    _fields_ = [("data", POINTER(c_uint8)), ("len", c_size_t)]

    def copy(self) -> bytes:
        if not self.data or self.len == 0:
            return b""
        return ctypes.string_at(self.data, self.len)


class Gain(ctypes.Structure):
    _fields_ = [("name", c_char_p), ("value", c_double)]


class Summary(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("major_version", c_uint32),
        ("minor_version", c_uint32),
        ("incompatible_features", c_uint32),
        ("created_wall_ns", c_uint64),
        ("total_lines", c_uint64),
        ("total_tiles", c_uint64),
        ("file_bytes", c_uint64),
        ("first_line_ns", c_uint64),
        ("last_line_ns", c_uint64),
        ("truncated_bytes", c_uint64),
        ("lowest_hz", c_double),
        ("highest_hz", c_double),
        ("recovered_by_scan", c_int32),
        ("newer_minor_version", c_int32),
    ]


class Segment(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("id", c_uint32),
        ("bin_count", c_uint32),
        ("fft_size", c_uint32),
        ("window", c_uint32),
        ("gain_count", c_uint32),
        ("start_wall_ns", c_uint64),
        ("start_monotonic_ns", c_uint64),
        ("end_monotonic_ns", c_uint64),
        ("line_count", c_uint64),
        ("start_hz", c_double),
        ("bin_width_hz", c_double),
        ("center_hz", c_double),
        ("span_hz", c_double),
        ("sample_rate", c_double),
        ("window_beta", c_double),
        ("window_enbw", c_double),
        ("overlap", c_double),
        ("rbw_hz", c_double),
        ("reference_level_dbm", c_double),
        ("dbfs_to_dbm_offset", c_double),
    ]


class Query(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("max_lines", c_uint32),
        ("max_bins", c_uint32),
        ("segment_id", c_uint32),
        ("has_segment_id", c_int32),
        ("lod", c_uint32),
        ("has_lod", c_int32),
        ("from_ns", c_uint64),
        ("to_ns", c_uint64),
        ("from_hz", c_double),
        ("to_hz", c_double),
    ]


class TileInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("segment_id", c_uint32),
        ("lod", c_uint32),
        ("time_block", c_uint32),
        ("freq_block", c_uint32),
        ("lines", c_uint32),
        ("bins", c_uint32),
        ("origin_db", c_float),
        ("first_line_ns", c_uint64),
        ("last_line_ns", c_uint64),
        ("start_hz", c_double),
        ("bin_width_hz", c_double),
    ]


class ExtractOptions(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("application_version", c_char_p),
        ("created_wall_ns", c_uint64),
    ]


class AcqConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("fft_size", c_uint32),
        ("window", c_uint32),
        ("center_hz", c_double),
        ("span_hz", c_double),
        ("sample_rate", c_double),
        ("window_beta", c_double),
        ("window_enbw", c_double),
        ("overlap", c_double),
        ("rbw_hz", c_double),
        ("reference_level_dbm", c_double),
        ("dbfs_to_dbm_offset", c_double),
        ("device_id", c_char_p),
        ("device_label", c_char_p),
        ("gains", POINTER(Gain)),
        ("gain_count", c_size_t),
    ]


class WriterConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("bins_per_line", c_uint32),
        ("max_bytes", c_uint64),
        ("max_seconds", c_double),
        ("min_free_bytes", c_uint64),
        ("session_name", c_char_p),
        ("notes", c_char_p),
        ("application_version", c_char_p),
        ("created_wall_ns", c_uint64),
        ("log", c_void_p),
        ("log_user", c_void_p),
    ]


class Frame(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("bins", POINTER(c_float)),
        ("count", c_size_t),
        ("start_hz", c_double),
        ("bin_width_hz", c_double),
        ("monotonic_ns", c_uint64),
        ("wall_ns", c_uint64),
        ("config", POINTER(AcqConfig)),
    ]


class FrameOutcome(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("segment_id", c_uint32),
        ("segment_opened", c_int32),
        ("retention_stopped", c_int32),
    ]


class StreamRecord(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("type", c_uint32),
        ("payload", Bytes),
    ]


class StreamLine(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("segment_id", c_uint32),
        ("bin_count", c_uint32),
        ("line", c_uint32),
        ("tiles_applied", c_uint64),
        ("start_hz", c_double),
        ("bin_width_hz", c_double),
        ("levels", POINTER(c_float)),
    ]


#: ``sweeps_log_fn``: (user, level, category, message).
LOG_FN = ctypes.CFUNCTYPE(None, c_void_p, c_int, Str, Str)


# ---------------------------------------------------------------------------
# Finding the library
# ---------------------------------------------------------------------------


def _candidate_names() -> list[str]:
    if sys.platform == "darwin":
        return ["libsweepsfile.dylib", "libsweepsfile.1.dylib"]
    if sys.platform == "win32":
        return ["sweepsfile.dll", "libsweepsfile.dll"]
    return ["libsweepsfile.so", "libsweepsfile.so.1"]


def _search_paths() -> list[str]:
    """Where to look, most explicit first.

    ``SWEEPSFILE_LIBRARY`` wins outright: a developer with several builds needs
    to say which one, and guessing wrong is how you end up debugging a library
    you are not editing. Everything after it is a convenience.
    """
    found: list[str] = []

    explicit = os.environ.get("SWEEPSFILE_LIBRARY")
    if explicit:
        found.append(explicit)

    # Beside the package, which is where a wheel that bundles the library would
    # put it.
    here = os.path.dirname(os.path.abspath(__file__))
    for name in _candidate_names():
        found.append(os.path.join(here, name))

    # A build tree, for the common case of running from a checkout.
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(here))))
    for build in ("build/sweepsfile-shared", "build/sweepsfile", "build/dev"):
        for name in _candidate_names():
            found.append(os.path.join(root, build, name))
            found.append(os.path.join(root, build, "prefix", "lib", name))

    return found


class LibraryNotFound(Exception):
    """No usable ``libsweepsfile`` shared library could be located."""


def load(path: str | None = None) -> ctypes.CDLL:
    """Loads the shared library and checks that it speaks this ABI.

    A **shared** build is required: ctypes has nothing to link against in a
    static archive. Configure the library with ``-DBUILD_SHARED_LIBS=ON``.
    """
    tried: list[str] = []

    candidates = [path] if path else _search_paths()
    library = None
    for candidate in candidates:
        if not candidate:
            continue
        tried.append(candidate)
        if os.path.isfile(candidate):
            library = ctypes.CDLL(candidate)
            break

    if library is None and not path:
        from ctypes.util import find_library

        located = find_library("sweepsfile")
        tried.append("find_library('sweepsfile')")
        if located:
            library = ctypes.CDLL(located)

    if library is None:
        raise LibraryNotFound(
            "cannot find a shared libsweepsfile. Build it with "
            "-DBUILD_SHARED_LIBS=ON and point SWEEPSFILE_LIBRARY at the result. "
            "Tried:\n  " + "\n  ".join(tried)
        )

    _declare(library)

    actual = library.sweeps_abi_version()
    if actual != ABI_VERSION:
        raise LibraryNotFound(
            f"libsweepsfile speaks ABI {actual}, this binding speaks {ABI_VERSION}"
        )

    return library


# ---------------------------------------------------------------------------
# Prototypes
#
# Every one of these is spelled out. ctypes defaults an unprototyped call to
# `int` return and C-int arguments, which silently truncates every pointer and
# every 64-bit value on the way in and out -- the failure mode being wrong
# numbers rather than a crash.
# ---------------------------------------------------------------------------

_STATUS = c_int
_HANDLE = c_void_p


def _declare(lib: ctypes.CDLL) -> None:
    def fn(name: str, restype, *argtypes) -> None:
        symbol = getattr(lib, name)
        symbol.restype = restype
        symbol.argtypes = list(argtypes)

    # Version and errors
    fn("sweeps_abi_version", c_uint32)
    fn("sweeps_library_version", Str)
    fn("sweeps_format_version", None, POINTER(c_uint32), POINTER(c_uint32))
    fn("sweeps_status_name", c_char_p, c_int)
    fn("sweeps_last_error", Str)

    # Format helpers
    fn("sweeps_dequantise_db", c_double, c_uint8, c_double)
    fn("sweeps_quantise_db", c_uint8, c_double, c_double)
    fn("sweeps_window_type_name", Str, c_int)
    fn("sweeps_event_kind_name", Str, c_uint16)

    # Struct defaults
    fn("sweeps_query_init", None, POINTER(Query))
    fn("sweeps_extract_options_init", None, POINTER(ExtractOptions))
    fn("sweeps_acq_config_init", None, POINTER(AcqConfig))
    fn("sweeps_writer_config_init", None, POINTER(WriterConfig))
    fn("sweeps_frame_init", None, POINTER(Frame))

    # Reader
    fn("sweeps_reader_open", _STATUS, c_char_p, POINTER(_HANDLE))
    fn("sweeps_reader_open_ex", _STATUS, c_char_p, LOG_FN, c_void_p, POINTER(_HANDLE))
    fn("sweeps_reader_close", None, _HANDLE)
    fn("sweeps_reader_summary", _STATUS, _HANDLE, POINTER(Summary))
    fn("sweeps_reader_name", Str, _HANDLE)
    fn("sweeps_reader_app_version", Str, _HANDLE)
    fn("sweeps_reader_manifest", _HANDLE, _HANDLE)

    fn("sweeps_reader_segment_count", c_size_t, _HANDLE)
    fn("sweeps_reader_segment_at", _STATUS, _HANDLE, c_size_t, POINTER(Segment))
    fn("sweeps_reader_segment_index_of", _STATUS, _HANDLE, c_uint32, POINTER(c_size_t))
    fn("sweeps_reader_segment_reason", Str, _HANDLE, c_size_t)
    fn("sweeps_reader_segment_device_id", Str, _HANDLE, c_size_t)
    fn("sweeps_reader_segment_device_label", Str, _HANDLE, c_size_t)
    fn("sweeps_reader_segment_gain_count", c_size_t, _HANDLE, c_size_t)
    fn(
        "sweeps_reader_segment_gain",
        _STATUS,
        _HANDLE,
        c_size_t,
        c_size_t,
        POINTER(Str),
        POINTER(c_double),
    )

    fn("sweeps_reader_verify", _STATUS, _HANDLE, POINTER(c_uint64))
    fn(
        "sweeps_reader_extract",
        _STATUS,
        _HANDLE,
        c_char_p,
        POINTER(Query),
        POINTER(ExtractOptions),
    )

    # Tiles
    fn("sweeps_reader_query", _STATUS, _HANDLE, POINTER(Query), POINTER(_HANDLE))
    fn("sweeps_tiles_free", None, _HANDLE)
    fn("sweeps_tiles_count", c_size_t, _HANDLE)
    fn("sweeps_tiles_at", _HANDLE, _HANDLE, c_size_t)
    fn("sweeps_tile_info", _STATUS, _HANDLE, POINTER(TileInfo))
    fn("sweeps_tile_data", POINTER(c_uint8), _HANDLE, POINTER(c_size_t))
    fn("sweeps_reader_choose_lod", c_uint32, _HANDLE, c_uint64, c_uint64, c_uint32, c_uint32)
    fn("sweeps_reader_has_tiles_at_lod", c_int, _HANDLE, c_uint32, c_uint32)
    fn(
        "sweeps_reader_spectrum_at",
        _STATUS,
        _HANDLE,
        c_uint64,
        c_uint32,
        POINTER(c_float),
        c_size_t,
        POINTER(c_size_t),
    )

    # Events
    fn("sweeps_reader_event_count", c_size_t, _HANDLE)
    fn("sweeps_reader_event_at", _HANDLE, _HANDLE, c_size_t)
    fn("sweeps_event_kind", c_uint16, _HANDLE)
    fn("sweeps_event_monotonic_ns", c_uint64, _HANDLE)
    fn("sweeps_event_wall_ns", c_uint64, _HANDLE)
    fn("sweeps_event_segment_id", c_uint32, _HANDLE)
    fn("sweeps_event_retune", _STATUS, _HANDLE, POINTER(c_double), POINTER(c_uint32))
    fn(
        "sweeps_event_parameter_changed",
        _STATUS,
        _HANDLE,
        POINTER(Str),
        POINTER(Str),
        POINTER(c_int),
        POINTER(c_int),
    )
    fn(
        "sweeps_event_sweep_pass",
        _STATUS,
        _HANDLE,
        POINTER(c_uint64),
        POINTER(c_double),
        POINTER(c_double),
        POINTER(c_double),
    )
    fn(
        "sweeps_event_marker",
        _STATUS,
        _HANDLE,
        POINTER(Str),
        POINTER(c_double),
        POINTER(c_double),
    )
    fn(
        "sweeps_event_annotation",
        _STATUS,
        _HANDLE,
        POINTER(Str),
        POINTER(c_double),
        POINTER(c_double),
    )
    fn("sweeps_event_segment_boundary", _STATUS, _HANDLE, POINTER(Str))
    fn("sweeps_event_throttle_changed", _STATUS, _HANDLE, POINTER(Str), POINTER(c_double))
    fn("sweeps_event_device_error", _STATUS, _HANDLE, POINTER(Str), POINTER(Str))
    fn(
        "sweeps_event_plugin",
        _STATUS,
        _HANDLE,
        POINTER(Str),
        POINTER(Str),
        POINTER(_HANDLE),
    )
    fn("sweeps_event_unknown_body", _STATUS, _HANDLE, POINTER(Bytes))
    fn(
        "sweeps_event_body_json",
        _STATUS,
        _HANDLE,
        c_int,
        c_char_p,
        c_size_t,
        POINTER(c_size_t),
    )

    # Plugin records
    fn("sweeps_reader_plugin_count", c_size_t, _HANDLE)
    fn("sweeps_reader_plugin_at", _HANDLE, _HANDLE, c_size_t)
    fn("sweeps_plugin_id", Str, _HANDLE)
    fn("sweeps_plugin_name", Str, _HANDLE)
    fn("sweeps_plugin_schema_version", c_uint32, _HANDLE)
    fn("sweeps_plugin_monotonic_ns", c_uint64, _HANDLE)
    fn("sweeps_plugin_body", _STATUS, _HANDLE, POINTER(Bytes))

    # Metadata -- reading
    fn("sweeps_metadata_count", c_size_t, _HANDLE)
    fn("sweeps_metadata_key_at", _STATUS, _HANDLE, c_size_t, POINTER(Str), POINTER(c_int))
    fn("sweeps_metadata_contains", c_int, _HANDLE, c_char_p)
    fn("sweeps_metadata_type_of", c_int, _HANDLE, c_char_p)
    fn("sweeps_metadata_get_string", Str, _HANDLE, c_char_p, c_char_p)
    fn("sweeps_metadata_get_i64", ctypes.c_int64, _HANDLE, c_char_p, ctypes.c_int64)
    fn("sweeps_metadata_get_f64", c_double, _HANDLE, c_char_p, c_double)
    fn("sweeps_metadata_get_bool", c_int, _HANDLE, c_char_p, c_int)
    fn("sweeps_metadata_get_bytes", _STATUS, _HANDLE, c_char_p, POINTER(Bytes))
    fn("sweeps_metadata_get_hash", _STATUS, _HANDLE, c_char_p, POINTER(_HANDLE))
    fn(
        "sweeps_metadata_to_json",
        _STATUS,
        _HANDLE,
        c_int,
        c_char_p,
        c_size_t,
        POINTER(c_size_t),
    )

    # Metadata -- building
    fn("sweeps_metadata_create", _HANDLE)
    fn("sweeps_metadata_destroy", None, _HANDLE)
    fn("sweeps_metadata_set_string", _STATUS, _HANDLE, c_char_p, c_char_p)
    fn("sweeps_metadata_set_i64", _STATUS, _HANDLE, c_char_p, ctypes.c_int64)
    fn("sweeps_metadata_set_f64", _STATUS, _HANDLE, c_char_p, c_double)
    fn("sweeps_metadata_set_bool", _STATUS, _HANDLE, c_char_p, c_int)
    fn("sweeps_metadata_set_bytes", _STATUS, _HANDLE, c_char_p, c_void_p, c_size_t)
    fn("sweeps_metadata_set_hash", _STATUS, _HANDLE, c_char_p, _HANDLE)

    # Writer
    fn("sweeps_writer_create", _STATUS, c_char_p, POINTER(WriterConfig), POINTER(_HANDLE))
    fn("sweeps_writer_close", _STATUS, _HANDLE)
    fn("sweeps_writer_destroy", None, _HANDLE)
    fn(
        "sweeps_writer_write_frame",
        _STATUS,
        _HANDLE,
        POINTER(Frame),
        POINTER(FrameOutcome),
    )
    fn("sweeps_writer_record_retune", _STATUS, _HANDLE, c_uint64, c_uint64, c_double, c_uint32)
    fn(
        "sweeps_writer_record_parameter_changed",
        _STATUS,
        _HANDLE,
        c_uint64,
        c_uint64,
        c_char_p,
        c_char_p,
        c_int,
        c_int,
    )
    fn(
        "sweeps_writer_record_sweep_pass",
        _STATUS,
        _HANDLE,
        c_uint64,
        c_uint64,
        c_uint64,
        c_double,
        c_double,
        c_double,
    )
    fn(
        "sweeps_writer_record_marker",
        _STATUS,
        _HANDLE,
        c_uint64,
        c_uint64,
        c_char_p,
        c_double,
        c_double,
    )
    fn(
        "sweeps_writer_record_annotation",
        _STATUS,
        _HANDLE,
        c_uint64,
        c_uint64,
        c_char_p,
        c_double,
        c_double,
    )
    fn(
        "sweeps_writer_record_segment_boundary",
        _STATUS,
        _HANDLE,
        c_uint64,
        c_uint64,
        c_char_p,
    )
    fn(
        "sweeps_writer_record_throttle_changed",
        _STATUS,
        _HANDLE,
        c_uint64,
        c_uint64,
        c_char_p,
        c_double,
    )
    fn(
        "sweeps_writer_record_device_error",
        _STATUS,
        _HANDLE,
        c_uint64,
        c_uint64,
        c_char_p,
        c_char_p,
    )
    fn(
        "sweeps_writer_record_plugin_event",
        _STATUS,
        _HANDLE,
        c_uint64,
        c_uint64,
        c_char_p,
        c_char_p,
        _HANDLE,
    )
    fn(
        "sweeps_writer_plugin_data",
        _STATUS,
        _HANDLE,
        c_char_p,
        c_char_p,
        c_uint32,
        c_uint64,
        c_void_p,
        c_size_t,
    )
    fn("sweeps_writer_path", Str, _HANDLE)
    fn("sweeps_writer_bytes_written", c_uint64, _HANDLE)
    fn("sweeps_writer_lines_written", c_uint64, _HANDLE)
    fn("sweeps_writer_segment_count", c_uint32, _HANDLE)
    fn("sweeps_writer_last_frame_ns", c_uint64, _HANDLE)
    fn("sweeps_writer_retention_reached", c_int, _HANDLE)
    fn("sweeps_writer_retention_reason", Str, _HANDLE)
    fn("sweeps_writer_last_segment_reason", Str, _HANDLE)

    # Live streams
    fn("sweeps_stream_reader_create", _STATUS, c_uint32, POINTER(_HANDLE))
    fn("sweeps_stream_reader_destroy", None, _HANDLE)
    fn("sweeps_stream_reader_feed", _STATUS, _HANDLE, c_void_p, c_size_t)
    fn(
        "sweeps_stream_reader_next_record",
        _STATUS,
        _HANDLE,
        POINTER(StreamRecord),
        POINTER(c_int),
    )
    fn("sweeps_stream_reader_buffered", c_size_t, _HANDLE)
    fn("sweeps_stream_mirror_create", _STATUS, c_uint32, POINTER(_HANDLE))
    fn("sweeps_stream_mirror_destroy", None, _HANDLE)
    fn("sweeps_stream_mirror_apply", _STATUS, _HANDLE, POINTER(StreamRecord))
    fn("sweeps_stream_mirror_line", _STATUS, _HANDLE, POINTER(StreamLine))
    fn("sweeps_stream_mirror_segment", _STATUS, _HANDLE, POINTER(Segment))
    fn("sweeps_stream_mirror_segment_reason", Str, _HANDLE)
    fn("sweeps_stream_mirror_segment_device_id", Str, _HANDLE)
    fn("sweeps_stream_mirror_segment_device_label", Str, _HANDLE)
    fn(
        "sweeps_stream_mirror_segment_gain",
        _STATUS,
        _HANDLE,
        c_size_t,
        POINTER(Str),
        POINTER(c_double),
    )
