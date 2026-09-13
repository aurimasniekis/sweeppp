// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

/* libsweepsfile -- the `.sweeps` session container, from C.
 *
 * This header is the library's stable ABI. Everything else it ships is C++:
 * linking `sweeps/SessionReader.hpp` requires a C++17-or-newer toolchain that
 * agrees with this one on name mangling, `std::string` layout and the exception
 * ABI, which rules out Python, Rust, Go, C and any C++ project built with a
 * different compiler. This header is how those reach the format.
 *
 * C99. It includes <stdint.h> and <stddef.h> and nothing else, deliberately --
 * `sweeps/Config.hpp` includes <cstdint> and declares a C++ function, so it is
 * not C-includable and SWEEPS_ABI_VERSION below is a literal rather than
 * generated from it.
 *
 * ---------------------------------------------------------------------------
 * ABI stability
 * ---------------------------------------------------------------------------
 *
 * SWEEPS_ABI_VERSION is a promise, and it is permanent from the first release.
 * Within one ABI version:
 *
 *   - functions may be added;
 *   - fields may be appended to a struct that carries `struct_size`;
 *   - enumerators may be added at the end of an enum whose values this library
 *     assigns (`sweeps_status_t`); enumerators mirroring the file format are
 *     frozen by the format itself and never change at all.
 *
 * Nothing else. A signature, an existing enumerator's value, a struct field's
 * offset or a struct field's meaning changing is a new ABI version. The library
 * version (`sweeps_library_version`) moves independently and answers a
 * different question: it says which release you have, not which contract it
 * honours.
 *
 * ---------------------------------------------------------------------------
 * Lifetimes
 * ---------------------------------------------------------------------------
 *
 * There are exactly two categories, and the rule differs:
 *
 *   Borrowed views -- `sweeps_str_t`, `sweeps_bytes_t`, and the pointer from
 *   `sweeps_tiles_data` -- point into the handle they came from and are valid
 *   until that handle is closed. Nothing is allocated for them and nothing is
 *   freed. The one exception is documented at each function that has it.
 *
 *   Owned handles -- `sweeps_reader_t`, `sweeps_writer_t`, `sweeps_tiles_t`,
 *   `sweeps_metadata_t` -- are released by their own `_close`, `_destroy` or
 *   `_free`. All are null-safe.
 *
 * Computed strings (the JSON renderings) belong to neither: they use the
 * two-call idiom, writing into a caller buffer. No allocator crosses this
 * boundary in either direction, so there is nothing here to leak.
 *
 * ---------------------------------------------------------------------------
 * Threads
 * ---------------------------------------------------------------------------
 *
 * A `sweeps_reader_t` may be used concurrently from several threads: every
 * operation on it is read-only, and the library holds no global state. A
 * `sweeps_writer_t` and a `sweeps_metadata_t` are single-threaded, as is the
 * last-error slot -- which is thread-local, so a failure on one thread is
 * never observed on another.
 *
 * ---------------------------------------------------------------------------
 * Text
 * ---------------------------------------------------------------------------
 *
 * Every `const char*` in and every `sweeps_str_t` out is UTF-8, paths included.
 * `str` in the format is length-prefixed UTF-8 and may contain anything a byte
 * string can, so `sweeps_str_t` carries a length -- but it is also always
 * NUL-terminated, because it is always a view of a `std::string` or a literal.
 * Passing `.data` to `printf("%s")` is safe; it may simply stop early.
 */
#ifndef SWEEPS_SWEEPS_H
#define SWEEPS_SWEEPS_H

#include <stddef.h>
#include <stdint.h>

/* Symbol visibility.
 *
 * SWEEPS_BUILD_SHARED is baked into this header's use by CMake when the library
 * was built shared, so a consumer cannot get the import/export decision wrong
 * by guessing. SWEEPS_BUILDING_LIBRARY is set only while compiling the library
 * itself. */
#if defined(_WIN32)
#if defined(SWEEPS_BUILD_SHARED)
#if defined(SWEEPS_BUILDING_LIBRARY)
#define SWEEPS_API __declspec(dllexport)
#else
#define SWEEPS_API __declspec(dllimport)
#endif
#else
#define SWEEPS_API
#endif
#elif defined(__GNUC__)
#define SWEEPS_API __attribute__((visibility("default")))
#else
#define SWEEPS_API
#endif

/* Since C++17 `noexcept` is part of a function's type, so the declaration here
 * and the definition in the library must agree or the definition does not
 * compile. Deliberately absent from the callback typedefs: a C++ consumer
 * passing an ordinary function pointer would otherwise fail to compile. */
#ifdef __cplusplus
#define SWEEPS_NOEXCEPT noexcept
#else
#define SWEEPS_NOEXCEPT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Version
 * -------------------------------------------------------------------------- */

/** The ABI this header describes. See the stability note at the top. */
#define SWEEPS_ABI_VERSION 1

/** The ABI the linked library implements. Compare against SWEEPS_ABI_VERSION:
 *  a mismatch means the header and the binary disagree, and nothing below is
 *  safe to call. */
SWEEPS_API uint32_t sweeps_abi_version(void) SWEEPS_NOEXCEPT;

/** The container version this build writes, and the highest it fully
 *  understands. Either pointer may be null. */
SWEEPS_API void sweeps_format_version(uint32_t* major, uint32_t* minor) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Strings and byte spans
 * -------------------------------------------------------------------------- */

/** A borrowed UTF-8 string. Always NUL-terminated at `data[len]`; see the text
 *  note at the top for why the length is carried anyway. `data` is never null
 *  for a successful call -- an absent or empty string is `{"", 0}`. */
typedef struct sweeps_str_t {
    const char* data;
    size_t len;
} sweeps_str_t;

/** A borrowed span of opaque bytes: a plugin record's body, an unknown event's
 *  body, a `bytes` metadata value. Distinct from `sweeps_str_t` because none of
 *  these is text and none is NUL-terminated. */
typedef struct sweeps_bytes_t {
    const uint8_t* data;
    size_t len;
} sweeps_bytes_t;

/** The library's own release, "1.0.0". Static storage; valid forever.
 *
 *  Not the same number as SWEEPS_ABI_VERSION and not derived from it: this
 *  moves whenever anything ships, the ABI version only when the contract
 *  changes. */
SWEEPS_API sweeps_str_t sweeps_library_version(void) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Errors
 * -------------------------------------------------------------------------- */

/** Why a call failed.
 *
 *  The values mirror the library's own `ErrorCode` but are assigned here and
 *  frozen here: deriving them from the C++ enum's ordinal would mean that
 *  inserting an enumerator in the middle of that enum silently renumbered this
 *  ABI. `SWEEPS_ERR_WRONG_TYPE` has no C++ counterpart -- it is what the typed
 *  accessors report when asked for a body or value of a type the item is not. */
typedef enum sweeps_status_t {
    SWEEPS_OK = 0,

    SWEEPS_ERR_UNKNOWN = -1,
    SWEEPS_ERR_INVALID_ARGUMENT = -2,
    SWEEPS_ERR_NOT_FOUND = -3,
    SWEEPS_ERR_UNSUPPORTED = -4,
    SWEEPS_ERR_UNAVAILABLE = -5,
    SWEEPS_ERR_IO = -6,
    SWEEPS_ERR_PARSE = -7,
    SWEEPS_ERR_OUT_OF_RANGE = -8,
    SWEEPS_ERR_OUT_OF_MEMORY = -9,
    SWEEPS_ERR_TIMED_OUT = -10,
    SWEEPS_ERR_CANCELLED = -11,
    SWEEPS_ERR_PERMISSION_DENIED = -12,
    SWEEPS_ERR_ALREADY_EXISTS = -13,
    SWEEPS_ERR_DEVICE = -14,
    SWEEPS_ERR_PROTOCOL = -15,
    SWEEPS_ERR_CORRUPT = -16,

    SWEEPS_ERR_WRONG_TYPE = -17,

    /* Pins the enum's width for bindings that read this header mechanically. */
    SWEEPS_STATUS_FORCE_INT32 = 0x7FFFFFFF
} sweeps_status_t;

/** The status as text -- "corrupt", "not found". Static storage, never null,
 *  including for a value this build does not know. */
SWEEPS_API const char* sweeps_status_name(sweeps_status_t status) SWEEPS_NOEXCEPT;

/** What went wrong, in words: which file, which offset, which record.
 *
 *  The status says how to branch; this says what to show an operator, and
 *  throwing it away would discard the more useful half. Thread-local, so a
 *  failure on another thread is never seen here, and never null -- `{"", 0}`
 *  when this thread has not failed yet.
 *
 *  Valid until the next libsweepsfile call **on this thread**. Successful calls
 *  do not clear it: the status is the discriminant, and clearing would put a
 *  thread-local write on paths that are otherwise pure reads. */
SWEEPS_API sweeps_str_t sweeps_last_error(void) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Format enumerations
 *
 * Mirrored from the format rather than remapped. These numbers are on disk and
 * frozen by the specification, so a second numbering would be a second thing to
 * keep in step, for no gain.
 * -------------------------------------------------------------------------- */

/** The FFT window a recording was taken with. Specification §4.3.1. */
typedef enum sweeps_window_type_t {
    SWEEPS_WINDOW_RECTANGULAR = 0,
    SWEEPS_WINDOW_HANN = 1,
    SWEEPS_WINDOW_HAMMING = 2,
    SWEEPS_WINDOW_BLACKMAN_HARRIS = 3,
    SWEEPS_WINDOW_FLAT_TOP = 4,
    SWEEPS_WINDOW_KAISER = 5,
    SWEEPS_WINDOW_FORCE_INT32 = 0x7FFFFFFF
} sweeps_window_type_t;

/** Record kinds in the chunk stream. Reported by nothing here yet; present
 *  because a `.sweeps` reader written against this header needs the vocabulary
 *  the specification uses. */
typedef enum sweeps_record_type_t {
    SWEEPS_RECORD_MANIFEST = 1,
    SWEEPS_RECORD_SEGMENT_OPEN = 2,
    SWEEPS_RECORD_SEGMENT_CLOSE = 3,
    SWEEPS_RECORD_EVENT = 4,
    SWEEPS_RECORD_TILE = 5,
    SWEEPS_RECORD_INDEX = 6,
    SWEEPS_RECORD_END_OF_STREAM = 7,
    SWEEPS_RECORD_TELEMETRY = 8,
    SWEEPS_RECORD_PLUGIN_DATA = 9,
    SWEEPS_RECORD_FORCE_INT32 = 0x7FFFFFFF
} sweeps_record_type_t;

/** Event kinds.
 *
 *  Carried as a `uint16_t` everywhere rather than as this enum, so that a kind
 *  written by a newer producer survives a read instead of being forced into a
 *  neighbouring meaning. `SWEEPS_EVENT_ALERT` is reserved: it is named, nothing
 *  writes it, and its body reads as an unknown one. */
typedef enum sweeps_event_kind_t {
    SWEEPS_EVENT_RETUNE = 1,
    SWEEPS_EVENT_PARAMETER_CHANGED = 2,
    SWEEPS_EVENT_SWEEP_PASS = 3,
    SWEEPS_EVENT_MARKER = 4,
    SWEEPS_EVENT_ANNOTATION = 5,
    SWEEPS_EVENT_ALERT = 6,
    SWEEPS_EVENT_SEGMENT_BOUNDARY = 7,
    SWEEPS_EVENT_THROTTLE_CHANGED = 8,
    SWEEPS_EVENT_DEVICE_ERROR = 9,
    SWEEPS_EVENT_PLUGIN = 10,
    SWEEPS_EVENT_FORCE_INT32 = 0x7FFFFFFF
} sweeps_event_kind_t;

/** Metadata value types. Specification §4.2; the numbering is on the wire. */
typedef enum sweeps_value_type_t {
    SWEEPS_VALUE_ABSENT = 0, /**< Not a wire tag: what `sweeps_metadata_type_of`
                                  reports for a key that is not there. */
    SWEEPS_VALUE_STRING = 1,
    SWEEPS_VALUE_INT = 2,
    SWEEPS_VALUE_FLOAT = 3,
    SWEEPS_VALUE_BOOL = 4,
    SWEEPS_VALUE_BYTES = 5,
    SWEEPS_VALUE_HASH = 6,
    SWEEPS_VALUE_ARRAY = 7,
    SWEEPS_VALUE_FORCE_INT32 = 0x7FFFFFFF
} sweeps_value_type_t;

/** Levels the library logs at. */
typedef enum sweeps_log_level_t {
    SWEEPS_LOG_TRACE = 0,
    SWEEPS_LOG_DEBUG = 1,
    SWEEPS_LOG_INFO = 2,
    SWEEPS_LOG_WARN = 3,
    SWEEPS_LOG_ERROR = 4,
    SWEEPS_LOG_FORCE_INT32 = 0x7FFFFFFF
} sweeps_log_level_t;

/** dB per stored quantisation step, and the span 256 of them cover. */
#define SWEEPS_DB_PER_STEP 0.5

/** Record types and event kinds from here up are private and experimental use,
 *  and must not appear in an interchanged file. */
#define SWEEPS_PRIVATE_USE_FIRST 0xFF00

/** Tile geometry. Fixed by the format: a tile is at most this many lines by
 *  this many bins, and both are needed to size a buffer for `sweeps_tile_data`
 *  without asking. */
#define SWEEPS_TILE_LINES 256
#define SWEEPS_TILE_BINS 1024

/** Time-decimation levels: native, /8 and /64. */
#define SWEEPS_LOD_LEVELS 3

/** The stored byte that means no measurement was ever made in that bin, and
 *  the dB value it converts to.
 *
 *  Coverage is not a level. A sweep of two disjoint spans never looks at the
 *  frequencies between them, and those bins carry this rather than a reading at
 *  the bottom of the scale -- so a consumer can leave them blank instead of
 *  drawing a noise floor the radio never measured. Measured levels are 1..255.
 */
#define SWEEPS_UNMEASURED_BYTE 0
#define SWEEPS_UNMEASURED_DB (-200.0)

/** Stored byte to dB, relative to a tile's own origin.
 *
 *  Here rather than left to the caller because getting it wrong is silent: the
 *  levels come out plausible and forty decibels off.
 *
 *  `SWEEPS_UNMEASURED_BYTE` converts to `SWEEPS_UNMEASURED_DB` whatever the
 *  origin. */
SWEEPS_API double sweeps_dequantise_db(uint8_t value, double origin_db) SWEEPS_NOEXCEPT;

/** dB to stored byte. Saturates at both ends rather than wrapping, and yields
 *  `SWEEPS_UNMEASURED_BYTE` for anything that is not a reading -- the
 *  unmeasured sentinel, a value below -190 dB, or a non-finite one. */
SWEEPS_API uint8_t sweeps_quantise_db(double db, double origin_db) SWEEPS_NOEXCEPT;

/** The canonical spelling of a window -- "hann", "blackman-harris". Static
 *  storage. This one reaches file bytes, in a segment's reason string. */
SWEEPS_API sweeps_str_t sweeps_window_type_name(sweeps_window_type_t window) SWEEPS_NOEXCEPT;

/** The text spelling of a raw event kind id -- "retune", "plugin", "unknown"
 *  for a kind this build does not know. Static storage. */
SWEEPS_API sweeps_str_t sweeps_event_kind_name(uint16_t kind) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Handles
 * -------------------------------------------------------------------------- */

/** An open file. Read-only, and safe to use from several threads at once. */
typedef struct sweeps_reader_t sweeps_reader_t;

/** A file being written. Single-threaded: every call writes on the calling
 *  thread, and the library owns no queue or lock. */
typedef struct sweeps_writer_t sweeps_writer_t;

/** The result of one `sweeps_reader_query`. Owned; free it. */
typedef struct sweeps_tiles_t sweeps_tiles_t;

/** One tile within a `sweeps_tiles_t`. Borrowed; valid until it is freed. */
typedef struct sweeps_tile_t sweeps_tile_t;

/** One event within a reader. Borrowed; valid until the reader is closed. */
typedef struct sweeps_event_t sweeps_event_t;

/** One plugin record within a reader. Borrowed likewise. */
typedef struct sweeps_plugin_record_t sweeps_plugin_record_t;

/** A typed key/value object: a session manifest, a plugin event's fields.
 *
 *  One type in two roles, told apart by `const`. A `const sweeps_metadata_t*` is
 *  borrowed -- the reader's manifest, a nested hash -- and lives as long as
 *  whatever it came from. A mutable one comes from `sweeps_metadata_create`,
 *  belongs to the caller, and is what the writer takes. */
typedef struct sweeps_metadata_t sweeps_metadata_t;

/** Where the library's diagnostics go: a damaged index, a truncated tail, a
 *  tile that failed its checksum.
 *
 *  Called on the thread that emitted the record, and never after the handle it
 *  was given to has been closed -- so a binding may free whatever `user` points
 *  at as soon as `sweeps_reader_close` or `sweeps_writer_destroy` returns. The
 *  sink must not throw, longjmp, or call back into this library on the same
 *  handle: it runs mid-parse.
 *
 *  Both `sweeps_str_t`s are valid for the duration of the call only. */
typedef void (*sweeps_log_fn_t)(void* user, sweeps_log_level_t level, sweeps_str_t category,
                                sweeps_str_t message);

/* --------------------------------------------------------------------------
 * Structs
 *
 * Every one begins with `uint32_t struct_size`, which the caller sets to
 * `sizeof` the struct. One rule, both directions: the library reads and writes
 * only `min(caller's size, its own)`, so a later release can append a field
 * without breaking an already-compiled caller.
 *
 *   - A `struct_size` of 0 is rejected. That is what makes `sweeps_query_t q =
 *     {0}` a loud error rather than a query for an empty time range.
 *   - An *input* struct larger than this library knows is rejected too: the
 *     caller set fields that would be silently dropped, and dropping them is
 *     worse than refusing.
 *   - On an *output* struct the library sets `struct_size` to how many bytes it
 *     actually filled, so a caller newer than the library can tell.
 *
 * Every input struct with a non-zero default has an `_init` function. Use it.
 * Zeroing one by hand is not equivalent: a zeroed `sweeps_query_t` asks for a
 * time range that ends at zero, and a zeroed `sweeps_acq_config_t` claims a
 * rectangular window and an ENBW of zero -- both of which are answered
 * faithfully rather than diagnosed.
 * -------------------------------------------------------------------------- */

/** How a session file was opened. Output only.
 *
 *  The session name and the writing application's version are accessors rather
 *  than fields: both are arbitrary-length strings, and a fixed char array here
 *  would mean silently truncating one. */
typedef struct sweeps_summary_t {
    uint32_t struct_size;

    /** The container version triple from the file header. Three values because
     *  that is what the file carries -- reporting only the major would hide the
     *  very thing the minor version exists to communicate. */
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t incompatible_features;

    uint64_t created_wall_ns;

    uint64_t total_lines;
    uint64_t total_tiles;
    uint64_t file_bytes;

    uint64_t first_line_ns;
    uint64_t last_line_ns;

    /** Bytes past the last intact record. Non-zero for a session that ended
     *  abruptly; the rest of the file was still read. */
    uint64_t truncated_bytes;

    double lowest_hz;
    double highest_hz;

    /** Non-zero when the index was missing or unusable and the file was
     *  recovered by scanning. Surfaced rather than hidden: an operator should
     *  know a session ended abruptly. */
    int32_t recovered_by_scan;

    /** Non-zero when the file's minor version is newer than this build knows.
     *  Not an error -- the file was read and additive content was skipped. */
    int32_t newer_minor_version;
} sweeps_summary_t;

/** One acquisition configuration's worth of session. Output only.
 *
 *  The acquisition scalars are spelled out here rather than nested as a
 *  `sweeps_acq_config_t`, because that struct is an *input* type: its `gains`
 *  member is a C array the caller owns, and the reader's gains live in a shape
 *  no C pointer can address. Reading them is `sweeps_reader_segment_gain`.
 *
 *  `reason`, `device_id` and `device_label` are accessors for the same reason
 *  the summary's name is. */
typedef struct sweeps_segment_t {
    uint32_t struct_size;

    /** The segment's identity, **not its position**. An extracted file
     *  legitimately contains segments whose ids neither start at zero nor run
     *  contiguously, which is why `sweeps_reader_segment_index_of` exists. */
    uint32_t id;

    uint32_t bin_count;
    uint32_t fft_size;
    uint32_t window; /**< A `sweeps_window_type_t`, as a fixed-width field. */
    uint32_t gain_count;

    uint64_t start_wall_ns;
    uint64_t start_monotonic_ns;
    uint64_t end_monotonic_ns; /**< 0 while the segment is still open. */
    uint64_t line_count;

    double start_hz;
    double bin_width_hz;

    double center_hz;
    double span_hz;
    double sample_rate;
    double window_beta;
    /** Equivalent noise bandwidth in bins, carried explicitly so a reader need
     *  not know how to regenerate the window to interpret the RBW. */
    double window_enbw;
    double overlap;
    double rbw_hz;
    double reference_level_dbm;
    /** Correction from dBFS to dBm for this device and gain setting. Applied at
     *  display time rather than baked into the stored data, so a later
     *  calibration fix applies retroactively to old sessions. */
    double dbfs_to_dbm_offset;
} sweeps_segment_t;

/** One gain stage, by parameter key. Free-form because it must describe any
 *  device without the format knowing which radios exist. */
typedef struct sweeps_gain_t {
    const char* name;
    double value;
} sweeps_gain_t;

/** A time and frequency range to fetch.
 *
 *  Shaped as tiles rather than "give me frames" on purpose: a history viewer,
 *  the extraction path and a tile endpoint are then all the same call.
 *
 *  **Use `sweeps_query_init`.** A zeroed query asks for everything up to
 *  nanosecond zero and hertz zero, and gets nothing, with no error to explain
 *  it. */
typedef struct sweeps_query_t {
    uint32_t struct_size;

    /** Caps on what comes back. The reader picks the coarsest pyramid level
     *  that satisfies `max_lines`, which is what keeps "draw three hours" from
     *  meaning "read three hours of lines". */
    uint32_t max_lines;
    uint32_t max_bins;

    /** Restrict to one segment, **by id**. Zero in `has_segment_id` means every
     *  segment overlapping the range -- which is what a query spanning a
     *  mid-session parameter change should return. */
    uint32_t segment_id;
    int32_t has_segment_id;

    /** Serve exactly this pyramid level, ignoring `max_lines`.
     *
     *  Leave `has_lod` zero for the normal case. A caller that knows which
     *  level it wants -- an export, a conformance test -- should say so rather
     *  than reverse-engineer a `max_lines` that happens to select it. A level
     *  with no tiles returns nothing rather than silently falling back. */
    uint32_t lod;
    int32_t has_lod;

    uint64_t from_ns;
    uint64_t to_ns;

    double from_hz;
    double to_hz;
} sweeps_query_t;

/** Fills a query with the defaults the C++ API uses: the whole session, 2048
 *  lines, 4096 bins, every segment, an automatic level. Also sets
 *  `struct_size`, so a caller never types it. */
SWEEPS_API void sweeps_query_init(sweeps_query_t* query) SWEEPS_NOEXCEPT;

/** One returned tile's extents.
 *
 *  Returned rather than assumed, because the reader may have served a coarser
 *  level than asked for, and because a query spanning a parameter change
 *  returns tiles from segments with different grids. */
typedef struct sweeps_tile_info_t {
    uint32_t struct_size;

    uint32_t segment_id;
    uint32_t lod;

    /** The tile's position in the segment's grid. Carried rather than
     *  reconstructed from `start_hz` and `first_line_ns`: reassembling full
     *  waterfall rows out of several frequency blocks needs both, and deriving
     *  them from floating-point frequencies is a rounding bug waiting to
     *  happen. */
    uint32_t time_block;
    uint32_t freq_block;

    uint32_t lines;
    uint32_t bins;

    /** dB the stored byte 0 represents. Per tile rather than per file, so a
     *  quiet band and a loud one each get the full 255-step range. */
    float origin_db;

    uint64_t first_line_ns;
    uint64_t last_line_ns;

    double start_hz;
    double bin_width_hz;
} sweeps_tile_info_t;

/** What an extraction records about itself.
 *
 *  The one input struct that is safe to zero beyond `struct_size`: an empty
 *  version means this library's own, and a zero timestamp means now. */
typedef struct sweeps_extract_options_t {
    uint32_t struct_size;

    /** Written to the manifest's `app_version`. Null or empty means this
     *  library's version, which is the honest answer when nothing else is
     *  supplied. */
    const char* application_version;

    /** Manifest `created` and the file header's stamp. 0 reads the wall
     *  clock. */
    uint64_t created_wall_ns;
} sweeps_extract_options_t;

SWEEPS_API void sweeps_extract_options_init(sweeps_extract_options_t* options) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Reader
 * -------------------------------------------------------------------------- */

/** Opens a `.sweeps` file, discarding diagnostics.
 *
 *  The file is mapped, so scrollback into a multi-hour session never loads the
 *  whole thing: a tile query touches only the pages it needs.
 *
 *  `path` is UTF-8 on every platform, Windows included. On success `*out`
 *  receives a reader to close with `sweeps_reader_close`; on failure it is set
 *  to null and the detail is in `sweeps_last_error`. */
SWEEPS_API sweeps_status_t sweeps_reader_open(const char* path,
                                              sweeps_reader_t** out) SWEEPS_NOEXCEPT;

/** The same, with somewhere for diagnostics to go.
 *
 *  Worth using. A damaged index, a truncated tail and an unreadable manifest
 *  are all recovered from rather than refused, and this callback is the only
 *  way to learn that any of them happened -- a blank session name with no
 *  explanation is the failure mode that matters here. `log` may be null. */
SWEEPS_API sweeps_status_t sweeps_reader_open_ex(const char* path, sweeps_log_fn_t log,
                                                 void* log_user,
                                                 sweeps_reader_t** out) SWEEPS_NOEXCEPT;

/** Closes a reader and invalidates every view taken from it. Null-safe. */
SWEEPS_API void sweeps_reader_close(sweeps_reader_t* reader) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_reader_summary(const sweeps_reader_t* reader,
                                                 sweeps_summary_t* out) SWEEPS_NOEXCEPT;

/** The session's name, and the version of the application that wrote it.
 *  Valid until the reader is closed. */
SWEEPS_API sweeps_str_t sweeps_reader_name(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT;
SWEEPS_API sweeps_str_t sweeps_reader_app_version(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT;

/** The session manifest, typed. Never null for a valid reader; empty when the
 *  file carries no manifest or the one it carries could not be decoded, which
 *  the log sink says. Valid until the reader is closed. */
SWEEPS_API const sweeps_metadata_t*
sweeps_reader_manifest(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT;

/* --- Segments --- */

SWEEPS_API size_t sweeps_reader_segment_count(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_reader_segment_at(const sweeps_reader_t* reader, size_t index,
                                                    sweeps_segment_t* out) SWEEPS_NOEXCEPT;

/** The position of the segment with this id, for the accessors that take one.
 *
 *  Ids are looked up rather than indexed because an extracted file contains
 *  segments whose ids neither start at zero nor run contiguously.
 *  `SWEEPS_ERR_NOT_FOUND` when there is no such segment. */
SWEEPS_API sweeps_status_t sweeps_reader_segment_index_of(const sweeps_reader_t* reader,
                                                          uint32_t id,
                                                          size_t* out_index) SWEEPS_NOEXCEPT;

/** Why this segment was opened -- "session start", "sample rate changed". All
 *  three return `{"", 0}` for an index that is out of range. */
SWEEPS_API sweeps_str_t sweeps_reader_segment_reason(const sweeps_reader_t* reader,
                                                     size_t index) SWEEPS_NOEXCEPT;
SWEEPS_API sweeps_str_t sweeps_reader_segment_device_id(const sweeps_reader_t* reader,
                                                        size_t index) SWEEPS_NOEXCEPT;
SWEEPS_API sweeps_str_t sweeps_reader_segment_device_label(const sweeps_reader_t* reader,
                                                           size_t index) SWEEPS_NOEXCEPT;

SWEEPS_API size_t sweeps_reader_segment_gain_count(const sweeps_reader_t* reader,
                                                   size_t index) SWEEPS_NOEXCEPT;

/** One gain stage of one segment. Either out-pointer may be null. */
SWEEPS_API sweeps_status_t sweeps_reader_segment_gain(const sweeps_reader_t* reader,
                                                      size_t segment_index, size_t gain_index,
                                                      sweeps_str_t* out_name,
                                                      double* out_value) SWEEPS_NOEXCEPT;

/* --- Integrity and extraction --- */

/** Walks every record and verifies every checksum, reporting how many were
 *  checked or failing at the first bad one. */
SWEEPS_API sweeps_status_t sweeps_reader_verify(const sweeps_reader_t* reader,
                                                uint64_t* out_records) SWEEPS_NOEXCEPT;

/** Copies a time and frequency range into a new standalone `.sweeps`.
 *
 *  A tile copy plus a new index and manifest -- no re-encoding and no
 *  re-quantisation, so the extracted tiles are bit-identical to the source.
 *  That is what the two-dimensional tiling exists for. `options` may be null
 *  for the defaults.
 *
 *  Only `from_ns`/`to_ns` and `from_hz`/`to_hz` of `range` are consulted: the
 *  parameter is a range, not a query. `segment_id`, `lod` and the line and bin
 *  caps shape a *read*, and an extraction copies whole tiles or none. Segment
 *  ids are preserved rather than renumbered, so the result's ids neither start
 *  at zero nor run contiguously -- which is what
 *  `sweeps_reader_segment_index_of` is for. */
SWEEPS_API sweeps_status_t sweeps_reader_extract(
    const sweeps_reader_t* reader, const char* destination, const sweeps_query_t* range,
    const sweeps_extract_options_t* options) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Tiles
 * -------------------------------------------------------------------------- */

/** Fetches the tiles covering a query.
 *
 *  `*out` receives a list to release with `sweeps_tiles_free`. It is owned
 *  rather than borrowed because the reader assembles it per call; every view
 *  taken from it dies with it. */
SWEEPS_API sweeps_status_t sweeps_reader_query(const sweeps_reader_t* reader,
                                               const sweeps_query_t* query,
                                               sweeps_tiles_t** out) SWEEPS_NOEXCEPT;

SWEEPS_API void sweeps_tiles_free(sweeps_tiles_t* tiles) SWEEPS_NOEXCEPT;

SWEEPS_API size_t sweeps_tiles_count(const sweeps_tiles_t* tiles) SWEEPS_NOEXCEPT;

/** The tile at this position, or null when out of range. Valid until the list
 *  is freed. */
SWEEPS_API const sweeps_tile_t* sweeps_tiles_at(const sweeps_tiles_t* tiles,
                                                size_t index) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_tile_info(const sweeps_tile_t* tile,
                                            sweeps_tile_info_t* out) SWEEPS_NOEXCEPT;

/** The tile's quantised levels: `lines` rows of `bins` bytes, row-major.
 *
 *  Byte-identical to a waterfall texture, so an upload is a memcpy. `out_bytes`
 *  receives `lines * bins` and may be null. Null on a null tile. */
SWEEPS_API const uint8_t* sweeps_tile_data(const sweeps_tile_t* tile,
                                           size_t* out_bytes) SWEEPS_NOEXCEPT;

/** The pyramid level whose line spacing best fits `max_lines` over a range.
 *
 *  Never returns a level with no tiles: short sessions never fill a coarse
 *  tile, and a truncated one loses the coarse levels first. */
SWEEPS_API uint32_t sweeps_reader_choose_lod(const sweeps_reader_t* reader, uint64_t from_ns,
                                             uint64_t to_ns, uint32_t max_lines,
                                             uint32_t segment_id) SWEEPS_NOEXCEPT;

/** Non-zero when any tile exists for this segment at this level. */
SWEEPS_API int sweeps_reader_has_tiles_at_lod(const sweeps_reader_t* reader, uint32_t segment_id,
                                              uint32_t lod) SWEEPS_NOEXCEPT;

/** Reconstructs the spectrum at one instant, so clicking a waterfall line can
 *  show that moment in a linked plot.
 *
 *  Written into the caller's buffer rather than returned, because the C++ call
 *  returns a vector by value and a view into it would dangle the moment this
 *  function returned. `*out_count` always receives the segment's bin count, so
 *  the two-call idiom works: pass `capacity` 0 to size, then again to fill.
 *  Fewer than `*out_count` bins written is not an error -- compare the two. */
SWEEPS_API sweeps_status_t sweeps_reader_spectrum_at(const sweeps_reader_t* reader,
                                                     uint64_t monotonic_ns, uint32_t segment_id,
                                                     float* out, size_t capacity,
                                                     size_t* out_count) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Events
 * -------------------------------------------------------------------------- */

SWEEPS_API size_t sweeps_reader_event_count(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT;

/** The event at this position, or null when out of range. Valid until the
 *  reader is closed.
 *
 *  There is deliberately no range filter here. The C++ API has one; in C it
 *  would return a list to own and free for something every caller can do with
 *  a loop over two timestamps.  */
SWEEPS_API const sweeps_event_t* sweeps_reader_event_at(const sweeps_reader_t* reader,
                                                        size_t index) SWEEPS_NOEXCEPT;

/** The raw wire id, which may be a kind this build does not know. */
SWEEPS_API uint16_t sweeps_event_kind(const sweeps_event_t* event) SWEEPS_NOEXCEPT;

SWEEPS_API uint64_t sweeps_event_monotonic_ns(const sweeps_event_t* event) SWEEPS_NOEXCEPT;
SWEEPS_API uint64_t sweeps_event_wall_ns(const sweeps_event_t* event) SWEEPS_NOEXCEPT;
SWEEPS_API uint32_t sweeps_event_segment_id(const sweeps_event_t* event) SWEEPS_NOEXCEPT;

/* One accessor per kind. Each answers for its own body only: asking a marker
 * for a retune is `SWEEPS_ERR_WRONG_TYPE`, never a plausible-looking reading of
 * other fields. Every out-pointer is optional.
 *
 * Kinds this build does not know -- and `SWEEPS_EVENT_ALERT`, which is reserved
 * and has no defined body -- are readable only through
 * `sweeps_event_unknown_body`, which keeps their bytes verbatim. */

SWEEPS_API sweeps_status_t sweeps_event_retune(const sweeps_event_t* event, double* out_center_hz,
                                               uint32_t* out_step_index) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_event_parameter_changed(
    const sweeps_event_t* event, sweeps_str_t* out_key, sweeps_str_t* out_value,
    int* out_grid_affecting, int* out_calibration_affecting) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_event_sweep_pass(const sweeps_event_t* event,
                                                   uint64_t* out_pass_id, double* out_start_hz,
                                                   double* out_stop_hz,
                                                   double* out_duration_seconds) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_event_marker(const sweeps_event_t* event, sweeps_str_t* out_label,
                                               double* out_frequency_hz,
                                               double* out_level_dbm) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_event_annotation(const sweeps_event_t* event,
                                                   sweeps_str_t* out_text, double* out_start_hz,
                                                   double* out_stop_hz) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_event_segment_boundary(const sweeps_event_t* event,
                                                         sweeps_str_t* out_reason) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t
sweeps_event_throttle_changed(const sweeps_event_t* event, sweeps_str_t* out_reason,
                              double* out_processed_fraction) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_event_device_error(const sweeps_event_t* event,
                                                     sweeps_str_t* out_device_id,
                                                     sweeps_str_t* out_message) SWEEPS_NOEXCEPT;

/** A plugin's own event. `plugin_id` is reverse-DNS -- `org.sweeppp.bandplan` --
 *  which is what keeps two plugins from colliding without a registry.
 *  `*out_fields` borrows from the reader. */
SWEEPS_API sweeps_status_t sweeps_event_plugin(
    const sweeps_event_t* event, sweeps_str_t* out_plugin_id, sweeps_str_t* out_event_name,
    const sweeps_metadata_t** out_fields) SWEEPS_NOEXCEPT;

/** The body of an event whose kind this build does not know, byte for byte.
 *  `SWEEPS_ERR_WRONG_TYPE` for a kind it does. */
SWEEPS_API sweeps_status_t sweeps_event_unknown_body(const sweeps_event_t* event,
                                                     sweeps_bytes_t* out_body) SWEEPS_NOEXCEPT;

/** The event's body as JSON, whatever its kind.
 *
 *  Two-call: pass `capacity` 0 to learn the length, then again to fill.
 *  `*out_length` is the length excluding the terminating NUL; up to `capacity`
 *  bytes are written including it, truncating like `snprintf`. Truncation is
 *  not an error -- compare `*out_length` with `capacity`.
 *
 *  The rendering goes through the same code as `sweeps events --json`, so a
 *  kind this build has never seen comes out as hex rather than as nothing. */
SWEEPS_API sweeps_status_t sweeps_event_body_json(const sweeps_event_t* event, int indent,
                                                  char* buffer, size_t capacity,
                                                  size_t* out_length) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Plugin records
 * -------------------------------------------------------------------------- */

SWEEPS_API size_t sweeps_reader_plugin_count(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT;

/** The plugin record at this position, or null when out of range. */
SWEEPS_API const sweeps_plugin_record_t* sweeps_reader_plugin_at(const sweeps_reader_t* reader,
                                                                 size_t index) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_str_t sweeps_plugin_id(const sweeps_plugin_record_t* record) SWEEPS_NOEXCEPT;
SWEEPS_API sweeps_str_t sweeps_plugin_name(const sweeps_plugin_record_t* record) SWEEPS_NOEXCEPT;
SWEEPS_API uint32_t sweeps_plugin_schema_version(const sweeps_plugin_record_t* record)
    SWEEPS_NOEXCEPT;

/** 0 when the record is not tied to a moment, which is what keeps it in an
 *  extraction whatever range was asked for. */
SWEEPS_API uint64_t sweeps_plugin_monotonic_ns(const sweeps_plugin_record_t* record)
    SWEEPS_NOEXCEPT;

/** The record's body, opaque to the container.
 *
 *  A view into the mapping rather than a copy: the checksum was verified during
 *  the scan, so handing back a pointer costs nothing and materialises no blob
 *  the caller never asked for. Valid until the reader is closed. */
SWEEPS_API sweeps_status_t sweeps_plugin_body(const sweeps_plugin_record_t* record,
                                              sweeps_bytes_t* out_body) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Metadata -- reading
 *
 * `find` in the C++ API returns null for an absent key, and these keep that
 * distinction: a `bytes` value of length zero, an empty string and an empty
 * hash are all *present*. Use `sweeps_metadata_type_of` or the status of a
 * pointer-returning accessor to tell them apart -- the value-returning
 * accessors take a fallback and cannot.
 * -------------------------------------------------------------------------- */

SWEEPS_API size_t sweeps_metadata_count(const sweeps_metadata_t* metadata) SWEEPS_NOEXCEPT;

/** The key at this position, in ascending byte order -- the order the format
 *  stores them in, which is what makes a byte-frozen file possible.
 *  `out_type` may be null. */
SWEEPS_API sweeps_status_t sweeps_metadata_key_at(const sweeps_metadata_t* metadata, size_t index,
                                                  sweeps_str_t* out_key,
                                                  sweeps_value_type_t* out_type) SWEEPS_NOEXCEPT;

SWEEPS_API int sweeps_metadata_contains(const sweeps_metadata_t* metadata,
                                        const char* key) SWEEPS_NOEXCEPT;

/** `SWEEPS_VALUE_ABSENT` when the key is not there. */
SWEEPS_API sweeps_value_type_t sweeps_metadata_type_of(const sweeps_metadata_t* metadata,
                                                       const char* key) SWEEPS_NOEXCEPT;

/** Each answers for its own type only and returns `fallback` otherwise. No
 *  coercion: asking for the wrong type gets the stated default rather than a
 *  plausible-looking reading of other data.
 *
 *  The string is borrowed from the metadata object and lives as long as it
 *  does; `fallback` is returned as given, so it must outlive the view too. */
SWEEPS_API sweeps_str_t sweeps_metadata_get_string(const sweeps_metadata_t* metadata,
                                                   const char* key,
                                                   const char* fallback) SWEEPS_NOEXCEPT;
SWEEPS_API int64_t sweeps_metadata_get_i64(const sweeps_metadata_t* metadata, const char* key,
                                           int64_t fallback) SWEEPS_NOEXCEPT;
SWEEPS_API double sweeps_metadata_get_f64(const sweeps_metadata_t* metadata, const char* key,
                                          double fallback) SWEEPS_NOEXCEPT;
SWEEPS_API int sweeps_metadata_get_bool(const sweeps_metadata_t* metadata, const char* key,
                                        int fallback) SWEEPS_NOEXCEPT;

/** `SWEEPS_ERR_NOT_FOUND` when the key is absent, `SWEEPS_ERR_WRONG_TYPE` when
 *  it holds something else -- which is how a zero-length `bytes` value stays
 *  distinguishable from no value at all. */
SWEEPS_API sweeps_status_t sweeps_metadata_get_bytes(const sweeps_metadata_t* metadata,
                                                     const char* key,
                                                     sweeps_bytes_t* out_value) SWEEPS_NOEXCEPT;

/** A nested hash, borrowed from its parent. Same two failures as above. */
SWEEPS_API sweeps_status_t
sweeps_metadata_get_hash(const sweeps_metadata_t* metadata, const char* key,
                         const sweeps_metadata_t** out_value) SWEEPS_NOEXCEPT;

/** The whole tree as JSON, for a human or for `jq`.
 *
 *  Two-call, exactly as `sweeps_event_body_json`. An `indent` of zero or less
 *  emits the compact form. Note that both calls render the document, so size
 *  once generously and reuse the buffer if this is in a loop.
 *
 *  Write-only by design: there is no JSON parser here and no round trip through
 *  text, so there is no quoting matrix to get wrong. Arrays are readable this
 *  way and have no typed accessor, which is the deliberate trade -- a typed one
 *  would have to answer for seven element types. */
SWEEPS_API sweeps_status_t sweeps_metadata_to_json(const sweeps_metadata_t* metadata, int indent,
                                                   char* buffer, size_t capacity,
                                                   size_t* out_length) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Metadata -- building
 *
 * For the writer. A mutable object is the caller's; a `const` one from a reader
 * cannot be passed here, which C's type system enforces at the call site.
 *
 * Every setter copies. Nothing here retains a pointer the caller passed in.
 * -------------------------------------------------------------------------- */

/** A new, empty object. Null only when out of memory. */
SWEEPS_API sweeps_metadata_t* sweeps_metadata_create(void) SWEEPS_NOEXCEPT;

/** Releases it, and every view taken from it. Null-safe. */
SWEEPS_API void sweeps_metadata_destroy(sweeps_metadata_t* metadata) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_metadata_set_string(sweeps_metadata_t* metadata, const char* key,
                                                      const char* value) SWEEPS_NOEXCEPT;
SWEEPS_API sweeps_status_t sweeps_metadata_set_i64(sweeps_metadata_t* metadata, const char* key,
                                                   int64_t value) SWEEPS_NOEXCEPT;
SWEEPS_API sweeps_status_t sweeps_metadata_set_f64(sweeps_metadata_t* metadata, const char* key,
                                                   double value) SWEEPS_NOEXCEPT;
SWEEPS_API sweeps_status_t sweeps_metadata_set_bool(sweeps_metadata_t* metadata, const char* key,
                                                    int value) SWEEPS_NOEXCEPT;
SWEEPS_API sweeps_status_t sweeps_metadata_set_bytes(sweeps_metadata_t* metadata, const char* key,
                                                     const void* data,
                                                     size_t bytes) SWEEPS_NOEXCEPT;

/** Copies `value` in as a nested hash. `value` may be a reader's, and stays the
 *  caller's afterwards either way. */
SWEEPS_API sweeps_status_t sweeps_metadata_set_hash(sweeps_metadata_t* metadata, const char* key,
                                                    const sweeps_metadata_t* value) SWEEPS_NOEXCEPT;

/* --------------------------------------------------------------------------
 * Writer
 * -------------------------------------------------------------------------- */

/** Everything that produced a measurement. Input only; see `sweeps_segment_t`.
 *
 *  **This is the structure the format's forward compatibility rests on.**
 *  Without it a recording is uninterpretable: bins are just numbers unless you
 *  know the centre, span, sample rate, FFT size, window and gain that produced
 *  them. Every segment records one in full, so tiles written under any past
 *  configuration stay interpretable without reference to anything outside the
 *  file.
 *
 *  **Use `sweeps_acq_config_init`.** Zeroing this one claims a rectangular
 *  window and an ENBW of zero, and both are written into the file as fact.
 *
 *  Every pointer is borrowed for the duration of the call that takes it. */
typedef struct sweeps_acq_config_t {
    uint32_t struct_size;

    uint32_t fft_size;
    uint32_t window; /**< A `sweeps_window_type_t`, as a fixed-width field. */

    double center_hz;
    double span_hz;
    double sample_rate;
    double window_beta;
    double window_enbw;
    double overlap;
    /** sample_rate * window_enbw / fft_size. */
    double rbw_hz;
    double reference_level_dbm;
    double dbfs_to_dbm_offset;

    const char* device_id;
    const char* device_label;

    const sweeps_gain_t* gains;
    size_t gain_count;
} sweeps_acq_config_t;

SWEEPS_API void sweeps_acq_config_init(sweeps_acq_config_t* config) SWEEPS_NOEXCEPT;

typedef struct sweeps_writer_config_t {
    uint32_t struct_size;

    /** Bins written per line. Frames are decimated onto this grid before
     *  storage. Display resolution -- about 2048 -- is roughly 150 MB an hour;
     *  full sweep resolution is roughly 6 GB an hour. */
    uint32_t bins_per_line;

    /** Retention caps. 0 means unbounded, except `min_free_bytes`, where 0
     *  disables the free-space guard entirely rather than setting it to
     *  nothing. `sweeps_writer_config_init` sets it to 512 MiB. */
    uint64_t max_bytes;
    double max_seconds;
    uint64_t min_free_bytes;

    const char* session_name;
    const char* notes;

    /** Recorded in the manifest as `app_version`. Null or empty means this
     *  library's own version.
     *
     *  Injectable because it lands in file bytes: pinning it, together with
     *  `created_wall_ns`, is what makes a byte-for-byte reproducible file
     *  possible, and that is what proves a change did not move the format. */
    const char* application_version;

    /** The file header's creation timestamp and the manifest's `created`.
     *  0 reads the wall clock now. */
    uint64_t created_wall_ns;

    /** Where diagnostics go. May be null. */
    sweeps_log_fn_t log;
    void* log_user;
} sweeps_writer_config_t;

SWEEPS_API void sweeps_writer_config_init(sweeps_writer_config_t* config) SWEEPS_NOEXCEPT;

/** One spectrum, as the writer needs to see it.
 *
 *  A view rather than a frame type of its own: the library has no opinion about
 *  how an application represents a spectrum, owns no buffer, and copies nothing
 *  it does not store. */
typedef struct sweeps_frame_t {
    uint32_t struct_size;

    const float* bins;
    size_t count;

    /** Absolute frequency of the first bin, and the spacing between bins. A
     *  partial sweep frame covers only its step's slice, so neither is
     *  derivable from the configuration alone. */
    double start_hz;
    double bin_width_hz;

    uint64_t monotonic_ns;
    uint64_t wall_ns;

    /** Never null. A change here is what opens a new segment. */
    const sweeps_acq_config_t* config;
} sweeps_frame_t;

SWEEPS_API void sweeps_frame_init(sweeps_frame_t* frame) SWEEPS_NOEXCEPT;

/** What one frame did to the file.
 *
 *  The reason a segment opened is not a field here, because this struct is
 *  filled from one the C++ writer returns by value -- a pointer into it would
 *  dangle before the call returned. It is
 *  `sweeps_writer_last_segment_reason` instead. */
typedef struct sweeps_frame_outcome_t {
    uint32_t struct_size;

    uint32_t segment_id;

    /** Non-zero when this frame's configuration or grid differed from the open
     *  segment's, so a new one was opened. */
    int32_t segment_opened;

    /** Non-zero once a retention cap has stopped the writer. Further frames are
     *  accepted and ignored. */
    int32_t retention_stopped;
} sweeps_frame_outcome_t;

/** Creates a `.sweeps` file. `path` is UTF-8; `config` may be null for the
 *  defaults `sweeps_writer_config_init` would set. */
SWEEPS_API sweeps_status_t sweeps_writer_create(const char* path,
                                                const sweeps_writer_config_t* config,
                                                sweeps_writer_t** out) SWEEPS_NOEXCEPT;

/** Finishes the file: flushes tiles, writes the index, patches the header.
 *  Idempotent.
 *
 *  **Call this, and check it.** `sweeps_writer_destroy` closes too, but it has
 *  no way to report that writing the index failed -- and a caller who never
 *  closed explicitly would learn about a full disk from a truncated file
 *  weeks later. A session ended by power loss is still readable, by design;
 *  one ended by an ignored error need not be. */
SWEEPS_API sweeps_status_t sweeps_writer_close(sweeps_writer_t* writer) SWEEPS_NOEXCEPT;

/** Closes if still open, discarding any error from doing so, and frees.
 *  Null-safe. */
SWEEPS_API void sweeps_writer_destroy(sweeps_writer_t* writer) SWEEPS_NOEXCEPT;

/** Stores one spectrum, opening a new segment if the grid changed.
 *  `outcome` may be null. */
SWEEPS_API sweeps_status_t
sweeps_writer_write_frame(sweeps_writer_t* writer, const sweeps_frame_t* frame,
                          sweeps_frame_outcome_t* outcome) SWEEPS_NOEXCEPT;

/* Events, one entry point per kind rather than a builder: emulating a variant
 * of ten alternatives as a mutable C object would be twenty functions and a
 * state machine, for a call that is a single line either way.
 *
 * The record's segment id is the segment open at that moment, not anything the
 * caller supplies: a live event is stamped where it actually landed. */

SWEEPS_API sweeps_status_t sweeps_writer_record_retune(sweeps_writer_t* writer,
                                                       uint64_t monotonic_ns, uint64_t wall_ns,
                                                       double center_hz,
                                                       uint32_t step_index) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_writer_record_parameter_changed(
    sweeps_writer_t* writer, uint64_t monotonic_ns, uint64_t wall_ns, const char* key,
    const char* value, int grid_affecting, int calibration_affecting) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_writer_record_sweep_pass(sweeps_writer_t* writer,
                                                           uint64_t monotonic_ns, uint64_t wall_ns,
                                                           uint64_t pass_id, double start_hz,
                                                           double stop_hz,
                                                           double duration_seconds) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_writer_record_marker(sweeps_writer_t* writer,
                                                       uint64_t monotonic_ns, uint64_t wall_ns,
                                                       const char* label, double frequency_hz,
                                                       double level_dbm) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_writer_record_annotation(sweeps_writer_t* writer,
                                                           uint64_t monotonic_ns, uint64_t wall_ns,
                                                           const char* text, double start_hz,
                                                           double stop_hz) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t
sweeps_writer_record_segment_boundary(sweeps_writer_t* writer, uint64_t monotonic_ns,
                                      uint64_t wall_ns, const char* reason) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_writer_record_throttle_changed(
    sweeps_writer_t* writer, uint64_t monotonic_ns, uint64_t wall_ns, const char* reason,
    double processed_fraction) SWEEPS_NOEXCEPT;

SWEEPS_API sweeps_status_t sweeps_writer_record_device_error(sweeps_writer_t* writer,
                                                             uint64_t monotonic_ns,
                                                             uint64_t wall_ns,
                                                             const char* device_id,
                                                             const char* message) SWEEPS_NOEXCEPT;

/** A plugin's own event. `fields` may be null for none. */
SWEEPS_API sweeps_status_t sweeps_writer_record_plugin_event(
    sweeps_writer_t* writer, uint64_t monotonic_ns, uint64_t wall_ns, const char* plugin_id,
    const char* event_name, const sweeps_metadata_t* fields) SWEEPS_NOEXCEPT;

/** A producer's own record, opaque to the container.
 *
 *  `monotonic_ns` of 0 means the record is not tied to a moment, which is what
 *  keeps it in an extraction whatever range was asked for. */
SWEEPS_API sweeps_status_t sweeps_writer_plugin_data(sweeps_writer_t* writer, const char* plugin_id,
                                                     const char* record_name,
                                                     uint32_t schema_version, uint64_t monotonic_ns,
                                                     const void* body,
                                                     size_t body_bytes) SWEEPS_NOEXCEPT;

/* --- What the writer has done so far --- */

/** The path this writer was created with, as given. Valid until it is
 *  destroyed. */
SWEEPS_API sweeps_str_t sweeps_writer_path(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT;

SWEEPS_API uint64_t sweeps_writer_bytes_written(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT;
SWEEPS_API uint64_t sweeps_writer_lines_written(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT;
SWEEPS_API uint32_t sweeps_writer_segment_count(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT;

/** Timestamp of the most recent frame written, which is what defines a
 *  segment's end. */
SWEEPS_API uint64_t sweeps_writer_last_frame_ns(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT;

/** Non-zero once a retention cap has stopped the writer, and why. */
SWEEPS_API int sweeps_writer_retention_reached(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT;
SWEEPS_API sweeps_str_t sweeps_writer_retention_reason(const sweeps_writer_t* writer)
    SWEEPS_NOEXCEPT;

/** Why the most recent segment opened, in the words written into the file.
 *
 *  Empty until a frame opens one. **Valid only until the next call on this
 *  writer** -- the one view in this header that does not live as long as its
 *  handle, because the string it mirrors is rebuilt per frame. Copy it if you
 *  need it later. */
SWEEPS_API sweeps_str_t sweeps_writer_last_segment_reason(const sweeps_writer_t* writer)
    SWEEPS_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SWEEPS_SWEEPS_H */
