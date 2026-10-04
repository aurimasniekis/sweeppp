// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

/* Reads a live `.sweeps` stream on stdin and prints each line's strongest bin.
 *
 *     sweeppp-cli record --device synthetic --duration 2 -o - | sweeps-stream-c
 *
 * What marks a line complete is the producer's to define (Appendix C.5). Sweep++
 * sends a PluginData commit after each line's tiles, so a PluginData record that
 * follows at least one tile is taken as the end of a line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sweeps/sweeps.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

static int fail(const char* what, sweeps_status_t status) {
    (void)fprintf(stderr, "%s: %s (%s)\n", what, sweeps_status_name(status),
                  sweeps_last_error().data);
    return EXIT_FAILURE;
}

static void print_segment(const sweeps_stream_mirror_t* mirror) {
    sweeps_segment_t segment;

    segment.struct_size = sizeof(segment);
    if (sweeps_stream_mirror_segment(mirror, &segment) == SWEEPS_OK) {
        (void)printf("segment %u: %u bins, %.3f .. %.3f MHz, rbw %.1f kHz\n", segment.id,
                     segment.bin_count, segment.start_hz / 1e6,
                     (segment.start_hz + segment.bin_width_hz * segment.bin_count) / 1e6,
                     segment.rbw_hz / 1e3);
    }
}

static void print_peak(const sweeps_stream_mirror_t* mirror) {
    sweeps_stream_line_t line;
    uint32_t bin;
    uint32_t peak = 0;

    line.struct_size = sizeof(line);
    if (sweeps_stream_mirror_line(mirror, &line) != SWEEPS_OK || line.bin_count == 0) {
        return;
    }
    for (bin = 1; bin < line.bin_count; ++bin) {
        if (line.levels[bin] > line.levels[peak]) {
            peak = bin;
        }
    }
    if (line.levels[peak] == (float)SWEEPS_UNMEASURED_DB) {
        return;
    }
    (void)printf("line %u: peak %.6f MHz %.1f dB\n", line.line,
                 (line.start_hz + line.bin_width_hz * peak) / 1e6, line.levels[peak]);
}

int main(void) {
    static unsigned char buffer[65536];
    sweeps_stream_reader_t* reader = NULL;
    sweeps_stream_mirror_t* mirror = NULL;
    sweeps_status_t status;
    size_t got;
    int tiles = 0;
    int ended = 0;
    int result = EXIT_SUCCESS;

#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
#endif

    status = sweeps_stream_reader_create(0, &reader);
    if (status != SWEEPS_OK) {
        return fail("reader", status);
    }
    status = sweeps_stream_mirror_create(0, &mirror);
    if (status != SWEEPS_OK) {
        sweeps_stream_reader_destroy(reader);
        return fail("mirror", status);
    }

    while (!ended && (got = fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        status = sweeps_stream_reader_feed(reader, buffer, got);
        while (status == SWEEPS_OK && !ended) {
            sweeps_stream_record_t record;
            int has_record = 0;

            record.struct_size = sizeof(record);
            status = sweeps_stream_reader_next_record(reader, &record, &has_record);
            if (status != SWEEPS_OK || !has_record) {
                break;
            }
            status = sweeps_stream_mirror_apply(mirror, &record);
            if (status != SWEEPS_OK) {
                break;
            }

            switch (record.type) {
            case SWEEPS_RECORD_SEGMENT_OPEN:
                print_segment(mirror);
                tiles = 0;
                break;
            case SWEEPS_RECORD_TILE:
                ++tiles;
                break;
            case SWEEPS_RECORD_PLUGIN_DATA:
                if (tiles > 0) {
                    print_peak(mirror);
                    tiles = 0;
                }
                break;
            case SWEEPS_RECORD_END_OF_STREAM:
                ended = 1;
                break;
            default:
                break;
            }
        }
        if (status != SWEEPS_OK) {
            result = fail("stream", status);
            break;
        }
    }

    if (result == EXIT_SUCCESS && !ended && sweeps_stream_reader_buffered(reader) > 0) {
        (void)fprintf(stderr, "the stream ended mid-record\n");
        result = EXIT_FAILURE;
    }

    sweeps_stream_mirror_destroy(mirror);
    sweeps_stream_reader_destroy(reader);
    return result;
}
