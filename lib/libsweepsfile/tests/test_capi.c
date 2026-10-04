// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

/* The C ABI, exercised from C.
 *
 * Compiled as C99 with no C++ compiler in the picture, which is the whole
 * point: it is the only thing in this repository that proves `sweeps/sweeps.h`
 * contains no C++. The C++ suite next door could include the header happily
 * while it quietly depended on `nullptr`, a default argument or a `namespace`.
 *
 * The assertions are deliberately the same numbers `test_golden.cpp` asserts.
 * That is what makes this a conformance test rather than a smoke test: the two
 * paths read one frozen file and must agree about what is in it.
 *
 * Plain assertions rather than doctest, which is C++-only, and one executable
 * registered with a plain add_test() rather than doctest_discover_tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sweeps/sweeps.h>

static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)fprintf(stderr, "%s:%d: FAILED %s\n", __FILE__, __LINE__, #cond);                \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_OK(expr)                                                                             \
    do {                                                                                           \
        sweeps_status_t status_ = (expr);                                                          \
        if (status_ != SWEEPS_OK) {                                                                \
            (void)fprintf(stderr, "%s:%d: FAILED %s -> %s (%s)\n", __FILE__, __LINE__, #expr,      \
                          sweeps_status_name(status_), sweeps_last_error().data);                  \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_STATUS(expr, expected)                                                               \
    do {                                                                                           \
        sweeps_status_t status_ = (expr);                                                          \
        if (status_ != (expected)) {                                                               \
            (void)fprintf(stderr, "%s:%d: FAILED %s -> %s, wanted %s\n", __FILE__, __LINE__,       \
                          #expr, sweeps_status_name(status_), sweeps_status_name(expected));       \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

/* Deliberately duplicated from tests/GoldenSession.hpp rather than shared: that
 * header is C++, and a C test that could not state these numbers for itself
 * would not be testing the C ABI against the format, only against a header. */
static const char* const kGoldenPath = SWEEPSFILE_TEST_DATA_DIR "/v1-golden.sweeps";
static const uint64_t kCreatedWallNs = 1770000000000000000ULL;

static int str_equals(sweeps_str_t str, const char* text) {
    return str.data != NULL && strlen(text) == str.len && memcmp(str.data, text, str.len) == 0;
}

/* ------------------------------------------------------------------ version */

static void test_version(void) {
    uint32_t major = 0;
    uint32_t minor = 0;
    sweeps_str_t version = sweeps_library_version();

    CHECK(sweeps_abi_version() == SWEEPS_ABI_VERSION);
    CHECK(version.len > 0);
    /* The NUL-termination guarantee the header makes, checked rather than
     * assumed: every binding will pass .data to something expecting a C string. */
    CHECK(version.data[version.len] == '\0');

    sweeps_format_version(&major, &minor);
    CHECK(major == 1);

    sweeps_format_version(NULL, NULL);
}

static void test_status_names(void) {
    CHECK(strcmp(sweeps_status_name(SWEEPS_OK), "ok") == 0);
    CHECK(sweeps_status_name(SWEEPS_ERR_CORRUPT) != NULL);
    CHECK(sweeps_status_name((sweeps_status_t)-9999) != NULL);
}

static void test_quantisation(void) {
    /* Round-trip at the origin and one step above it. Getting this wrong is
     * silent -- plausible levels, forty decibels out -- so it is checked here
     * rather than left to a caller to rediscover. */
    CHECK(sweeps_quantise_db(-100.0, -100.0) == 1);
    CHECK(sweeps_quantise_db(-99.5, -100.0) == 1);
    CHECK(sweeps_dequantise_db(1, -100.0) == -99.5);
    CHECK(sweeps_dequantise_db(2, -100.0) == -99.0);
    /* Saturating rather than wrapping at both ends. */
    CHECK(sweeps_quantise_db(-150.0, -100.0) == 1);
    CHECK(sweeps_quantise_db(1000.0, -100.0) == 255);
    /* Byte 0 is coverage rather than a level, at any origin. */
    CHECK(sweeps_quantise_db(SWEEPS_UNMEASURED_DB, -100.0) == SWEEPS_UNMEASURED_BYTE);
    CHECK(sweeps_dequantise_db(SWEEPS_UNMEASURED_BYTE, -100.0) == SWEEPS_UNMEASURED_DB);
    CHECK(sweeps_dequantise_db(SWEEPS_UNMEASURED_BYTE, -40.0) == SWEEPS_UNMEASURED_DB);

    CHECK(str_equals(sweeps_window_type_name(SWEEPS_WINDOW_HANN), "hann"));
    CHECK(str_equals(sweeps_event_kind_name(SWEEPS_EVENT_MARKER), "marker"));
    CHECK(str_equals(sweeps_event_kind_name(60000), "unknown"));
}

/* ------------------------------------------------------------------- errors */

static void test_open_failure(void) {
    sweeps_reader_t* reader = (sweeps_reader_t*)0x1; /* must be overwritten with NULL */
    sweeps_status_t status = sweeps_reader_open("/nonexistent/no-such-session.sweeps", &reader);

    CHECK(status != SWEEPS_OK);
    CHECK(reader == NULL);
    /* The status says how to branch, the message says which file. Losing the
     * second is the reason this thread-local exists at all. */
    CHECK(sweeps_last_error().len > 0);

    CHECK_STATUS(sweeps_reader_open(NULL, &reader), SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK_STATUS(sweeps_reader_open(kGoldenPath, NULL), SWEEPS_ERR_INVALID_ARGUMENT);

    /* Null handles are diagnosed, not dereferenced. */
    CHECK(sweeps_reader_segment_count(NULL) == 0);
    CHECK(sweeps_reader_name(NULL).len == 0);
    sweeps_reader_close(NULL);
}

static void test_struct_size_contract(void) {
    sweeps_reader_t* reader = NULL;
    sweeps_summary_t summary;
    sweeps_query_t query;
    sweeps_tiles_t* tiles = NULL;

    CHECK_OK(sweeps_reader_open(kGoldenPath, &reader));
    if (reader == NULL) {
        return;
    }

    /* A zeroed struct is refused rather than answered. This is the check that
     * makes `sweeps_query_t q = {0}` -- the C reflex -- a loud error instead of
     * a query for an empty time range that returns nothing. */
    memset(&summary, 0, sizeof(summary));
    CHECK_STATUS(sweeps_reader_summary(reader, &summary), SWEEPS_ERR_INVALID_ARGUMENT);

    memset(&query, 0, sizeof(query));
    CHECK_STATUS(sweeps_reader_query(reader, &query, &tiles), SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK(tiles == NULL);

    /* An input struct bigger than the library knows means the caller was built
     * against a newer header and set fields we would drop. */
    sweeps_query_init(&query);
    query.struct_size = (uint32_t)sizeof(query) + 8u;
    CHECK_STATUS(sweeps_reader_query(reader, &query, &tiles), SWEEPS_ERR_INVALID_ARGUMENT);

    /* A smaller one is the forward-compatibility case: an older caller, whose
     * prefix we fill and whose struct_size tells it how much it got. */
    memset(&summary, 0xAB, sizeof(summary));
    summary.struct_size = offsetof(sweeps_summary_t, total_lines);
    CHECK_OK(sweeps_reader_summary(reader, &summary));
    CHECK(summary.struct_size == offsetof(sweeps_summary_t, total_lines));
    CHECK(summary.major_version == 1);
    /* Past the caller's declared size, untouched. */
    CHECK(((const unsigned char*)&summary)[offsetof(sweeps_summary_t, total_lines)] == 0xAB);

    CHECK_STATUS(sweeps_reader_summary(reader, NULL), SWEEPS_ERR_INVALID_ARGUMENT);

    sweeps_reader_close(reader);
}

/* ------------------------------------------------------------- golden file  */

static void test_golden_summary(sweeps_reader_t* reader) {
    sweeps_summary_t summary;
    summary.struct_size = sizeof(summary);

    CHECK_OK(sweeps_reader_summary(reader, &summary));
    CHECK(summary.struct_size == sizeof(summary));
    CHECK(summary.major_version == 1);
    CHECK(summary.minor_version == 0);
    CHECK(summary.created_wall_ns == kCreatedWallNs);
    CHECK(summary.recovered_by_scan == 0);
    CHECK(summary.truncated_bytes == 0);
    CHECK(summary.newer_minor_version == 0);
    CHECK(summary.total_lines == 340); /* kWideLines + kNarrowLines */
    CHECK(summary.total_tiles > 0);

    CHECK(str_equals(sweeps_reader_name(reader), "golden"));
    CHECK(str_equals(sweeps_reader_app_version(reader), "0.0.0-golden"));
}

static void test_golden_segments(sweeps_reader_t* reader) {
    sweeps_segment_t segment;
    size_t index = 999;
    sweeps_str_t name;
    double value = 0.0;

    CHECK(sweeps_reader_segment_count(reader) == 2);

    segment.struct_size = sizeof(segment);
    CHECK_OK(sweeps_reader_segment_at(reader, 0, &segment));
    CHECK(segment.id == 0);
    CHECK(segment.bin_count == 1200);
    CHECK(segment.fft_size == 4096);
    CHECK(segment.window == SWEEPS_WINDOW_HANN);
    CHECK(segment.start_wall_ns == kCreatedWallNs);
    CHECK(str_equals(sweeps_reader_segment_reason(reader, 0), "session start"));

    segment.struct_size = sizeof(segment);
    CHECK_OK(sweeps_reader_segment_at(reader, 1, &segment));
    CHECK(segment.id == 1);
    CHECK(segment.bin_count == 128);
    CHECK(segment.fft_size == 8192);
    CHECK(segment.gain_count == 2);
    CHECK(str_equals(sweeps_reader_segment_device_id(reader, 1), "golden-0"));
    CHECK(str_equals(sweeps_reader_segment_device_label(reader, 1), "Golden reference device"));

    /* The gains are accessors rather than a pointer into the struct, because a
     * vector of std::pair<std::string, double> has no C-addressable layout. */
    CHECK(sweeps_reader_segment_gain_count(reader, 1) == 2);
    CHECK_OK(sweeps_reader_segment_gain(reader, 1, 0, &name, &value));
    CHECK(str_equals(name, "lna"));
    CHECK(value == 24.0);
    CHECK_OK(sweeps_reader_segment_gain(reader, 1, 1, &name, &value));
    CHECK(str_equals(name, "vga"));
    CHECK(value == 16.0);
    CHECK_STATUS(sweeps_reader_segment_gain(reader, 1, 2, &name, &value), SWEEPS_ERR_OUT_OF_RANGE);

    /* Ids are looked up, never indexed: an extracted file's ids neither start
     * at zero nor run contiguously. */
    CHECK_OK(sweeps_reader_segment_index_of(reader, 1, &index));
    CHECK(index == 1);
    CHECK_STATUS(sweeps_reader_segment_index_of(reader, 77, &index), SWEEPS_ERR_NOT_FOUND);

    segment.struct_size = sizeof(segment);
    CHECK_STATUS(sweeps_reader_segment_at(reader, 2, &segment), SWEEPS_ERR_OUT_OF_RANGE);
    CHECK(sweeps_reader_segment_reason(reader, 99).len == 0);
}

static void test_golden_manifest(sweeps_reader_t* reader) {
    const sweeps_metadata_t* manifest = sweeps_reader_manifest(reader);
    sweeps_str_t key;
    sweeps_value_type_t type = SWEEPS_VALUE_ABSENT;
    size_t needed = 0;
    char buffer[4096];
    sweeps_bytes_t bytes;

    CHECK(manifest != NULL);
    CHECK(sweeps_metadata_count(manifest) == 10);

    CHECK(str_equals(sweeps_metadata_get_string(manifest, "name", ""), "golden"));
    CHECK(str_equals(sweeps_metadata_get_string(manifest, "created", ""), "2026-02-02T02:40:00Z"));
    CHECK(sweeps_metadata_get_i64(manifest, "format_version", -1) == 1);
    CHECK(sweeps_metadata_get_i64(manifest, "bins_per_line", -1) == 1200);
    CHECK(sweeps_metadata_get_i64(manifest, "tile_bins", -1) == SWEEPS_TILE_BINS);
    CHECK(sweeps_metadata_get_i64(manifest, "tile_lines", -1) == SWEEPS_TILE_LINES);
    CHECK(sweeps_metadata_get_i64(manifest, "lod_levels", -1) == SWEEPS_LOD_LEVELS);
    CHECK(sweeps_metadata_get_f64(manifest, "db_per_step", 0.0) == SWEEPS_DB_PER_STEP);

    /* Types are part of the contract, not an accident of how a value was set. */
    CHECK(sweeps_metadata_type_of(manifest, "created") == SWEEPS_VALUE_STRING);
    CHECK(sweeps_metadata_type_of(manifest, "db_per_step") == SWEEPS_VALUE_FLOAT);
    CHECK(sweeps_metadata_type_of(manifest, "tile_bins") == SWEEPS_VALUE_INT);

    /* Absent stays distinguishable from empty, which is the whole reason the
     * pointer-returning accessors carry a status. */
    CHECK(sweeps_metadata_type_of(manifest, "not-a-key") == SWEEPS_VALUE_ABSENT);
    CHECK(sweeps_metadata_contains(manifest, "name") == 1);
    CHECK(sweeps_metadata_contains(manifest, "not-a-key") == 0);
    CHECK_STATUS(sweeps_metadata_get_bytes(manifest, "not-a-key", &bytes), SWEEPS_ERR_NOT_FOUND);
    CHECK_STATUS(sweeps_metadata_get_bytes(manifest, "name", &bytes), SWEEPS_ERR_WRONG_TYPE);
    /* No coercion: the wrong type gets the stated fallback, never a reading of
     * other data. */
    CHECK(sweeps_metadata_get_i64(manifest, "name", -7) == -7);
    CHECK(str_equals(sweeps_metadata_get_string(manifest, "tile_bins", "fb"), "fb"));

    CHECK_OK(sweeps_metadata_key_at(manifest, 0, &key, &type));
    CHECK(key.len > 0);
    CHECK_STATUS(sweeps_metadata_key_at(manifest, 10, &key, &type), SWEEPS_ERR_OUT_OF_RANGE);

    /* Two-call sizing: once with no buffer to learn the length, once to fill. */
    CHECK_OK(sweeps_metadata_to_json(manifest, 0, NULL, 0, &needed));
    CHECK(needed > 0);
    CHECK(needed < sizeof(buffer));
    CHECK_OK(sweeps_metadata_to_json(manifest, 0, buffer, sizeof(buffer), &needed));
    CHECK(strlen(buffer) == needed);
    CHECK(buffer[0] == '{');
    CHECK(strstr(buffer, "\"name\": \"golden\"") != NULL ||
          strstr(buffer, "\"name\":\"golden\"") != NULL);

    /* Truncation is reported by the length, not by an error -- the snprintf
     * contract, so a caller can size on the first call and never check again. */
    CHECK_OK(sweeps_metadata_to_json(manifest, 0, buffer, 8, &needed));
    CHECK(needed > 8);
    CHECK(strlen(buffer) == 7);
}

static void test_golden_events(sweeps_reader_t* reader) {
    const size_t count = sweeps_reader_event_count(reader);
    size_t i = 0;
    int sawAnnotation = 0;
    int sawMarker = 0;

    CHECK(count >= 2);

    for (i = 0; i < count; ++i) {
        const sweeps_event_t* event = sweeps_reader_event_at(reader, i);
        sweeps_str_t text;
        double a = 0.0;
        double b = 0.0;
        char json[512];
        size_t needed = 0;

        CHECK(event != NULL);
        if (event == NULL) {
            continue;
        }

        CHECK(sweeps_event_kind(event) != 0);
        CHECK(sweeps_event_kind_name(sweeps_event_kind(event)).len > 0);

        /* Whatever the kind, the body renders -- including a kind this build
         * has never seen, whose bytes come out as hex rather than as nothing. */
        CHECK_OK(sweeps_event_body_json(event, 0, json, sizeof(json), &needed));
        CHECK(needed > 0);

        if (sweeps_event_kind(event) == SWEEPS_EVENT_ANNOTATION) {
            CHECK_OK(sweeps_event_annotation(event, &text, &a, &b));
            CHECK(str_equals(text, "golden"));
            CHECK(a == 95e6);
            CHECK(b == 96e6);
            /* Asking the wrong accessor is refused rather than answered with a
             * plausible-looking reading of other fields. */
            CHECK_STATUS(sweeps_event_marker(event, &text, &a, &b), SWEEPS_ERR_WRONG_TYPE);
            CHECK_STATUS(sweeps_event_retune(event, &a, NULL), SWEEPS_ERR_WRONG_TYPE);
            CHECK_STATUS(sweeps_event_unknown_body(event, NULL), SWEEPS_ERR_WRONG_TYPE);
            sawAnnotation = 1;
        } else if (sweeps_event_kind(event) == SWEEPS_EVENT_MARKER) {
            CHECK_OK(sweeps_event_marker(event, &text, &a, &b));
            CHECK(str_equals(text, "m1"));
            CHECK(a == 401e6);
            CHECK(b == -42.25);
            CHECK(sweeps_event_monotonic_ns(event) == 1000000000ULL + 1000ULL);
            sawMarker = 1;
        } else if (sweeps_event_kind(event) == SWEEPS_EVENT_SEGMENT_BOUNDARY) {
            CHECK_OK(sweeps_event_segment_boundary(event, &text));
            CHECK(text.len > 0);
        }
    }

    CHECK(sawAnnotation == 1);
    CHECK(sawMarker == 1);

    CHECK(sweeps_reader_event_at(reader, count) == NULL);
    CHECK(sweeps_event_kind(NULL) == 0);
    CHECK_STATUS(sweeps_event_marker(NULL, NULL, NULL, NULL), SWEEPS_ERR_INVALID_ARGUMENT);

    /* The golden session carries no plugin records; the accessors must still
     * answer rather than trip. */
    CHECK(sweeps_reader_plugin_count(reader) == 0);
    CHECK(sweeps_reader_plugin_at(reader, 0) == NULL);
}

static void test_golden_tiles(sweeps_reader_t* reader) {
    sweeps_query_t query;
    sweeps_tiles_t* tiles = NULL;
    size_t count = 0;
    size_t i = 0;
    size_t bins = 0;
    float spectrum[2048];

    /* Both structural properties the fixture exists to cover. */
    CHECK(sweeps_reader_has_tiles_at_lod(reader, 0, 0) == 1);
    CHECK(sweeps_reader_has_tiles_at_lod(reader, 1, 2) == 1);
    CHECK(sweeps_reader_has_tiles_at_lod(reader, 99, 0) == 0);
    CHECK(sweeps_reader_choose_lod(reader, 0, UINT64_MAX, 2048, 0) < SWEEPS_LOD_LEVELS);

    sweeps_query_init(&query);
    CHECK(query.struct_size == sizeof(query));
    CHECK(query.max_lines == 2048);
    CHECK(query.max_bins == 4096);
    CHECK(query.to_ns == UINT64_MAX);

    CHECK_OK(sweeps_reader_query(reader, &query, &tiles));
    CHECK(tiles != NULL);
    if (tiles == NULL) {
        return;
    }

    count = sweeps_tiles_count(tiles);
    CHECK(count > 0);

    for (i = 0; i < count; ++i) {
        const sweeps_tile_t* tile = sweeps_tiles_at(tiles, i);
        sweeps_tile_info_t info;
        size_t bytes = 0;
        const uint8_t* data = NULL;

        CHECK(tile != NULL);
        if (tile == NULL) {
            continue;
        }

        info.struct_size = sizeof(info);
        CHECK_OK(sweeps_tile_info(tile, &info));
        CHECK(info.lines > 0);
        CHECK(info.bins > 0);
        CHECK(info.lines <= SWEEPS_TILE_LINES);
        CHECK(info.bins <= SWEEPS_TILE_BINS);
        CHECK(info.last_line_ns >= info.first_line_ns);

        data = sweeps_tile_data(tile, &bytes);
        CHECK(data != NULL);
        /* The tile is byte-identical to a waterfall texture: lines rows of bins
         * bytes, so an upload is a memcpy and this arithmetic must hold. */
        CHECK(bytes == (size_t)info.lines * (size_t)info.bins);

        if (data != NULL && bytes > 0) {
            const double db = sweeps_dequantise_db(data[0], (double)info.origin_db);
            CHECK(db >= (double)info.origin_db);
        }
    }

    CHECK(sweeps_tiles_at(tiles, count) == NULL);
    sweeps_tiles_free(tiles);
    sweeps_tiles_free(NULL);

    /* Restricting to one segment, and the two-call spectrum idiom. */
    sweeps_query_init(&query);
    query.has_segment_id = 1;
    query.segment_id = 1;
    tiles = NULL;
    CHECK_OK(sweeps_reader_query(reader, &query, &tiles));
    CHECK(tiles != NULL);
    sweeps_tiles_free(tiles);

    CHECK_OK(sweeps_reader_spectrum_at(reader, 1000000000ULL, 0, NULL, 0, &bins));
    CHECK(bins == 1200);
    CHECK_OK(sweeps_reader_spectrum_at(reader, 1000000000ULL, 0, spectrum,
                                       sizeof(spectrum) / sizeof(spectrum[0]), &bins));
    CHECK(bins == 1200);
    CHECK(spectrum[0] < 0.0F);
    /* A segment that is not there is not-found, not a silent empty spectrum. */
    CHECK_STATUS(sweeps_reader_spectrum_at(reader, 1000000000ULL, 99, spectrum, 8, &bins),
                 SWEEPS_ERR_NOT_FOUND);
}

static void test_golden_verify_and_extract(sweeps_reader_t* reader) {
    const char* const extracted = "capi-extract.sweeps";
    uint64_t records = 0;
    sweeps_query_t range;
    sweeps_extract_options_t options;
    sweeps_reader_t* second = NULL;
    sweeps_summary_t summary;

    /* Every record intact, which is a stronger statement than "it parsed". */
    CHECK_OK(sweeps_reader_verify(reader, &records));
    CHECK(records > 0);

    /* A time range, not a segment filter: extract consults only the time and
     * frequency bounds, so this is how you ask for one segment's worth. The
     * golden session's first segment ends around 1.8 s and its second runs to
     * about 7.8 s, so this window leaves exactly one behind. */
    sweeps_query_init(&range);
    range.from_ns = 3000000000ULL;
    range.to_ns = 8000000000ULL;

    sweeps_extract_options_init(&options);
    options.application_version = "capi-test";
    options.created_wall_ns = kCreatedWallNs;

    CHECK_OK(sweeps_reader_extract(reader, extracted, &range, &options));

    CHECK_OK(sweeps_reader_open(extracted, &second));
    if (second != NULL) {
        summary.struct_size = sizeof(summary);
        CHECK_OK(sweeps_reader_summary(second, &summary));
        CHECK(summary.created_wall_ns == kCreatedWallNs);
        CHECK(str_equals(sweeps_reader_app_version(second), "capi-test"));
        CHECK(sweeps_reader_segment_count(second) == 1);
        CHECK_OK(sweeps_reader_verify(second, &records));
        sweeps_reader_close(second);
    }
    (void)remove(extracted);
}

/* ------------------------------------------------------------------- writer */

static void test_writer_round_trip(void) {
    const char* const path = "capi-round-trip.sweeps";
    sweeps_writer_config_t config;
    sweeps_acq_config_t acquisition;
    sweeps_gain_t gains[2];
    sweeps_writer_t* writer = NULL;
    sweeps_metadata_t* fields = NULL;
    sweeps_reader_t* reader = NULL;
    sweeps_summary_t summary;
    float bins[256];
    int line = 0;
    int i = 0;
    uint64_t records = 0;
    const uint8_t body[4] = {1, 2, 3, 4};

    sweeps_writer_config_init(&config);
    CHECK(config.bins_per_line == 2048);
    CHECK(config.min_free_bytes == 512ULL * 1024 * 1024);

    config.bins_per_line = 256;
    config.session_name = "capi";
    config.notes = "written from C";
    config.application_version = "capi-test";
    config.created_wall_ns = kCreatedWallNs;
    /* A free-space check that trips would change the file. */
    config.min_free_bytes = 0;

    CHECK_OK(sweeps_writer_create(path, &config, &writer));
    if (writer == NULL) {
        return;
    }
    CHECK(str_equals(sweeps_writer_path(writer), path));

    gains[0].name = "lna";
    gains[0].value = 24.0;
    gains[1].name = "vga";
    gains[1].value = 16.0;

    sweeps_acq_config_init(&acquisition);
    /* The defaults that make zeroing this struct a trap, checked here so the
     * _init function cannot silently stop setting them. */
    CHECK(acquisition.window == SWEEPS_WINDOW_HANN);
    CHECK(acquisition.window_beta == 8.6);
    CHECK(acquisition.window_enbw == 1.5);

    acquisition.center_hz = 100e6;
    acquisition.span_hz = 2.5e6;
    acquisition.sample_rate = 2.5e6;
    acquisition.fft_size = 1024;
    acquisition.rbw_hz = 2.5e6 * 1.5 / 1024.0;
    acquisition.device_id = "capi-0";
    acquisition.device_label = "C ABI device";
    acquisition.gains = gains;
    acquisition.gain_count = 2;

    for (i = 0; i < 256; ++i) {
        bins[i] = -95.0F;
    }

    for (line = 0; line < 300; ++line) {
        sweeps_frame_t frame;
        sweeps_frame_outcome_t outcome;

        bins[line % 256] = -20.0F;

        sweeps_frame_init(&frame);
        frame.bins = bins;
        frame.count = 256;
        frame.start_hz = 98.75e6;
        frame.bin_width_hz = 2.5e6 / 256.0;
        frame.monotonic_ns = 1000000000ULL + (uint64_t)line * 20000000ULL;
        frame.wall_ns = frame.monotonic_ns;
        frame.config = &acquisition;

        outcome.struct_size = sizeof(outcome);
        CHECK_OK(sweeps_writer_write_frame(writer, &frame, &outcome));

        if (line == 0) {
            CHECK(outcome.segment_opened == 1);
            CHECK(sweeps_writer_last_segment_reason(writer).len > 0);
        }

        bins[line % 256] = -95.0F;
    }

    /* A forgotten config is diagnosed rather than dropping the frame with a
     * success status, which is what the C++ writer does with it. */
    {
        sweeps_frame_t frame;
        sweeps_frame_init(&frame);
        frame.bins = bins;
        frame.count = 256;
        CHECK_STATUS(sweeps_writer_write_frame(writer, &frame, NULL), SWEEPS_ERR_INVALID_ARGUMENT);
    }

    CHECK_OK(
        sweeps_writer_record_marker(writer, 1200000000ULL, 1200000000ULL, "m1", 99.5e6, -41.5));
    CHECK_OK(
        sweeps_writer_record_annotation(writer, 1300000000ULL, 1300000000ULL, "band", 99e6, 100e6));
    CHECK_OK(sweeps_writer_record_retune(writer, 1400000000ULL, 1400000000ULL, 100e6, 3));
    CHECK_OK(sweeps_writer_record_parameter_changed(writer, 1500000000ULL, 1500000000ULL, "gain",
                                                    "24", 0, 1));
    CHECK_OK(sweeps_writer_record_sweep_pass(writer, 1600000000ULL, 1600000000ULL, 7, 88e6, 108e6,
                                             0.25));
    CHECK_OK(sweeps_writer_record_segment_boundary(writer, 1700000000ULL, 1700000000ULL, "manual"));
    CHECK_OK(
        sweeps_writer_record_throttle_changed(writer, 1800000000ULL, 1800000000ULL, "cpu", 0.5));
    CHECK_OK(sweeps_writer_record_device_error(writer, 1900000000ULL, 1900000000ULL, "capi-0",
                                               "overflow"));

    fields = sweeps_metadata_create();
    CHECK(fields != NULL);
    CHECK_OK(sweeps_metadata_set_string(fields, "who", "capi"));
    CHECK_OK(sweeps_metadata_set_i64(fields, "count", 3));
    CHECK_OK(sweeps_metadata_set_f64(fields, "ratio", 0.5));
    CHECK_OK(sweeps_metadata_set_bool(fields, "ok", 1));
    CHECK_OK(sweeps_metadata_set_bytes(fields, "blob", body, sizeof(body)));
    CHECK(sweeps_metadata_count(fields) == 5);
    CHECK_STATUS(sweeps_metadata_set_string(NULL, "k", "v"), SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK_STATUS(sweeps_metadata_set_string(fields, NULL, "v"), SWEEPS_ERR_INVALID_ARGUMENT);

    CHECK_OK(sweeps_writer_record_plugin_event(writer, 2000000000ULL, 2000000000ULL,
                                               "org.sweeppp.capi", "hello", fields));
    CHECK_OK(sweeps_writer_plugin_data(writer, "org.sweeppp.capi", "state", 2, 2100000000ULL, body,
                                       sizeof(body)));

    CHECK(sweeps_writer_lines_written(writer) == 300);
    CHECK(sweeps_writer_segment_count(writer) == 1);
    CHECK(sweeps_writer_bytes_written(writer) > 0);
    CHECK(sweeps_writer_retention_reached(writer) == 0);
    CHECK(sweeps_writer_last_frame_ns(writer) > 0);

    /* Closing explicitly, and checking it: the destructor closes too, but it
     * has nowhere to report that writing the index failed. */
    CHECK_OK(sweeps_writer_close(writer));
    sweeps_writer_destroy(writer);
    sweeps_writer_destroy(NULL);

    CHECK_OK(sweeps_reader_open(path, &reader));
    if (reader != NULL) {
        const sweeps_plugin_record_t* record = NULL;
        sweeps_bytes_t payload;

        summary.struct_size = sizeof(summary);
        CHECK_OK(sweeps_reader_summary(reader, &summary));
        CHECK(summary.total_lines == 300);
        CHECK(summary.created_wall_ns == kCreatedWallNs);
        CHECK(str_equals(sweeps_reader_name(reader), "capi"));
        CHECK(str_equals(sweeps_reader_app_version(reader), "capi-test"));
        CHECK(str_equals(sweeps_reader_segment_device_label(reader, 0), "C ABI device"));
        CHECK(sweeps_reader_segment_gain_count(reader, 0) == 2);
        CHECK(sweeps_reader_event_count(reader) >= 9);

        CHECK_OK(sweeps_reader_verify(reader, &records));
        CHECK(records > 0);

        CHECK(sweeps_reader_plugin_count(reader) == 1);
        record = sweeps_reader_plugin_at(reader, 0);
        CHECK(record != NULL);
        if (record != NULL) {
            CHECK(str_equals(sweeps_plugin_id(record), "org.sweeppp.capi"));
            CHECK(str_equals(sweeps_plugin_name(record), "state"));
            CHECK(sweeps_plugin_schema_version(record) == 2);
            CHECK(sweeps_plugin_monotonic_ns(record) == 2100000000ULL);
            CHECK_OK(sweeps_plugin_body(record, &payload));
            CHECK(payload.len == sizeof(body));
            CHECK(payload.data != NULL && memcmp(payload.data, body, sizeof(body)) == 0);
        }

        /* The plugin event's fields survive the round trip as a nested object. */
        {
            size_t i2 = 0;
            int sawPlugin = 0;
            const size_t events = sweeps_reader_event_count(reader);
            for (i2 = 0; i2 < events; ++i2) {
                const sweeps_event_t* event = sweeps_reader_event_at(reader, i2);
                sweeps_str_t id;
                sweeps_str_t which;
                const sweeps_metadata_t* got = NULL;

                if (event == NULL || sweeps_event_kind(event) != SWEEPS_EVENT_PLUGIN) {
                    continue;
                }
                CHECK_OK(sweeps_event_plugin(event, &id, &which, &got));
                CHECK(str_equals(id, "org.sweeppp.capi"));
                CHECK(str_equals(which, "hello"));
                CHECK(got != NULL);
                if (got != NULL) {
                    sweeps_bytes_t blob;
                    CHECK(str_equals(sweeps_metadata_get_string(got, "who", ""), "capi"));
                    CHECK(sweeps_metadata_get_i64(got, "count", 0) == 3);
                    CHECK(sweeps_metadata_get_f64(got, "ratio", 0.0) == 0.5);
                    CHECK(sweeps_metadata_get_bool(got, "ok", 0) == 1);
                    CHECK_OK(sweeps_metadata_get_bytes(got, "blob", &blob));
                    CHECK(blob.len == sizeof(body));
                }
                sawPlugin = 1;
            }
            CHECK(sawPlugin == 1);
        }

        sweeps_reader_close(reader);
    }

    sweeps_metadata_destroy(fields);
    sweeps_metadata_destroy(NULL);
    (void)remove(path);
}

static void test_metadata_builder(void) {
    sweeps_metadata_t* outer = sweeps_metadata_create();
    sweeps_metadata_t* inner = sweeps_metadata_create();
    const sweeps_metadata_t* nested = NULL;
    char json[512];
    size_t needed = 0;

    CHECK(outer != NULL && inner != NULL);
    if (outer == NULL || inner == NULL) {
        sweeps_metadata_destroy(outer);
        sweeps_metadata_destroy(inner);
        return;
    }

    CHECK_OK(sweeps_metadata_set_string(inner, "leaf", "value"));
    CHECK_OK(sweeps_metadata_set_hash(outer, "child", inner));
    /* The setter copies, so the caller keeps owning what it passed. */
    CHECK_OK(sweeps_metadata_set_string(inner, "after", "still mine"));
    CHECK(sweeps_metadata_count(inner) == 2);

    CHECK_OK(sweeps_metadata_get_hash(outer, "child", &nested));
    CHECK(nested != NULL);
    if (nested != NULL) {
        CHECK(sweeps_metadata_count(nested) == 1);
        CHECK(str_equals(sweeps_metadata_get_string(nested, "leaf", ""), "value"));
    }

    CHECK_STATUS(sweeps_metadata_get_hash(outer, "leaf", &nested), SWEEPS_ERR_NOT_FOUND);
    CHECK_OK(sweeps_metadata_to_json(outer, 2, json, sizeof(json), &needed));
    CHECK(needed > 0 && strlen(json) == needed);

    sweeps_metadata_destroy(inner);
    sweeps_metadata_destroy(outer);
}

/* ------------------------------------------------------------- live streams */

/* The golden stream's numbers, restated from tests/test_stream.cpp, which
 * writes it. */
static const char* const kGoldenStreamPath = SWEEPSFILE_TEST_DATA_DIR "/v1-golden.sweepstream";

static unsigned char* read_file(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    unsigned char* data = NULL;
    long length;

    *size = 0;
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 &&
        fseek(file, 0, SEEK_SET) == 0) {
        data = (unsigned char*)malloc((size_t)length);
        if (data != NULL && fread(data, 1, (size_t)length, file) == (size_t)length) {
            *size = (size_t)length;
        } else {
            free(data);
            data = NULL;
        }
    }
    (void)fclose(file);
    return data;
}

/* Within half a quantisation step, which is all a stored byte promises. */
static int near_db(float actual, double expected) {
    double diff = (double)actual - expected;
    if (diff < 0) {
        diff = -diff;
    }
    return diff <= 0.25 + 1e-4;
}

/* Little-endian u32, for finding a record's payload by hand. */
static uint32_t read_u32(const unsigned char* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

typedef struct stream_tally_t {
    size_t records;
    size_t tiles;
    size_t plugin_data;
    size_t unknown;
    size_t end_of_stream;
    int checked_first_segment;
} stream_tally_t;

static void check_first_segment(const sweeps_stream_mirror_t* mirror) {
    sweeps_stream_line_t line;
    sweeps_segment_t segment;
    sweeps_str_t gain_name;
    double gain_value = 0.0;

    line.struct_size = sizeof(line);
    CHECK_OK(sweeps_stream_mirror_line(mirror, &line));
    CHECK(line.segment_id == 7);
    CHECK(line.bin_count == 1500);
    CHECK(line.start_hz == 100e6);
    CHECK(line.bin_width_hz == 1000.0);
    CHECK(line.line == 1);
    CHECK(line.tiles_applied == 3);
    CHECK(line.levels != NULL);
    if (line.levels != NULL) {
        CHECK(near_db(line.levels[0], -100.0));
        CHECK(near_db(line.levels[39], -80.5));
        CHECK(line.levels[10] == (float)SWEEPS_UNMEASURED_DB);
        CHECK(near_db(line.levels[700], -20.0));
        CHECK(near_db(line.levels[1200], -15.0));
    }

    segment.struct_size = sizeof(segment);
    CHECK_OK(sweeps_stream_mirror_segment(mirror, &segment));
    CHECK(segment.id == 7);
    CHECK(segment.bin_count == 1500);
    CHECK(segment.fft_size == 2048);
    CHECK(segment.window == SWEEPS_WINDOW_HANN);
    CHECK(segment.sample_rate == 2e6);
    CHECK(segment.center_hz == 100.75e6);
    CHECK(segment.start_monotonic_ns == 1000000000ULL);
    CHECK(segment.gain_count == 1);
    CHECK(str_equals(sweeps_stream_mirror_segment_reason(mirror), "stream start"));
    CHECK(str_equals(sweeps_stream_mirror_segment_device_id(mirror), "golden-stream"));
    CHECK(str_equals(sweeps_stream_mirror_segment_device_label(mirror), "Golden stream device"));
    CHECK_OK(sweeps_stream_mirror_segment_gain(mirror, 0, &gain_name, &gain_value));
    CHECK(str_equals(gain_name, "lna"));
    CHECK(gain_value == 24.0);
    CHECK_STATUS(sweeps_stream_mirror_segment_gain(mirror, 1, NULL, NULL), SWEEPS_ERR_OUT_OF_RANGE);
}

/* Drains every complete record, applying each to the mirror. Returns the first
 * failure, or SWEEPS_OK when the reader wants more bytes. */
static sweeps_status_t drain_stream(sweeps_stream_reader_t* reader, sweeps_stream_mirror_t* mirror,
                                    stream_tally_t* tally) {
    for (;;) {
        sweeps_stream_record_t record;
        int has_record = 0;
        sweeps_status_t status;

        record.struct_size = sizeof(record);
        status = sweeps_stream_reader_next_record(reader, &record, &has_record);
        if (status != SWEEPS_OK || !has_record) {
            return status;
        }

        ++tally->records;
        switch (record.type) {
        case SWEEPS_RECORD_TILE:
            ++tally->tiles;
            break;
        case SWEEPS_RECORD_PLUGIN_DATA:
            ++tally->plugin_data;
            break;
        case SWEEPS_RECORD_END_OF_STREAM:
            ++tally->end_of_stream;
            CHECK(record.payload.len == 0);
            break;
        case 0x0042:
            ++tally->unknown;
            CHECK(record.payload.len == 1 && record.payload.data[0] == 0x5A);
            break;
        default:
            break;
        }

        CHECK_OK(sweeps_stream_mirror_apply(mirror, &record));

        /* Line 1 of segment 7 is complete by its SegmentClose, which leaves
         * the line as it was. */
        if (record.type == SWEEPS_RECORD_SEGMENT_CLOSE) {
            check_first_segment(mirror);
            tally->checked_first_segment = 1;
        }
    }
}

/* Feeds `bytes` in chunks of 1, 2, 3, 5, 8, ... 233 and back to 1, so record
 * and header boundaries land everywhere but where they would by accident. */
static sweeps_status_t feed_in_chunks(sweeps_stream_reader_t* reader,
                                      sweeps_stream_mirror_t* mirror, const unsigned char* bytes,
                                      size_t size, stream_tally_t* tally) {
    size_t offset = 0;
    size_t a = 1;
    size_t b = 2;

    while (offset < size) {
        size_t chunk = a < size - offset ? a : size - offset;
        size_t next = a + b;
        sweeps_status_t status = sweeps_stream_reader_feed(reader, bytes + offset, chunk);

        if (status != SWEEPS_OK) {
            return status;
        }
        offset += chunk;
        status = drain_stream(reader, mirror, tally);
        if (status != SWEEPS_OK) {
            return status;
        }

        a = b;
        b = next > 233 ? 1 : next;
    }
    return SWEEPS_OK;
}

static void test_stream_golden(const unsigned char* bytes, size_t size) {
    sweeps_stream_reader_t* reader = NULL;
    sweeps_stream_mirror_t* mirror = NULL;
    sweeps_stream_line_t line;
    stream_tally_t tally;

    memset(&tally, 0, sizeof(tally));
    CHECK_OK(sweeps_stream_reader_create(0, &reader));
    CHECK_OK(sweeps_stream_mirror_create(0, &mirror));
    if (reader == NULL || mirror == NULL) {
        sweeps_stream_reader_destroy(reader);
        sweeps_stream_mirror_destroy(mirror);
        return;
    }

    line.struct_size = sizeof(line);
    CHECK_STATUS(sweeps_stream_mirror_line(mirror, &line), SWEEPS_ERR_NOT_FOUND);
    CHECK(sweeps_stream_mirror_segment_reason(mirror).len == 0);

    CHECK_OK(feed_in_chunks(reader, mirror, bytes, size, &tally));
    CHECK(tally.records == 12);
    CHECK(tally.tiles == 4);
    CHECK(tally.plugin_data == 3);
    CHECK(tally.unknown == 1);
    CHECK(tally.end_of_stream == 1);
    CHECK(tally.checked_first_segment == 1);
    CHECK(sweeps_stream_reader_buffered(reader) == 0);

    /* The second segment replaced the first. */
    CHECK_OK(sweeps_stream_mirror_line(mirror, &line));
    CHECK(line.segment_id == 8);
    CHECK(line.bin_count == 300);
    CHECK(line.start_hz == 433.05e6);
    CHECK(line.bin_width_hz == 2500.0);
    CHECK(line.line == 0);
    CHECK(line.tiles_applied == 1);
    if (line.levels != NULL) {
        CHECK(near_db(line.levels[0], -90.0));
        CHECK(near_db(line.levels[299], -81.0));
    }
    CHECK(str_equals(sweeps_stream_mirror_segment_reason(mirror), "acquisition changed"));

    sweeps_stream_reader_destroy(reader);
    sweeps_stream_mirror_destroy(mirror);
}

static void test_stream_truncated(const unsigned char* bytes, size_t size) {
    sweeps_stream_reader_t* reader = NULL;
    sweeps_stream_mirror_t* mirror = NULL;
    stream_tally_t tally;

    CHECK_OK(sweeps_stream_mirror_create(0, &mirror));

    /* Inside the header: nothing to report yet, and nothing wrong. */
    memset(&tally, 0, sizeof(tally));
    CHECK_OK(sweeps_stream_reader_create(0, &reader));
    CHECK_OK(feed_in_chunks(reader, mirror, bytes, 10, &tally));
    CHECK(tally.records == 0);
    CHECK(sweeps_stream_reader_buffered(reader) == 10);
    sweeps_stream_reader_destroy(reader);

    /* Halfway: what arrived intact is read, the rest is pending, not an
     * error -- a connection that drops is a truncated file, not a corrupt
     * one. */
    memset(&tally, 0, sizeof(tally));
    CHECK_OK(sweeps_stream_reader_create(0, &reader));
    CHECK_OK(feed_in_chunks(reader, mirror, bytes, size / 2, &tally));
    CHECK(tally.records > 0 && tally.records < 12);
    CHECK(tally.end_of_stream == 0);
    CHECK(sweeps_stream_reader_buffered(reader) > 0);
    sweeps_stream_reader_destroy(reader);

    sweeps_stream_mirror_destroy(mirror);
}

static void test_stream_broken(const unsigned char* golden, size_t size) {
    sweeps_stream_reader_t* reader = NULL;
    sweeps_stream_mirror_t* mirror = NULL;
    sweeps_stream_record_t record;
    unsigned char* damaged = (unsigned char*)malloc(size);
    stream_tally_t tally;
    int has_record = 1;
    const char http[] = "GET / HTTP/1.1\r\n";
    size_t second_payload;

    CHECK(damaged != NULL);
    if (damaged == NULL) {
        return;
    }
    CHECK_OK(sweeps_stream_mirror_create(0, &mirror));
    record.struct_size = sizeof(record);

    /* A bit flipped in the second record's payload (the first tile's data):
     * the first record is read, then the stream is broken for good. */
    second_payload = 16 + 12 + read_u32(golden + 16 + 4) + 12;
    memcpy(damaged, golden, size);
    damaged[second_payload + 100] ^= 0x01;

    memset(&tally, 0, sizeof(tally));
    CHECK_OK(sweeps_stream_reader_create(0, &reader));
    CHECK_STATUS(feed_in_chunks(reader, mirror, damaged, size, &tally), SWEEPS_ERR_CORRUPT);
    CHECK(tally.records == 1);
    CHECK(sweeps_last_error().len > 0);
    CHECK_STATUS(sweeps_stream_reader_next_record(reader, &record, &has_record),
                 SWEEPS_ERR_CORRUPT);
    CHECK(has_record == 0);
    CHECK_STATUS(sweeps_stream_reader_feed(reader, golden, size), SWEEPS_ERR_CORRUPT);
    sweeps_stream_reader_destroy(reader);

    /* Not a stream at all. */
    CHECK_OK(sweeps_stream_reader_create(0, &reader));
    CHECK_STATUS(sweeps_stream_reader_feed(reader, http, sizeof(http) - 1), SWEEPS_ERR_PROTOCOL);
    CHECK_STATUS(sweeps_stream_reader_feed(reader, golden + 16, size - 16), SWEEPS_ERR_PROTOCOL);
    CHECK_STATUS(sweeps_stream_reader_next_record(reader, &record, &has_record),
                 SWEEPS_ERR_PROTOCOL);
    sweeps_stream_reader_destroy(reader);

    /* A record over the reader's limit: the SegmentOpen fits, the first tile
     * does not. */
    memset(&tally, 0, sizeof(tally));
    CHECK_OK(sweeps_stream_reader_create(512, &reader));
    CHECK_STATUS(feed_in_chunks(reader, mirror, golden, size, &tally), SWEEPS_ERR_PROTOCOL);
    CHECK(tally.records == 1);
    sweeps_stream_reader_destroy(reader);

    free(damaged);
    sweeps_stream_mirror_destroy(mirror);
}

static void test_stream_misuse(void) {
    sweeps_stream_reader_t* reader = NULL;
    sweeps_stream_mirror_t* mirror = NULL;
    sweeps_stream_record_t record;
    sweeps_stream_line_t line;
    const uint8_t garbage[3] = {1, 2, 3};
    int has_record = 0;

    CHECK_STATUS(sweeps_stream_reader_create(0, NULL), SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK_STATUS(sweeps_stream_mirror_create(0, NULL), SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK_STATUS(sweeps_stream_reader_feed(NULL, garbage, 3), SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK(sweeps_stream_reader_buffered(NULL) == 0);
    CHECK_STATUS(sweeps_stream_mirror_line(NULL, &line), SWEEPS_ERR_INVALID_ARGUMENT);
    sweeps_stream_reader_destroy(NULL);
    sweeps_stream_mirror_destroy(NULL);

    CHECK_OK(sweeps_stream_reader_create(0, &reader));
    CHECK_OK(sweeps_stream_mirror_create(0, &mirror));
    if (reader == NULL || mirror == NULL) {
        sweeps_stream_reader_destroy(reader);
        sweeps_stream_mirror_destroy(mirror);
        return;
    }

    CHECK_STATUS(sweeps_stream_reader_feed(reader, NULL, 3), SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK_OK(sweeps_stream_reader_feed(reader, NULL, 0));

    memset(&record, 0, sizeof(record));
    CHECK_STATUS(sweeps_stream_reader_next_record(reader, &record, &has_record),
                 SWEEPS_ERR_INVALID_ARGUMENT);
    record.struct_size = sizeof(record);
    CHECK_STATUS(sweeps_stream_reader_next_record(reader, &record, NULL),
                 SWEEPS_ERR_INVALID_ARGUMENT);

    /* A record a caller framed itself: a damaged tile is refused, an unknown
     * type ignored, and a zeroed struct diagnosed. */
    record.type = SWEEPS_RECORD_TILE;
    record.payload.data = garbage;
    record.payload.len = sizeof(garbage);
    CHECK(sweeps_stream_mirror_apply(mirror, &record) != SWEEPS_OK);
    record.type = 0x10000U + SWEEPS_RECORD_SEGMENT_OPEN;
    CHECK_OK(sweeps_stream_mirror_apply(mirror, &record));
    record.payload.data = NULL;
    CHECK_STATUS(sweeps_stream_mirror_apply(mirror, &record), SWEEPS_ERR_INVALID_ARGUMENT);
    record.struct_size = 0;
    CHECK_STATUS(sweeps_stream_mirror_apply(mirror, &record), SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK_STATUS(sweeps_stream_mirror_apply(mirror, NULL), SWEEPS_ERR_INVALID_ARGUMENT);

    sweeps_stream_reader_destroy(reader);
    sweeps_stream_mirror_destroy(mirror);
}

static void test_streams(void) {
    size_t size = 0;
    unsigned char* golden = read_file(kGoldenStreamPath, &size);

    CHECK(golden != NULL && size > 16);
    if (golden == NULL) {
        (void)fprintf(stderr, "cannot read %s\n", kGoldenStreamPath);
        return;
    }

    test_stream_golden(golden, size);
    test_stream_truncated(golden, size);
    test_stream_broken(golden, size);
    test_stream_misuse();
    free(golden);
}

/* --------------------------------------------------------------------- main */

int main(void) {
    sweeps_reader_t* reader = NULL;

    test_version();
    test_status_names();
    test_quantisation();
    test_open_failure();
    test_struct_size_contract();

    if (sweeps_reader_open(kGoldenPath, &reader) != SWEEPS_OK || reader == NULL) {
        (void)fprintf(stderr, "cannot open %s: %s\n", kGoldenPath, sweeps_last_error().data);
        return EXIT_FAILURE;
    }

    test_golden_summary(reader);
    test_golden_segments(reader);
    test_golden_manifest(reader);
    test_golden_events(reader);
    test_golden_tiles(reader);
    test_golden_verify_and_extract(reader);
    sweeps_reader_close(reader);

    test_writer_round_trip();
    test_metadata_builder();
    test_streams();

    if (g_failures != 0) {
        (void)fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    (void)printf("capi: all checks passed\n");
    return EXIT_SUCCESS;
}
