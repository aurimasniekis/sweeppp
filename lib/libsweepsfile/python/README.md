# sweepsfile

Read and write `.sweeps` spectrum session files from Python.

MIT · no dependencies · numpy optional

A [ctypes](https://docs.python.org/3/library/ctypes.html) binding over
`libsweepsfile`'s C ABI. No compiler, no extension module, no build step — which
is the whole reason the C ABI exists, and would be handed straight back by a
binding that needed a toolchain to install.

```python
import sweepsfile

with sweepsfile.open("session.sweeps") as session:
    print(session.name, session.summary.duration_seconds, "s")

    for segment in session.segments:
        print(f"segment {segment.id}: {segment.center_hz/1e6:.3f} MHz, "
              f"RBW {segment.rbw_hz:.1f} Hz, gains {segment.gains}")

    for tile in session.query(from_hz=88e6, to_hz=108e6, max_lines=1024):
        levels = tile.levels_db()      # (lines, bins) float32 — numpy
```

## Requirements

A **shared** `libsweepsfile`. ctypes has nothing to load out of a static
archive, so build the library with `-DBUILD_SHARED_LIBS=ON`:

```sh
cmake -S lib/libsweepsfile -B build/shared -DBUILD_SHARED_LIBS=ON
cmake --build build/shared
cmake --install build/shared --prefix /usr/local
```

It is found at import time: `SWEEPSFILE_LIBRARY` if set, then beside this
package, then a build tree in the checkout, then the loader's own search path.
`sweepsfile.load_library(path)` does the same from code. A wheel deliberately
does not carry one — it would have to pick an architecture, a C++ runtime and a
version of the format, and be wrong for anyone who built the library themselves.

## Reading

`open()` returns a `Reader`; use it as a context manager.

| | |
|---|---|
| `.name`, `.app_version` | who wrote it |
| `.summary` | versions, extents, counts, `duration_seconds`, `created` |
| `.manifest` | the session metadata, as a plain `dict` |
| `.segments` | one per acquisition configuration, with `.gains` |
| `.events` | the event stream, decoded on access |
| `.plugin_records` | producers' own records |
| `.query(**bounds)` | tiles covering a time and frequency range |
| `.spectrum_at(ns, segment_id)` | one instant, in dB |
| `.verify()` | walk every record, check every CRC |
| `.extract(path, **range)` | copy a range to a new standalone file |

**Everything is copied at the boundary.** In C these are views into the mapping,
valid until the reader closes; in Python an object escapes its creating scope by
default, so a `dict` or a `Tile` that pointed into a closed reader would be a
use-after-free with no traceback to explain it. Results stay valid after
`close()`. The reader itself does not, and using a closed one raises.

`events` is lazy — a long session carries a great many events and decoding all
of them to answer `len()` would make opening a file cost what reading it should.
It indexes, slices and iterates like a list.

### Tiles

A tile is `lines` rows of `bins` quantised bytes, row-major and byte-identical
to a waterfall texture.

```python
tile.data                  # bytes, always
tile.db_at(line, bin)      # one cell in dB, always
tile.levels()              # (lines, bins) uint8   — numpy
tile.levels_db()           # (lines, bins) float32 — numpy
```

The numpy methods have no list-of-lists fallback on purpose: a tile is up to a
quarter of a million cells, and materialising that as Python ints would be
slower and larger than the file it came from.

## Writing

```python
from sweepsfile import AcquisitionConfig

config = AcquisitionConfig(
    center_hz=100e6, span_hz=2.5e6, sample_rate=2.5e6, fft_size=1024,
    device_id="hackrf-0", gains={"lna": 24.0, "vga": 16.0},
)

with sweepsfile.create("out.sweeps", bins_per_line=2048, session_name="scan") as writer:
    for frame in frames:
        writer.write_frame(frame.bins, config=config,
                           start_hz=frame.start_hz, bin_width_hz=frame.bin_width_hz,
                           monotonic_ns=frame.t)
    writer.record_marker(t, wall, "carrier", 99.5e6, -41.5)
```

Leaving the `with` block closes **and checks**: closing is what flushes the
pending tiles, writes the index and patches the header, and a failure there is
how you learn the disk filled. A session ended by power loss is still readable
by design; one ended by an ignored error need not be.

`write_frame` takes a numpy array, an `array('f')`, or any sequence of floats —
levels in dB, not bytes. `AcquisitionConfig`'s defaults are the library's own,
not zeros: the C API needs an `_init` function because a zeroed config claims a
rectangular window and an ENBW of zero and writes both into the file as fact,
and a dataclass makes that impossible by accident.

Metadata — a plugin event's fields — is any mapping, mapped by type:

| format | Python |
|---|---|
| `string` `int` `float` `bool` `bytes` | `str` `int` `float` `bool` `bytes` |
| `hash` | `dict` |
| `array` | `list`, **read-only** |

`bool` is checked before `int`, because in Python it *is* one and the obvious
ordering would record every flag as a 0 or 1 of type int. Lists are refused
rather than written: the format's arrays are homogeneous and coerce a mismatched
element to its type's default, so writing one could quietly lose data — encode
the list yourself and make the loss your decision.

## Errors

Every call checks its status and raises. `SweepsError` carries both halves the C
API keeps: `.status` is the code to branch on, and the message names the file
and the offset.

```python
try:
    session = sweepsfile.open(path)
except sweepsfile.NotFoundError:
    ...
except sweepsfile.CorruptError as e:
    print(e.status, e)
```

## Tests

```sh
SWEEPSFILE_LIBRARY=/path/to/libsweepsfile.so python3 -m unittest discover -s tests -t .
```

They assert the same numbers about the frozen reference session that the C and
C++ suites do. Three languages reading one file and agreeing about what is in it
is the claim; a binding that agreed only with itself would be testing nothing.
The `python` ctest case runs them automatically in a `-DBUILD_SHARED_LIBS=ON`
build.
