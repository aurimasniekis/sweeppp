// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

/* Writes and reads a session with nothing but the installed C header.
 *
 * The mirror of ../consumer/main.cpp, one language down. It includes only
 * <sweeps/sweeps.h>: if that header ever needs a `.hpp` beside it to be useful,
 * or stops being installed, this stops compiling.
 *
 * Both halves of the interface, deliberately -- the C ABI would be half a
 * feature if a binding could read `.sweeps` files and not produce them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sweeps/sweeps.h>

static int fail(const char* what, sweeps_status_t status) {
    (void)fprintf(stderr, "%s: %s (%s)\n", what, sweeps_status_name(status),
                  sweeps_last_error().data);
    return EXIT_FAILURE;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "sweeps-consumer-c.sweeps";

    sweeps_writer_config_t config;
    sweeps_acq_config_t acquisition;
    sweeps_gain_t gain;
    sweeps_writer_t* writer = NULL;
    sweeps_reader_t* reader = NULL;
    sweeps_summary_t summary;
    sweeps_status_t status;
    float bins[256];
    int line;
    int i;
    uint64_t records = 0;
    uint32_t major = 0;
    uint32_t minor = 0;

    if (sweeps_abi_version() != SWEEPS_ABI_VERSION) {
        (void)fprintf(stderr, "abi mismatch: header %d, library %u\n", SWEEPS_ABI_VERSION,
                      sweeps_abi_version());
        return EXIT_FAILURE;
    }

    sweeps_writer_config_init(&config);
    config.bins_per_line = 256;
    config.session_name = "consumer-c";
    config.created_wall_ns = 1700000000000000000ULL;
    config.min_free_bytes = 0;

    status = sweeps_writer_create(path, &config, &writer);
    if (status != SWEEPS_OK) {
        return fail("write failed", status);
    }

    gain.name = "lna";
    gain.value = 24.0;

    sweeps_acq_config_init(&acquisition);
    acquisition.center_hz = 100e6;
    acquisition.span_hz = 2.5e6;
    acquisition.sample_rate = 2.5e6;
    acquisition.fft_size = 1024;
    acquisition.device_id = "consumer-0";
    acquisition.gains = &gain;
    acquisition.gain_count = 1;

    for (i = 0; i < 256; ++i) {
        bins[i] = -95.0F;
    }

    for (line = 0; line < 300; ++line) {
        sweeps_frame_t frame;

        bins[line % 256] = -20.0F;

        sweeps_frame_init(&frame);
        frame.bins = bins;
        frame.count = 256;
        frame.start_hz = 98.75e6;
        frame.bin_width_hz = 2.5e6 / 256.0;
        frame.monotonic_ns = 1000000000ULL + (uint64_t)line * 20000000ULL;
        frame.wall_ns = frame.monotonic_ns;
        frame.config = &acquisition;

        status = sweeps_writer_write_frame(writer, &frame, NULL);
        if (status != SWEEPS_OK) {
            sweeps_writer_destroy(writer);
            return fail("frame failed", status);
        }

        bins[line % 256] = -95.0F;
    }

    status = sweeps_writer_record_marker(writer, 1500000000ULL, 1500000000ULL, "m1", 99.5e6, -41.5);
    if (status != SWEEPS_OK) {
        sweeps_writer_destroy(writer);
        return fail("marker failed", status);
    }

    /* Explicitly, and checked: the destructor closes too, but it has nowhere to
     * report that writing the index failed. */
    status = sweeps_writer_close(writer);
    sweeps_writer_destroy(writer);
    if (status != SWEEPS_OK) {
        return fail("close failed", status);
    }

    status = sweeps_reader_open(path, &reader);
    if (status != SWEEPS_OK) {
        return fail("open failed", status);
    }

    status = sweeps_reader_verify(reader, &records);
    if (status != SWEEPS_OK) {
        sweeps_reader_close(reader);
        return fail("verify failed", status);
    }

    summary.struct_size = sizeof(summary);
    status = sweeps_reader_summary(reader, &summary);
    if (status != SWEEPS_OK) {
        sweeps_reader_close(reader);
        return fail("summary failed", status);
    }

    sweeps_format_version(&major, &minor);
    (void)printf("sweeps %u.%u | libsweepsfile %s, abi %u | %s | %llu records, %llu tiles, "
                 "%llu lines | %.6f .. %.6f MHz\n",
                 summary.major_version, summary.minor_version, sweeps_library_version().data,
                 sweeps_abi_version(), sweeps_reader_name(reader).data, (unsigned long long)records,
                 (unsigned long long)summary.total_tiles, (unsigned long long)summary.total_lines,
                 summary.lowest_hz / 1e6, summary.highest_hz / 1e6);

    if (summary.total_tiles == 0 || sweeps_reader_segment_count(reader) != 1 ||
        summary.total_lines != 300 || sweeps_reader_event_count(reader) < 1) {
        sweeps_reader_close(reader);
        (void)fprintf(stderr, "the round-tripped session is not what was written\n");
        return EXIT_FAILURE;
    }

    sweeps_reader_close(reader);
    (void)remove(path);
    return EXIT_SUCCESS;
}
