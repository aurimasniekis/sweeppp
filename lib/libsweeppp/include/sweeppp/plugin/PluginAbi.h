// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

/* Sweep++ plugins, from C.
 *
 * This header is the whole boundary. Everything else Sweep++ ships is C++ in a
 * *static* library, which is the fact that shapes this file: a plugin that
 * linked `sweeppp::sweeppp` and called `SdrDeviceManager::instance()` would get
 * its own private registry, register into it, and register into nothing the
 * host can see. Registration by linkage cannot work here, so the boundary is a
 * host-supplied function table -- `sweeppp_host_api_t` -- and a descriptor the
 * plugin hands back.
 *
 * The rule that follows, and it is the one to remember:
 *
 *     A plugin may link libsweeppp for VALUES -- Color, toml_util, Result,
 *     BandPlan, anything that is pure computation over its arguments. It must
 *     never touch a SINGLETON: Paths::instance(), SdrDeviceManager::instance(),
 *     FftBackendManager::instance(), Log. Those are per-image state, and the
 *     plugin's copy is not the host's. Everything that reaches the host goes
 *     through `sweeppp_host_api_t`.
 *
 * C99. It includes <stdint.h> and <stddef.h> and nothing else, deliberately:
 * `sweeppp/core/Version.hpp` is C++ and cannot be included here, so
 * SWEEPPP_PLUGIN_ABI_VERSION below is a literal that `kPluginAbiVersion`
 * mirrors rather than the other way round.
 *
 * ---------------------------------------------------------------------------
 * ABI stability
 * ---------------------------------------------------------------------------
 *
 * SWEEPPP_PLUGIN_ABI_VERSION is a promise. Within one ABI version:
 *
 *   - functions may be added to the end of a vtable that carries `struct_size`;
 *   - fields may be appended to a struct that carries `struct_size`;
 *   - enumerators may be added at the end of an enum.
 *
 * Nothing else. A signature, an existing enumerator's value, a field's offset
 * or a field's meaning changing is a new ABI version, and the host then refuses
 * the plugin by number rather than crashing on the layout.
 *
 * Every struct here begins with `uint32_t struct_size`, set by whoever created
 * it to `sizeof` its own view of the type. Both sides read only
 * `min(their size, the other's)`, so an old host can accept a new plugin's
 * larger struct and a new host can accept an old plugin's smaller one. A
 * `struct_size` of 0 is always rejected -- that is what makes a zeroed struct a
 * loud error rather than a silently empty one.
 *
 * Until the first release there is nothing to be compatible WITH, so a facet
 * may still be redefined in place rather than appended to -- the SDR one was,
 * wholesale. That freedom ends at the first tagged version, and it ends without
 * a warning: an out-of-tree plugin built against a redefined facet reads the
 * new layout with the old offsets, which is not a refusal by number but a
 * struct full of plausible nonsense. Every consumer is in this tree today, and
 * that is the only reason it was safe.
 *
 * ARRAYS need one extra rule, because indexing needs a stride and the elements
 * of `sweeppp_manifest_t::authors` cannot each be a different size in practice:
 * the reader takes the FIRST element's `struct_size` as the stride for the
 * whole array. All elements of one array come from one build, so this is
 * well-defined; an array with a count above zero whose first element has a
 * `struct_size` of 0 is rejected.
 *
 * ---------------------------------------------------------------------------
 * Lifetimes
 * ---------------------------------------------------------------------------
 *
 * Nothing is allocated across this boundary in either direction, and no
 * allocator crosses it except ImGui's (see the UI section, where it is the
 * whole point).
 *
 *   From the plugin: everything reachable from `sweeppp_plugin_query` --- the
 *   descriptor, the manifest, every string in it, the facet array and every
 *   vtable --- must have static storage duration and must stay valid and
 *   unchanged for the life of the process. The host reads them long after
 *   query returns, and keeps reading them after `deactivate`.
 *
 *   From the host: a `sweeppp_host_api_t*` handed to `activate` is valid until
 *   `deactivate` returns. Every `sweeppp_str_t` a host callback returns is
 *   valid until the next call into the host from the same thread. Every struct
 *   pointer passed *to* a plugin callback -- a plot context, a frame -- is
 *   valid for the duration of that call only.
 *
 * ---------------------------------------------------------------------------
 * Threads
 * ---------------------------------------------------------------------------
 *
 * Which thread calls what is not a detail here; it is the difference between a
 * slow plugin costing frames and a slow plugin stalling the radio.
 *
 *   `activate` / `deactivate`   the thread that loaded the plugin. Once each.
 *   UI facet callbacks          the UI thread, once per frame, never
 *                               concurrently, inside the host's clip-rect scope.
 *   Frame processor             a host-owned worker thread, one frame at a
 *                               time, in sequence order. May take as long as it
 *                               likes: the host queues, and drops this
 *                               plugin's own frames when the queue fills.
 *   SDR device vtable           the caller's thread -- the UI for a parameter,
 *                               the sweep thread for `retune`. Never
 *                               concurrently with itself.
 *   SDR stream callbacks        the PLUGIN's own transfer thread, which on a
 *                               USB radio is a libusb callback. See the
 *                               streaming section: those five must not block.
 *   FFT plan execute            any pipeline worker, concurrently, if the
 *                               backend advertises thread-safe execute.
 *
 * Host callbacks (`log`, `path`, the register/unregister pairs, the event
 * channel) are safe from any thread.
 *
 * A plugin callback MUST NOT let an exception escape. The host catches what it
 * can and disables the facet with the reason, but an exception crossing a C
 * frame is undefined behaviour and not something a catch can promise to hold.
 *
 * ---------------------------------------------------------------------------
 * Text
 * ---------------------------------------------------------------------------
 *
 * Every `sweeppp_str_t` is UTF-8, paths included. `data` is never null: an
 * absent or empty string is `{"", 0}`. Strings are NUL-terminated in practice
 * because they are always views of a literal or a `std::string`, so passing
 * `.data` to `printf("%s")` is safe -- but the length is what is authoritative.
 */
#ifndef SWEEPPP_PLUGIN_ABI_H
#define SWEEPPP_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Symbol visibility.
 *
 * A plugin is always a shared object, so unlike libsweepsfile's SWEEPS_API
 * there is no static case and no import case: the entry points are exported by
 * whoever compiles them, and the host reaches them by name through dlsym. */
#if defined(_WIN32)
#define SWEEPPP_PLUGIN_API __declspec(dllexport)
#elif defined(__GNUC__)
#define SWEEPPP_PLUGIN_API __attribute__((visibility("default")))
#else
#define SWEEPPP_PLUGIN_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Version
 * -------------------------------------------------------------------------- */

/** The ABI this header describes. See the stability note at the top. */
#define SWEEPPP_PLUGIN_ABI_VERSION 1

/* --------------------------------------------------------------------------
 * Strings
 * -------------------------------------------------------------------------- */

/** A borrowed UTF-8 string. See the text note at the top for the lifetime. */
typedef struct sweeppp_str_t {
    const char* data;
    size_t len;
} sweeppp_str_t;

/* --------------------------------------------------------------------------
 * Errors
 * -------------------------------------------------------------------------- */

/** Why a call failed.
 *
 *  The values mirror `sweeps_status_t`, which mirrors the C++ `ErrorCode`, and
 *  are frozen here for the same reason they are frozen there: deriving them
 *  from an enum's ordinal would mean inserting an enumerator in the middle of
 *  that enum silently renumbered this ABI. */
typedef enum sweeppp_plugin_status_t {
    SWEEPPP_PLUGIN_OK = 0,

    SWEEPPP_PLUGIN_ERR_UNKNOWN = -1,
    SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT = -2,
    SWEEPPP_PLUGIN_ERR_NOT_FOUND = -3,
    SWEEPPP_PLUGIN_ERR_UNSUPPORTED = -4,
    SWEEPPP_PLUGIN_ERR_UNAVAILABLE = -5,
    SWEEPPP_PLUGIN_ERR_IO = -6,
    SWEEPPP_PLUGIN_ERR_PARSE = -7,
    SWEEPPP_PLUGIN_ERR_OUT_OF_RANGE = -8,
    SWEEPPP_PLUGIN_ERR_OUT_OF_MEMORY = -9,
    SWEEPPP_PLUGIN_ERR_TIMED_OUT = -10,
    SWEEPPP_PLUGIN_ERR_CANCELLED = -11,
    SWEEPPP_PLUGIN_ERR_PERMISSION_DENIED = -12,
    SWEEPPP_PLUGIN_ERR_ALREADY_EXISTS = -13,
    SWEEPPP_PLUGIN_ERR_DEVICE = -14,
    SWEEPPP_PLUGIN_ERR_PROTOCOL = -15,
    SWEEPPP_PLUGIN_ERR_CORRUPT = -16,

    SWEEPPP_PLUGIN_STATUS_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_plugin_status_t;

/** A typed scalar, in and out.
 *
 *  Four alternatives, and they are exactly TOML's scalar types -- which is not
 *  a coincidence twice over: `SdrValue` is the same four, so a device
 *  parameter crosses here without an encoding trick, and a session manifest's
 *  values are the same four, so a plugin reading the running configuration and
 *  writing it into a record it emits uses one vocabulary for both.
 *
 *  The numbering mirrors `sweeps_value_type_t` where they overlap, for the
 *  same reason that one mirrors the file format: two numberings for one set of
 *  meanings is one more than can be kept in step. */
typedef enum sweeppp_value_type_t {
    SWEEPPP_VALUE_ABSENT = 0, /**< Not a value: what a missing key reports. */
    SWEEPPP_VALUE_STRING = 1,
    SWEEPPP_VALUE_INT = 2,
    SWEEPPP_VALUE_FLOAT = 3,
    SWEEPPP_VALUE_BOOL = 4,
    SWEEPPP_VALUE_TYPE_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_value_type_t;

/** Only the member matching `type` is meaningful. `text` is borrowed for the
 *  duration of the call it came from or was passed to. */
typedef struct sweeppp_value_t {
    uint32_t struct_size;
    sweeppp_value_type_t type;
    int32_t boolean;
    int64_t integer;
    double number;
    sweeppp_str_t text;
} sweeppp_value_t;

/** A named value, for a plugin event's fields. */
typedef struct sweeppp_field_t {
    uint32_t struct_size;
    sweeppp_str_t key;
    sweeppp_value_t value;
} sweeppp_field_t;

/** Levels the host logs at. Mirrors `sweeppp::LogLevel`. */
typedef enum sweeppp_log_level_t {
    SWEEPPP_LOG_TRACE = 0,
    SWEEPPP_LOG_DEBUG = 1,
    SWEEPPP_LOG_INFO = 2,
    SWEEPPP_LOG_WARN = 3,
    SWEEPPP_LOG_ERROR = 4,
    SWEEPPP_LOG_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_log_level_t;

/* --------------------------------------------------------------------------
 * Manifest
 *
 * In the binary, not in a sidecar file. A manifest that can disagree with the
 * code it describes is a manifest that eventually does, and the failure -- a
 * plugin whose declared id is not the id it registers under -- is exactly the
 * kind that surfaces as something else entirely.
 * -------------------------------------------------------------------------- */

typedef struct sweeppp_author_t {
    uint32_t struct_size;
    sweeppp_str_t name;
    sweeppp_str_t email; /**< May be empty. */
} sweeppp_author_t;

/** What a link points at, so the UI can group and label without parsing URLs. */
typedef enum sweeppp_link_type_t {
    SWEEPPP_LINK_HOMEPAGE = 1,
    SWEEPPP_LINK_REPOSITORY = 2,
    SWEEPPP_LINK_DOCUMENTATION = 3,
    SWEEPPP_LINK_ISSUES = 4,
    SWEEPPP_LINK_FUNDING = 5,
    SWEEPPP_LINK_OTHER = 6,
    SWEEPPP_LINK_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_link_type_t;

typedef struct sweeppp_link_t {
    uint32_t struct_size;
    sweeppp_link_type_t type;
    sweeppp_str_t url;
} sweeppp_link_t;

/** Another plugin this one needs.
 *
 *  `min_version` and `max_version` are inclusive dotted-decimal bounds, and
 *  either may be empty for "no bound". A dependency that is `optional` is
 *  reported when unmet but does not stop the plugin loading. */
typedef struct sweeppp_dependency_t {
    uint32_t struct_size;
    sweeppp_str_t plugin_id;
    sweeppp_str_t min_version;
    sweeppp_str_t max_version;
    int32_t optional;
} sweeppp_dependency_t;

/** Who a plugin is.
 *
 *  `id` is reverse-DNS -- "org.sweeppp.bandplan" -- and is capped at 128 bytes to
 *  match `sweeps::kMaxPluginIdBytes`. The cap is the file format's, not a
 *  preference: a plugin writing records into a `.sweeps` session is identified
 *  by this string on disk, so an id this ABI accepted and the container refused
 *  would be a plugin that works until it records.
 *
 *  `version` and `min_host_version` are dotted decimal ("1.2.3"), compared
 *  component by component with a missing component reading as zero.
 *
 *  `requires_restart_to_disable` is the plugin's own declaration that
 *  withdrawal cannot be done live -- it hands out objects whose lifetime it
 *  does not control, say. The host also decides this for itself when a facet
 *  refuses to withdraw; either is enough to mark the row. */
typedef struct sweeppp_manifest_t {
    uint32_t struct_size;

    sweeppp_str_t id;
    sweeppp_str_t version;
    sweeppp_str_t name;
    sweeppp_str_t description;

    const sweeppp_author_t* authors;
    uint32_t author_count;

    const sweeppp_link_t* links;
    uint32_t link_count;

    const sweeppp_dependency_t* dependencies;
    uint32_t dependency_count;

    sweeppp_str_t min_host_version;

    int32_t requires_restart_to_disable;
} sweeppp_manifest_t;

/** The longest an `id` may be, mirroring `sweeps::kMaxPluginIdBytes`. */
#define SWEEPPP_MAX_PLUGIN_ID_BYTES 128

/** What a plugin's *file* is called: `sweeppp-plugin-bandplan.so`.
 *
 *  A packaging convention rather than a load requirement -- a file in a
 *  directory that exists to hold plugins is a plugin whatever it is called --
 *  but it is what lets a plugin be installed into a directory it does not own.
 *
 *  The host will scan a shared library directory like `/usr/lib`, and there it
 *  considers only files with this prefix. It has to: deciding whether an
 *  arbitrary shared object is a Sweep++ plugin means `dlopen`ing it, and
 *  `dlopen` runs its initialisers. Doing that to every library on the system
 *  to find the two that are ours is not a cost or a risk worth taking, and the
 *  name is what makes the question answerable without asking it.
 *
 *  `lib` in front is accepted too, since some build systems insist on it. */
#define SWEEPPP_PLUGIN_FILE_PREFIX "sweeppp-plugin-"

/* --------------------------------------------------------------------------
 * Facets
 *
 * A facet is one thing a plugin contributes. The kind selects which vtable
 * `sweeppp_facet_t::vtable` points at, and that indirection is the whole
 * reason a single plugin can be a band plan *and* a device driver *and* draw
 * its own settings without three separate entry points.
 * -------------------------------------------------------------------------- */

typedef enum sweeppp_facet_kind_t {
    /** Sees every published spectrum frame, on a host-owned worker thread. */
    SWEEPPP_FACET_FRAME_PROCESSOR = 1,
    /** Contributes an SDR driver: enumerate, open, stream. */
    SWEEPPP_FACET_SDR_DEVICE = 2,
    /** Contributes an FFT implementation. */
    SWEEPPP_FACET_FFT_BACKEND = 3,
    /** Contributes what is known about a frequency: band plans, channel
     *  lists, beacon tables. */
    SWEEPPP_FACET_CONTRIBUTOR = 4,
    /** Draws into the host's own windows. */
    SWEEPPP_FACET_UI_EXTENSION = 5,
    /** Contributes an antenna switcher: a box between a receive port and
     *  several antennas. Not an SDR device -- it has no samples, no tuning and
     *  no stream. */
    SWEEPPP_FACET_RF_PATH = 6,
    SWEEPPP_FACET_KIND_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_facet_kind_t;

typedef struct sweeppp_facet_t {
    uint32_t struct_size;
    sweeppp_facet_kind_t kind;

    /** Stable within the plugin. For an SDR facet this is the driver name the
     *  registry keys on ("hackrf"); for an FFT facet the backend name. */
    sweeppp_str_t id;
    sweeppp_str_t name;
    sweeppp_str_t description;

    /** Cast to the vtable for `kind`. Never null. */
    const void* vtable;
} sweeppp_facet_t;

/* --------------------------------------------------------------------------
 * Frame processor facet
 * -------------------------------------------------------------------------- */

/** One published spectrum, flattened.
 *
 *  A projection of `sweeppp::SpectrumFrame`, not the whole of it: the parts a
 *  processor needs to locate a frame in time and frequency and read its bins.
 *  `bins_dbfs` points into a frame the host holds by `shared_ptr<const>` and is
 *  valid for the call only -- a processor that wants to alter the data
 *  republishes rather than writing here, because every other consumer is
 *  looking at the same buffer. */
typedef struct sweeppp_frame_t {
    uint32_t struct_size;

    uint64_t sequence;
    uint64_t host_time_ns;
    uint64_t wall_time_ns;

    uint64_t sweep_pass;
    uint32_t sweep_step;
    int32_t pass_complete;

    double start_hz;
    double bin_width_hz;

    const float* bins_dbfs;
    size_t bin_count;

    uint32_t average_count;
    float clipped_fraction;
} sweeppp_frame_t;

typedef struct sweeppp_frame_processor_vtable_t {
    uint32_t struct_size;

    /** One frame, on the host's worker thread for this facet, in sequence
     *  order and never concurrently with itself. May take as long as it needs;
     *  the cost of being slow is this plugin's own dropped frames. */
    void (*on_frame)(void* instance, const sweeppp_frame_t* frame);
} sweeppp_frame_processor_vtable_t;

/* --------------------------------------------------------------------------
 * Contributor facet
 *
 * What is known about a frequency, from whoever knows it. Band plans are the
 * obvious case and the first consumer; channel lists, licence databases and
 * alert regions are the same shape, which is why the facet is not called
 * "band plan".
 *
 * Several contributors answer the same frequency at once and ALL of their
 * answers are readable -- a band plan saying "2.4 GHz ISM, an allocation"
 * alongside a channel list saying "Wi-Fi channel 6, a specific 20 MHz
 * channel". Which of them titles the marker is the operator's ordering, not an
 * accident of which span happens to be narrower.
 * -------------------------------------------------------------------------- */

typedef enum sweeppp_contribution_type_t {
    /** A wide allocation. Indicative: "this region is for X". */
    SWEEPPP_CONTRIBUTION_BAND = 1,
    /** A defined channel with a width: Wi-Fi channel 6, FPV raceband F4. */
    SWEEPPP_CONTRIBUTION_CHANNEL = 2,
    /** One known frequency: a beacon, a repeater output, a control channel.
     *  `stop_hz == start_hz`. */
    SWEEPPP_CONTRIBUTION_SPOT = 3,
    SWEEPPP_CONTRIBUTION_TYPE_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_contribution_type_t;

/** Whether the host draws what this facet contributes.
 *
 *  HOST is 0 so a zeroed vtable gets the useful behaviour: the reason this
 *  facet exists is that every contributor otherwise reimplements spans,
 *  labels and insets. NONE is for a plugin whose visual the host has no
 *  vocabulary for; it draws through a UI facet instead and still contributes
 *  its data to the ranked answer. */
typedef enum sweeppp_contribution_render_t {
    SWEEPPP_CONTRIBUTION_RENDER_HOST = 0,
    SWEEPPP_CONTRIBUTION_RENDER_NONE = 1,
    SWEEPPP_CONTRIBUTION_RENDER_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_contribution_render_t;

typedef struct sweeppp_contribution_t {
    uint32_t struct_size;
    sweeppp_contribution_type_t type;

    sweeppp_str_t name;        /**< What it is called. The marker's title. */
    sweeppp_str_t description; /**< A sentence, or empty. Shown on hover. */
    sweeppp_str_t category;    /**< What the colour encodes: "amateur", "wifi". */

    double start_hz;
    double stop_hz; /**< == start_hz for a SPOT. */
    float color[4]; /**< RGBA, 0..1. */
} sweeppp_contribution_t;

/** One row of what a contributor lets the operator tick on and off -- a plan,
 *  a service, a channel -- for a host that draws the tree itself, such as the
 *  browser. Rows come in drawing order: a row's children follow it, one level
 *  deeper. */
typedef struct sweeppp_tree_row_t {
    uint32_t struct_size;
    uint32_t depth;    /**< 0 for a top-level row. */
    sweeppp_str_t key; /**< Opaque to the host; handed back to `tree_toggle`. */
    sweeppp_str_t name;
    sweeppp_str_t detail;      /**< Drawn dim beside the name: "12 · 2.4 - 2.5 GHz". */
    sweeppp_str_t description; /**< Shown on hover, or empty. */
    float color[4];            /**< A swatch beside the name; alpha 0 for none. */
    uint8_t any_on;            /**< Something at or under this row is shown. */
    uint8_t all_on;            /**< Everything at or under it is. */
    uint8_t reserved[2];
} sweeppp_tree_row_t;

typedef struct sweeppp_contributor_vtable_t {
    uint32_t struct_size;
    sweeppp_contribution_render_t render;

    /* Datasets -- one per band-plan file, per channel table. Zero is legal for
     * a contributor with a single fixed dataset. The host owns the selector,
     * which is what lets a contributor ship no UI code at all. */
    uint32_t (*dataset_count)(void* instance);
    sweeppp_str_t (*dataset_name)(void* instance, uint32_t index);
    sweeppp_str_t (*dataset_description)(void* instance, uint32_t index);
    uint32_t (*active_dataset)(void* instance);
    sweeppp_plugin_status_t (*select_dataset)(void* instance, uint32_t index);

    /* Both return how many there ARE, which may exceed `capacity`, so a caller
     * sizes a buffer with one call and fills it with a second. */
    uint32_t (*contributions_in)(void* instance, double from_hz, double to_hz,
                                 sweeppp_contribution_t* out, uint32_t capacity);
    /** Everything overlapping `hz`, most specific first -- a contributor knows
     *  its own nesting better than the host can infer it. Zero is a normal
     *  answer. */
    uint32_t (*contributions_at)(void* instance, double hz, sweeppp_contribution_t* out,
                                 uint32_t capacity);

    /** The operator dismissed one contribution on the plot. Optional; may be
     *  null, and a contributor that leaves it so simply cannot be dismissed
     *  that way.
     *
     *  A request to the plugin rather than a note the host keeps, because the
     *  plugin's own settings have to agree about it afterwards: a channel
     *  dismissed on the plot must come back unticked in the tree that lists
     *  it, not reappear the next time the plugin is asked. */
    sweeppp_plugin_status_t (*hide)(void* instance, const sweeppp_contribution_t* which);

    /** The selection tree as rows. Optional; null for a contributor with
     *  nothing to tick. Returns how many there are, as `contributions_in`
     *  does. The strings stay valid until the next `tree_rows` or
     *  `tree_toggle` on that instance. */
    uint32_t (*tree_rows)(void* instance, sweeppp_tree_row_t* out, uint32_t capacity);

    /** The operator clicked one row's tick: the same as clicking it in the
     *  plugin's own tree, whatever that means for the row's state. */
    sweeppp_plugin_status_t (*tree_toggle)(void* instance, sweeppp_str_t key);
} sweeppp_contributor_vtable_t;

/* --------------------------------------------------------------------------
 * SDR device facet
 *
 * Push-mode, and a mirror of the host's own `ISdrDevice` member for member.
 *
 * The alternative -- a `read_samples` the host pumps -- reads simpler and
 * cannot carry a real radio. libhackrf and libbladeRF hand over a completed USB
 * transfer from their own thread; a driver that had to answer a pull would
 * buffer that transfer and copy it out again, which at 100 MS/s is a second
 * pass over 200 MB/s that buys nothing. Worse, it fixes the format: a pull
 * returning `float*` means converting on the transfer thread, and complex<float>
 * at 100 MS/s is 800 MB/s, more than the link delivers. So the plugin borrows a
 * block from the host's pool, fills it in the radio's own format, and hands it
 * back. Nothing converts and nothing is copied twice.
 *
 * Pull mode is not carried alongside. `ISdrDevice::readSamples` already
 * synthesises it from the push stream for tests and simple tools, so a driver
 * gets it by implementing none of it.
 *
 * ---------------------------------------------------------------------------
 * Lifetimes, which differ per call and are the easiest thing here to get wrong
 * ---------------------------------------------------------------------------
 *
 *   Reachable from                            Valid until
 *   ----------------------------------------- ---------------------------------
 *   the vtables themselves                    for ever (static storage, as
 *                                             everywhere else in this header)
 *   `enumerate`'s out[], `info`'s *out        the call returns; the host copies
 *                                             inside it
 *   `parameters`' out[] and all under it      the next `parameters` on that
 *                                             device, or `destroy`
 *   `health_readings`' out[]                  the next `health_readings` on that
 *                                             device
 *   `get_parameter`'s out->text,              the next call into that
 *   `open`'s *error                           device/factory from this thread
 *   sweeppp_sdr_block_t::data                 `publish` or `release` for that
 *                                             handle
 *
 * `parameters` is the ONE exception to this header's everything-is-static rule,
 * and it is there because a real radio needs it: a bladeRF's gain modes come
 * from `bladerf_get_gain_modes()` at open, so no static table can express them.
 * -------------------------------------------------------------------------- */

/** What one sample looks like on the wire.
 *
 *  Ordinal for ordinal with `sweeppp::SampleFormat`. The host converts with a
 *  switch rather than a cast, so an enumerator added on one side and not the
 *  other is a compile error there rather than a misread buffer here. */
typedef enum sweeppp_sdr_format_t {
    SWEEPPP_SDR_FORMAT_CU8 = 0,   /**< 2 bytes/frame. */
    SWEEPPP_SDR_FORMAT_CS8 = 1,   /**< 2 bytes/frame. */
    SWEEPPP_SDR_FORMAT_CU12 = 2,  /**< 3 bytes/frame, packed. */
    SWEEPPP_SDR_FORMAT_CS12 = 3,  /**< 3 bytes/frame, packed. */
    SWEEPPP_SDR_FORMAT_CU16 = 4,  /**< 4 bytes/frame. */
    SWEEPPP_SDR_FORMAT_CS16 = 5,  /**< 4 bytes/frame. */
    SWEEPPP_SDR_FORMAT_CU32 = 6,  /**< 8 bytes/frame. */
    SWEEPPP_SDR_FORMAT_CS32 = 7,  /**< 8 bytes/frame. */
    SWEEPPP_SDR_FORMAT_CF16 = 8,  /**< 4 bytes/frame, IEEE binary16. */
    SWEEPPP_SDR_FORMAT_CF32 = 9,  /**< 8 bytes/frame. */
    SWEEPPP_SDR_FORMAT_CF64 = 10, /**< 16 bytes/frame. */
    SWEEPPP_SDR_FORMAT_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_sdr_format_t;

/** Mirrors `sweeppp::SdrParameterType`. */
typedef enum sweeppp_sdr_parameter_type_t {
    SWEEPPP_SDR_PARAM_BOOL = 0,
    SWEEPPP_SDR_PARAM_INT = 1,
    SWEEPPP_SDR_PARAM_DOUBLE = 2,
    SWEEPPP_SDR_PARAM_ENUM = 3, /**< Int or string constrained to `enum_values`. */
    SWEEPPP_SDR_PARAM_STRING = 4,
    SWEEPPP_SDR_PARAM_TYPE_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_sdr_parameter_type_t;

/** One selectable value of an enum parameter. */
typedef struct sweeppp_sdr_enum_value_t {
    uint32_t struct_size;
    sweeppp_str_t value; /**< Stored form, e.g. "rx1". */
    sweeppp_str_t label; /**< Shown form, e.g. "RX1 (SMA)". */
    sweeppp_str_t description;
} sweeppp_sdr_enum_value_t;

/** Something a radio can say about its own condition.
 *
 *  Formatted by the driver rather than handed over as a bare number, because
 *  the unit and the sensible precision are things only the driver knows, and
 *  the panel showing them must not need per-device knowledge to do it. A
 *  `maximum` at or below `minimum` means the reading has no meaningful scale --
 *  a power source, a serial -- and is shown as text alone. */
typedef struct sweeppp_sdr_health_t {
    uint32_t struct_size;
    sweeppp_str_t label;
    sweeppp_str_t value;
    float numeric;
    float minimum;
    float maximum;
    int32_t alarm; /**< Outside what the hardware is happy with. */
} sweeppp_sdr_health_t;

/** A version the device reports, and how current the driver believes it is. */
typedef struct sweeppp_sdr_version_t {
    uint32_t struct_size;
    sweeppp_str_t version;
    sweeppp_str_t known_latest;
    int32_t ahead_of_driver;
} sweeppp_sdr_version_t;

/** A self-describing control.
 *
 *  Complete on purpose. The SDR panel is generated from these alone -- there is
 *  no per-device UI code anywhere in the project -- so a field left out is not
 *  an undocumented control, it is an unreachable one. */
typedef struct sweeppp_sdr_parameter_t {
    uint32_t struct_size;

    sweeppp_str_t key;   /**< Stable identifier, persisted in profiles. */
    sweeppp_str_t label; /**< "LNA gain" */
    sweeppp_str_t group; /**< Panel section: "Gain", "Tuning", "Filters". */
    sweeppp_sdr_parameter_type_t type;

    sweeppp_str_t unit; /**< "Hz", "dB", "S/s"; empty when dimensionless. */
    double minimum;
    double maximum;
    /** Quantisation the hardware actually enforces. A HackRF's LNA gain moves
     *  in 8 dB steps; a continuous slider would let an operator set a value the
     *  radio silently rounds, and then disbelieve the readout. */
    double step;

    const sweeppp_sdr_enum_value_t* enum_values;
    uint32_t enum_value_count;

    int32_t read_only;
    /** Redefines the frequency grid, so it closes the session segment and opens
     *  a new one. Sample rate, bandwidth, centre frequency. */
    int32_t grid_affecting;
    /** Shifts the noise floor without changing the grid, so later analysis has
     *  to know it happened. Gain stages, reference level. */
    int32_t calibration_affecting;
    /** Cannot change while streaming; the panel disables it rather than letting
     *  the device reject it. */
    int32_t requires_stop;

    sweeppp_str_t description;
    sweeppp_value_t default_value;

    /** Another parameter this one depends on, and the values of it that make
     *  this one apply. An empty key means always.
     *
     *  Declared rather than left to the panel, which knows nothing about any
     *  particular radio and must not start: a manual gain means nothing while
     *  the gain mode is automatic, and only the driver can say so. */
    sweeppp_str_t applies_when_key;
    const sweeppp_str_t* applies_when_values;
    uint32_t applies_when_value_count;
} sweeppp_sdr_parameter_t;

/** Identity of a radio, enough to list it and to reopen it later.
 *
 *  No `driver` field: the facet's own id is what the registry keys on, and a
 *  device claiming a different one would be a device the host could not find
 *  its way back to. */
typedef struct sweeppp_sdr_device_info_t {
    uint32_t struct_size;
    sweeppp_str_t id; /**< Unique within the driver; usually the serial. */
    sweeppp_str_t label;
    sweeppp_str_t serial;
    sweeppp_str_t hardware_revision; /**< The variant alone: "r9", "xA9". */
    double min_frequency_hz;
    double max_frequency_hz;
    double min_sample_rate;
    double max_sample_rate;

    sweeppp_sdr_version_t firmware;
    sweeppp_sdr_version_t fpga;

    /** Link capacity in bytes/s, zero when unknown. The host publishes it into
     *  the telemetry itself, so every plugin driver gets the Performance
     *  panel's link-utilisation bar for nothing -- an operator seeing 95% of a
     *  USB 2.0 link knows the cable is the problem, not the settings. */
    uint64_t link_capacity_bytes_per_sec;
    sweeppp_str_t link_description; /**< "USB 3.0 SuperSpeed" */
} sweeppp_sdr_device_info_t;

/* ---- streaming ----------------------------------------------------------
 *
 * Nothing in `sweeppp_sdr_stream_t` blocks, allocates or takes a contended
 * lock. That is what makes it callable from inside a libusb callback, which is
 * exactly where a HackRF driver calls `publish`.
 *
 * POOL EXHAUSTION IS A NORMAL ANSWER, NOT A FAILURE. `acquire` returning a null
 * handle means the consumer side is behind. The plugin MUST NOT wait for a
 * block: waiting on a transfer thread turns a host-side backlog into a
 * device-side overrun, which is a worse failure and a misleading one. Drop the
 * data, call `report_pool_exhausted`, and return.
 */

/** A block lent to the plugin, to be filled and handed straight back. */
typedef struct sweeppp_sdr_block_t {
    uint32_t struct_size;
    /** NULL when the pool is exhausted -- see above. */
    void* handle;
    uint8_t* data;
    size_t capacity_bytes;
} sweeppp_sdr_block_t;

/** Everything about a filled block except the block. Mirrors `IqBlock`. */
typedef struct sweeppp_sdr_delivery_t {
    uint32_t struct_size;

    void* handle;  /**< From the `acquire` that lent this block. */
    size_t frames; /**< Complex samples, NOT bytes. Clamped to what fits. */
    sweeppp_sdr_format_t format;

    /** The plugin's own, monotonic per stream. The host detects gaps with it,
     *  which is how an overrun is spotted on a radio that reports none. */
    uint64_t sequence;

    /** Stamped when the block STARTED filling, not when it was published; 0
     *  means the host stamps it on arrival. */
    uint64_t host_time_ns;
    uint64_t device_time_ns; /**< Device metadata where there is any, else 0. */

    /** In force when these samples were captured. Carried per block because
     *  during a sweep they change every few milliseconds, and the worker that
     *  eventually processes this block must not have to guess which step it
     *  belonged to. */
    double center_hz;
    double sample_rate;

    uint32_t sweep_step;
    uint64_t sweep_pass;
    /** Captured inside the post-retune settle window. Carried rather than
     *  dropped at the source, so the discard decision stays in one place and
     *  shows up in the telemetry. */
    int32_t settling;
} sweeppp_sdr_delivery_t;

/** The host's side of a running stream. Valid until `stop` returns.
 *
 *  `publish` is serialised by the plugin -- deliveries must not overlap, and
 *  their sequence numbers must ascend. The rest are callable from any thread. */
typedef struct sweeppp_sdr_stream_t {
    uint32_t struct_size;
    void* stream;

    /** Borrows a block. A null `handle` means the pool is exhausted, which is
     *  an answer: drop and report, never wait. */
    sweeppp_sdr_block_t (*acquire)(void* stream);

    /** Hands one back filled. The handle is consumed either way. */
    void (*publish)(void* stream, const sweeppp_sdr_delivery_t* delivery);

    /** Hands one back UNfilled, for a stop caught mid-transfer. A handle
     *  neither published nor released is a block the pool never gets back. */
    void (*release)(void* stream, void* handle);

    /** Samples the DEVICE lost -- its own FIFO overran. */
    void (*report_device_overrun)(void* stream, uint64_t samples);
    /** Samples dropped because `acquire` came back empty. */
    void (*report_pool_exhausted)(void* stream, uint64_t samples);
} sweeppp_sdr_stream_t;

/** Mirrors `sweeppp::StreamConfig`. */
typedef struct sweeppp_sdr_stream_config_t {
    uint32_t struct_size;
    size_t frames_per_block;
    size_t block_count;
    sweeppp_sdr_format_t format;
} sweeppp_sdr_stream_config_t;

/** How many of each an array-filling call will take. Bounded because the count
 *  comes from the plugin and sizes a buffer: a driver returning a nonsense
 *  figure should cost a truncated list, not the address space. One call each
 *  rather than ask-then-fill, so the strings need only survive one call. */
#define SWEEPPP_SDR_MAX_PARAMETERS 128
#define SWEEPPP_SDR_MAX_ENUM_VALUES 64
#define SWEEPPP_SDR_MAX_SAMPLE_RATES 64
#define SWEEPPP_SDR_MAX_HEALTH_READINGS 32
#define SWEEPPP_SDR_MAX_RX_PORTS 16

/** One RF input the tuner can listen on.
 *
 *  A first-class object rather than an enum parameter spelling "RX1"/"RX2":
 *  the sweep planner routes by these, so it needs the frequency limits and the
 *  switching cost, and the antenna assignment keys on `id`, so the id has to
 *  outlive a firmware update that reorders the list. */
typedef struct sweeppp_sdr_rx_port_t {
    uint32_t struct_size;
    sweeppp_str_t id;        /**< Stable; what an assignment stores. "rx1" */
    sweeppp_str_t label;     /**< "RX1" */
    sweeppp_str_t connector; /**< What is printed on the case: "SMA (J1)". */

    /** Where this port can tune, when it is narrower than the device's own
     *  range. Zero means `info`'s limits apply -- `info` reports the union
     *  across ports, so a plan inside it may still be unreachable on one. */
    double min_hz;
    double max_hz;

    int32_t bias_tee;      /**< This port can supply DC on the connector. */
    int32_t requires_stop; /**< Selecting it needs the stream stopped. */

    /** Settle after selecting, beyond a retune. The host charges this once per
     *  transition when it predicts a pass, and waits it out before trusting
     *  anything the new port delivers. */
    double switch_seconds;
} sweeppp_sdr_rx_port_t;

/** A radio.
 *
 *  Everything after `destroy` may be NULL, and the host falls back to what
 *  `ISdrDevice` does by default. A trivial driver implements four entries; a
 *  HackRF implements all of them. */
typedef struct sweeppp_sdr_device_vtable_t {
    uint32_t struct_size;

    /** Releases the device. Called exactly once, never while streaming. */
    void (*destroy)(void* device);

    /** Identity, again -- firmware, FPGA and link speed all need an open
     *  handle, so `enumerate` cannot know them. */
    sweeppp_plugin_status_t (*info)(void* device, sweeppp_sdr_device_info_t* out);

    /** Every control this radio exposes, written into `out` up to `capacity`;
     *  returns how many there are. See the lifetime table above -- this is the
     *  one call whose results may point at memory the device owns. */
    uint32_t (*parameters)(void* device, sweeppp_sdr_parameter_t* out, uint32_t capacity);

    sweeppp_plugin_status_t (*get_parameter)(void* device, sweeppp_str_t key, sweeppp_value_t* out);
    /** The host coerces against the descriptor first, so a value arriving here
     *  is already clamped and snapped to what `parameters` declared. */
    sweeppp_plugin_status_t (*set_parameter)(void* device, sweeppp_str_t key,
                                             const sweeppp_value_t* value);

    /** What `publish` will actually carry, which may differ from what the
     *  stream config asked for. */
    sweeppp_sdr_format_t (*native_format)(void* device);

    /** Rates the hardware really supports. Zero means continuous within the
     *  range `info` reports. */
    uint32_t (*supported_sample_rates)(void* device, double* out, uint32_t capacity);

    sweeppp_plugin_status_t (*start)(void* device, const sweeppp_sdr_stream_config_t* config,
                                     const sweeppp_sdr_stream_t* stream);
    /** Must not return until the plugin's transfer thread has stopped touching
     *  `stream`, because the host tears it down as soon as this returns. */
    void (*stop)(void* device);
    int32_t (*streaming)(void* device);

    /** Retunes while streaming. Separate from `set_parameter("center_hz")`
     *  because the sweep engine calls it thousands of times a second and needs
     *  the fastest path the driver has. */
    sweeppp_plugin_status_t (*retune)(void* device, double center_hz);

    /** Seconds the hardware needs after a retune before its output can be
     *  trusted. The sweep engine discards this much rather than guessing, and
     *  it is per-device because a HackRF and a bladeRF differ by an order of
     *  magnitude. */
    double (*retune_settle_seconds)(void* device);

    /** How much signal time arrives in one delivery.
     *
     *  The hard floor on how fast a sweep can retune. A USB radio hands over a
     *  whole transfer at once, and everything in it carries one tuning because
     *  that is all the driver can know. Retune faster than this and each
     *  delivery spans several steps, so most steps are attributed nothing while
     *  a few collect everything -- which looks like a sweep hopping at random.
     *  Zero means the device does not constrain the sweep. */
    double (*delivery_granularity_seconds)(void* device, double sample_rate);

    /** Whatever the device can say about itself. Called at a few Hz, never per
     *  frame: on a USB radio each of these is a control transfer, and issuing a
     *  handful every rendered frame competes with the sample stream for the
     *  same bus. */
    uint32_t (*health_readings)(void* device, sweeppp_sdr_health_t* out, uint32_t capacity);

    /** The RF inputs this radio has, written into `out` up to `capacity`;
     *  returns how many there are.
     *
     *  Same lifetime rule as `parameters`: what the strings point at need only
     *  survive until the next such call on this device. A driver with one
     *  connector implements none of these three and the host treats it as
     *  having a single implicit input.
     *
     *  Not a compile-time list: bladeRF asks the hardware how many RX channels
     *  it has, which is why the host reads this after `open` rather than at
     *  registration. */
    uint32_t (*rx_ports)(void* device, sweeppp_sdr_rx_port_t* out, uint32_t capacity);

    /** Which one is selected, by id. */
    sweeppp_str_t (*selected_rx_port)(void* device);

    /** Selects one. A port declaring `requires_stop` has had its stream
     *  stopped by the host before this is called. */
    sweeppp_plugin_status_t (*select_rx_port)(void* device, sweeppp_str_t id);
} sweeppp_sdr_device_vtable_t;

typedef struct sweeppp_sdr_factory_vtable_t {
    uint32_t struct_size;

    /** Radios attached now. Written into `out` up to `capacity`; returns how
     *  many there are. Called on a timer, so it must be cheap. */
    uint32_t (*enumerate)(void* instance, sweeppp_sdr_device_info_t* out, uint32_t capacity);

    /** Opens one. `id` empty means the first available.
     *
     *  `error` receives why, for a failure worth explaining. ERR_DEVICE on its
     *  own is a mystery; "LIBUSB_ERROR_ACCESS (on Linux this is usually a
     *  missing udev rule)" is something an operator can act on, and there is
     *  nowhere else for that sentence to live. May be left untouched. */
    sweeppp_plugin_status_t (*open)(void* instance, sweeppp_str_t id, void** device,
                                    const sweeppp_sdr_device_vtable_t** vtable,
                                    sweeppp_str_t* error);
} sweeppp_sdr_factory_vtable_t;

/* --------------------------------------------------------------------------
 * RF path facet
 *
 * An antenna switcher: a 2/4/8/16/32-way box sitting behind one receive port,
 * whose inputs each carry an antenna of their own, so the chain the host
 * routes through becomes port -> switcher -> input -> antenna.
 *
 * Its own facet kind rather than a repurposed SDR device, and the distinction
 * is not pedantic: a switcher has no samples, no tuning and no stream, and
 * listing one among the radios would be a lie the device chooser could not
 * recover from -- an operator picking it would get a receiver that never
 * delivers a block.
 * -------------------------------------------------------------------------- */

/** How many inputs one switcher will report. Bounded for the same reason
 *  everything else here is: the count comes from the plugin and sizes a
 *  buffer. Above any real box -- a 32-way is the largest commonly built. */
#define SWEEPPP_RF_PATH_MAX_INPUTS 64

/** Identity of a switcher, enough to list it and to reopen it later. */
typedef struct sweeppp_rf_path_info_t {
    uint32_t struct_size;
    sweeppp_str_t id;    /**< Unique within the driver; usually the serial. */
    sweeppp_str_t label; /**< "Mini-Circuits USB-1SP8T-63H" */
    sweeppp_str_t model;
    sweeppp_str_t serial;

    uint32_t input_count;

    /** Selecting an input needs the receiver's stream stopped. Rare -- a relay
     *  box does not care -- but a solid-state switch that glitches the RF
     *  during transition does, and the host cycles the stream around it the
     *  same way it does for a receive port that asks. */
    int32_t requires_stop;

    /** Settle after selecting, in seconds. A mechanical relay is tens of
     *  milliseconds and an operator's predicted pass time should say so. */
    double switch_seconds;
} sweeppp_rf_path_info_t;

/** One input of a switcher. */
typedef struct sweeppp_rf_path_input_t {
    uint32_t struct_size;
    sweeppp_str_t id;    /**< Stable; what an assignment stores. "in1" */
    sweeppp_str_t label; /**< "J1" */

    /** Where this input is usable, when the box itself narrows it -- a
     *  filtered or amplified port. Zero means the switcher does not
     *  constrain it and only the antenna's own range applies. */
    double min_hz;
    double max_hz;
} sweeppp_rf_path_input_t;

/** An antenna switcher. Everything after `destroy` may be NULL. */
typedef struct sweeppp_rf_path_vtable_t {
    uint32_t struct_size;

    /** Releases it. Called exactly once. */
    void (*destroy)(void* path);

    sweeppp_plugin_status_t (*info)(void* path, sweeppp_rf_path_info_t* out);

    /** The inputs, written into `out` up to `capacity`; returns how many there
     *  are. Same lifetime rule as the SDR facet's `parameters`: valid until
     *  the next such call on this switcher. */
    uint32_t (*inputs)(void* path, sweeppp_rf_path_input_t* out, uint32_t capacity);

    /** Selects one, by position in `inputs`. An index rather than an id
     *  because this one *is* on the sweep path: the engine may drive it
     *  several times a pass and must not be comparing strings to do it. */
    sweeppp_plugin_status_t (*select_input)(void* path, uint32_t index);
    uint32_t (*selected_input)(void* path);

    /** Whatever the box can say about itself -- supply voltage, relay cycle
     *  count, temperature. Called at a few Hz, never per frame. */
    uint32_t (*health_readings)(void* path, sweeppp_sdr_health_t* out, uint32_t capacity);
} sweeppp_rf_path_vtable_t;

/** One driver's worth of switchers. Mirrors `sweeppp_sdr_factory_vtable_t`,
 *  because a switcher is discovered and opened exactly like a radio even
 *  though it is not one. */
typedef struct sweeppp_rf_path_factory_vtable_t {
    uint32_t struct_size;

    /** Switchers attached now. Called on a timer, so it must be cheap. */
    uint32_t (*enumerate)(void* instance, sweeppp_rf_path_info_t* out, uint32_t capacity);

    /** Opens one. `id` empty means the first available. `error` receives the
     *  sentence an operator can act on. */
    sweeppp_plugin_status_t (*open)(void* instance, sweeppp_str_t id, void** path,
                                    const sweeppp_rf_path_vtable_t** vtable, sweeppp_str_t* error);
} sweeppp_rf_path_factory_vtable_t;

/* --------------------------------------------------------------------------
 * FFT backend facet
 * -------------------------------------------------------------------------- */

typedef struct sweeppp_fft_capabilities_t {
    uint32_t struct_size;
    int32_t is_gpu;
    size_t min_size;
    size_t max_size;
    int32_t power_of_two_only;
    int32_t supports_batch;
    int32_t supports_in_place;
    int32_t thread_safe_execute;
    int32_t thread_safe_planning;
} sweeppp_fft_capabilities_t;

typedef struct sweeppp_fft_plan_config_t {
    uint32_t struct_size;
    size_t size;
    size_t batch_count;
    int32_t inverse;
    int32_t in_place;
    /** 0 fast, 1 balanced, 2 thorough. */
    int32_t quality;
} sweeppp_fft_plan_config_t;

typedef struct sweeppp_fft_plan_vtable_t {
    uint32_t struct_size;
    void (*destroy)(void* plan);

    /** `input` and `output` are `size` interleaved complex<float> pairs.
     *  Callable concurrently from several threads with separate buffers when
     *  the backend advertises `thread_safe_execute`. */
    void (*execute)(void* plan, const float* input, float* output);
    void (*execute_batch)(void* plan, const float* input, float* output, size_t count);
} sweeppp_fft_plan_vtable_t;

typedef struct sweeppp_fft_backend_vtable_t {
    uint32_t struct_size;

    sweeppp_plugin_status_t (*capabilities)(void* instance, sweeppp_fft_capabilities_t* out);

    sweeppp_plugin_status_t (*create_plan)(void* instance, const sweeppp_fft_plan_config_t* config,
                                           void** plan, const sweeppp_fft_plan_vtable_t** vtable);

    /** Nearest size at or above `desired` that this backend accepts. */
    size_t (*snap_size)(void* instance, size_t desired);

    /** Discards cached plans and wisdom. Optional; may be null. */
    void (*reset)(void* instance);
} sweeppp_fft_backend_vtable_t;

/* --------------------------------------------------------------------------
 * UI extension facet
 *
 * Small, named spots rather than a general "draw anywhere". A plugin that can
 * draw anywhere can draw over the spectrum, and the spectrum is the
 * instrument.
 * -------------------------------------------------------------------------- */

typedef enum sweeppp_ui_spot_t {
    SWEEPPP_UI_SPOT_SPECTRUM_OVERLAY = 1,
    SWEEPPP_UI_SPOT_WATERFALL_OVERLAY = 2,
    /** The body of this plugin's own row in the Plugins panel. */
    SWEEPPP_UI_SPOT_SETTINGS = 3,
    /** A chip in the status bar, beside the host's own. */
    SWEEPPP_UI_SPOT_STATUS_CHIP = 4,
    /** A button on the toolbar, beside the panel launchers.
     *
     *  Drawn into a window the host owns, sharing its ImGui id stack with the
     *  host's own controls and with every other plugin's. The dispatcher
     *  cannot scope that -- it is in a library that also builds headless and
     *  has no ImGui to push an id with -- so `makeUiVtable` pushes the
     *  instance pointer around every draw instead. A plugin written straight
     *  against this ABI has no such wrapper and must qualify what it submits:
     *  two plugins that both label a popup "##settings" are one popup as far
     *  as ImGui is concerned. */
    SWEEPPP_UI_SPOT_TOOLBAR = 5,
    /** A floating window of the plugin's own, dispatched once per frame at the
     *  top level -- beside the application's own windows, and outside every
     *  popup and child the other spots draw inside.
     *
     *  A plugin that begins a window from SETTINGS gets one that vanishes with
     *  the menu popup it was begun in; one that begins it from TOOLBAR gets a
     *  window whose position is a lie about where it was drawn. This spot is
     *  the answer to both. The plugin owns whether it is open. */
    SWEEPPP_UI_SPOT_WINDOW = 6,
    /** A button in the status bar, after the host's own panel buttons.
     *
     *  Distinct from STATUS_CHIP, which is a readout dispatched among the
     *  chips at the far left of the same bar. A plugin whose panel is opened
     *  from the bar wants its button beside the host's -- next to
     *  Performance -- not wedged between two numbers that are being read.
     *
     *  Shares the bar's ImGui id stack, so the same two rules as TOOLBAR
     *  apply: qualify every id submitted here, and end the draw with
     *  `ImGui::SameLine()` so the next control lands beside it. */
    SWEEPPP_UI_SPOT_STATUS_ACTION = 7,
    SWEEPPP_UI_SPOT_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_ui_spot_t;

/** Whether an overlay is drawn beneath the traces or over them. Under is the
 *  right default: a band plan is context, and context that hides the
 *  measurement is worse than no context. */
typedef enum sweeppp_ui_layer_t {
    SWEEPPP_UI_LAYER_UNDER = 0,
    SWEEPPP_UI_LAYER_OVER = 1,
    SWEEPPP_UI_LAYER_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_ui_layer_t;

/** Bit for a spot in `sweeppp_ui_vtable_t::spots`. */
#define SWEEPPP_UI_SPOT_BIT(spot) (1u << ((unsigned)(spot) - 1u))

/** Where a plot is on screen and what it is showing.
 *
 *  Mirrors the host's own `SpectrumLayout`. The mapping functions are not in
 *  the vtable because they are four lines of arithmetic each: the C++ wrapper
 *  reimplements them identically rather than paying an indirect call per
 *  point, and the definitions are here so a plugin written straight against
 *  the C ABI gets the same answers.
 *
 *      x  = origin_x + (hz - from_hz) / (to_hz - from_hz) * size_x
 *      hz = from_hz + (x - origin_x) / size_x * (to_hz - from_hz)
 *      y  = origin_y + (1 - (db - min_db) / (max_db - min_db)) * size_y
 *
 *  `draw_list` is an `ImDrawList*` already clipped to the plot rectangle. */
typedef struct sweeppp_plot_context_t {
    uint32_t struct_size;

    sweeppp_ui_spot_t spot;
    sweeppp_ui_layer_t layer;

    void* draw_list;

    float origin_x;
    float origin_y;
    float size_x;
    float size_y;

    double from_hz;
    double to_hz;
    float min_db;
    float max_db;

    /** Where the operator's marker is, so an overlay can annotate the same
     *  frequency the readouts are describing. Zero when no marker is placed. */
    double marker_hz;

    /** How strongly the active theme wants an overlay to tint the plot.
     *
     *  From the theme, so a plugin's overlay is as loud as the rest of the
     *  chrome and gets quieter with it when the operator picks a subtler
     *  theme. A plugin that needs its own value ignores this; one that does
     *  not gets a themed default for free -- the same `contribution_alpha` the
     *  host tints its own contribution spans with. */
    float overlay_alpha;
} sweeppp_plot_context_t;

typedef struct sweeppp_ui_vtable_t {
    uint32_t struct_size;

    /** Which spots this facet draws in: an OR of SWEEPPP_UI_SPOT_BIT(...).
     *  A spot whose bit is clear is never called even if its pointer is set. */
    uint32_t spots;

    /** Which layer the overlays want. Ignored for the other spots. */
    sweeppp_ui_layer_t layer;

    /** Spectrum and waterfall overlays. `ctx->spot` says which. */
    void (*draw_overlay)(void* instance, const sweeppp_plot_context_t* ctx);

    /** The plugin's settings, inside its own row. Returns 0 for "nothing to
     *  show", which the host renders as such rather than leaving a blank. */
    int32_t (*draw_settings)(void* instance);

    void (*draw_status_chip)(void* instance);
    void (*draw_toolbar)(void* instance);

    /** The plugin's own floating window. Call `ImGui::Begin`/`End` here; the
     *  host is between frames' top-level windows and inside nothing. */
    void (*draw_window)(void* instance);

    /** Which contribution type this facet's toolbar button governs, or 0.
     *
     *  What lets the host route a keystroke to the right plugin's panel
     *  without learning any plugin's name. The operator's B key already flips
     *  whether the allocations are painted; shift+B asks for the panel behind
     *  the button that does the same thing, and this is how the host knows
     *  which of the loaded plugins that is.
     *
     *  Keyed on the type rather than on an id for the same reason the switch
     *  itself is: a second channel list is reached by the same key without
     *  anything being taught its name. Two facets claiming one type both get
     *  asked, which is the honest answer -- they are both panels for it. */
    sweeppp_contribution_type_t toolbar_governs;

    /** The operator asked for this facet's panel from the keyboard.
     *
     *  Called on the UI thread, before `draw_toolbar` in the same frame. It
     *  cannot open the popup itself: an ImGui popup id is hashed against the
     *  id stack of the window it is opened in, and the plugin's popup lives
     *  inside the toolbar's child window under an id the C++ wrapper pushes
     *  per instance. So record the request and call `ImGui::OpenPopup` from
     *  `draw_toolbar`, where both ends agree on the id.
     *
     *  Asked again while the panel is already open, the right answer is to
     *  close it: the operator pressed the same keys, and a keystroke that only
     *  ever opens is one that cannot be taken back the way it was made. */
    void (*open_panel)(void* instance);

    /** A button in the status bar, after the host's own panel buttons.
     *
     *  Appended after `open_panel` rather than placed beside `draw_toolbar`,
     *  because a struct field moved is a struct field that breaks every
     *  already-built plugin. A host older than this member never reads it, and
     *  a plugin older than it declares a smaller `struct_size` -- which is
     *  what `SWEEPPP_ABI_HAS` is checking before the call. */
    void (*draw_status_action)(void* instance);
} sweeppp_ui_vtable_t;

/* --------------------------------------------------------------------------
 * The host
 * -------------------------------------------------------------------------- */

/** Directories and files the host will name for a plugin. */
typedef enum sweeppp_path_kind_t {
    SWEEPPP_PATH_CONFIG_DIR = 1,
    SWEEPPP_PATH_RESOURCES_DIR = 2,
    SWEEPPP_PATH_SESSIONS_DIR = 3,
    /** This plugin's own settings file, `<configDir>/plugins/<id>.toml`. */
    SWEEPPP_PATH_PLUGIN_SETTINGS = 4,
    /** This plugin's own directory for anything larger,
     *  `<configDir>/plugins/<id>/`. Created on first request. */
    SWEEPPP_PATH_PLUGIN_DATA_DIR = 5,
    /** The directory the plugin's own binary was loaded from. */
    SWEEPPP_PATH_PLUGIN_BINARY_DIR = 6,
    SWEEPPP_PATH_FORCE_INT32 = 0x7FFFFFFF
} sweeppp_path_kind_t;

/* --------------------------------------------------------------------------
 * Events
 *
 * Typed, and open. Both halves matter, and they pull against each other:
 *
 *   A `void*` and a topic string would be open and untyped -- two sides
 *   agreeing on a struct layout with nothing enforcing it, no versioning, and
 *   a mismatch that reads as plausible garbage rather than as an error. That
 *   is the one discipline the rest of this ABI never drops.
 *
 *   A closed enum of kinds with a union body would be typed and shut. Every
 *   new event would be an ABI change, and a plugin could never define an
 *   event of its own for another plugin to listen to -- which is most of what
 *   an ecosystem does.
 *
 * So the type is a NAME, and the payload is the struct that name denotes.
 * Identity is carried explicitly rather than by `typeid`, which is what the
 * host's own `EventBus` uses and cannot cross a C boundary. Each payload
 * carries `struct_size` like everything else here, so a payload may grow
 * within its schema version; `schema_version` is for when it cannot.
 *
 * Adding an event type is therefore not an ABI change at all. A plugin
 * declares its own name and its own struct and other plugins subscribe by
 * name, exactly as they do for the host's.
 *
 * NAMES are reverse-DNS. `sweeppp.` is reserved for the host: a plugin may
 * publish only names beginning with its own plugin id, which is what stops a
 * plugin publishing a retune the radio never made.
 * -------------------------------------------------------------------------- */

/** One event, whatever its type.
 *
 *  `payload` points at the struct `type` names, and `payload_size` is what a
 *  subscriber checks before casting -- an older plugin reading a grown payload
 *  reads the prefix it knows, and a subscriber handed something smaller than
 *  it expects can refuse rather than read past the end.
 *
 *  Every string, and the payload, are borrowed for the duration of the call. */
typedef struct sweeppp_event_t {
    uint32_t struct_size;

    /** Reverse-DNS: "sweeppp.retune", "org.sweeppp.bandplan.plan_changed". */
    sweeppp_str_t type;
    uint32_t schema_version;

    uint64_t monotonic_ns;
    uint64_t wall_ns;

    /** Who published: the plugin's id, or empty for the host itself. */
    sweeppp_str_t source;

    const void* payload;
    size_t payload_size;
} sweeppp_event_t;

/** Delivered on the thread that published, which for a retune is the sweep
 *  thread at thousands of events a second. Must not block, and must not
 *  publish to the type it is handling. */
typedef void (*sweeppp_event_fn_t)(void* user, const sweeppp_event_t* event);

/* The host's own event types. Payload structs, one per name.
 *
 * These mirror `sweeppp::EventBus`'s events field for field. Nothing is
 * summarised on the way across: a plugin reading a live retune and one reading
 * it back out of a recording see the same numbers. */

#define SWEEPPP_EVENT_DEVICE_OPENED "sweeppp.device_opened"
typedef struct sweeppp_device_opened_event_t {
    uint32_t struct_size;
    sweeppp_str_t device_id;
    sweeppp_str_t label;
    sweeppp_str_t serial;
} sweeppp_device_opened_event_t;

#define SWEEPPP_EVENT_DEVICE_CLOSED "sweeppp.device_closed"
typedef struct sweeppp_device_closed_event_t {
    uint32_t struct_size;
    sweeppp_str_t device_id;
    sweeppp_str_t reason;
} sweeppp_device_closed_event_t;

#define SWEEPPP_EVENT_DEVICE_ERROR "sweeppp.device_error"
typedef struct sweeppp_device_error_event_t {
    uint32_t struct_size;
    sweeppp_str_t device_id;
    sweeppp_str_t message;
} sweeppp_device_error_event_t;

#define SWEEPPP_EVENT_PARAMETER_CHANGED "sweeppp.parameter_changed"
typedef struct sweeppp_parameter_changed_event_t {
    uint32_t struct_size;
    sweeppp_str_t key;
    sweeppp_str_t value;
    /** The change redefines the frequency grid, so it closes the current
     *  session segment and opens a new one. */
    int32_t grid_affecting;
    /** It shifts the noise floor without changing the grid, so analysis of
     *  earlier tiles has to know it happened. */
    int32_t calibration_affecting;
} sweeppp_parameter_changed_event_t;

#define SWEEPPP_EVENT_RETUNE "sweeppp.retune"
typedef struct sweeppp_retune_event_t {
    uint32_t struct_size;
    double center_hz;
    uint32_t step_index;
} sweeppp_retune_event_t;

#define SWEEPPP_EVENT_SWEEP_PASS "sweeppp.sweep_pass"
typedef struct sweeppp_sweep_pass_event_t {
    uint32_t struct_size;
    uint64_t pass_id;
    double start_hz;
    double stop_hz;
    double duration_seconds;
} sweeppp_sweep_pass_event_t;

#define SWEEPPP_EVENT_MARKER "sweeppp.marker"
typedef struct sweeppp_marker_event_t {
    uint32_t struct_size;
    sweeppp_str_t label;
    double frequency_hz;
    double level_dbm;
} sweeppp_marker_event_t;

#define SWEEPPP_EVENT_ANNOTATION "sweeppp.annotation"
typedef struct sweeppp_annotation_event_t {
    uint32_t struct_size;
    sweeppp_str_t text;
    double start_hz;
    double stop_hz;
} sweeppp_annotation_event_t;

#define SWEEPPP_EVENT_THROTTLE_CHANGED "sweeppp.throttle_changed"
typedef struct sweeppp_throttle_changed_event_t {
    uint32_t struct_size;
    sweeppp_str_t reason;
    double processed_fraction;
} sweeppp_throttle_changed_event_t;

/** A payload of named, typed values, for an event whose shape is data rather
 *  than a struct -- and the one that survives being written into a session and
 *  read back by something that never heard of the plugin that wrote it. */
#define SWEEPPP_EVENT_FIELDS "sweeppp.fields"
typedef struct sweeppp_fields_event_t {
    uint32_t struct_size;
    const sweeppp_field_t* fields;
    uint32_t field_count;
} sweeppp_fields_event_t;

/** ImGui, handed across the boundary.
 *
 *  The plugin links its OWN copy of ImGui and adopts the host's context and
 *  allocators, which is ImGui's documented pattern for exactly this. What
 *  makes it dangerous here is `sweeppp_imconfig.h`: the host builds ImGui with
 *  32-bit `ImDrawIdx` and 32-bit `ImWchar`, so a plugin built without that
 *  header has a different `ImDrawVert`, a different index size, and corrupts
 *  every draw list it touches -- silently, and not at the call that did it.
 *
 *  Hence the sizes. They are what `ImGui::DebugCheckVersionAndDataLayout`
 *  compares, and the C++ wrapper calls it during activation and refuses to
 *  draw on a mismatch. ImPlot has no equivalent check, so its version string
 *  is carried and compared instead.
 *
 *  Null in a headless build. A UI facet in a host with no UI is listed and not
 *  drawn, which is a fact worth reporting rather than a failure. */
typedef struct sweeppp_imgui_binding_t {
    uint32_t struct_size;

    void* imgui_context;  /**< ImGuiContext* */
    void* implot_context; /**< ImPlotContext* */

    void* (*alloc_fn)(size_t size, void* user);
    void (*free_fn)(void* ptr, void* user);
    void* allocator_user;

    sweeppp_str_t imgui_version;
    sweeppp_str_t implot_version;

    size_t size_of_io;
    size_t size_of_style;
    size_t size_of_vec2;
    size_t size_of_vec4;
    size_t size_of_draw_vert;
    size_t size_of_draw_idx;
} sweeppp_imgui_binding_t;

/** Everything a plugin can ask of the host.
 *
 *  Every callback takes `host` as its first argument -- the host's own opaque
 *  pointer, not the plugin's -- so one table serves every loaded plugin, and
 *  `plugin_id` says which one is calling where that matters. */
typedef struct sweeppp_host_api_t {
    uint32_t struct_size;

    /** The ABI the host implements, and the application's own release. The
     *  first is what compatibility is decided on; the second is what a plugin
     *  compares against its `min_host_version`. */
    uint32_t abi_version;
    sweeppp_str_t host_version;

    void* host;

    void (*log)(void* host, sweeppp_log_level_t level, sweeppp_str_t category,
                sweeppp_str_t message);

    /** A path, as UTF-8. Valid until the next host call on this thread. */
    sweeppp_str_t (*path)(void* host, sweeppp_str_t plugin_id, sweeppp_path_kind_t kind);

    /* Registration. Each pair takes a facet the plugin already declared, so
     * the host has the name and description to list it with. A register that
     * returns ALREADY_EXISTS lost a name race -- two plugins claiming the
     * driver "hackrf" -- and the loser is listed with that as its reason
     * rather than silently replacing the winner. */
    sweeppp_plugin_status_t (*register_sdr_driver)(void* host, sweeppp_str_t plugin_id,
                                                   const sweeppp_facet_t* facet, void* instance);
    sweeppp_plugin_status_t (*unregister_sdr_driver)(void* host, sweeppp_str_t plugin_id,
                                                     sweeppp_str_t driver);

    sweeppp_plugin_status_t (*register_rf_path)(void* host, sweeppp_str_t plugin_id,
                                                const sweeppp_facet_t* facet, void* instance);
    sweeppp_plugin_status_t (*unregister_rf_path)(void* host, sweeppp_str_t plugin_id,
                                                  sweeppp_str_t driver);

    sweeppp_plugin_status_t (*register_fft_backend)(void* host, sweeppp_str_t plugin_id,
                                                    const sweeppp_facet_t* facet, void* instance);
    sweeppp_plugin_status_t (*unregister_fft_backend)(void* host, sweeppp_str_t plugin_id,
                                                      sweeppp_str_t name);

    sweeppp_plugin_status_t (*register_contributor)(void* host, sweeppp_str_t plugin_id,
                                                    const sweeppp_facet_t* facet, void* instance);
    sweeppp_plugin_status_t (*unregister_contributor)(void* host, sweeppp_str_t plugin_id,
                                                      sweeppp_str_t id);

    sweeppp_plugin_status_t (*register_ui_extension)(void* host, sweeppp_str_t plugin_id,
                                                     const sweeppp_facet_t* facet, void* instance);
    sweeppp_plugin_status_t (*unregister_ui_extension)(void* host, sweeppp_str_t plugin_id,
                                                       sweeppp_str_t id);

    /** Why a facet the manifest declared is not being offered.
     *
     *  A plugin that decides at activation that it cannot provide something --
     *  no GPU for its FFT backend, no ImGui it agrees with for its overlay --
     *  would otherwise leave a declared facet sitting in the listing marked
     *  inactive with nothing said about it, which is the silent-skip this
     *  whole design exists to avoid. This is how it says so.
     *
     *  An empty `reason` clears a previously reported one. */
    void (*report_facet)(void* host, sweeppp_str_t plugin_id, sweeppp_facet_kind_t kind,
                         sweeppp_str_t facet_id, sweeppp_str_t reason);

    /** Frame bus. The host owns the subscription and the worker thread behind
     *  it, because `FrameBus::subscribe` takes a raw pointer that has to
     *  outlive it and a plugin cannot be trusted with that lifetime. Returns 0
     *  on failure. */
    uint64_t (*frame_subscribe)(void* host, sweeppp_str_t plugin_id, const sweeppp_facet_t* facet,
                                void* instance);
    void (*frame_unsubscribe)(void* host, uint64_t subscription);

    /** Listens for one event type, or for every type when `type` is empty.
     *
     *  Delivered on the publishing thread -- a retune comes from the sweep
     *  thread thousands of times a second -- so a handler must return
     *  promptly. Anything slow belongs behind a frame-processor facet, which
     *  already has a thread and a queue of its own. */
    uint64_t (*subscribe_event)(void* host, sweeppp_str_t type, sweeppp_event_fn_t callback,
                                void* user);
    void (*unsubscribe_event)(void* host, uint64_t subscription);

    /** Publishes one.
     *
     *  The type must begin with this plugin's own id. `sweeppp.` is the
     *  host's, and a plugin that could publish `sweeppp.retune` could make a
     *  session say the radio moved when it did not -- with nothing downstream
     *  able to tell the difference. Anything else is INVALID_ARGUMENT.
     *
     *  `event->source` is filled in by the host; whatever the plugin put there
     *  is ignored. */
    sweeppp_plugin_status_t (*publish_event)(void* host, sweeppp_str_t plugin_id,
                                             const sweeppp_event_t* event);

    /* ---------------------------------------------------------------------
     * The running configuration, read-only.
     *
     * Flat dotted keys over typed values -- "sweep.start", "display.grid",
     * "device.driver" -- which is a session manifest's shape and the same
     * shape the profile is written to disk in. One vocabulary, not three: a
     * plugin that reads `sweep.start` here and puts it in a record it writes
     * below has not had to translate anything, and a key it sees is a key an
     * operator can find in their own `settings.toml`.
     *
     * Read-only on purpose. A plugin that could write here could retune the
     * radio as a side effect of a settings panel, and "which plugin moved the
     * sweep" is not a question the event log could answer. A plugin that wants
     * to ask for something uses the event channel, where the request is
     * addressed and refusable.
     * --------------------------------------------------------------------- */

    /** Every key, written into `out` up to `capacity`; returns how many there
     *  are. Sorted, so a listing is stable between calls. */
    uint32_t (*profile_keys)(void* host, sweeppp_str_t* out, uint32_t capacity);

    /** One value. NOT_FOUND for a key that is not set, which is an answer
     *  rather than a failure. */
    sweeppp_plugin_status_t (*profile_get)(void* host, sweeppp_str_t key, sweeppp_value_t* out);

    /* ---------------------------------------------------------------------
     * The session container.
     *
     * `.sweeps` reserves a record type and an event kind for plugins, and they
     * have been in the format since the first release precisely so that this
     * did not have to become a sidecar file. A plugin's analysis belongs in
     * the same file as the sweep it analysed, timestamped on the same clock,
     * and survives being handed to somebody who does not have the plugin --
     * an unknown plugin record reads back as opaque bytes rather than
     * corrupting the file.
     * --------------------------------------------------------------------- */

    /** Whether a session is being retained right now. Everything below is
     *  UNAVAILABLE when it is not, which is the normal state before Start. */
    int32_t (*session_open)(void* host);

    /** An opaque record, under this plugin's id.
     *
     *  `record_name` names the shape and `schema_version` versions it, so a
     *  reader that knows an older layout can tell rather than guess. The body
     *  is copied before this returns; nothing is retained.
     *
     *  Queued, not written: the recorder owns the only thread that touches the
     *  writer, and a plugin calling this from its frame-processor thread must
     *  not be the second one. */
    sweeppp_plugin_status_t (*session_write_record)(void* host, sweeppp_str_t plugin_id,
                                                    sweeppp_str_t record_name,
                                                    uint32_t schema_version, const void* body,
                                                    size_t body_size);

    /** A plugin event with typed fields, on the session's own time axis.
     *
     *  The difference from a record is what reads it back: an event lands in
     *  the event stream beside retunes and markers, so a viewer shows it on
     *  the timeline without knowing what it means. Use this for things that
     *  happen at an instant, and a record for data. */
    sweeppp_plugin_status_t (*session_write_event)(void* host, sweeppp_str_t plugin_id,
                                                   sweeppp_str_t event_name,
                                                   const sweeppp_field_t* fields,
                                                   uint32_t field_count);

    /** Null when this host has no UI. */
    const sweeppp_imgui_binding_t* imgui;

    /* ---------------------------------------------------------------------
     * The host's chrome, for a plugin drawing in it.
     *
     * The only host state a plugin may write, and it is deliberately the
     * narrowest: which KINDS of contribution the spectrum paints. That switch
     * is the host's own drawing, keyed on a type this ABI defines rather than
     * on any plugin's identity -- it is what the B and C keys flip, and a
     * plugin's toolbar button being the same switch is the whole point. A flag
     * of the plugin's own beside it would be a second opinion about the same
     * pixels.
     *
     * Writable where the running configuration above is not, because nothing
     * here reaches the radio: the objection to a writable profile is that a
     * settings panel could retune as a side effect, and a drawing switch
     * cannot.
     *
     * SPOT rides with CHANNEL, because that is how the host paints them: a
     * beacon is a channel of no width, not a third thing to switch.
     *
     * A host with no UI answers 0 and ignores the setter, the same as `imgui`
     * being null there.
     * --------------------------------------------------------------------- */
    int32_t (*contributions_shown)(void* host, sweeppp_contribution_type_t type);
    void (*set_contributions_shown)(void* host, sweeppp_contribution_type_t type, int32_t shown);

    /** Whether the host's icon font is in the atlas.
     *
     *  So a plugin's button falls back to words in exactly the build where the
     *  host's own do, rather than being the one control on the bar drawn as an
     *  empty box. */
    int32_t (*icons_available)(void* host);

    /** Asks the operator where to save a file, through the host's own native
     *  dialog.
     *
     *  A plugin cannot open one for itself. The dialog lives in the
     *  application rather than the library, and a plugin linking a second copy
     *  of it would put two native file-dialog implementations in one process
     *  -- which on macOS means two ways of driving the same panel, and on
     *  Linux a second D-Bus portal connection.
     *
     *  So a plugin that wants to export asked the operator to go and find the
     *  file afterwards. That is not a limitation worth keeping: choosing where
     *  something goes is the ordinary way to save, and the plugin already
     *  knows how to write the bytes.
     *
     *  `extension` is bare -- "csv", not "*.csv" or ".csv". The returned path
     *  is empty when the operator cancelled, and empty as well in a host with
     *  no user interface at all; a plugin that must write regardless falls
     *  back to SWEEPPP_PATH_PLUGIN_DATA_DIR when this member is null.
     *
     *  Blocks until the operator answers, so: the UI thread, and never a
     *  worker. Borrowed like `path`, and valid only until this thread's next
     *  host call. */
    sweeppp_str_t (*save_file_dialog)(void* host, sweeppp_str_t title, sweeppp_str_t suggested_name,
                                      sweeppp_str_t extension);
} sweeppp_host_api_t;

/* --------------------------------------------------------------------------
 * The plugin
 * -------------------------------------------------------------------------- */

/** Somewhere to put values that belong in the profile being saved.
 *
 *  The host namespaces every key under `plugins.<id>.`, so two plugins cannot
 *  collide and a key in a profile file says which plugin owns it. */
typedef struct sweeppp_profile_writer_t {
    uint32_t struct_size;
    void* sink;
    void (*set)(void* sink, sweeppp_str_t key, const sweeppp_value_t* value);
} sweeppp_profile_writer_t;

/** The same values, coming back out of a profile being applied. Namespaced the
 *  same way, so a plugin asks for the key it wrote. */
typedef struct sweeppp_profile_reader_t {
    uint32_t struct_size;
    void* source;
    sweeppp_plugin_status_t (*get)(void* source, sweeppp_str_t key, sweeppp_value_t* out);
    uint32_t (*keys)(void* source, sweeppp_str_t* out, uint32_t capacity);
} sweeppp_profile_reader_t;

typedef struct sweeppp_plugin_desc_t {
    uint32_t struct_size;

    sweeppp_manifest_t manifest;

    const sweeppp_facet_t* facets;
    uint32_t facet_count;

    /** Called once, after compatibility has been decided and before any facet
     *  is registered. `instance` receives whatever the plugin wants handed
     *  back to its callbacks; it may be left null.
     *
     *  Returning anything but OK lists the plugin as failed with that status
     *  and registers nothing. */
    sweeppp_plugin_status_t (*activate)(const sweeppp_host_api_t* host, void** instance);

    /** Called once, after every facet has been withdrawn. The plugin must have
     *  stopped everything of its own by the time this returns -- threads,
     *  timers, callbacks -- because the host will not call it again and cannot
     *  unload the image (see the note in PluginHost.hpp). */
    void (*deactivate)(void* instance);

    /** Just before a profile is written, so a plugin can put its own state in
     *  it. Both may be null.
     *
     *  Distinct from `<configDir>/plugins/<id>.toml`, and the split is the
     *  same one the application already makes for itself: the settings file is
     *  how this installation is configured, a profile is a setup an operator
     *  saves, names and comes back to. A band plan choice that belongs to
     *  "2.4 GHz survey" travels here; the plugin's default lives in its
     *  settings file.
     *
     *  On the UI thread, with nothing else running. Do not register or
     *  withdraw facets from either. */
    void (*profile_save)(void* instance, const sweeppp_profile_writer_t* writer);

    /** Just after a profile has been applied, with whatever `profile_save`
     *  put there. A profile written before this plugin existed simply has no
     *  keys, so `get` reports NOT_FOUND and the plugin keeps its own defaults
     *  -- which is what makes adding a plugin to an existing setup harmless. */
    void (*profile_load)(void* instance, const sweeppp_profile_reader_t* reader);
} sweeppp_plugin_desc_t;

/* --------------------------------------------------------------------------
 * Entry points
 *
 * Two, and the cheap one exists so that an incompatibility can be REPORTED.
 * A host that only had `sweeppp_plugin_query` and got null back could say no
 * more than "the plugin refused"; with the version first it can say "plugin
 * ABI 2, host ABI 1", which tells the operator which half to update.
 * -------------------------------------------------------------------------- */

/** The ABI this plugin was built against. Must be the first thing the host
 *  calls, and must not touch anything but a constant. */
SWEEPPP_PLUGIN_API uint32_t sweeppp_plugin_abi_version(void);

/** The descriptor, or null when this plugin cannot serve `host_abi`.
 *
 *  The argument lets a plugin support several host ABIs from one binary by
 *  returning a different descriptor for each. A plugin that supports exactly
 *  one returns null for anything else, and the host reports the mismatch from
 *  `sweeppp_plugin_abi_version` above.
 *
 *  Called once per process. Must not allocate anything it expects the host to
 *  free, must not start threads, and must not assume it can draw -- activation
 *  is where a plugin comes to life. */
SWEEPPP_PLUGIN_API const sweeppp_plugin_desc_t* sweeppp_plugin_query(uint32_t host_abi);

/** Types of the two entry points, for the host's `dlsym` casts. */
typedef uint32_t (*sweeppp_plugin_abi_version_fn)(void);
typedef const sweeppp_plugin_desc_t* (*sweeppp_plugin_query_fn)(uint32_t host_abi);

/** The names those symbols are looked up under. */
#define SWEEPPP_PLUGIN_ABI_VERSION_SYMBOL "sweeppp_plugin_abi_version"
#define SWEEPPP_PLUGIN_QUERY_SYMBOL "sweeppp_plugin_query"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SWEEPPP_PLUGIN_ABI_H */
