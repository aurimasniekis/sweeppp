# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""Failures, as exceptions.

The C ABI returns a status and parks the detail in a thread-local slot, which is
the right shape for C and the wrong one for Python: a status nobody checks is a
wrong answer, where an exception nobody catches is a traceback. Every call in
this package therefore checks, and raises.

Both halves are kept. ``SweepsError.status`` is the code the C API returned --
the thing to branch on -- and the message is what the library said, naming the
file and the offset. Discarding the second is what makes "corrupt" useless to
whoever has to fix it.
"""

from __future__ import annotations

from .enums import Status


class SweepsError(Exception):
    """A libsweepsfile call failed.

    Catch this for anything. The subclasses below exist for the handful of
    outcomes callers actually branch on; every other status raises this class
    with :attr:`status` set, so a code without its own subclass is still exact.
    """

    def __init__(self, status: Status | int, message: str = "") -> None:
        try:
            self.status = Status(status)
        except ValueError:
            # A status from a library newer than this binding. Keep the number
            # rather than refusing to construct the exception that reports it.
            self.status = status  # type: ignore[assignment]
        self.message = message
        super().__init__(message or f"libsweepsfile error {int(status)}")


class InvalidArgumentError(SweepsError):
    """A handle, pointer or field this library refused. Usually a caller bug."""


class NotFoundError(SweepsError):
    """No such file, segment, key or moment."""


class UnsupportedError(SweepsError):
    """Understood, but this build cannot do it -- a newer major format version."""


class IoError(SweepsError):
    """The filesystem said no."""


class ParseError(SweepsError):
    """The bytes are not what the format says they should be."""


class OutOfRangeError(SweepsError):
    """An index past the end. Raised as :class:`IndexError` where a sequence is
    being indexed; this is for the calls that are not sequence access."""


class CorruptError(SweepsError):
    """A checksum failed: what was read back is not what was written."""


class WrongTypeError(SweepsError):
    """A typed accessor was asked for a body or value of a type this item is not.

    The C ABI's own code, with no C++ counterpart. It is what keeps "this event
    is not a marker" from being answered with a plausible-looking reading of
    some other event's fields.
    """


_BY_STATUS = {
    Status.INVALID_ARGUMENT: InvalidArgumentError,
    Status.NOT_FOUND: NotFoundError,
    Status.UNSUPPORTED: UnsupportedError,
    Status.IO: IoError,
    Status.PARSE: ParseError,
    Status.OUT_OF_RANGE: OutOfRangeError,
    Status.CORRUPT: CorruptError,
    Status.WRONG_TYPE: WrongTypeError,
}


def raise_for_status(lib, status: int, context: str = "") -> None:
    """Turns a non-zero status into the matching exception.

    The message is read from ``sweeps_last_error`` immediately, before anything
    else on this thread can overwrite it -- which is the entire lifetime that
    slot promises.
    """
    if status == 0:
        return

    detail = lib.sweeps_last_error().text()
    if not detail:
        name = lib.sweeps_status_name(status)
        detail = name.decode("utf-8", "replace") if name else f"status {status}"
    if context:
        detail = f"{context}: {detail}"

    try:
        kind = _BY_STATUS.get(Status(status), SweepsError)
    except ValueError:
        kind = SweepsError
    raise kind(status, detail)
