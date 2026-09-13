// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

/// The C ABI declared in `sweeps/sweeps.h`.
///
/// Everything here is a thin shim: no format knowledge lives in this file, and
/// nothing below reimplements anything the C++ API already does. What it does
/// own is the two things C cannot express -- the translation of `Result<T>` into
/// a status plus a thread-local message, and the guarantee that no exception
/// ever reaches a C frame.

#include "sweeps/Config.hpp"
#include "sweeps/FileFormat.hpp"
#include "sweeps/Metadata.hpp"
#include "sweeps/Result.hpp"
#include "sweeps/SessionReader.hpp"
#include "sweeps/SessionWriter.hpp"
#include "sweeps/WindowType.hpp"
#include "sweeps/sweeps.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Handles.
//
// Real structs rather than a `SessionReader*` in disguise, because each has to
// carry something the C++ object does not: a stashed path, a scratch string a
// view can point at, and a magic number.
//
// The magic is not decoration. Use-after-free and passing the wrong handle are
// the two commonest defects in hand-written bindings, and one comparison per
// entry point turns both from a crash somewhere later into
// SWEEPS_ERR_INVALID_ARGUMENT here. It is zeroed on destruction, so a freed
// handle fails the check for as long as the memory stays mapped.
// ---------------------------------------------------------------------------

namespace {
constexpr std::uint32_t kReaderMagic = 0x53575052; // 'SWPR'
constexpr std::uint32_t kWriterMagic = 0x53575057; // 'SWPW'
constexpr std::uint32_t kTilesMagic = 0x53575054;  // 'SWPT'
} // namespace

struct sweeps_reader_t {
    std::uint32_t magic = kReaderMagic;
    std::unique_ptr<sweeps::SessionReader> reader;
};

struct sweeps_writer_t {
    std::uint32_t magic = kWriterMagic;
    std::unique_ptr<sweeps::SessionWriter> writer;
    /// The caller's own UTF-8 spelling, kept because `SessionWriter::path()`
    /// returns a `filesystem::path` whose `.string()` builds a temporary -- and
    /// can throw on Windows for a path the active code page cannot represent.
    std::string path;
    /// `FrameOutcome` is returned by value, so its `reason` dies with the
    /// temporary. Mirrored here so a view of it can outlive the call that
    /// produced it, for exactly as long as the header promises: until the next
    /// call on this writer.
    std::string lastSegmentReason;
};

struct sweeps_tiles_t {
    std::uint32_t magic = kTilesMagic;
    std::vector<sweeps::HistoryTile> tiles;
};

namespace {

// ---------------------------------------------------------------------------
// The last-error slot.
// ---------------------------------------------------------------------------

/// Thread-local rather than an out-parameter on seventy functions.
///
/// The errno shape is what FFI generators expect and what every comparable C
/// library does; an out-param that ninety-five percent of call sites pass null
/// for is noise at every one of them. Thread-local is what makes it safe: a
/// failure on one thread is never observed on another, so two readers running
/// concurrently cannot overwrite each other's diagnosis.
std::string& lastErrorSlot() noexcept {
    // Function-local so construction is ordered, and `noexcept` because a
    // throwing initialiser here would terminate inside the error path itself.
    // A thread_local std::string's constructor allocates nothing.
    static thread_local std::string message;
    return message;
}

/// Records a message, and cannot itself fail.
///
/// The caller is frequently an out-of-memory handler, so an allocation here
/// that threw would replace a reportable failure with a crash. On that path the
/// slot keeps whatever it held, which is worse than the true message and far
/// better than terminating.
void setLastError(std::string message) noexcept {
    try {
        lastErrorSlot() = std::move(message);
    } catch (...) {
    }
}

void setLastError(const char* message) noexcept {
    try {
        lastErrorSlot().assign(message);
    } catch (...) {
    }
}

sweeps_status_t toStatus(sweeps::ErrorCode code) noexcept {
    // Spelled out, with no `default:`, so that adding an ErrorCode enumerator
    // is a -Wswitch warning here rather than a silent collapse into UNKNOWN.
    switch (code) {
    case sweeps::ErrorCode::Unknown:
        return SWEEPS_ERR_UNKNOWN;
    case sweeps::ErrorCode::InvalidArgument:
        return SWEEPS_ERR_INVALID_ARGUMENT;
    case sweeps::ErrorCode::NotFound:
        return SWEEPS_ERR_NOT_FOUND;
    case sweeps::ErrorCode::Unsupported:
        return SWEEPS_ERR_UNSUPPORTED;
    case sweeps::ErrorCode::Unavailable:
        return SWEEPS_ERR_UNAVAILABLE;
    case sweeps::ErrorCode::IoError:
        return SWEEPS_ERR_IO;
    case sweeps::ErrorCode::ParseError:
        return SWEEPS_ERR_PARSE;
    case sweeps::ErrorCode::OutOfRange:
        return SWEEPS_ERR_OUT_OF_RANGE;
    case sweeps::ErrorCode::OutOfMemory:
        return SWEEPS_ERR_OUT_OF_MEMORY;
    case sweeps::ErrorCode::TimedOut:
        return SWEEPS_ERR_TIMED_OUT;
    case sweeps::ErrorCode::Cancelled:
        return SWEEPS_ERR_CANCELLED;
    case sweeps::ErrorCode::PermissionDenied:
        return SWEEPS_ERR_PERMISSION_DENIED;
    case sweeps::ErrorCode::AlreadyExists:
        return SWEEPS_ERR_ALREADY_EXISTS;
    case sweeps::ErrorCode::DeviceError:
        return SWEEPS_ERR_DEVICE;
    case sweeps::ErrorCode::ProtocolError:
        return SWEEPS_ERR_PROTOCOL;
    case sweeps::ErrorCode::Corrupt:
        return SWEEPS_ERR_CORRUPT;
    }
    return SWEEPS_ERR_UNKNOWN;
}

/// Reports a C++ error through the C channel: the code becomes the status, the
/// message becomes the thread-local detail.
sweeps_status_t fail(const sweeps::Error& error) noexcept {
    setLastError(error.message());
    return toStatus(error.code());
}

/// Reports a failure this shim diagnosed itself, before reaching the library.
sweeps_status_t fail(sweeps_status_t status, const char* message) noexcept {
    setLastError(message);
    return status;
}

// ---------------------------------------------------------------------------
// The exception barrier.
// ---------------------------------------------------------------------------

/// Runs an entry point's body with no exception able to leave it.
///
/// An exception crossing into a C frame is `std::terminate`, not a bug report,
/// and the paths that can throw are not exotic: every `std::string`
/// construction, every vector growth, `std::filesystem` on a path it cannot
/// represent. Aborting on a large-but-valid tile would be a poor answer for a
/// library whose headline property is that a damaged file is still readable.
///
/// One wrapper rather than seventy try/catch blocks, and a lambda rather than a
/// macro: it formats, it steps in a debugger, and clangd understands it.
template <typename Body>
sweeps_status_t guard(Body&& body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        return fail(SWEEPS_ERR_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception& error) {
        return fail(SWEEPS_ERR_UNKNOWN, error.what());
    } catch (...) {
        return fail(SWEEPS_ERR_UNKNOWN, "unknown exception");
    }
}

/// The same barrier for the accessors that return a value rather than a status.
///
/// Most of these wrap a `noexcept` C++ accessor and cannot throw at all, but
/// wrapping every one uniformly removes "which of these did I check?" from
/// review, and costs nothing at runtime.
template <typename T, typename Body>
T guardValue(T fallback, Body&& body) noexcept {
    try {
        return body();
    } catch (...) {
        return fallback;
    }
}

/// The barrier for the destructors, which have nothing to report and nowhere to
/// report it.
template <typename Body>
void guardVoid(Body&& body) noexcept {
    try {
        body();
    } catch (...) {
    }
}

/// A `sweeps_str_t` over a string that outlives the call.
///
/// Every use is a view of a `std::string` member of a live handle or of a
/// string literal, which is what makes the NUL-termination guarantee in the
/// header true rather than aspirational.
sweeps_str_t view(const std::string& text) noexcept {
    sweeps_str_t out;
    out.data = text.c_str();
    out.len = text.size();
    return out;
}

sweeps_str_t view(const char* text, size_t len) noexcept {
    sweeps_str_t out;
    out.data = text;
    out.len = len;
    return out;
}

constexpr sweeps_str_t kEmptyStr{"", 0};

sweeps_bytes_t viewBytes(const void* data, std::size_t len) noexcept {
    sweeps_bytes_t out;
    out.data = static_cast<const std::uint8_t*>(data);
    out.len = len;
    return out;
}

// ---------------------------------------------------------------------------
// Handle validation.
// ---------------------------------------------------------------------------

const sweeps::SessionReader* readerOf(const sweeps_reader_t* handle) noexcept {
    return handle != nullptr && handle->magic == kReaderMagic ? handle->reader.get() : nullptr;
}

sweeps::SessionWriter* writerOf(const sweeps_writer_t* handle) noexcept {
    return handle != nullptr && handle->magic == kWriterMagic ? handle->writer.get() : nullptr;
}

const std::vector<sweeps::HistoryTile>* tilesOf(const sweeps_tiles_t* handle) noexcept {
    return handle != nullptr && handle->magic == kTilesMagic ? &handle->tiles : nullptr;
}

// The borrowed view types are the library's own objects under another name.
//
// No wrapper and no allocation: a `SessionEvent` lives in the reader's vector
// for the reader's lifetime, a nested `Metadata` inside its parent, a
// `PluginRecord` in its own vector. Handing back a pointer to one is exactly
// the lifetime the header promises, and interposing a wrapper would introduce
// an object with a shorter life than the thing it describes -- which is the bug
// this arrangement avoids rather than the one it risks.

const sweeps::SessionEvent* eventOf(const sweeps_event_t* handle) noexcept {
    return reinterpret_cast<const sweeps::SessionEvent*>(handle);
}

const sweeps::PluginRecord* pluginOf(const sweeps_plugin_record_t* handle) noexcept {
    return reinterpret_cast<const sweeps::PluginRecord*>(handle);
}

const sweeps::HistoryTile* tileOf(const sweeps_tile_t* handle) noexcept {
    return reinterpret_cast<const sweeps::HistoryTile*>(handle);
}

const sweeps::Metadata* metadataOf(const sweeps_metadata_t* handle) noexcept {
    return reinterpret_cast<const sweeps::Metadata*>(handle);
}

sweeps::Metadata* mutableMetadataOf(sweeps_metadata_t* handle) noexcept {
    return reinterpret_cast<sweeps::Metadata*>(handle);
}

const sweeps_metadata_t* asMetadataHandle(const sweeps::Metadata& metadata) noexcept {
    return reinterpret_cast<const sweeps_metadata_t*>(&metadata);
}

const sweeps_event_t* asEventHandle(const sweeps::SessionEvent& event) noexcept {
    return reinterpret_cast<const sweeps_event_t*>(&event);
}

const sweeps_plugin_record_t* asPluginHandle(const sweeps::PluginRecord& record) noexcept {
    return reinterpret_cast<const sweeps_plugin_record_t*>(&record);
}

const sweeps_tile_t* asTileHandle(const sweeps::HistoryTile& tile) noexcept {
    return reinterpret_cast<const sweeps_tile_t*>(&tile);
}

// ---------------------------------------------------------------------------
// struct_size marshalling.
//
// One rule in both directions: neither side ever reads or writes past the
// smaller of the two declarations. That is what lets a later release append a
// field without breaking a caller compiled against today's header.
// ---------------------------------------------------------------------------

/// A `struct_size` smaller than its own field cannot describe anything, and
/// would leave the field itself half written. Rejected rather than honoured.
constexpr std::uint32_t kMinStructSize = sizeof(std::uint32_t);

/// Copies as much of a fully populated struct as the caller has room for, and
/// reports how much that was through the caller's own `struct_size`.
template <typename Struct>
sweeps_status_t emitStruct(const Struct& source, Struct* out) noexcept {
    if (out == nullptr) {
        return fail(SWEEPS_ERR_INVALID_ARGUMENT, "output struct is null");
    }
    if (out->struct_size < kMinStructSize) {
        return fail(SWEEPS_ERR_INVALID_ARGUMENT,
                    "struct_size is zero or nonsensical -- set it to sizeof the struct, "
                    "or use the matching _init function");
    }

    const std::size_t bytes = std::min<std::size_t>(out->struct_size, sizeof(Struct));
    Struct staged = source;
    staged.struct_size = static_cast<std::uint32_t>(bytes);
    std::memcpy(out, &staged, bytes);
    return SWEEPS_OK;
}

/// Reads an input struct over this library's defaults.
///
/// A caller's struct larger than ours is refused rather than truncated: it
/// carries fields we would drop on the floor, and dropping them silently is the
/// one outcome worse than failing.
template <typename Struct, typename Init>
sweeps_status_t readStruct(const Struct* in, Init&& init, Struct& out) noexcept {
    if (in == nullptr) {
        return fail(SWEEPS_ERR_INVALID_ARGUMENT, "input struct is null");
    }
    if (in->struct_size < kMinStructSize) {
        return fail(SWEEPS_ERR_INVALID_ARGUMENT,
                    "struct_size is zero or nonsensical -- set it to sizeof the struct, "
                    "or use the matching _init function");
    }
    if (in->struct_size > sizeof(Struct)) {
        return fail(SWEEPS_ERR_INVALID_ARGUMENT,
                    "struct_size is larger than this library knows: the caller was built "
                    "against a newer sweeps.h");
    }

    init(&out);
    std::memcpy(&out, in, in->struct_size);
    out.struct_size = in->struct_size;
    return SWEEPS_OK;
}

// ---------------------------------------------------------------------------
// Odds and ends.
// ---------------------------------------------------------------------------

/// A path from UTF-8, on every platform.
///
/// `filesystem::path` built from a narrow string uses the *active code page* on
/// Windows, so a session whose name is not spelled in the operator's own locale
/// would simply not be found -- and the header promises UTF-8 everywhere. The
/// conversion is spelled through `char8_t` where the standard provides it,
/// because `u8path` is deprecated from C++20 and this library compiles at 17,
/// 20 and 23.
std::filesystem::path toPath(const char* utf8) {
#if defined(_WIN32)
#if defined(__cpp_lib_char8_t)
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8)));
#else
    return std::filesystem::u8path(utf8);
#endif
#else
    return std::filesystem::path(utf8);
#endif
}

/// Null becomes the empty string, so every `const char*` field is optional.
std::string_view textOf(const char* text) noexcept {
    return text != nullptr ? std::string_view(text) : std::string_view();
}

/// Bridges the C callback into the library's `Log`.
///
/// The category arrives as a `string_view` and is materialised into a
/// `std::string` before it crosses, because the header guarantees every
/// `sweeps_str_t` is NUL-terminated and a view is not. It is a handful of bytes
/// and the sink is called for damaged files, not per frame.
sweeps::Log makeLog(sweeps_log_fn_t sink, void* user) {
    if (sink == nullptr) {
        return {};
    }
    return sweeps::Log(
        [sink, user](sweeps::LogLevel level, std::string_view category, std::string message) {
            const std::string categoryText(category);
            sink(user, static_cast<sweeps_log_level_t>(level), view(categoryText), view(message));
        });
}

/// The two-call idiom, in one place.
///
/// Writes at most `capacity` bytes *including* the terminating NUL, truncating
/// like `snprintf`, and always reports the full length. Truncation is not an
/// error: the caller compares the two numbers, which is what makes the sizing
/// call (`buffer` null, `capacity` zero) work at all.
sweeps_status_t emitText(const std::string& text, char* buffer, std::size_t capacity,
                         std::size_t* outLength) noexcept {
    if (outLength != nullptr) {
        *outLength = text.size();
    }
    if (buffer != nullptr && capacity > 0) {
        const std::size_t copied = std::min(capacity - 1, text.size());
        std::memcpy(buffer, text.data(), copied);
        buffer[copied] = '\0';
    }
    return SWEEPS_OK;
}

void assign(sweeps_str_t* out, const std::string& text) noexcept {
    if (out != nullptr) {
        *out = view(text);
    }
}

sweeps::HistoryQuery toHistoryQuery(const sweeps_query_t& query) noexcept {
    sweeps::HistoryQuery out;
    out.fromNs = query.from_ns;
    out.toNs = query.to_ns;
    out.fromHz = query.from_hz;
    out.toHz = query.to_hz;
    out.maxLines = query.max_lines;
    out.maxBins = query.max_bins;
    if (query.has_segment_id != 0) {
        out.segmentId = query.segment_id;
    }
    if (query.has_lod != 0) {
        out.lod = query.lod;
    }
    return out;
}

/// Builds an `AcquisitionConfig` from the C struct.
///
/// The gains are copied rather than borrowed: the C side is an array of
/// `{const char*, double}` and the C++ side a vector of `{std::string, double}`,
/// and no pointer can make one look like the other. That asymmetry is also why
/// `sweeps_segment_t` spells the acquisition scalars out instead of nesting this
/// struct -- see the header.
sweeps::AcquisitionConfig toAcquisitionConfig(const sweeps_acq_config_t& config) {
    sweeps::AcquisitionConfig out;
    out.centerHz = config.center_hz;
    out.spanHz = config.span_hz;
    out.sampleRate = config.sample_rate;
    out.fftSize = config.fft_size;
    out.window = static_cast<sweeps::WindowType>(config.window);
    out.windowBeta = config.window_beta;
    out.windowEnbw = config.window_enbw;
    out.overlap = config.overlap;
    out.rbwHz = config.rbw_hz;
    out.referenceLevelDbm = config.reference_level_dbm;
    out.dbfsToDbmOffset = config.dbfs_to_dbm_offset;
    out.deviceId = std::string(textOf(config.device_id));
    out.deviceLabel = std::string(textOf(config.device_label));

    out.gains.reserve(config.gain_count);
    for (std::size_t index = 0; index < config.gain_count; ++index) {
        const sweeps_gain_t& gain = config.gains[index];
        out.gains.emplace_back(std::string(textOf(gain.name)), gain.value);
    }
    return out;
}

/// The segment at an index, or null. Index rather than id, deliberately: ids
/// are looked up with `sweeps_reader_segment_index_of`, because an extracted
/// file's ids neither start at zero nor run contiguously.
const sweeps::SegmentInfo* segmentAt(const sweeps_reader_t* handle, std::size_t index) noexcept {
    const sweeps::SessionReader* reader = readerOf(handle);
    if (reader == nullptr || index >= reader->segments().size()) {
        return nullptr;
    }
    return &reader->segments()[index];
}

/// Reports a typed accessor's body mismatch, which is the same three lines in
/// ten places.
sweeps_status_t wrongType(const char* expected) noexcept {
    setLastError(std::string("this event does not carry a ") + expected + " body");
    return SWEEPS_ERR_WRONG_TYPE;
}

} // namespace

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

extern "C" uint32_t sweeps_abi_version(void) SWEEPS_NOEXCEPT {
    return SWEEPS_ABI_VERSION;
}

extern "C" sweeps_str_t sweeps_library_version(void) SWEEPS_NOEXCEPT {
    return view(SWEEPSFILE_VERSION_STRING, sizeof(SWEEPSFILE_VERSION_STRING) - 1);
}

extern "C" void sweeps_format_version(uint32_t* major, uint32_t* minor) SWEEPS_NOEXCEPT {
    if (major != nullptr) {
        *major = sweeps::kMajorVersion;
    }
    if (minor != nullptr) {
        *minor = sweeps::kMinorVersion;
    }
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

extern "C" const char* sweeps_status_name(sweeps_status_t status) SWEEPS_NOEXCEPT {
    switch (status) {
    case SWEEPS_OK:
        return "ok";
    case SWEEPS_ERR_UNKNOWN:
        return "unknown error";
    case SWEEPS_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case SWEEPS_ERR_NOT_FOUND:
        return "not found";
    case SWEEPS_ERR_UNSUPPORTED:
        return "unsupported";
    case SWEEPS_ERR_UNAVAILABLE:
        return "unavailable";
    case SWEEPS_ERR_IO:
        return "i/o error";
    case SWEEPS_ERR_PARSE:
        return "parse error";
    case SWEEPS_ERR_OUT_OF_RANGE:
        return "out of range";
    case SWEEPS_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case SWEEPS_ERR_TIMED_OUT:
        return "timed out";
    case SWEEPS_ERR_CANCELLED:
        return "cancelled";
    case SWEEPS_ERR_PERMISSION_DENIED:
        return "permission denied";
    case SWEEPS_ERR_ALREADY_EXISTS:
        return "already exists";
    case SWEEPS_ERR_DEVICE:
        return "device error";
    case SWEEPS_ERR_PROTOCOL:
        return "protocol error";
    case SWEEPS_ERR_CORRUPT:
        return "corrupt";
    case SWEEPS_ERR_WRONG_TYPE:
        return "wrong type";
    case SWEEPS_STATUS_FORCE_INT32:
        break;
    }
    return "unrecognised status";
}

extern "C" sweeps_str_t sweeps_last_error(void) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, []() { return view(lastErrorSlot()); });
}

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

extern "C" double sweeps_dequantise_db(uint8_t value, double origin_db) SWEEPS_NOEXCEPT {
    return sweeps::dequantiseDb(value, origin_db);
}

extern "C" uint8_t sweeps_quantise_db(double db, double origin_db) SWEEPS_NOEXCEPT {
    return sweeps::quantiseDb(db, origin_db);
}

extern "C" sweeps_str_t sweeps_window_type_name(sweeps_window_type_t window) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        const std::string_view name = sweeps::toString(static_cast<sweeps::WindowType>(window));
        return view(name.data(), name.size());
    });
}

extern "C" sweeps_str_t sweeps_event_kind_name(uint16_t kind) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        const std::string_view name = sweeps::eventKindName(kind);
        return view(name.data(), name.size());
    });
}

// ---------------------------------------------------------------------------
// Struct defaults
//
// These exist because zeroing is not equivalent, and C callers zero. Every
// value below is the one the C++ struct declares, so the two APIs cannot drift
// into recording different things about the same measurement.
// ---------------------------------------------------------------------------

extern "C" void sweeps_query_init(sweeps_query_t* query) SWEEPS_NOEXCEPT {
    if (query == nullptr) {
        return;
    }
    const sweeps::HistoryQuery defaults;
    std::memset(query, 0, sizeof(*query));
    query->struct_size = sizeof(*query);
    query->max_lines = defaults.maxLines;
    query->max_bins = defaults.maxBins;
    query->from_ns = defaults.fromNs;
    query->to_ns = defaults.toNs;
    query->from_hz = defaults.fromHz;
    query->to_hz = defaults.toHz;
}

extern "C" void sweeps_extract_options_init(sweeps_extract_options_t* options) SWEEPS_NOEXCEPT {
    if (options == nullptr) {
        return;
    }
    std::memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
}

extern "C" void sweeps_acq_config_init(sweeps_acq_config_t* config) SWEEPS_NOEXCEPT {
    if (config == nullptr) {
        return;
    }
    const sweeps::AcquisitionConfig defaults;
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->window = static_cast<std::uint32_t>(defaults.window);
    config->window_beta = defaults.windowBeta;
    config->window_enbw = defaults.windowEnbw;
}

extern "C" void sweeps_writer_config_init(sweeps_writer_config_t* config) SWEEPS_NOEXCEPT {
    if (config == nullptr) {
        return;
    }
    const sweeps::WriterConfig defaults;
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->bins_per_line = defaults.binsPerLine;
    config->max_bytes = defaults.maxBytes;
    config->max_seconds = defaults.maxSeconds;
    config->min_free_bytes = defaults.minFreeBytes;
}

extern "C" void sweeps_frame_init(sweeps_frame_t* frame) SWEEPS_NOEXCEPT {
    if (frame == nullptr) {
        return;
    }
    std::memset(frame, 0, sizeof(*frame));
    frame->struct_size = sizeof(*frame);
}

// ---------------------------------------------------------------------------
// Reader -- lifecycle
// ---------------------------------------------------------------------------

extern "C" sweeps_status_t sweeps_reader_open_ex(const char* path, sweeps_log_fn_t log,
                                                 void* log_user,
                                                 sweeps_reader_t** out) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        if (out == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "out is null");
        }
        *out = nullptr;
        if (path == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "path is null");
        }

        auto reader = sweeps::SessionReader::open(toPath(path), makeLog(log, log_user));
        if (!reader) {
            return fail(reader.error());
        }

        auto handle = std::make_unique<sweeps_reader_t>();
        handle->reader = std::move(*reader);
        *out = handle.release();
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_reader_open(const char* path,
                                              sweeps_reader_t** out) SWEEPS_NOEXCEPT {
    return sweeps_reader_open_ex(path, nullptr, nullptr, out);
}

extern "C" void sweeps_reader_close(sweeps_reader_t* reader) SWEEPS_NOEXCEPT {
    guardVoid([&]() {
        if (reader == nullptr || reader->magic != kReaderMagic) {
            return;
        }
        reader->magic = 0;
        delete reader;
    });
}

extern "C" sweeps_status_t sweeps_reader_summary(const sweeps_reader_t* reader,
                                                 sweeps_summary_t* out) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionReader* session = readerOf(reader);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "reader handle is null or already closed");
        }

        const sweeps::SessionSummary& summary = session->summary();
        sweeps_summary_t staged{};
        staged.struct_size = sizeof(staged);
        staged.major_version = summary.majorVersion;
        staged.minor_version = summary.minorVersion;
        staged.incompatible_features = summary.incompatibleFeatures;
        staged.created_wall_ns = summary.createdWallNs;
        staged.total_lines = summary.totalLines;
        staged.total_tiles = summary.totalTiles;
        staged.file_bytes = summary.fileBytes;
        staged.first_line_ns = summary.firstLineNs;
        staged.last_line_ns = summary.lastLineNs;
        staged.truncated_bytes = summary.truncatedBytes;
        staged.lowest_hz = summary.lowestHz;
        staged.highest_hz = summary.highestHz;
        staged.recovered_by_scan = summary.recoveredByScan ? 1 : 0;
        staged.newer_minor_version = summary.newerMinorVersion ? 1 : 0;
        return emitStruct(staged, out);
    });
}

extern "C" sweeps_str_t sweeps_reader_name(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        const sweeps::SessionReader* session = readerOf(reader);
        return session != nullptr ? view(session->summary().name) : kEmptyStr;
    });
}

extern "C" sweeps_str_t sweeps_reader_app_version(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        const sweeps::SessionReader* session = readerOf(reader);
        return session != nullptr ? view(session->summary().appVersion) : kEmptyStr;
    });
}

extern "C" const sweeps_metadata_t*
sweeps_reader_manifest(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT {
    return guardValue<const sweeps_metadata_t*>(nullptr, [&]() -> const sweeps_metadata_t* {
        const sweeps::SessionReader* session = readerOf(reader);
        return session != nullptr ? asMetadataHandle(session->manifest()) : nullptr;
    });
}

// ---------------------------------------------------------------------------
// Reader -- segments
// ---------------------------------------------------------------------------

extern "C" size_t sweeps_reader_segment_count(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT {
    return guardValue<size_t>(0, [&]() -> size_t {
        const sweeps::SessionReader* session = readerOf(reader);
        return session != nullptr ? session->segments().size() : 0;
    });
}

extern "C" sweeps_status_t sweeps_reader_segment_at(const sweeps_reader_t* reader, size_t index,
                                                    sweeps_segment_t* out) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SegmentInfo* segment = segmentAt(reader, index);
        if (segment == nullptr) {
            return fail(SWEEPS_ERR_OUT_OF_RANGE, "no segment at that index");
        }

        const sweeps::AcquisitionConfig& config = segment->config;
        sweeps_segment_t staged{};
        staged.struct_size = sizeof(staged);
        staged.id = segment->id;
        staged.bin_count = segment->grid.binCount;
        staged.fft_size = config.fftSize;
        staged.window = static_cast<std::uint32_t>(config.window);
        staged.gain_count = static_cast<std::uint32_t>(config.gains.size());
        staged.start_wall_ns = segment->startWallNs;
        staged.start_monotonic_ns = segment->startMonotonicNs;
        staged.end_monotonic_ns = segment->endMonotonicNs;
        staged.line_count = segment->lineCount;
        staged.start_hz = segment->grid.startHz;
        staged.bin_width_hz = segment->grid.binWidthHz;
        staged.center_hz = config.centerHz;
        staged.span_hz = config.spanHz;
        staged.sample_rate = config.sampleRate;
        staged.window_beta = config.windowBeta;
        staged.window_enbw = config.windowEnbw;
        staged.overlap = config.overlap;
        staged.rbw_hz = config.rbwHz;
        staged.reference_level_dbm = config.referenceLevelDbm;
        staged.dbfs_to_dbm_offset = config.dbfsToDbmOffset;
        return emitStruct(staged, out);
    });
}

extern "C" sweeps_status_t sweeps_reader_segment_index_of(const sweeps_reader_t* reader,
                                                          uint32_t id,
                                                          size_t* out_index) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionReader* session = readerOf(reader);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "reader handle is null or already closed");
        }

        const std::vector<sweeps::SegmentInfo>& segments = session->segments();
        for (std::size_t index = 0; index < segments.size(); ++index) {
            if (segments[index].id == id) {
                if (out_index != nullptr) {
                    *out_index = index;
                }
                return SWEEPS_OK;
            }
        }
        return fail(SWEEPS_ERR_NOT_FOUND, "no segment with that id");
    });
}

extern "C" sweeps_str_t sweeps_reader_segment_reason(const sweeps_reader_t* reader,
                                                     size_t index) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        const sweeps::SegmentInfo* segment = segmentAt(reader, index);
        return segment != nullptr ? view(segment->reason) : kEmptyStr;
    });
}

extern "C" sweeps_str_t sweeps_reader_segment_device_id(const sweeps_reader_t* reader,
                                                        size_t index) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        const sweeps::SegmentInfo* segment = segmentAt(reader, index);
        return segment != nullptr ? view(segment->config.deviceId) : kEmptyStr;
    });
}

extern "C" sweeps_str_t sweeps_reader_segment_device_label(const sweeps_reader_t* reader,
                                                           size_t index) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        const sweeps::SegmentInfo* segment = segmentAt(reader, index);
        return segment != nullptr ? view(segment->config.deviceLabel) : kEmptyStr;
    });
}

extern "C" size_t sweeps_reader_segment_gain_count(const sweeps_reader_t* reader,
                                                   size_t index) SWEEPS_NOEXCEPT {
    return guardValue<size_t>(0, [&]() -> size_t {
        const sweeps::SegmentInfo* segment = segmentAt(reader, index);
        return segment != nullptr ? segment->config.gains.size() : 0;
    });
}

extern "C" sweeps_status_t sweeps_reader_segment_gain(const sweeps_reader_t* reader,
                                                      size_t segment_index, size_t gain_index,
                                                      sweeps_str_t* out_name,
                                                      double* out_value) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SegmentInfo* segment = segmentAt(reader, segment_index);
        if (segment == nullptr) {
            return fail(SWEEPS_ERR_OUT_OF_RANGE, "no segment at that index");
        }
        if (gain_index >= segment->config.gains.size()) {
            return fail(SWEEPS_ERR_OUT_OF_RANGE, "no gain stage at that index");
        }

        const std::pair<std::string, double>& gain = segment->config.gains[gain_index];
        assign(out_name, gain.first);
        if (out_value != nullptr) {
            *out_value = gain.second;
        }
        return SWEEPS_OK;
    });
}

// ---------------------------------------------------------------------------
// Reader -- integrity and extraction
// ---------------------------------------------------------------------------

extern "C" sweeps_status_t sweeps_reader_verify(const sweeps_reader_t* reader,
                                                uint64_t* out_records) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionReader* session = readerOf(reader);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "reader handle is null or already closed");
        }

        auto records = session->verify();
        if (!records) {
            return fail(records.error());
        }
        if (out_records != nullptr) {
            *out_records = *records;
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t
sweeps_reader_extract(const sweeps_reader_t* reader, const char* destination,
                      const sweeps_query_t* range,
                      const sweeps_extract_options_t* options) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionReader* session = readerOf(reader);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "reader handle is null or already closed");
        }
        if (destination == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "destination is null");
        }

        sweeps_query_t stagedRange;
        if (const sweeps_status_t status = readStruct(range, sweeps_query_init, stagedRange);
            status != SWEEPS_OK) {
            return status;
        }

        sweeps_extract_options_t stagedOptions;
        if (options != nullptr) {
            if (const sweeps_status_t status =
                    readStruct(options, sweeps_extract_options_init, stagedOptions);
                status != SWEEPS_OK) {
                return status;
            }
        } else {
            sweeps_extract_options_init(&stagedOptions);
        }

        sweeps::ExtractOptions extract;
        extract.applicationVersion = std::string(textOf(stagedOptions.application_version));
        extract.createdWallNs = stagedOptions.created_wall_ns;

        const sweeps::Status result =
            session->extract(toPath(destination), toHistoryQuery(stagedRange), extract);
        return result ? SWEEPS_OK : fail(result.error());
    });
}

// ---------------------------------------------------------------------------
// Tiles
// ---------------------------------------------------------------------------

extern "C" sweeps_status_t sweeps_reader_query(const sweeps_reader_t* reader,
                                               const sweeps_query_t* query,
                                               sweeps_tiles_t** out) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        if (out == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "out is null");
        }
        *out = nullptr;

        const sweeps::SessionReader* session = readerOf(reader);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "reader handle is null or already closed");
        }

        sweeps_query_t staged;
        if (const sweeps_status_t status = readStruct(query, sweeps_query_init, staged);
            status != SWEEPS_OK) {
            return status;
        }

        auto tiles = session->query(toHistoryQuery(staged));
        if (!tiles) {
            return fail(tiles.error());
        }

        auto handle = std::make_unique<sweeps_tiles_t>();
        handle->tiles = std::move(*tiles);
        *out = handle.release();
        return SWEEPS_OK;
    });
}

extern "C" void sweeps_tiles_free(sweeps_tiles_t* tiles) SWEEPS_NOEXCEPT {
    guardVoid([&]() {
        if (tiles == nullptr || tiles->magic != kTilesMagic) {
            return;
        }
        tiles->magic = 0;
        delete tiles;
    });
}

extern "C" size_t sweeps_tiles_count(const sweeps_tiles_t* tiles) SWEEPS_NOEXCEPT {
    return guardValue<size_t>(0, [&]() -> size_t {
        const std::vector<sweeps::HistoryTile>* list = tilesOf(tiles);
        return list != nullptr ? list->size() : 0;
    });
}

extern "C" const sweeps_tile_t* sweeps_tiles_at(const sweeps_tiles_t* tiles,
                                                size_t index) SWEEPS_NOEXCEPT {
    return guardValue<const sweeps_tile_t*>(nullptr, [&]() -> const sweeps_tile_t* {
        const std::vector<sweeps::HistoryTile>* list = tilesOf(tiles);
        if (list == nullptr || index >= list->size()) {
            return nullptr;
        }
        return asTileHandle((*list)[index]);
    });
}

extern "C" sweeps_status_t sweeps_tile_info(const sweeps_tile_t* tile,
                                            sweeps_tile_info_t* out) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::HistoryTile* source = tileOf(tile);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "tile is null");
        }

        sweeps_tile_info_t staged{};
        staged.struct_size = sizeof(staged);
        staged.segment_id = source->segmentId;
        staged.lod = source->lod;
        staged.time_block = source->timeBlock;
        staged.freq_block = source->freqBlock;
        staged.lines = source->lines;
        staged.bins = source->bins;
        staged.origin_db = source->originDb;
        staged.first_line_ns = source->firstLineNs;
        staged.last_line_ns = source->lastLineNs;
        staged.start_hz = source->startHz;
        staged.bin_width_hz = source->binWidthHz;
        return emitStruct(staged, out);
    });
}

extern "C" const uint8_t* sweeps_tile_data(const sweeps_tile_t* tile,
                                           size_t* out_bytes) SWEEPS_NOEXCEPT {
    return guardValue<const uint8_t*>(nullptr, [&]() -> const uint8_t* {
        const sweeps::HistoryTile* source = tileOf(tile);
        if (source == nullptr) {
            if (out_bytes != nullptr) {
                *out_bytes = 0;
            }
            return nullptr;
        }
        if (out_bytes != nullptr) {
            *out_bytes = source->data.size();
        }
        return source->data.data();
    });
}

extern "C" uint32_t sweeps_reader_choose_lod(const sweeps_reader_t* reader, uint64_t from_ns,
                                             uint64_t to_ns, uint32_t max_lines,
                                             uint32_t segment_id) SWEEPS_NOEXCEPT {
    return guardValue<uint32_t>(0, [&]() -> uint32_t {
        const sweeps::SessionReader* session = readerOf(reader);
        return session != nullptr ? session->chooseLod(from_ns, to_ns, max_lines, segment_id) : 0;
    });
}

extern "C" int sweeps_reader_has_tiles_at_lod(const sweeps_reader_t* reader, uint32_t segment_id,
                                              uint32_t lod) SWEEPS_NOEXCEPT {
    return guardValue(0, [&]() {
        const sweeps::SessionReader* session = readerOf(reader);
        return session != nullptr && session->hasTilesAtLod(segment_id, lod) ? 1 : 0;
    });
}

extern "C" sweeps_status_t sweeps_reader_spectrum_at(const sweeps_reader_t* reader,
                                                     uint64_t monotonic_ns, uint32_t segment_id,
                                                     float* out, size_t capacity,
                                                     size_t* out_count) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionReader* session = readerOf(reader);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "reader handle is null or already closed");
        }

        auto spectrum = session->spectrumAt(monotonic_ns, segment_id);
        if (!spectrum) {
            return fail(spectrum.error());
        }

        if (out_count != nullptr) {
            *out_count = spectrum->size();
        }
        if (out != nullptr && capacity > 0) {
            const std::size_t copied = std::min(capacity, spectrum->size());
            std::memcpy(out, spectrum->data(), copied * sizeof(float));
        }
        return SWEEPS_OK;
    });
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

extern "C" size_t sweeps_reader_event_count(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT {
    return guardValue<size_t>(0, [&]() -> size_t {
        const sweeps::SessionReader* session = readerOf(reader);
        return session != nullptr ? session->events().size() : 0;
    });
}

extern "C" const sweeps_event_t* sweeps_reader_event_at(const sweeps_reader_t* reader,
                                                        size_t index) SWEEPS_NOEXCEPT {
    return guardValue<const sweeps_event_t*>(nullptr, [&]() -> const sweeps_event_t* {
        const sweeps::SessionReader* session = readerOf(reader);
        if (session == nullptr || index >= session->events().size()) {
            return nullptr;
        }
        return asEventHandle(session->events()[index]);
    });
}

extern "C" uint16_t sweeps_event_kind(const sweeps_event_t* event) SWEEPS_NOEXCEPT {
    const sweeps::SessionEvent* source = eventOf(event);
    return source != nullptr ? source->kind : 0;
}

extern "C" uint64_t sweeps_event_monotonic_ns(const sweeps_event_t* event) SWEEPS_NOEXCEPT {
    const sweeps::SessionEvent* source = eventOf(event);
    return source != nullptr ? source->monotonicNs : 0;
}

extern "C" uint64_t sweeps_event_wall_ns(const sweeps_event_t* event) SWEEPS_NOEXCEPT {
    const sweeps::SessionEvent* source = eventOf(event);
    return source != nullptr ? source->wallNs : 0;
}

extern "C" uint32_t sweeps_event_segment_id(const sweeps_event_t* event) SWEEPS_NOEXCEPT {
    const sweeps::SessionEvent* source = eventOf(event);
    return source != nullptr ? source->segmentId : 0;
}

extern "C" sweeps_status_t sweeps_event_retune(const sweeps_event_t* event, double* out_center_hz,
                                               uint32_t* out_step_index) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        const auto* data = source->as<sweeps::RetuneData>();
        if (data == nullptr) {
            return wrongType("retune");
        }
        if (out_center_hz != nullptr) {
            *out_center_hz = data->centerHz;
        }
        if (out_step_index != nullptr) {
            *out_step_index = data->stepIndex;
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t
sweeps_event_parameter_changed(const sweeps_event_t* event, sweeps_str_t* out_key,
                               sweeps_str_t* out_value, int* out_grid_affecting,
                               int* out_calibration_affecting) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        const auto* data = source->as<sweeps::ParameterChangedData>();
        if (data == nullptr) {
            return wrongType("parameter-changed");
        }
        assign(out_key, data->key);
        assign(out_value, data->value);
        if (out_grid_affecting != nullptr) {
            *out_grid_affecting = data->gridAffecting ? 1 : 0;
        }
        if (out_calibration_affecting != nullptr) {
            *out_calibration_affecting = data->calibrationAffecting ? 1 : 0;
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_event_sweep_pass(const sweeps_event_t* event,
                                                   uint64_t* out_pass_id, double* out_start_hz,
                                                   double* out_stop_hz,
                                                   double* out_duration_seconds) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        const auto* data = source->as<sweeps::SweepPassData>();
        if (data == nullptr) {
            return wrongType("sweep-pass");
        }
        if (out_pass_id != nullptr) {
            *out_pass_id = data->passId;
        }
        if (out_start_hz != nullptr) {
            *out_start_hz = data->startHz;
        }
        if (out_stop_hz != nullptr) {
            *out_stop_hz = data->stopHz;
        }
        if (out_duration_seconds != nullptr) {
            *out_duration_seconds = data->durationSeconds;
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_event_marker(const sweeps_event_t* event, sweeps_str_t* out_label,
                                               double* out_frequency_hz,
                                               double* out_level_dbm) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        const auto* data = source->as<sweeps::MarkerData>();
        if (data == nullptr) {
            return wrongType("marker");
        }
        assign(out_label, data->label);
        if (out_frequency_hz != nullptr) {
            *out_frequency_hz = data->frequencyHz;
        }
        if (out_level_dbm != nullptr) {
            *out_level_dbm = data->levelDbm;
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_event_annotation(const sweeps_event_t* event,
                                                   sweeps_str_t* out_text, double* out_start_hz,
                                                   double* out_stop_hz) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        const auto* data = source->as<sweeps::AnnotationData>();
        if (data == nullptr) {
            return wrongType("annotation");
        }
        assign(out_text, data->text);
        if (out_start_hz != nullptr) {
            *out_start_hz = data->startHz;
        }
        if (out_stop_hz != nullptr) {
            *out_stop_hz = data->stopHz;
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_event_segment_boundary(const sweeps_event_t* event,
                                                         sweeps_str_t* out_reason) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        const auto* data = source->as<sweeps::SegmentBoundaryData>();
        if (data == nullptr) {
            return wrongType("segment-boundary");
        }
        assign(out_reason, data->reason);
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t
sweeps_event_throttle_changed(const sweeps_event_t* event, sweeps_str_t* out_reason,
                              double* out_processed_fraction) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        const auto* data = source->as<sweeps::ThrottleChangedData>();
        if (data == nullptr) {
            return wrongType("throttle-changed");
        }
        assign(out_reason, data->reason);
        if (out_processed_fraction != nullptr) {
            *out_processed_fraction = data->processedFraction;
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_event_device_error(const sweeps_event_t* event,
                                                     sweeps_str_t* out_device_id,
                                                     sweeps_str_t* out_message) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        const auto* data = source->as<sweeps::DeviceErrorData>();
        if (data == nullptr) {
            return wrongType("device-error");
        }
        assign(out_device_id, data->deviceId);
        assign(out_message, data->message);
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t
sweeps_event_plugin(const sweeps_event_t* event, sweeps_str_t* out_plugin_id,
                    sweeps_str_t* out_event_name,
                    const sweeps_metadata_t** out_fields) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        const auto* data = source->as<sweeps::PluginEventData>();
        if (data == nullptr) {
            return wrongType("plugin");
        }
        assign(out_plugin_id, data->pluginId);
        assign(out_event_name, data->eventName);
        if (out_fields != nullptr) {
            *out_fields = asMetadataHandle(data->fields);
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_event_unknown_body(const sweeps_event_t* event,
                                                     sweeps_bytes_t* out_body) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        const auto* data = source->as<sweeps::UnknownEventData>();
        if (data == nullptr) {
            return wrongType("unknown");
        }
        if (out_body != nullptr) {
            *out_body = viewBytes(data->body.data(), data->body.size());
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_event_body_json(const sweeps_event_t* event, int indent,
                                                  char* buffer, size_t capacity,
                                                  size_t* out_length) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::SessionEvent* source = eventOf(event);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "event is null");
        }
        return emitText(sweeps::eventBodyMetadata(*source).toJson(indent), buffer, capacity,
                        out_length);
    });
}

// ---------------------------------------------------------------------------
// Plugin records
// ---------------------------------------------------------------------------

extern "C" size_t sweeps_reader_plugin_count(const sweeps_reader_t* reader) SWEEPS_NOEXCEPT {
    return guardValue<size_t>(0, [&]() -> size_t {
        const sweeps::SessionReader* session = readerOf(reader);
        return session != nullptr ? session->pluginRecords().size() : 0;
    });
}

extern "C" const sweeps_plugin_record_t* sweeps_reader_plugin_at(const sweeps_reader_t* reader,
                                                                 size_t index) SWEEPS_NOEXCEPT {
    return guardValue<const sweeps_plugin_record_t*>(
        nullptr, [&]() -> const sweeps_plugin_record_t* {
            const sweeps::SessionReader* session = readerOf(reader);
            if (session == nullptr || index >= session->pluginRecords().size()) {
                return nullptr;
            }
            return asPluginHandle(session->pluginRecords()[index]);
        });
}

extern "C" sweeps_str_t sweeps_plugin_id(const sweeps_plugin_record_t* record) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        const sweeps::PluginRecord* source = pluginOf(record);
        return source != nullptr ? view(source->pluginId) : kEmptyStr;
    });
}

extern "C" sweeps_str_t sweeps_plugin_name(const sweeps_plugin_record_t* record) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        const sweeps::PluginRecord* source = pluginOf(record);
        return source != nullptr ? view(source->recordName) : kEmptyStr;
    });
}

extern "C" uint32_t
sweeps_plugin_schema_version(const sweeps_plugin_record_t* record) SWEEPS_NOEXCEPT {
    const sweeps::PluginRecord* source = pluginOf(record);
    return source != nullptr ? source->schemaVersion : 0;
}

extern "C" uint64_t
sweeps_plugin_monotonic_ns(const sweeps_plugin_record_t* record) SWEEPS_NOEXCEPT {
    const sweeps::PluginRecord* source = pluginOf(record);
    return source != nullptr ? source->monotonicNs : 0;
}

extern "C" sweeps_status_t sweeps_plugin_body(const sweeps_plugin_record_t* record,
                                              sweeps_bytes_t* out_body) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::PluginRecord* source = pluginOf(record);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "plugin record is null");
        }
        if (out_body != nullptr) {
            *out_body = viewBytes(source->body, source->bodyBytes);
        }
        return SWEEPS_OK;
    });
}

// ---------------------------------------------------------------------------
// Metadata -- reading
// ---------------------------------------------------------------------------

extern "C" size_t sweeps_metadata_count(const sweeps_metadata_t* metadata) SWEEPS_NOEXCEPT {
    return guardValue<size_t>(0, [&]() -> size_t {
        const sweeps::Metadata* source = metadataOf(metadata);
        return source != nullptr ? source->size() : 0;
    });
}

extern "C" sweeps_status_t sweeps_metadata_key_at(const sweeps_metadata_t* metadata, size_t index,
                                                  sweeps_str_t* out_key,
                                                  sweeps_value_type_t* out_type) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::Metadata* source = metadataOf(metadata);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "metadata is null");
        }
        if (index >= source->size()) {
            return fail(SWEEPS_ERR_OUT_OF_RANGE, "no key at that index");
        }

        auto entry = source->begin();
        std::advance(entry, static_cast<std::ptrdiff_t>(index));
        assign(out_key, entry->first);
        if (out_type != nullptr) {
            *out_type = static_cast<sweeps_value_type_t>(entry->second.type());
        }
        return SWEEPS_OK;
    });
}

extern "C" int sweeps_metadata_contains(const sweeps_metadata_t* metadata,
                                        const char* key) SWEEPS_NOEXCEPT {
    return guardValue(0, [&]() {
        const sweeps::Metadata* source = metadataOf(metadata);
        return source != nullptr && key != nullptr && source->contains(key) ? 1 : 0;
    });
}

extern "C" sweeps_value_type_t sweeps_metadata_type_of(const sweeps_metadata_t* metadata,
                                                       const char* key) SWEEPS_NOEXCEPT {
    return guardValue(SWEEPS_VALUE_ABSENT, [&]() {
        const sweeps::Metadata* source = metadataOf(metadata);
        if (source == nullptr || key == nullptr) {
            return SWEEPS_VALUE_ABSENT;
        }
        const sweeps::Value* value = source->find(key);
        return value != nullptr ? static_cast<sweeps_value_type_t>(value->type())
                                : SWEEPS_VALUE_ABSENT;
    });
}

extern "C" sweeps_str_t sweeps_metadata_get_string(const sweeps_metadata_t* metadata,
                                                   const char* key,
                                                   const char* fallback) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() -> sweeps_str_t {
        const sweeps::Metadata* source = metadataOf(metadata);
        if (source != nullptr && key != nullptr) {
            if (const sweeps::Value* value = source->find(key)) {
                // asStringRef rather than asString: the latter returns a copy,
                // and a view into it would dangle the moment this returned.
                if (const std::string* text = value->asStringRef()) {
                    return view(*text);
                }
            }
        }
        return fallback != nullptr ? view(fallback, std::strlen(fallback)) : kEmptyStr;
    });
}

extern "C" int64_t sweeps_metadata_get_i64(const sweeps_metadata_t* metadata, const char* key,
                                           int64_t fallback) SWEEPS_NOEXCEPT {
    return guardValue(fallback, [&]() {
        const sweeps::Metadata* source = metadataOf(metadata);
        return source != nullptr && key != nullptr ? source->getInt(key, fallback) : fallback;
    });
}

extern "C" double sweeps_metadata_get_f64(const sweeps_metadata_t* metadata, const char* key,
                                          double fallback) SWEEPS_NOEXCEPT {
    return guardValue(fallback, [&]() {
        const sweeps::Metadata* source = metadataOf(metadata);
        return source != nullptr && key != nullptr ? source->getFloat(key, fallback) : fallback;
    });
}

extern "C" int sweeps_metadata_get_bool(const sweeps_metadata_t* metadata, const char* key,
                                        int fallback) SWEEPS_NOEXCEPT {
    return guardValue(fallback, [&]() {
        const sweeps::Metadata* source = metadataOf(metadata);
        if (source == nullptr || key == nullptr) {
            return fallback;
        }
        return source->getBool(key, fallback != 0) ? 1 : 0;
    });
}

extern "C" sweeps_status_t sweeps_metadata_get_bytes(const sweeps_metadata_t* metadata,
                                                     const char* key,
                                                     sweeps_bytes_t* out_value) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::Metadata* source = metadataOf(metadata);
        if (source == nullptr || key == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "metadata or key is null");
        }
        const sweeps::Value* value = source->find(key);
        if (value == nullptr) {
            return fail(SWEEPS_ERR_NOT_FOUND, "no such key");
        }
        const std::vector<std::byte>* bytes = value->asBytes();
        if (bytes == nullptr) {
            return fail(SWEEPS_ERR_WRONG_TYPE, "that key does not hold bytes");
        }
        if (out_value != nullptr) {
            *out_value = viewBytes(bytes->data(), bytes->size());
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t
sweeps_metadata_get_hash(const sweeps_metadata_t* metadata, const char* key,
                         const sweeps_metadata_t** out_value) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::Metadata* source = metadataOf(metadata);
        if (source == nullptr || key == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "metadata or key is null");
        }
        const sweeps::Value* value = source->find(key);
        if (value == nullptr) {
            return fail(SWEEPS_ERR_NOT_FOUND, "no such key");
        }
        const sweeps::Metadata* hash = value->asHash();
        if (hash == nullptr) {
            return fail(SWEEPS_ERR_WRONG_TYPE, "that key does not hold a hash");
        }
        if (out_value != nullptr) {
            *out_value = asMetadataHandle(*hash);
        }
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_metadata_to_json(const sweeps_metadata_t* metadata, int indent,
                                                   char* buffer, size_t capacity,
                                                   size_t* out_length) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        const sweeps::Metadata* source = metadataOf(metadata);
        if (source == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "metadata is null");
        }
        return emitText(source->toJson(indent), buffer, capacity, out_length);
    });
}

// ---------------------------------------------------------------------------
// Metadata -- building
// ---------------------------------------------------------------------------

extern "C" sweeps_metadata_t* sweeps_metadata_create(void) SWEEPS_NOEXCEPT {
    return guardValue<sweeps_metadata_t*>(nullptr, []() -> sweeps_metadata_t* {
        return reinterpret_cast<sweeps_metadata_t*>(new sweeps::Metadata());
    });
}

extern "C" void sweeps_metadata_destroy(sweeps_metadata_t* metadata) SWEEPS_NOEXCEPT {
    guardVoid([&]() { delete mutableMetadataOf(metadata); });
}

namespace {

/// The shared preamble of every setter.
sweeps::Metadata* settable(sweeps_metadata_t* metadata, const char* key) noexcept {
    return metadata != nullptr && key != nullptr ? mutableMetadataOf(metadata) : nullptr;
}

} // namespace

extern "C" sweeps_status_t sweeps_metadata_set_string(sweeps_metadata_t* metadata, const char* key,
                                                      const char* value) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::Metadata* target = settable(metadata, key);
        if (target == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "metadata or key is null");
        }
        target->setString(key, std::string(textOf(value)));
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_metadata_set_i64(sweeps_metadata_t* metadata, const char* key,
                                                   int64_t value) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::Metadata* target = settable(metadata, key);
        if (target == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "metadata or key is null");
        }
        target->setInt(key, value);
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_metadata_set_f64(sweeps_metadata_t* metadata, const char* key,
                                                   double value) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::Metadata* target = settable(metadata, key);
        if (target == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "metadata or key is null");
        }
        target->setFloat(key, value);
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_metadata_set_bool(sweeps_metadata_t* metadata, const char* key,
                                                    int value) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::Metadata* target = settable(metadata, key);
        if (target == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "metadata or key is null");
        }
        target->setBool(key, value != 0);
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_metadata_set_bytes(sweeps_metadata_t* metadata, const char* key,
                                                     const void* data,
                                                     size_t bytes) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::Metadata* target = settable(metadata, key);
        if (target == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "metadata or key is null");
        }
        if (data == nullptr && bytes != 0) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "data is null but a length was given");
        }

        const auto* first = static_cast<const std::byte*>(data);
        target->setBytes(key, std::vector<std::byte>(first, first + bytes));
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t
sweeps_metadata_set_hash(sweeps_metadata_t* metadata, const char* key,
                         const sweeps_metadata_t* value) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::Metadata* target = settable(metadata, key);
        if (target == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "metadata or key is null");
        }
        const sweeps::Metadata* source = metadataOf(value);
        target->setHash(key, source != nullptr ? *source : sweeps::Metadata{});
        return SWEEPS_OK;
    });
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

extern "C" sweeps_status_t sweeps_writer_create(const char* path,
                                                const sweeps_writer_config_t* config,
                                                sweeps_writer_t** out) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        if (out == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "out is null");
        }
        *out = nullptr;
        if (path == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "path is null");
        }

        sweeps_writer_config_t staged;
        if (config != nullptr) {
            if (const sweeps_status_t status =
                    readStruct(config, sweeps_writer_config_init, staged);
                status != SWEEPS_OK) {
                return status;
            }
        } else {
            sweeps_writer_config_init(&staged);
        }

        sweeps::WriterConfig settings;
        settings.binsPerLine = staged.bins_per_line;
        settings.maxBytes = staged.max_bytes;
        settings.maxSeconds = staged.max_seconds;
        settings.minFreeBytes = staged.min_free_bytes;
        settings.sessionName = std::string(textOf(staged.session_name));
        settings.notes = std::string(textOf(staged.notes));
        settings.applicationVersion = std::string(textOf(staged.application_version));
        settings.createdWallNs = staged.created_wall_ns;
        settings.log = makeLog(staged.log, staged.log_user);

        auto writer = sweeps::SessionWriter::create(toPath(path), std::move(settings));
        if (!writer) {
            return fail(writer.error());
        }

        auto handle = std::make_unique<sweeps_writer_t>();
        handle->writer = std::move(*writer);
        handle->path = path;
        *out = handle.release();
        return SWEEPS_OK;
    });
}

extern "C" sweeps_status_t sweeps_writer_close(sweeps_writer_t* writer) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::SessionWriter* session = writerOf(writer);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "writer handle is null or already destroyed");
        }
        const sweeps::Status result = session->close();
        return result ? SWEEPS_OK : fail(result.error());
    });
}

extern "C" void sweeps_writer_destroy(sweeps_writer_t* writer) SWEEPS_NOEXCEPT {
    guardVoid([&]() {
        if (writer == nullptr || writer->magic != kWriterMagic) {
            return;
        }
        writer->magic = 0;
        delete writer;
    });
}

extern "C" sweeps_status_t
sweeps_writer_write_frame(sweeps_writer_t* writer, const sweeps_frame_t* frame,
                          sweeps_frame_outcome_t* outcome) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::SessionWriter* session = writerOf(writer);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "writer handle is null or already destroyed");
        }

        sweeps_frame_t stagedFrame;
        if (const sweeps_status_t status = readStruct(frame, sweeps_frame_init, stagedFrame);
            status != SWEEPS_OK) {
            return status;
        }
        // The C++ writer treats a null config as a silent no-op, which in C++ can
        // only be a programming error and in C is a forgotten field. Diagnosing
        // it is worth the small divergence: the alternative is a frame that
        // vanishes with a success status.
        if (stagedFrame.config == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "frame.config is null");
        }

        sweeps_acq_config_t stagedConfig;
        if (const sweeps_status_t status =
                readStruct(stagedFrame.config, sweeps_acq_config_init, stagedConfig);
            status != SWEEPS_OK) {
            return status;
        }

        const sweeps::AcquisitionConfig acquisition = toAcquisitionConfig(stagedConfig);

        sweeps::FrameView view;
        view.bins = stagedFrame.bins;
        view.count = stagedFrame.count;
        view.startHz = stagedFrame.start_hz;
        view.binWidthHz = stagedFrame.bin_width_hz;
        view.monotonicNs = stagedFrame.monotonic_ns;
        view.wallNs = stagedFrame.wall_ns;
        view.config = &acquisition;

        auto result = session->writeFrame(view);
        if (!result) {
            return fail(result.error());
        }

        writer->lastSegmentReason = result->reason;

        if (outcome != nullptr) {
            sweeps_frame_outcome_t staged{};
            staged.struct_size = sizeof(staged);
            staged.segment_id = result->segmentId;
            staged.segment_opened = result->segmentOpened ? 1 : 0;
            staged.retention_stopped = result->retentionStopped ? 1 : 0;
            return emitStruct(staged, outcome);
        }
        return SWEEPS_OK;
    });
}

namespace {

/// Appends one event, which is the same six lines for every kind.
template <typename Body>
sweeps_status_t recordEvent(sweeps_writer_t* writer, sweeps::SessionEvent::Kind kind,
                            std::uint64_t monotonicNs, std::uint64_t wallNs, Body body) noexcept {
    return guard([&]() -> sweeps_status_t {
        sweeps::SessionWriter* session = writerOf(writer);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "writer handle is null or already destroyed");
        }
        const sweeps::Status result = session->recordEvent(
            sweeps::SessionEvent::of(kind, monotonicNs, wallNs, std::move(body)));
        return result ? SWEEPS_OK : fail(result.error());
    });
}

} // namespace

extern "C" sweeps_status_t sweeps_writer_record_retune(sweeps_writer_t* writer,
                                                       uint64_t monotonic_ns, uint64_t wall_ns,
                                                       double center_hz,
                                                       uint32_t step_index) SWEEPS_NOEXCEPT {
    sweeps::RetuneData body;
    body.centerHz = center_hz;
    body.stepIndex = step_index;
    return recordEvent(writer, sweeps::SessionEvent::Kind::Retune, monotonic_ns, wall_ns,
                       std::move(body));
}

extern "C" sweeps_status_t sweeps_writer_record_parameter_changed(
    sweeps_writer_t* writer, uint64_t monotonic_ns, uint64_t wall_ns, const char* key,
    const char* value, int grid_affecting, int calibration_affecting) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::ParameterChangedData body;
        body.key = std::string(textOf(key));
        body.value = std::string(textOf(value));
        body.gridAffecting = grid_affecting != 0;
        body.calibrationAffecting = calibration_affecting != 0;
        return recordEvent(writer, sweeps::SessionEvent::Kind::ParameterChanged, monotonic_ns,
                           wall_ns, std::move(body));
    });
}

extern "C" sweeps_status_t
sweeps_writer_record_sweep_pass(sweeps_writer_t* writer, uint64_t monotonic_ns, uint64_t wall_ns,
                                uint64_t pass_id, double start_hz, double stop_hz,
                                double duration_seconds) SWEEPS_NOEXCEPT {
    sweeps::SweepPassData body;
    body.passId = pass_id;
    body.startHz = start_hz;
    body.stopHz = stop_hz;
    body.durationSeconds = duration_seconds;
    return recordEvent(writer, sweeps::SessionEvent::Kind::SweepPass, monotonic_ns, wall_ns,
                       std::move(body));
}

extern "C" sweeps_status_t sweeps_writer_record_marker(sweeps_writer_t* writer,
                                                       uint64_t monotonic_ns, uint64_t wall_ns,
                                                       const char* label, double frequency_hz,
                                                       double level_dbm) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::MarkerData body;
        body.label = std::string(textOf(label));
        body.frequencyHz = frequency_hz;
        body.levelDbm = level_dbm;
        return recordEvent(writer, sweeps::SessionEvent::Kind::Marker, monotonic_ns, wall_ns,
                           std::move(body));
    });
}

extern "C" sweeps_status_t sweeps_writer_record_annotation(sweeps_writer_t* writer,
                                                           uint64_t monotonic_ns, uint64_t wall_ns,
                                                           const char* text, double start_hz,
                                                           double stop_hz) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::AnnotationData body;
        body.text = std::string(textOf(text));
        body.startHz = start_hz;
        body.stopHz = stop_hz;
        return recordEvent(writer, sweeps::SessionEvent::Kind::Annotation, monotonic_ns, wall_ns,
                           std::move(body));
    });
}

extern "C" sweeps_status_t
sweeps_writer_record_segment_boundary(sweeps_writer_t* writer, uint64_t monotonic_ns,
                                      uint64_t wall_ns, const char* reason) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::SegmentBoundaryData body;
        body.reason = std::string(textOf(reason));
        return recordEvent(writer, sweeps::SessionEvent::Kind::SegmentBoundary, monotonic_ns,
                           wall_ns, std::move(body));
    });
}

extern "C" sweeps_status_t
sweeps_writer_record_throttle_changed(sweeps_writer_t* writer, uint64_t monotonic_ns,
                                      uint64_t wall_ns, const char* reason,
                                      double processed_fraction) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::ThrottleChangedData body;
        body.reason = std::string(textOf(reason));
        body.processedFraction = processed_fraction;
        return recordEvent(writer, sweeps::SessionEvent::Kind::ThrottleChanged, monotonic_ns,
                           wall_ns, std::move(body));
    });
}

extern "C" sweeps_status_t
sweeps_writer_record_device_error(sweeps_writer_t* writer, uint64_t monotonic_ns, uint64_t wall_ns,
                                  const char* device_id, const char* message) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::DeviceErrorData body;
        body.deviceId = std::string(textOf(device_id));
        body.message = std::string(textOf(message));
        return recordEvent(writer, sweeps::SessionEvent::Kind::DeviceError, monotonic_ns, wall_ns,
                           std::move(body));
    });
}

extern "C" sweeps_status_t
sweeps_writer_record_plugin_event(sweeps_writer_t* writer, uint64_t monotonic_ns, uint64_t wall_ns,
                                  const char* plugin_id, const char* event_name,
                                  const sweeps_metadata_t* fields) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::SessionWriter* session = writerOf(writer);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "writer handle is null or already destroyed");
        }

        const sweeps::Metadata* source = metadataOf(fields);
        const sweeps::Metadata empty;
        const sweeps::Status result =
            session->writePluginEvent(textOf(plugin_id), textOf(event_name), monotonic_ns, wall_ns,
                                      source != nullptr ? *source : empty);
        return result ? SWEEPS_OK : fail(result.error());
    });
}

extern "C" sweeps_status_t sweeps_writer_plugin_data(sweeps_writer_t* writer, const char* plugin_id,
                                                     const char* record_name,
                                                     uint32_t schema_version, uint64_t monotonic_ns,
                                                     const void* body,
                                                     size_t body_bytes) SWEEPS_NOEXCEPT {
    return guard([&]() -> sweeps_status_t {
        sweeps::SessionWriter* session = writerOf(writer);
        if (session == nullptr) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "writer handle is null or already destroyed");
        }
        if (body == nullptr && body_bytes != 0) {
            return fail(SWEEPS_ERR_INVALID_ARGUMENT, "body is null but a length was given");
        }

        const sweeps::Status result = session->writePluginData(
            textOf(plugin_id), textOf(record_name), schema_version, monotonic_ns, body, body_bytes);
        return result ? SWEEPS_OK : fail(result.error());
    });
}

extern "C" sweeps_str_t sweeps_writer_path(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT {
    return guardValue(
        kEmptyStr, [&]() { return writerOf(writer) != nullptr ? view(writer->path) : kEmptyStr; });
}

extern "C" uint64_t sweeps_writer_bytes_written(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT {
    const sweeps::SessionWriter* session = writerOf(writer);
    return session != nullptr ? session->bytesWritten() : 0;
}

extern "C" uint64_t sweeps_writer_lines_written(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT {
    const sweeps::SessionWriter* session = writerOf(writer);
    return session != nullptr ? session->linesWritten() : 0;
}

extern "C" uint32_t sweeps_writer_segment_count(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT {
    const sweeps::SessionWriter* session = writerOf(writer);
    return session != nullptr ? session->segmentCount() : 0;
}

extern "C" uint64_t sweeps_writer_last_frame_ns(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT {
    const sweeps::SessionWriter* session = writerOf(writer);
    return session != nullptr ? session->lastFrameNs() : 0;
}

extern "C" int sweeps_writer_retention_reached(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT {
    const sweeps::SessionWriter* session = writerOf(writer);
    return session != nullptr && session->retentionReached() ? 1 : 0;
}

extern "C" sweeps_str_t
sweeps_writer_retention_reason(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        const sweeps::SessionWriter* session = writerOf(writer);
        return session != nullptr ? view(session->retentionReason()) : kEmptyStr;
    });
}

extern "C" sweeps_str_t
sweeps_writer_last_segment_reason(const sweeps_writer_t* writer) SWEEPS_NOEXCEPT {
    return guardValue(kEmptyStr, [&]() {
        return writerOf(writer) != nullptr ? view(writer->lastSegmentReason) : kEmptyStr;
    });
}
