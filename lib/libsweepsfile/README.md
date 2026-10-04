# libsweepsfile

Reader and writer for the `.sweeps` spectrum session container.

MIT · C++17 · no dependencies

`.sweeps` stores recorded radio-spectrum observations as a two-dimensionally
tiled, time-decimated pyramid, alongside the acquisition configuration that
produced them and a stream of events describing everything that changed while
recording. It is designed so that:

- **scrollback into a multi-hour session is cheap** — tiles are blocked in time
  *and* frequency, and a level-of-detail pyramid means drawing three hours does
  not mean reading three hours of lines;
- **a truncated file is readable** — every record carries its own length and a
  CRC, so a session ended by power loss recovers everything that reached the
  disk;
- **a mid-session parameter change destroys nothing** — each segment owns its
  own frequency grid and full configuration;
- **extraction is a copy** — cutting a time and frequency range out of a session
  is a tile copy plus a new index, bit-identical to the source.

The byte format is specified in [`sweeps-format-v1.md`](sweeps-format-v1.md), in
enough detail to write a reader in another language without reading any of this
C++. It installs alongside the headers, so `find_package(sweepsfile)` puts the
specification where the library is.

## Using it

```cmake
find_package(sweepsfile REQUIRED)
target_link_libraries(app PRIVATE sweeps::sweepsfile)
```

```cpp
#include <sweeps/SessionReader.hpp>

auto reader = sweeps::SessionReader::open("session.sweeps");
if (!reader) {
    return reader.error().describe();
}

sweeps::HistoryQuery query;
query.fromHz = 88e6;
query.toHz = 108e6;
query.maxLines = 1024;   // the reader picks the pyramid level that fits

for (const sweeps::HistoryTile& tile : (*reader)->query(query).value()) {
    // tile.data is row-major, `lines` rows of `bins` bytes -- byte-identical to
    // a waterfall texture. tile.dbAt(line, bin) dequantises.
}
```

Writing is `sweeps::SessionWriter`, which is **synchronous** and owns no thread,
queue or lock. A recorder that must not stall acquisition needs a queue, but the
shape of that queue is an application's decision; Sweep++'s
`session::SessionRecorder` is one such wrapper.

## From C, and from anything that speaks C

`sweeps/sweeps.h` is a stable C99 ABI over the same reader and writer. It exists
because everything above is C++: linking `SessionReader.hpp` needs a C++17
toolchain that agrees with this one about name mangling, `std::string` layout
and the exception ABI — which rules out Python, Rust, Go, C, and any C++ project
built by a different compiler. That is most of the audience the MIT licence and
the specification are for.

```c
#include <sweeps/sweeps.h>

sweeps_reader_t* reader = NULL;
if (sweeps_reader_open("session.sweeps", &reader) != SWEEPS_OK) {
    fprintf(stderr, "%s\n", sweeps_last_error().data);
    return 1;
}

sweeps_query_t query;
sweeps_query_init(&query);          /* never memset one; see below */
query.from_hz = 88e6;
query.to_hz   = 108e6;
query.max_lines = 1024;

sweeps_tiles_t* tiles = NULL;
sweeps_reader_query(reader, &query, &tiles);
for (size_t i = 0; i < sweeps_tiles_count(tiles); ++i) {
    sweeps_tile_info_t info;
    size_t bytes = 0;
    const uint8_t* data;

    info.struct_size = sizeof(info);
    sweeps_tile_info(sweeps_tiles_at(tiles, i), &info);
    data = sweeps_tile_data(sweeps_tiles_at(tiles, i), &bytes);
    /* row-major, info.lines rows of info.bins bytes;
       sweeps_dequantise_db(data[n], info.origin_db) gives dB */
}
sweeps_tiles_free(tiles);
sweeps_reader_close(reader);
```

Four rules cover the whole surface.

**Errors.** Every fallible call returns `sweeps_status_t`; `sweeps_last_error()`
carries the detail — which file, which offset — as a thread-local string valid
until the next call on that thread. No exception can cross the boundary: every
entry point is wrapped, so a `std::bad_alloc` arrives as
`SWEEPS_ERR_OUT_OF_MEMORY` rather than as `std::terminate`.

**Lifetimes.** Two categories, and only two. A `sweeps_str_t`, a
`sweeps_bytes_t` or a tile's bytes is *borrowed*: it points into the handle it
came from and is valid until that handle is closed, with nothing to free. A
`sweeps_reader_t`, `sweeps_writer_t`, `sweeps_tiles_t` or `sweeps_metadata_t` is
*owned* and released by its own call. JSON renderings belong to neither — they
write into a buffer you supply, sized by calling once with a capacity of zero.
No allocator crosses the boundary in either direction.

**Structs.** Each begins with `struct_size`, which you set to `sizeof`. The
library touches only `min(yours, its own)`, so a later release can append a
field without breaking a compiled caller. Every input struct with a non-zero
default has an `_init` function, and **you must use it**: a zeroed
`sweeps_query_t` asks for a time range ending at nanosecond zero and gets
nothing, successfully, and a zeroed `sweeps_acq_config_t` records a rectangular
window and an ENBW of zero as fact. A `struct_size` of zero is rejected, which
is what turns `= {0}` into a diagnosis rather than a silent wrong answer.

**Threads.** A reader is safe to use from several threads at once; a writer and
a metadata object are not.

`SWEEPS_ABI_VERSION` is the contract, and it is permanent from the first
release: functions may be added and struct fields appended, but no signature,
enumerator value or field meaning changes without a new ABI version. The library
version moves independently and answers a different question.

### Live streams

A `sweeps_stream_reader_t` splits a byte stream (Appendix C) into records,
checking the 16-byte stream header itself; a bad header, checksum or oversized
record breaks it for good. A `sweeps_stream_mirror_t` applies SegmentOpen, Tile
and SegmentClose records to the current line and ignores the rest. Encrypted
links are out of scope: feed the plaintext. In C++ the same is
`sweeps::RecordFramer` and `sweeps::LineMirror`; in Python, `StreamReader` and
`StreamMirror`.

```c
sweeps_stream_reader_feed(reader, buffer, got);
while (sweeps_stream_reader_next_record(reader, &record, &has) == SWEEPS_OK && has) {
    sweeps_stream_mirror_apply(mirror, &record);
    if (record.type == SWEEPS_RECORD_PLUGIN_DATA) {    /* the producer's commit */
        sweeps_stream_mirror_line(mirror, &line);    /* line.levels[0 .. bin_count) */
    }
}
```

`tests/consumer-c/stream.c` is a complete one:
`sweeppp-cli record --device synthetic --duration 2 -o - | sweeps-stream-c`.

### Linking from a project with no C++ compiler

`find_package(sweepsfile)` works from `project(app LANGUAGES C)`. It has to do
one thing to make that true: the library is C++ however it is built, and a
static archive of C++ objects carries no record of the runtime it needs, so the
package config enables `CXX` when the imported target is static. Without that a
C-only project links with the C driver and fails on `operator new` and
`__cxa_throw`. Building shared (`-DBUILD_SHARED_LIBS=ON`) needs none of this —
the runtime is in the library's own load commands — and is the better shape for
`dlopen`-style bindings such as Python's `ctypes`.

`tests/consumer-c/` is a `LANGUAGES C` consumer of the installed package that
writes and reads a session, and it is what keeps both halves of that honest.

## From Python

[`python/`](python/) is a ctypes binding over that C ABI — no compiler, no
extension module, no dependencies. numpy is used where it earns its place and is
not required.

```python
import sweepsfile

with sweepsfile.open("session.sweeps") as session:
    print(session.name, session.summary.duration_seconds, "s")
    print(session.manifest)                    # a plain dict

    for tile in session.query(from_hz=88e6, to_hz=108e6, max_lines=1024):
        levels = tile.levels_db()              # (lines, bins) float32
```

Writing is `sweepsfile.create()`, with the same reader/writer parity the C ABI
has. Everything is copied at the boundary, so results outlive the reader that
produced them — in C those are views into the mapping, and in Python an object
escapes its creating scope by default rather than by effort.

It needs a **shared** build (`-DBUILD_SHARED_LIBS=ON`): ctypes has nothing to
load out of a static archive. `make python` builds one and runs the suite
against it. See [`python/README.md`](python/README.md).

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
cmake --install build --prefix /usr/local
```

Or through this directory's own `Makefile`, which needs nothing above it:

|                |                                                           |
|----------------|-----------------------------------------------------------|
| `make build`   | the static library, CLI and tests                         |
| `make test`    | the suite, **install round-trips included**               |
| `make shared`  | a shared build, which is what C and ctypes consumers want |
| `make python`  | shared, plus the Python binding's suite                   |
| `make cxx23`   | the C++23 branch, where the API uses `std::expected`      |
| `make check`   | all of it                                                 |
| `make install` | to `PREFIX`, default `/usr/local`                         |

| Option                    | Default            | Effect                                                      |
|---------------------------|--------------------|-------------------------------------------------------------|
| `SWEEPSFILE_BUILD_TESTS`  | on when standalone | The conformance suite.                                      |
| `SWEEPSFILE_BUILD_CLI`    | `ON`               | The `sweeps` tool.                                          |
| `SWEEPSFILE_INSTALL`      | `ON`               | Install and export rules.                                   |
| `SWEEPSFILE_CXX_STANDARD` | `17`               | Standard to compile at. At 23 the API uses `std::expected`. |

The same directory can be added with `add_subdirectory()` from a larger project,
which is how Sweep++ consumes it.

## `expected`, and why there is no dependency

`Result<T>` is the library's fallible return type. Built at C++23 it is
`std::expected<T, Error>`; below that it is a copy of `tl::expected` vendored
into `sweeps/vendor/`, with its namespace and macros renamed. Both are detected
and fixed **when the library is configured**, and recorded in the generated
`sweeps/Config.hpp` — never re-decided in the header from the consumer's own
standard, which would let a consumer see a different type of a different layout
behind the same signatures.

So there is nothing to fetch, nothing to install alongside, and no
`find_dependency()` that can fail. A project already on C++23 configures with
`-DSWEEPSFILE_CXX_STANDARD=23` and gets `std::expected` across the boundary with
no conversion.

## The `sweeps` tool

```
sweeps info     <file>            metadata, segments, events
sweeps verify   <file>            walk every record, check every CRC
sweeps extract  <file> -o <out>   copy a time/frequency range, tiles unmodified
sweeps events   <file>            the event stream as JSON Lines
sweeps manifest <file>            the session metadata as JSON
sweeps plugins  <file>            the plugin records a session carries
sweeps dump     <file>            dequantised levels as CSV
```

`dump` is the one that matters for interoperability: it turns a session into
plain numbers for anyone with no C++ toolchain at all. `manifest` and `events`
emit JSON because their content nests — event bodies differ by kind, and no
fixed CSV header describes them:

```sh
sweeps manifest x.sweeps | jq -r .name
sweeps events   x.sweeps | jq -s 'group_by(.kind) | map({kind: .[0].kind, n: length})'
```

## Conformance

The suite includes a byte-frozen reference file,
`tests/data/v1-golden.sweeps`, which the current writer must reproduce byte for
byte. That, rather than a version number, is what says the format has not moved
— a version number cannot say it, because the failure mode is a change nobody
thought to bump a version for.

It is regenerated only when the format was *meant* to change, with
`SWEEPSFILE_UPDATE_GOLDEN=1`. `tests/data/v1-golden.sweepstream` is the same
for a live stream, and what the C and Python stream tests read.

`tests/consumer/` builds against the *installed* package rather than the source
tree, because "it installs" is a claim that is only true if something checks it.
`tests/consumer-c/` does the same in C, which is what catches the two ways the C
ABI can be published and be useless: an install rule that silently drops
`sweeps.h`, and a link that a C-only project cannot complete.

Both are ctest cases (`install`, `consumer-cxx`, `consumer-c`) rather than steps
in a script somewhere above this directory, so the library carries its own proof
and one `ctest` covers the build tree and the installed package alike. They
register only in a standalone build: under `add_subdirectory()`, `cmake
--install` would install the *parent* project, which is not the question being
asked.

`tests/test_capi.c` is compiled **as C**, which is the only thing that proves
`sweeps/sweeps.h` contains no C++ — the C++ suite would happily accept a header
that had grown a `namespace`. It asserts the same numbers about the golden file
that `test_golden.cpp` does, and `test_capi.cpp` compares the two APIs reading
that file side by side, down to the tile bytes.
