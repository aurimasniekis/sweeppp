# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""The loaded shared library, as a process-wide singleton.

One handle rather than one per reader: ``ctypes.CDLL`` caches at the OS level
anyway, and the prototypes in :mod:`sweepsfile._ffi` are attached to the object,
so loading twice would mean declaring twice for no gain.

Loading is deferred until something needs it. Importing ``sweepsfile`` to read
``__version__`` should not fail on a machine where the library is not installed.
"""

from __future__ import annotations

import ctypes
import threading

from . import _ffi

_lock = threading.Lock()
_library: ctypes.CDLL | None = None


def library() -> ctypes.CDLL:
    """The loaded library, loading it on first use."""
    global _library
    if _library is None:
        with _lock:
            if _library is None:
                _library = _ffi.load()
    return _library


def load_library(path: str) -> ctypes.CDLL:
    """Loads a specific ``libsweepsfile`` and uses it from here on.

    For a checkout with several builds, or a test that must run against the one
    it just compiled. ``SWEEPSFILE_LIBRARY`` in the environment does the same
    thing without a code change.
    """
    global _library
    with _lock:
        _library = _ffi.load(path)
    return _library
