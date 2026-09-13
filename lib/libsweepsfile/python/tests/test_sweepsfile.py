# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: MIT

"""The Python binding, against the frozen reference session.

The assertions about the golden file are deliberately the same numbers
``test_capi.c`` and ``test_golden.cpp`` assert. Three languages reading one
frozen file and agreeing about what is in it is the claim; a binding that only
agreed with itself would be testing nothing.

stdlib ``unittest`` rather than pytest, so the suite has no more dependencies
than the library it binds.
"""

from __future__ import annotations

import json
import os
import tempfile
import unittest

import sweepsfile
from sweepsfile import AcquisitionConfig, EventKind, ValueType, WindowType

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None


DATA_DIR = os.environ.get(
    "SWEEPSFILE_TEST_DATA_DIR",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "data"),
)
GOLDEN = os.path.join(DATA_DIR, "v1-golden.sweeps")

# Pinned by tests/GoldenSession.hpp. Restated rather than imported, because a
# binding that could not state them for itself would be checking a header
# against itself.
CREATED_WALL_NS = 1_770_000_000_000_000_000
TOTAL_LINES = 340


class TestLibrary(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(sweepsfile.abi_version(), sweepsfile.ABI_VERSION)
        self.assertTrue(sweepsfile.library_version())
        self.assertEqual(sweepsfile.format_version()[0], 1)

    def test_quantisation_round_trips(self):
        # Silent when wrong -- plausible levels, forty decibels off -- so it is
        # checked rather than trusted.
        self.assertEqual(sweepsfile.quantise_db(-100.0, -100.0), 1)
        self.assertEqual(sweepsfile.quantise_db(-99.5, -100.0), 1)
        self.assertEqual(sweepsfile.dequantise_db(1, -100.0), -99.5)
        self.assertEqual(sweepsfile.dequantise_db(2, -100.0), -99.0)
        # Saturating rather than wrapping at both ends.
        self.assertEqual(sweepsfile.quantise_db(-150.0, -100.0), 1)
        self.assertEqual(sweepsfile.quantise_db(500.0, -100.0), 255)

    def test_unmeasured_is_not_a_level(self):
        # A gap between two swept spans must not come back as a quiet reading.
        self.assertEqual(
            sweepsfile.quantise_db(sweepsfile.UNMEASURED_DB, -100.0),
            sweepsfile.UNMEASURED_BYTE,
        )
        self.assertEqual(
            sweepsfile.dequantise_db(sweepsfile.UNMEASURED_BYTE, -100.0),
            sweepsfile.UNMEASURED_DB,
        )
        self.assertEqual(
            sweepsfile.dequantise_db(sweepsfile.UNMEASURED_BYTE, -40.0),
            sweepsfile.UNMEASURED_DB,
        )
        self.assertEqual(sweepsfile.quantise_db(float("-inf"), -100.0), sweepsfile.UNMEASURED_BYTE)
        self.assertEqual(sweepsfile.quantise_db(float("nan"), -100.0), sweepsfile.UNMEASURED_BYTE)

    def test_names(self):
        self.assertEqual(sweepsfile.window_type_name(WindowType.HANN), "hann")
        self.assertEqual(sweepsfile.event_kind_name(EventKind.MARKER), "marker")
        self.assertEqual(sweepsfile.event_kind_name(60000), "unknown")


class TestGolden(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.session = sweepsfile.open(GOLDEN)

    @classmethod
    def tearDownClass(cls):
        cls.session.close()

    def test_summary(self):
        summary = self.session.summary
        self.assertEqual(summary.format_version, (1, 0))
        self.assertEqual(summary.created_wall_ns, CREATED_WALL_NS)
        self.assertEqual(summary.total_lines, TOTAL_LINES)
        self.assertGreater(summary.total_tiles, 0)
        self.assertFalse(summary.recovered_by_scan)
        self.assertEqual(summary.truncated_bytes, 0)
        self.assertFalse(summary.newer_minor_version)
        self.assertAlmostEqual(summary.duration_seconds, 6.78, places=6)

        self.assertEqual(self.session.name, "golden")
        self.assertEqual(self.session.app_version, "0.0.0-golden")

        # Nanosecond timestamps survive the trip to datetime, which a float
        # seconds conversion would not manage.
        created = summary.created
        self.assertEqual(created.year, 2026)
        self.assertEqual(created.tzinfo.utcoffset(created).total_seconds(), 0)

    def test_segments(self):
        segments = self.session.segments
        self.assertEqual(len(segments), 2)

        self.assertEqual(segments[0].id, 0)
        self.assertEqual(segments[0].bin_count, 1200)
        self.assertEqual(segments[0].fft_size, 4096)
        self.assertEqual(segments[0].window, WindowType.HANN)
        self.assertEqual(segments[0].reason, "session start")
        self.assertEqual(segments[0].start_wall_ns, CREATED_WALL_NS)

        self.assertEqual(segments[1].bin_count, 128)
        self.assertEqual(segments[1].fft_size, 8192)
        self.assertEqual(segments[1].device_id, "golden-0")
        self.assertEqual(segments[1].device_label, "Golden reference device")
        self.assertEqual(segments[1].gains, {"lna": 24.0, "vga": 16.0})
        self.assertAlmostEqual(
            segments[1].stop_hz, segments[1].start_hz + segments[1].bin_width_hz * 128
        )

    def test_segment_by_id(self):
        # By id, never by position: an extracted file's ids neither start at
        # zero nor run contiguously.
        self.assertEqual(self.session.segment_by_id(1).bin_count, 128)
        with self.assertRaises(sweepsfile.NotFoundError):
            self.session.segment_by_id(77)

    def test_manifest_is_a_dict_with_real_types(self):
        manifest = self.session.manifest
        self.assertEqual(len(manifest), 10)
        self.assertEqual(manifest["name"], "golden")
        self.assertEqual(manifest["created"], "2026-02-02T02:40:00Z")
        self.assertEqual(manifest["format_version"], 1)
        self.assertEqual(manifest["bins_per_line"], 1200)
        self.assertEqual(manifest["tile_bins"], sweepsfile.TILE_BINS)
        self.assertEqual(manifest["tile_lines"], sweepsfile.TILE_LINES)
        self.assertEqual(manifest["lod_levels"], sweepsfile.LOD_LEVELS)
        self.assertEqual(manifest["db_per_step"], sweepsfile.DB_PER_STEP)

        # Types are part of the contract, not an accident of how a value was
        # set: `created` is a string so that `strings` on the file still shows a
        # date, and `db_per_step` is a float so a half never arrives as a zero.
        self.assertIsInstance(manifest["created"], str)
        self.assertIsInstance(manifest["db_per_step"], float)
        self.assertIsInstance(manifest["tile_bins"], int)
        self.assertNotIsInstance(manifest["tile_bins"], bool)

        # And it is a plain dict, so it survives its reader and serialises.
        self.assertIs(type(manifest), dict)
        json.dumps(manifest)

    def test_events(self):
        events = self.session.events
        self.assertGreaterEqual(len(events), 2)

        by_kind = {event.kind: event for event in events}
        marker = by_kind[EventKind.MARKER]
        self.assertEqual(marker.kind_name, "marker")
        self.assertEqual(marker.body["label"], "m1")
        self.assertEqual(marker.body["frequency_hz"], 401e6)
        self.assertEqual(marker.body["level_dbm"], -42.25)
        self.assertEqual(marker.monotonic_ns, 1_000_001_000)

        annotation = by_kind[EventKind.ANNOTATION]
        self.assertEqual(annotation.body["text"], "golden")
        self.assertEqual(annotation.body["start_hz"], 95e6)

        boundary = by_kind[EventKind.SEGMENT_BOUNDARY]
        self.assertTrue(boundary.body["reason"])

    def test_events_are_a_lazy_sequence(self):
        events = self.session.events
        self.assertEqual(events[-1].kind, events[len(events) - 1].kind)
        self.assertEqual(len(events[:2]), 2)
        self.assertEqual(len(list(events)), len(events))
        with self.assertRaises(IndexError):
            events[len(events)]

    def test_tiles(self):
        tiles = self.session.query()
        self.assertGreater(len(tiles), 0)

        for tile in tiles:
            self.assertGreater(tile.lines, 0)
            self.assertGreater(tile.bins, 0)
            self.assertLessEqual(tile.lines, sweepsfile.TILE_LINES)
            self.assertLessEqual(tile.bins, sweepsfile.TILE_BINS)
            # Byte-identical to a waterfall texture: lines rows of bins bytes,
            # so an upload is a memcpy and this arithmetic must hold.
            self.assertEqual(len(tile.data), tile.lines * tile.bins)
            self.assertGreaterEqual(tile.db_at(0, 0), tile.origin_db)
            self.assertGreaterEqual(tile.last_line_ns, tile.first_line_ns)

        with self.assertRaises(IndexError):
            tiles[0].db_at(tiles[0].lines, 0)

    def test_query_bounds(self):
        # Restricting to one segment, and to one pyramid level.
        one = self.session.query(segment_id=1)
        self.assertTrue(all(tile.segment_id == 1 for tile in one))

        native = self.session.query(segment_id=1, lod=0)
        self.assertTrue(all(tile.lod == 0 for tile in native))

        # A level with no tiles returns nothing rather than silently falling
        # back to one that has some. The level is asked for rather than assumed:
        # a short session's coarse levels still hold one partial tile each, so
        # naming a number here would be testing the fixture, not the rule.
        empty_lod = sweepsfile.LOD_LEVELS + 1
        self.assertFalse(self.session.has_tiles_at_lod(1, empty_lod))
        self.assertEqual(self.session.query(segment_id=1, lod=empty_lod), [])

        self.assertTrue(self.session.has_tiles_at_lod(0, 0))
        self.assertTrue(self.session.has_tiles_at_lod(1, 2))
        self.assertFalse(self.session.has_tiles_at_lod(99, 0))
        self.assertLess(self.session.choose_lod(0, 2**63, 2048, 0), sweepsfile.LOD_LEVELS)

    @unittest.skipIf(np is None, "numpy not installed")
    def test_tile_arrays(self):
        tile = self.session.query(segment_id=1, lod=0)[0]

        levels = tile.levels()
        self.assertEqual(levels.shape, (tile.lines, tile.bins))
        self.assertEqual(levels.dtype, np.uint8)

        db = tile.levels_db()
        self.assertEqual(db.shape, (tile.lines, tile.bins))
        self.assertEqual(db.dtype, np.float32)
        # The array path and the scalar path must agree, or one of them is a
        # different reading of the same bytes.
        self.assertAlmostEqual(float(db[0, 0]), tile.db_at(0, 0), places=4)
        self.assertAlmostEqual(
            float(db.min()), tile.origin_db + int(levels.min()) * sweepsfile.DB_PER_STEP, places=4
        )

    def test_spectrum_at(self):
        spectrum = self.session.spectrum_at(1_000_000_000, 0)
        self.assertEqual(len(spectrum), 1200)
        self.assertLess(float(spectrum[0]), 0.0)

        with self.assertRaises(sweepsfile.NotFoundError):
            self.session.spectrum_at(1_000_000_000, 99)

    def test_verify(self):
        # Every record intact, which is a stronger statement than "it parsed".
        self.assertGreater(self.session.verify(), 0)

    def test_no_plugin_records(self):
        self.assertEqual(len(self.session.plugin_records), 0)

    def test_extract_round_trips(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = os.path.join(directory, "extract.sweeps")
            # A time range, not a segment filter: the golden session's first
            # segment ends around 1.8 s, so this window leaves one behind.
            self.session.extract(
                destination,
                from_ns=3_000_000_000,
                to_ns=8_000_000_000,
                application_version="py-extract",
                created_wall_ns=CREATED_WALL_NS,
            )

            with sweepsfile.open(destination) as extracted:
                self.assertEqual(extracted.app_version, "py-extract")
                self.assertEqual(extracted.summary.created_wall_ns, CREATED_WALL_NS)
                self.assertEqual(len(extracted.segments), 1)
                self.assertGreater(extracted.verify(), 0)

    def test_extract_refuses_query_only_bounds(self):
        # An extraction copies whole tiles or none, so the caps and the segment
        # filter have nothing to say. Refused loudly rather than ignored, which
        # is how you get a file that is not the range you asked for.
        with tempfile.TemporaryDirectory() as directory:
            for bad in ("segment_id", "lod", "max_lines", "max_bins"):
                with self.assertRaises(TypeError):
                    self.session.extract(os.path.join(directory, "x.sweeps"), **{bad: 1})


class TestErrors(unittest.TestCase):
    def test_missing_file(self):
        with self.assertRaises(sweepsfile.SweepsError) as caught:
            sweepsfile.open("/definitely/not/here/session.sweeps")
        # The status says how to branch; the message says which file. Losing the
        # second is what makes an error useless to whoever has to fix it.
        self.assertIn("session.sweeps", str(caught.exception))
        self.assertNotEqual(caught.exception.status, sweepsfile.Status.OK)

    def test_closed_reader_raises_rather_than_crashing(self):
        session = sweepsfile.open(GOLDEN)
        name = session.name
        session.close()
        session.close()  # idempotent

        self.assertEqual(name, "golden")  # the copy outlives the reader
        with self.assertRaises(sweepsfile.SweepsError):
            _ = session.name

    def test_values_outlive_their_reader(self):
        # The reason everything is copied at the boundary. In C these are views
        # into the mapping; a Python object holding one would be a
        # use-after-free with no traceback to explain it.
        with sweepsfile.open(GOLDEN) as session:
            manifest = session.manifest
            segments = session.segments
            events = list(session.events)
            tile = session.query(segment_id=1, lod=0)[0]

        self.assertEqual(manifest["name"], "golden")
        self.assertEqual(segments[0].reason, "session start")
        self.assertTrue(any(event.body for event in events))
        self.assertEqual(len(tile.data), tile.lines * tile.bins)

    def test_context_manager_closes(self):
        with sweepsfile.open(GOLDEN) as session:
            pass
        with self.assertRaises(sweepsfile.SweepsError):
            _ = session.summary


class TestWriter(unittest.TestCase):
    def _config(self):
        return AcquisitionConfig(
            center_hz=100e6,
            span_hz=2.5e6,
            sample_rate=2.5e6,
            fft_size=1024,
            rbw_hz=2.5e6 * 1.5 / 1024.0,
            device_id="py-0",
            device_label="Python device",
            gains={"lna": 24.0, "vga": 16.0},
        )

    def test_acquisition_config_defaults_are_the_librarys(self):
        # The trap the C API needs an _init function for: a zeroed config claims
        # a rectangular window and an ENBW of zero, and writes both into the
        # file as fact. A dataclass makes that impossible by accident.
        config = AcquisitionConfig()
        self.assertEqual(config.window, WindowType.HANN)
        self.assertEqual(config.window_beta, 8.6)
        self.assertEqual(config.window_enbw, 1.5)

    def test_round_trip(self):
        config = self._config()

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "python.sweeps")

            with sweepsfile.create(
                path,
                bins_per_line=256,
                session_name="python",
                notes="written from python",
                application_version="py-test",
                created_wall_ns=CREATED_WALL_NS,
                min_free_bytes=0,
            ) as writer:
                bins = [-95.0] * 256
                for line in range(300):
                    bins[line % 256] = -20.0
                    outcome = writer.write_frame(
                        bins,
                        config=config,
                        start_hz=98.75e6,
                        bin_width_hz=2.5e6 / 256.0,
                        monotonic_ns=1_000_000_000 + line * 20_000_000,
                        wall_ns=1_000_000_000 + line * 20_000_000,
                    )
                    if line == 0:
                        self.assertTrue(outcome.segment_opened)
                        self.assertTrue(outcome.reason)
                    bins[line % 256] = -95.0

                writer.record_marker(1_200_000_000, 1_200_000_000, "m1", 99.5e6, -41.5)
                writer.record_annotation(1_300_000_000, 1_300_000_000, "band", 99e6, 100e6)
                writer.record_retune(1_400_000_000, 1_400_000_000, 100e6, 3)
                writer.record_parameter_changed(
                    1_500_000_000, 1_500_000_000, "gain", "24", calibration_affecting=True
                )
                writer.record_sweep_pass(
                    1_600_000_000, 1_600_000_000, 7, 88e6, 108e6, 0.25
                )
                writer.record_segment_boundary(1_650_000_000, 1_650_000_000, "manual")
                writer.record_throttle_changed(1_700_000_000, 1_700_000_000, "cpu", 0.5)
                writer.record_device_error(
                    1_800_000_000, 1_800_000_000, "py-0", "overflow"
                )
                writer.record_plugin_event(
                    1_900_000_000,
                    1_900_000_000,
                    "org.sweeppp.py",
                    "hello",
                    {
                        "who": "python",
                        "count": 3,
                        "ratio": 0.5,
                        "ok": True,
                        "blob": b"\x01\x02",
                        "nested": {"leaf": "value"},
                    },
                )
                writer.write_plugin_data(
                    "org.sweeppp.py",
                    "state",
                    b"\x01\x02\x03\x04",
                    schema_version=2,
                    monotonic_ns=2_000_000_000,
                )

                self.assertEqual(writer.lines_written, 300)
                self.assertEqual(writer.segment_count, 1)
                self.assertGreater(writer.bytes_written, 0)
                self.assertFalse(writer.retention_reached)
                self.assertEqual(os.path.basename(writer.path), "python.sweeps")

            with sweepsfile.open(path) as session:
                self.assertEqual(session.name, "python")
                self.assertEqual(session.app_version, "py-test")
                self.assertEqual(session.summary.total_lines, 300)
                self.assertEqual(session.summary.created_wall_ns, CREATED_WALL_NS)
                self.assertGreater(session.verify(), 0)

                segment = session.segments[0]
                self.assertEqual(segment.device_label, "Python device")
                self.assertEqual(segment.gains, {"lna": 24.0, "vga": 16.0})
                self.assertEqual(segment.window, WindowType.HANN)

                kinds = {event.kind for event in session.events}
                self.assertEqual(
                    kinds,
                    {
                        EventKind.MARKER,
                        EventKind.ANNOTATION,
                        EventKind.RETUNE,
                        EventKind.PARAMETER_CHANGED,
                        EventKind.SWEEP_PASS,
                        EventKind.SEGMENT_BOUNDARY,
                        EventKind.THROTTLE_CHANGED,
                        EventKind.DEVICE_ERROR,
                        EventKind.PLUGIN,
                    },
                )

                plugin = next(
                    event for event in session.events if event.kind == EventKind.PLUGIN
                )
                fields = plugin.body["fields"]
                self.assertEqual(plugin.body["plugin_id"], "org.sweeppp.py")
                self.assertEqual(fields["who"], "python")
                self.assertEqual(fields["count"], 3)
                self.assertEqual(fields["ratio"], 0.5)
                self.assertEqual(fields["blob"], b"\x01\x02")
                self.assertEqual(fields["nested"], {"leaf": "value"})

                # bool before int, and it round-trips as a bool rather than as 1.
                self.assertIs(fields["ok"], True)
                self.assertIsInstance(fields["count"], int)
                self.assertNotIsInstance(fields["count"], bool)

                self.assertEqual(len(session.plugin_records), 1)
                record = session.plugin_records[0]
                self.assertEqual(record.plugin_id, "org.sweeppp.py")
                self.assertEqual(record.name, "state")
                self.assertEqual(record.schema_version, 2)
                self.assertEqual(record.body, b"\x01\x02\x03\x04")

    @unittest.skipIf(np is None, "numpy not installed")
    def test_accepts_numpy_frames(self):
        config = self._config()
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "numpy.sweeps")
            with sweepsfile.create(path, bins_per_line=64, min_free_bytes=0) as writer:
                # Non-contiguous and the wrong dtype on purpose: both are
                # normalised rather than silently reinterpreted.
                grid = np.linspace(-100.0, -20.0, 128, dtype=np.float64)[::2]
                self.assertFalse(grid.flags["C_CONTIGUOUS"])
                for line in range(8):
                    writer.write_frame(
                        grid,
                        config=config,
                        start_hz=98.75e6,
                        bin_width_hz=2.5e6 / 64.0,
                        monotonic_ns=1_000_000_000 + line * 20_000_000,
                    )

            with sweepsfile.open(path) as session:
                self.assertEqual(session.summary.total_lines, 8)
                self.assertGreater(session.verify(), 0)

    def test_rejects_raw_bytes_as_a_frame(self):
        config = self._config()
        with tempfile.TemporaryDirectory() as directory:
            with sweepsfile.create(
                os.path.join(directory, "x.sweeps"), min_free_bytes=0
            ) as writer:
                with self.assertRaises(TypeError):
                    writer.write_frame(
                        b"\x00" * 256,
                        config=config,
                        start_hz=0.0,
                        bin_width_hz=1.0,
                        monotonic_ns=1,
                    )

    def test_metadata_refuses_lists(self):
        # `Value::ofArray` takes its element type from the first element and
        # replaces anything else with that type's default, so a list would be
        # written having quietly lost data. Refused rather than coerced.
        with tempfile.TemporaryDirectory() as directory:
            with sweepsfile.create(
                os.path.join(directory, "x.sweeps"), min_free_bytes=0
            ) as writer:
                with self.assertRaises(sweepsfile.SweepsError):
                    writer.record_plugin_event(1, 1, "p", "e", {"values": [1, 2, 3]})
                with self.assertRaises(TypeError):
                    writer.record_plugin_event(1, 1, "p", "e", {"when": object()})

    def test_close_is_checked_and_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "closed.sweeps")
            writer = sweepsfile.create(path, bins_per_line=32, min_free_bytes=0)
            writer.close()
            writer.close()
            writer.destroy()
            writer.destroy()

            with sweepsfile.open(path) as session:
                self.assertEqual(session.summary.total_lines, 0)


if __name__ == "__main__":
    unittest.main()
