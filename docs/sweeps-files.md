# `.sweeps` files

Everything Sweep++ records is saved as a `.sweeps` file. This page explains what
is inside one, and how to read and write them from your own programs.

The code that reads and writes them, `lib/libsweepsfile`, is a separate
**MIT-licensed** library with no dependencies, so anyone can use it, including
in closed-source software.

## What's in a recording

A `.sweeps` file stores the spectrum over time as a waterfall that can be
scrolled and zoomed, along with everything needed to interpret it:

- **The manifest:** the session's metadata, such as its name, when it was
  recorded, the version of the application that wrote it, and notes.
- **Segments:** a segment is a stretch of recording with one frequency grid and
  one set of acquisition settings (the radio, sample rate, FFT size, window and
  so on).
  Changing the range or resolution starts a new segment, so nothing recorded
  earlier is lost or resampled.
- **Tiles:** the levels themselves, stored in blocks of 1024 frequency bins by
  256 lines, one byte per level in 0.5 dB steps.
- **A level-of-detail pyramid:** each tile also exists at 1/8 and 1/64 of the
  time resolution, keeping the peak of what it summarises. A viewer showing
  three hours on one screen reads a few thousand lines rather than every one of
  them.
- **Events:** everything that happened while recording: retunes, setting
  changes, sweep passes, markers, detections, and data written by plugins.
- **An index**, written when the file is closed, so a reader can jump straight
  to any tile.

Every record in the file carries its own length and a checksum. If recording
stops without closing the file (a crash, or a power cut), a reader rebuilds the
index by scanning the file and recovers everything that reached the disk. The
History viewer shows **Rebuilt by scan** when that has happened.

Cutting a time and frequency range out of a recording copies tiles byte for
byte, so an extract is exactly the same data as the original.

## Reading and writing from other programs

There are four ways in, from needing nothing from this repository to needing a
matching C++ toolchain:

| Interface                                                         | Needs                                           | For                                                                |
|-------------------------------------------------------------------|-------------------------------------------------|--------------------------------------------------------------------|
| [`sweeps-format-v1.md`](../lib/libsweepsfile/sweeps-format-v1.md) | nothing                                         | Writing an implementation from scratch, in any language.           |
| The [`sweeps` tool](cli.md#sweeps)                                | running a program                               | Scripts, `jq`, quick inspection, CSV out of `sweeps dump`.         |
| `sweeps/sweeps.h`                                                 | a C99 compiler, or `ctypes`/`cffi`/`dlopen`     | Python, Rust, Go, C: a stable ABI over the full reader and writer. |
| `sweeps/*.hpp`                                                    | a C++17 toolchain compatible with the library's | C++ programs, with `Result<T>` and no conversion layer.            |

**The C header is the one that reaches programs in other languages.** The C++
API needs a compiler that agrees with the library's about name mangling and
`std::string` layout, and the `sweeps` tool needs a separate process. The C ABI
has neither restriction. `SWEEPS_ABI_VERSION` is fixed from the first release:
functions can be added and struct fields appended, but nothing existing changes
meaning. See
[the C section of the library's README](../lib/libsweepsfile/README.md#from-c-and-from-anything-that-speaks-c)
for its rules.

**Python** has a ctypes binding over the C ABI in
[`lib/libsweepsfile/python/`](../lib/libsweepsfile/python/). It needs no
compiler and no extension module, only a shared build of the library.

```python
import sweepsfile

with sweepsfile.open("session.sweeps") as session:
    print(session.name, session.summary.duration_seconds, "s")
    for tile in session.query(from_hz=88e6, to_hz=108e6, max_lines=1024):
        levels = tile.levels_db()   # (lines, bins) float32
```

**From the command line**, `sweeps dump` turns a recording into CSV, and
`sweeps manifest` and `sweeps events` print JSON. See [`sweeps`](cli.md#sweeps).

## Streams

The same records also travel live, as a stream (Appendix C of the format):
`sweeppp-cli record -o -` writes one to stdout. `sweeps.h` reads it:

- `sweeps_stream_reader_create`, `_feed` and `_next_record` check the stream
  header and split the bytes into records, however they arrive;
- `sweeps_stream_mirror_create`, `_apply`, `_line` and `_segment` keep the
  current line: its grid and its levels in dB.

```c
sweeps_stream_record_t record = {.struct_size = sizeof record};
sweeps_stream_line_t line = {.struct_size = sizeof line};

sweeps_stream_reader_feed(reader, buffer, got);
for (int has = 1; sweeps_stream_reader_next_record(reader, &record, &has) == SWEEPS_OK && has;)
    sweeps_stream_mirror_apply(mirror, &record);
sweeps_stream_mirror_line(mirror, &line);   /* line.levels[0 .. line.bin_count) */
```

Python has the same as `sweepsfile.StreamReader` and `StreamMirror`. A
server's encrypted link is not a plain stream: this API reads what
`record -o -` writes, not what `serve` sends.

## Using libsweepsfile on its own

The library is **MIT**, **C++17**, and depends on nothing: no radio driver, no
FFT library, no GUI and no threads. It builds and installs without the rest of
Sweep++:

```sh
cmake -S lib/libsweepsfile -B build/sweepsfile
cmake --build build/sweepsfile
ctest --test-dir build/sweepsfile
cmake --install build/sweepsfile --prefix /usr/local
```

Then, in your own project:

```cmake
find_package(sweepsfile REQUIRED)
target_link_libraries(app PRIVATE sweeps::sweepsfile)
```

That also works from a C-only project (`project(app LANGUAGES C)`), using
`sweeps/sweeps.h`. Build shared (`-DBUILD_SHARED_LIBS=ON`) for Python or any
other `dlopen`-style binding.

From the Sweep++ checkout, `make sweepsfile` builds and tests the library on its
own, at C++17 and C++23, static and shared, runs the Python binding's tests, and
checks that the installed package works from both a C++ and a C project. It is
part of `make check`, so the library's standalone build is tested on every
change rather than only as part of Sweep++.

## Further reading

- [`lib/libsweepsfile/README.md`](../lib/libsweepsfile/README.md): the C++, C
  and Python APIs, build options, and how conformance is tested.
- [`lib/libsweepsfile/sweeps-format-v1.md`](../lib/libsweepsfile/sweeps-format-v1.md):
  the byte-level format, in enough detail to write a reader in another language
  without reading the C++.
