// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "SessionFixture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <fstream>
#include <set>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Metadata.hpp>
#include <sweeps/SessionReader.hpp>
#include <sweeps/SessionWriter.hpp>
#include <utility>
#include <vector>

using namespace sweeps;
using namespace sweeps::test;

TEST_CASE("quantisation round-trips within half a dB") {
    // 0.5 dB per step over a 127.5 dB window, which is far beyond these
    // radios' ~50-70 dB of usable dynamic range -- so quantisation is never
    // the limiting factor.
    constexpr double kOrigin = -140.0;

    for (double db = -139.5; db <= -13.0; db += 0.37) {
        CAPTURE(db);
        const std::uint8_t stored = quantiseDb(db, kOrigin);
        const double restored = dequantiseDb(stored, kOrigin);
        CHECK(std::abs(restored - db) <= kDbPerStep / 2.0 + 1e-9);
    }

    // Saturates rather than wrapping: a value outside the window must clamp,
    // never alias to the opposite end of the scale. The bottom of the scale is
    // byte 1, because 0 is reserved.
    CHECK(quantiseDb(-160.0, kOrigin) == 1);
    CHECK(quantiseDb(kOrigin, kOrigin) == 1);
    CHECK(quantiseDb(1000.0, kOrigin) == 255);
}

TEST_CASE("byte 0 says nothing was measured, not that it was quiet") {
    constexpr double kOrigin = -140.0;

    // The distinction the whole sentinel exists for: a gap between two swept
    // spans must not come back as a level at the bottom of the gradient.
    CHECK(quantiseDb(kUnmeasuredDb, kOrigin) == kUnmeasuredByte);
    CHECK(quantiseDb(-std::numeric_limits<double>::infinity(), kOrigin) == kUnmeasuredByte);
    CHECK(quantiseDb(std::numeric_limits<double>::quiet_NaN(), kOrigin) == kUnmeasuredByte);
    CHECK(dequantiseDb(kUnmeasuredByte, kOrigin) == kUnmeasuredDb);
    CHECK_FALSE(isMeasured(kUnmeasuredByte));

    // Round-trips as unmeasured whatever the origin, so a reader never has to
    // know which tile a byte came from to tell coverage from level.
    for (const double origin : {-200.0, -140.0, -40.0, 0.0}) {
        CAPTURE(origin);
        CHECK(quantiseDb(dequantiseDb(kUnmeasuredByte, origin), origin) == kUnmeasuredByte);
    }

    // Every measured byte stays measured, including the extremes.
    CHECK(isMeasured(1));
    CHECK(isMeasured(255));
    CHECK(isMeasuredDb(dequantiseDb(1, kOrigin)));
}

TEST_CASE("a gap between two swept spans is stored as unmeasured") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("gap.sweeps");

    // One line covering two spans with nothing measured between them, which is
    // exactly what a discontinuous sweep plan produces.
    constexpr std::size_t kBins = 256;
    constexpr std::size_t kGapFrom = 96;
    constexpr std::size_t kGapTo = 160;

    TestFrame frame = makeFrame(1'000'000, 100e6, 10e3, kBins, -70.0F);
    for (std::size_t i = kGapFrom; i < kGapTo; ++i) {
        frame.bins[i] = static_cast<float>(kUnmeasuredDb);
    }

    {
        WriterConfig config;
        config.binsPerLine = kBins;
        auto writer = SessionWriter::create(path, config);
        REQUIRE(writer.has_value());
        for (int line = 0; line < 4; ++line) {
            frame.monotonicNs = 1'000'000 + static_cast<std::uint64_t>(line) * 20'000'000;
            frame.wallNs = frame.monotonicNs;
            REQUIRE((*writer)->writeFrame(frame.view()).has_value());
        }
        REQUIRE((*writer)->close().has_value());
    }

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    HistoryQuery request;
    request.fromNs = 0;
    request.toNs = std::numeric_limits<std::uint64_t>::max();
    request.fromHz = 100e6;
    request.toHz = 100e6 + 10e3 * kBins;
    request.maxLines = 64;
    request.maxBins = kBins;

    auto tiles = (*reader)->query(request);
    REQUIRE(tiles.has_value());
    REQUIRE_FALSE(tiles->empty());

    const double gapFromHz = 100e6 + 10e3 * static_cast<double>(kGapFrom);
    const double gapToHz = 100e6 + 10e3 * static_cast<double>(kGapTo);

    std::size_t measuredBins = 0;
    std::size_t gapBins = 0;
    for (const HistoryTile& tile : *tiles) {
        for (std::uint32_t line = 0; line < tile.lines; ++line) {
            for (std::uint32_t bin = 0; bin < tile.bins; ++bin) {
                const double centreHz =
                    tile.startHz + tile.binWidthHz * (static_cast<double>(bin) + 0.5);
                if (centreHz >= gapFromHz && centreHz < gapToHz) {
                    CHECK_FALSE(isMeasured(tile.at(line, bin)));
                    ++gapBins;
                } else {
                    CHECK(isMeasured(tile.at(line, bin)));
                    ++measuredBins;
                }
            }
        }
    }
    CHECK(gapBins > 0);
    CHECK(measuredBins > 0);
}

TEST_CASE("a written session reopens with its metadata intact") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("session.sweeps");

    const WrittenSession written = writeSession(path, 600, false);

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    const SessionSummary& summary = (*reader)->summary();
    CHECK(summary.majorVersion == kMajorVersion);
    CHECK(summary.minorVersion == kMinorVersion);
    CHECK_FALSE(summary.recoveredByScan);
    CHECK(summary.totalLines == written.lineCount);
    CHECK(summary.totalTiles > 0);
    CHECK(summary.lowestHz == doctest::Approx(90e6));
    CHECK(summary.highestHz == doctest::Approx(110e6));

    REQUIRE((*reader)->segments().size() == 1);
    const SegmentInfo& segment = (*reader)->segments().front();
    CHECK(segment.grid.binCount == 2048);
    CHECK(segment.config.fftSize == 4096);
    CHECK(segment.config.window == WindowType::Hann);
    CHECK(segment.config.deviceLabel == "Test device");
}

TEST_CASE("a burst survives at every LOD level") {
    // The decisive property of the pyramid. Decimating with max-hold preserves
    // a transient; decimating with mean would erase it -- and finding
    // transients is the entire reason scrollback exists.
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("burst.sweeps");

    const WrittenSession written = writeSession(path, 2000, true);

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    for (std::uint32_t lod = 0; lod < kLodLevels; ++lod) {
        CAPTURE(lod);

        HistoryQuery request;
        request.fromNs = written.firstNs;
        request.toNs = written.lastNs;
        request.fromHz = 90e6;
        request.toHz = 110e6;
        request.lod = lod;

        auto tiles = (*reader)->query(request);
        REQUIRE(tiles.has_value());
        REQUIRE_FALSE(tiles->empty());

        float peak = -1000.0F;
        for (const HistoryTile& tile : *tiles) {
            for (std::uint32_t line = 0; line < tile.lines; ++line) {
                for (std::uint32_t bin = 0; bin < tile.bins; ++bin) {
                    peak = std::max(peak, static_cast<float>(tile.dbAt(line, bin)));
                }
            }
        }

        // The burst is 70 dB above the floor; it must still be there.
        CHECK(peak > -30.0F);
    }
}

TEST_CASE("a time query returns a contiguous, correctly-timestamped span") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("range.sweeps");

    const WrittenSession written = writeSession(path, 1000, false);

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    // "The last N seconds" is a query against the store.
    const std::uint64_t fromNs = written.lastNs - 2'000'000'000;

    HistoryQuery request;
    request.fromNs = fromNs;
    request.toNs = written.lastNs;
    request.maxLines = 4096;

    auto tiles = (*reader)->query(request);
    REQUIRE(tiles.has_value());
    REQUIRE_FALSE(tiles->empty());

    for (const HistoryTile& tile : *tiles) {
        // Every returned tile must actually overlap the range asked for.
        CHECK(tile.lastLineNs >= fromNs);
        CHECK(tile.firstLineNs <= written.lastNs);
        CHECK(tile.lines > 0);
        CHECK(tile.bins > 0);
        CHECK(tile.data.size() == static_cast<std::size_t>(tile.lines) * tile.bins);
    }
}

TEST_CASE("a coarser LOD is chosen for a long range") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("lod.sweeps");

    const WrittenSession written = writeSession(path, 3000, false);

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    // Asking for the whole session on a small budget must not read every line.
    const std::uint32_t coarse = (*reader)->chooseLod(written.firstNs, written.lastNs, 64, 0);
    const std::uint32_t fine =
        (*reader)->chooseLod(written.firstNs, written.firstNs + 100'000'000, 4096, 0);

    CHECK(coarse > fine);
    CHECK(fine == 0);
}

TEST_CASE("a mid-session parameter change opens a new segment and destroys nothing") {
    // The test that mid-run operator changes are harmless. A single global
    // grid would make every tile written before the change unreadable.
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("segments.sweeps");

    {
        WriterConfig config;
        config.binsPerLine = 1024;
        auto writer = SessionWriter::create(path, config);
        REQUIRE(writer.has_value());

        std::uint64_t timeNs = 1'000'000'000;
        const auto advance = [&timeNs] {
            timeNs += 20'000'000;
            return timeNs;
        };

        const auto phase = [&](int count, double startHz, float floorDb, std::uint32_t fftSize) {
            for (int i = 0; i < count; ++i) {
                const TestFrame frame =
                    makeFrame(advance(), startHz, 19531.25, 1024, floorDb,
                              static_cast<std::size_t>(-1), 0.0F, fftSize, 20e6);
                REQUIRE((*writer)->writeFrame(frame.view()).has_value());
            }
        };

        // Phase 1: 90-110 MHz, 4096-point FFT.
        phase(300, 90e6, -90.0F, 4096);
        // Phase 2: FFT size changed. Grid-affecting -> new segment.
        phase(300, 90e6, -85.0F, 8192);
        // Phase 3: frequency range changed too.
        phase(300, 400e6, -80.0F, 8192);

        REQUIRE((*writer)->close().has_value());
        CHECK((*writer)->segmentCount() == 3);
    }

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());
    REQUIRE((*reader)->segments().size() == 3);

    const std::vector<SegmentInfo>& segments = (*reader)->segments();

    // Each segment kept its own grid and its own configuration.
    CHECK(segments[0].config.fftSize == 4096);
    CHECK(segments[1].config.fftSize == 8192);
    CHECK(segments[2].config.fftSize == 8192);

    CHECK(segments[0].grid.startHz == doctest::Approx(90e6));
    CHECK(segments[1].grid.startHz == doctest::Approx(90e6));
    CHECK(segments[2].grid.startHz == doctest::Approx(400e6));

    // The reason is recorded, so a timeline can say what changed.
    CHECK(segments[0].reason == "session start");
    CHECK(segments[1].reason.find("FFT size") != std::string::npos);

    // Data written before each change is still readable at its original grid.
    for (const SegmentInfo& segment : segments) {
        CAPTURE(segment.id);
        HistoryQuery request;
        request.segmentId = segment.id;
        request.fromHz = segment.grid.startHz;
        request.toHz = segment.grid.stopHz();
        request.maxLines = 4096;

        auto tiles = (*reader)->query(request);
        REQUIRE(tiles.has_value());
        REQUIRE_FALSE(tiles->empty());
        for (const HistoryTile& tile : *tiles) {
            CHECK(tile.segmentId == segment.id);
            CHECK(tile.binWidthHz == doctest::Approx(segment.grid.binWidthHz));
        }
    }

    // A query spanning a change returns correctly-tagged tiles from both sides.
    HistoryQuery spanning;
    spanning.fromHz = 90e6;
    spanning.toHz = 420e6;
    spanning.maxLines = 4096;

    auto tiles = (*reader)->query(spanning);
    REQUIRE(tiles.has_value());

    std::set<std::uint32_t> segmentsSeen;
    for (const HistoryTile& tile : *tiles) {
        segmentsSeen.insert(tile.segmentId);
    }
    CHECK(segmentsSeen.size() >= 2);
}

TEST_CASE("extraction copies tiles byte-for-byte and stands alone") {
    const ScopedTempDir temp;
    const std::filesystem::path source = temp.file("source.sweeps");
    const std::filesystem::path extracted = temp.file("extract.sweeps");

    const WrittenSession written = writeSession(source, 1500, true);

    auto reader = SessionReader::open(source);
    REQUIRE(reader.has_value());

    HistoryQuery range;
    range.fromNs = written.firstNs;
    range.toNs = written.lastNs;
    range.fromHz = 90e6;
    range.toHz = 100e6;

    REQUIRE((*reader)->extract(extracted, range).has_value());
    REQUIRE(std::filesystem::exists(extracted));

    // Opens on its own, with no reference to the source.
    auto extractReader = SessionReader::open(extracted);
    REQUIRE(extractReader.has_value());
    CHECK_FALSE((*extractReader)->summary().recoveredByScan);
    REQUIRE_FALSE((*extractReader)->segments().empty());

    // Tiles are bit-identical: copied, never re-encoded or re-quantised.
    HistoryQuery full;
    full.fromNs = 0;
    full.toNs = std::numeric_limits<std::uint64_t>::max();
    full.maxLines = 100'000;

    auto sourceTiles = (*reader)->query(full);
    auto extractTiles = (*extractReader)->query(full);
    REQUIRE(sourceTiles.has_value());
    REQUIRE(extractTiles.has_value());
    REQUIRE_FALSE(extractTiles->empty());

    std::size_t compared = 0;
    for (const HistoryTile& extractedTile : *extractTiles) {
        const auto match = std::find_if(
            sourceTiles->begin(), sourceTiles->end(), [&](const HistoryTile& candidate) {
                return candidate.segmentId == extractedTile.segmentId &&
                       candidate.lod == extractedTile.lod &&
                       candidate.firstLineNs == extractedTile.firstLineNs &&
                       std::abs(candidate.startHz - extractedTile.startHz) < 1.0;
            });
        if (match == sourceTiles->end()) {
            continue;
        }

        CHECK(extractedTile.originDb == match->originDb);
        CHECK(extractedTile.lines == match->lines);
        CHECK(extractedTile.bins == match->bins);
        CHECK(extractedTile.data == match->data);
        ++compared;
    }
    CHECK(compared > 0);
}

TEST_CASE("extracting a later segment keeps every tile at its own frequencies") {
    // The latent bug this whole extraction exists to fix.
    //
    // Segment ids are preserved by an extract, not renumbered, so an extract
    // that excludes segment 0 contains segments whose ids start above zero. A
    // reader that indexes its segment vector by id -- treating id as a position
    // -- then attributes segment 2's grid to segment 1, and every tile in the
    // file is drawn at the wrong frequencies. Silently: nothing fails, the
    // numbers are simply wrong.
    const ScopedTempDir temp;
    const std::filesystem::path source = temp.file("multi.sweeps");
    const std::filesystem::path extracted = temp.file("tail.sweeps");

    constexpr double kSegment0Hz = 90e6;
    constexpr double kSegment1Hz = 400e6;
    constexpr double kSegment2Hz = 800e6;

    {
        WriterConfig config;
        config.binsPerLine = 512;
        auto writer = SessionWriter::create(source, config);
        REQUIRE(writer.has_value());

        std::uint64_t timeNs = 1'000'000'000;
        const auto phase = [&](int count, double startHz, std::uint32_t fftSize) {
            for (int i = 0; i < count; ++i) {
                timeNs += 20'000'000;
                const TestFrame frame =
                    makeFrame(timeNs, startHz, 19531.25, 512, -90.0F, static_cast<std::size_t>(-1),
                              0.0F, fftSize, 20e6);
                REQUIRE((*writer)->writeFrame(frame.view()).has_value());
            }
        };

        phase(300, kSegment0Hz, 4096);
        phase(300, kSegment1Hz, 8192);
        phase(300, kSegment2Hz, 16384);

        REQUIRE((*writer)->close().has_value());
        CHECK((*writer)->segmentCount() == 3);
    }

    auto reader = SessionReader::open(source);
    REQUIRE(reader.has_value());
    REQUIRE((*reader)->segments().size() == 3);

    // Everything from 400 MHz upward: segments 1 and 2, never segment 0.
    HistoryQuery range;
    range.fromNs = 0;
    range.toNs = std::numeric_limits<std::uint64_t>::max();
    range.fromHz = kSegment1Hz;
    range.toHz = std::numeric_limits<double>::max();

    REQUIRE((*reader)->extract(extracted, range).has_value());

    auto tail = SessionReader::open(extracted);
    REQUIRE(tail.has_value());

    const std::vector<SegmentInfo>& segments = (*tail)->segments();
    REQUIRE(segments.size() == 2);

    // The ids are preserved, so they do not match positions.
    CHECK(segments[0].id == 1);
    CHECK(segments[1].id == 2);
    CHECK(segments[0].grid.startHz == doctest::Approx(kSegment1Hz));
    CHECK(segments[1].grid.startHz == doctest::Approx(kSegment2Hz));

    // Lookup by id, not by position.
    REQUIRE((*tail)->segment(1) != nullptr);
    REQUIRE((*tail)->segment(2) != nullptr);
    CHECK((*tail)->segment(0) == nullptr);
    CHECK((*tail)->segment(2)->config.fftSize == 16384);

    // The payoff: every tile must carry the frequency of the segment that
    // actually produced it. Indexed by position, segment 1's tiles would come
    // back at 400 MHz reported as segment 2's grid, and segment 2's tiles would
    // fall off the end entirely.
    HistoryQuery full;
    full.fromNs = 0;
    full.toNs = std::numeric_limits<std::uint64_t>::max();
    full.maxLines = 100'000;

    auto tiles = (*tail)->query(full);
    REQUIRE(tiles.has_value());
    REQUIRE_FALSE(tiles->empty());

    std::set<std::uint32_t> seen;
    for (const HistoryTile& tile : *tiles) {
        CAPTURE(tile.segmentId);
        const SegmentInfo* owner = (*tail)->segment(tile.segmentId);
        REQUIRE(owner != nullptr);
        CHECK(tile.startHz == doctest::Approx(owner->grid.startHz));
        CHECK(tile.binWidthHz == doctest::Approx(owner->grid.binWidthHz));
        seen.insert(tile.segmentId);
    }
    CHECK(seen == std::set<std::uint32_t>{1, 2});

    // The reader's per-segment helpers take an id too.
    CHECK((*tail)->hasTilesAtLod(1, 0));
    CHECK((*tail)->hasTilesAtLod(2, 0));
    CHECK_FALSE((*tail)->hasTilesAtLod(0, 0));

    auto spectrum = (*tail)->spectrumAt(segments[1].startMonotonicNs + 1'000'000'000, 2);
    CHECK(spectrum.has_value());
}

TEST_CASE("a truncated file still opens, recovering every complete tile") {
    // Sessions end abruptly -- power loss, a kill. The index is written at
    // close, but tiles are self-describing, so a scan recovers everything that
    // reached the disk. This is a designed path, not a fallback.
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("truncated.sweeps");

    (void)writeSession(path, 1200, false);

    const auto originalSize = std::filesystem::file_size(path);

    std::uint64_t intactTiles = 0;
    {
        auto reader = SessionReader::open(path);
        REQUIRE(reader.has_value());
        intactTiles = (*reader)->summary().totalTiles;
        CHECK_FALSE((*reader)->summary().recoveredByScan);
    }

    // Chop off the index and part of the last tile.
    std::filesystem::resize_file(path, originalSize * 3 / 4);

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    const SessionSummary& summary = (*reader)->summary();
    CHECK(summary.recoveredByScan);
    CHECK(summary.totalTiles > 0);
    CHECK(summary.totalTiles < intactTiles);

    // The recovered tiles are usable, not merely counted.
    HistoryQuery request;
    request.maxLines = 4096;
    auto tiles = (*reader)->query(request);
    REQUIRE(tiles.has_value());
    REQUIRE_FALSE(tiles->empty());
    for (const HistoryTile& tile : *tiles) {
        CHECK(tile.data.size() == static_cast<std::size_t>(tile.lines) * tile.bins);
    }
}

TEST_CASE("a file with corrupt bytes stops at the damage rather than misreading it") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("corrupt.sweeps");
    (void)writeSession(path, 800, false);

    // Flip bytes in the middle of the tile stream. The per-record checksum is
    // what stops a partially written record from being read as a good one.
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file);
        const auto size = std::filesystem::file_size(path);
        file.seekp(static_cast<std::streamoff>(size / 2));
        const std::array<char, 64> garbage{};
        file.write(garbage.data(), garbage.size());
    }

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    // The index survived, so the reader trusts it -- but every tile is
    // checksum-verified on read. A damaged tile is refused rather than handed
    // back as data and drawn as if it were a measurement.
    HistoryQuery request;
    request.maxLines = 100'000;
    auto tiles = (*reader)->query(request);
    REQUIRE(tiles.has_value());

    // Some tiles were destroyed, so fewer come back than the index lists.
    CHECK(tiles->size() < (*reader)->summary().totalTiles);
    for (const HistoryTile& tile : *tiles) {
        CHECK(tile.data.size() == static_cast<std::size_t>(tile.lines) * tile.bins);
    }

    // And verify() says so plainly rather than reporting a clean file.
    CHECK_FALSE((*reader)->verify().has_value());
}

TEST_CASE("verify accepts an intact file and counts its records") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("intact.sweeps");
    (void)writeSession(path, 400, false);

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    auto records = (*reader)->verify();
    REQUIRE(records.has_value());
    // Manifest, segment open, tiles, segment close, index, end-of-stream.
    CHECK(*records >= 5);
}

TEST_CASE("retention caps bound file growth") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("capped.sweeps");

    WriterConfig config;
    config.binsPerLine = 2048;
    config.maxBytes = 128 * 1024;

    auto writer = SessionWriter::create(path, config);
    REQUIRE(writer.has_value());

    for (std::uint32_t i = 0; i < 5000; ++i) {
        const TestFrame frame =
            makeFrame(1'000'000'000 + i * 20'000'000, 90e6, 9765.625, 2048, -90.0F);
        REQUIRE((*writer)->writeFrame(frame.view()).has_value());
    }
    REQUIRE((*writer)->close().has_value());

    CHECK((*writer)->retentionReached());
    CHECK_FALSE((*writer)->retentionReason().empty());

    // The cap counts buffered tiles as well as written bytes, so the overshoot
    // is bounded by one tile flush plus the index -- not by however much
    // happened to be sitting in memory.
    const std::uint64_t tileSetBytes =
        static_cast<std::uint64_t>(kTileLines) * config.binsPerLine * kLodLevels;
    CHECK(std::filesystem::file_size(path) < config.maxBytes + tileSetBytes);
}

TEST_CASE("events are recorded and returned in a time range") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("events.sweeps");

    {
        auto writer = SessionWriter::create(path);
        REQUIRE(writer.has_value());

        for (std::uint32_t i = 0; i < 100; ++i) {
            const std::uint64_t timeNs = 1'000'000'000 + i * 20'000'000;
            const TestFrame frame = makeFrame(timeNs, 90e6, 9765.625, 2048, -90.0F);
            REQUIRE((*writer)->writeFrame(frame.view()).has_value());

            if (i == 50) {
                ParameterChangedData body;
                body.key = "lna_gain";
                body.value = "24";
                body.gridAffecting = false;
                body.calibrationAffecting = true;
                REQUIRE((*writer)
                            ->recordEvent(SessionEvent::of(SessionEvent::Kind::ParameterChanged,
                                                           timeNs, timeNs, std::move(body)))
                            .has_value());
            }
        }
        REQUIRE((*writer)->close().has_value());
    }

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    const std::vector<SessionEvent>& events = (*reader)->events();
    const auto gainChange =
        std::find_if(events.begin(), events.end(), [](const SessionEvent& event) {
            const auto* body = event.as<ParameterChangedData>();
            return body != nullptr && body->key == "lna_gain";
        });
    REQUIRE(gainChange != events.end());
    CHECK(gainChange->kindEnum() == SessionEvent::Kind::ParameterChanged);

    const ParameterChangedData* body = gainChange->as<ParameterChangedData>();
    REQUIRE(body != nullptr);
    CHECK(body->value == "24");
    // Both flags travel independently. Calibration-affecting means the noise
    // floor moved and later analysis of these tiles must know; grid-affecting
    // means the segment closed. Neither implies the other.
    CHECK(body->calibrationAffecting);
    CHECK_FALSE(body->gridAffecting);

    const std::vector<SessionEvent> inRange =
        (*reader)->eventsInRange(gainChange->monotonicNs - 1, gainChange->monotonicNs + 1);
    CHECK_FALSE(inRange.empty());
}

TEST_CASE("every event kind round-trips through its own payload") {
    // The event id determines the payload, exactly as a record type does. What
    // this pins is that every field of every kind survives the round trip --
    // each body has a different shape, so there is nowhere for a dropped field
    // to hide.
    std::vector<SessionEvent> written;

    RetuneData retune;
    retune.centerHz = 433.92e6;
    retune.stepIndex = 7;
    written.push_back(SessionEvent::of(SessionEvent::Kind::Retune, 1, 2, retune));

    ParameterChangedData parameter;
    parameter.key = "fft_size";
    parameter.value = "8192";
    parameter.gridAffecting = true;
    parameter.calibrationAffecting = false;
    written.push_back(SessionEvent::of(SessionEvent::Kind::ParameterChanged, 3, 4, parameter));

    SweepPassData pass;
    pass.passId = 99;
    pass.startHz = 88e6;
    pass.stopHz = 108e6;
    pass.durationSeconds = 0.25;
    written.push_back(SessionEvent::of(SessionEvent::Kind::SweepPass, 5, 6, pass));

    MarkerData marker;
    marker.label = "m1";
    marker.frequencyHz = 100.1e6;
    marker.levelDbm = -42.25;
    written.push_back(SessionEvent::of(SessionEvent::Kind::Marker, 7, 8, marker));

    AnnotationData annotation;
    annotation.text = "birdie";
    annotation.startHz = 1e6;
    annotation.stopHz = 2e6;
    written.push_back(SessionEvent::of(SessionEvent::Kind::Annotation, 9, 10, annotation));

    SegmentBoundaryData boundary;
    boundary.reason = "FFT size 4096 -> 8192";
    written.push_back(SessionEvent::of(SessionEvent::Kind::SegmentBoundary, 11, 12, boundary));

    ThrottleChangedData throttle;
    throttle.reason = "cpu";
    throttle.processedFraction = 0.375;
    written.push_back(SessionEvent::of(SessionEvent::Kind::ThrottleChanged, 13, 14, throttle));

    DeviceErrorData deviceError;
    deviceError.deviceId = "hackrf-0";
    deviceError.message = "usb timeout";
    written.push_back(SessionEvent::of(SessionEvent::Kind::DeviceError, 15, 16, deviceError));

    for (const SessionEvent& event : written) {
        CAPTURE(eventKindName(event.kind));

        std::vector<std::byte> payload;
        encodeEvent(payload, event);

        ByteReader reader(payload.data(), payload.size());
        auto decoded = decodeEvent(reader);
        REQUIRE(decoded.has_value());

        CHECK(decoded->kind == event.kind);
        CHECK(decoded->monotonicNs == event.monotonicNs);
        CHECK(decoded->wallNs == event.wallNs);
        CHECK(decoded->body.index() == event.body.index());
    }

    const RetuneData* decodedRetune = nullptr;
    {
        std::vector<std::byte> payload;
        encodeEvent(payload, written.front());
        ByteReader reader(payload.data(), payload.size());
        auto decoded = decodeEvent(reader);
        REQUIRE(decoded.has_value());
        decodedRetune = decoded->as<RetuneData>();
        REQUIRE(decodedRetune != nullptr);
        // A u32 step index, not a double that happens to hold an integer.
        CHECK(decodedRetune->stepIndex == 7);
        CHECK(decodedRetune->centerHz == doctest::Approx(433.92e6));
    }
}

TEST_CASE("a plugin event carries a nested hash of its own fields") {
    Metadata band;
    band.setString("name", "70cm");
    band.setFloat("start_hz", 430e6);
    band.setFloat("stop_hz", 440e6);

    Metadata fields;
    fields.setInt("matches", 2);
    fields.setHash("band", band);
    fields.setArray("levels", {Value::ofFloat(-88.5), Value::ofFloat(-31.0)});

    PluginEventData body;
    body.pluginId = "org.sweeppp.bandplan";
    body.eventName = "band-entered";
    body.fields = fields;

    std::vector<std::byte> payload;
    encodeEvent(payload, SessionEvent::of(SessionEvent::Kind::Plugin, 100, 200, body));

    ByteReader reader(payload.data(), payload.size());
    auto decoded = decodeEvent(reader);
    REQUIRE(decoded.has_value());

    const PluginEventData* data = decoded->as<PluginEventData>();
    REQUIRE(data != nullptr);
    CHECK(data->pluginId == "org.sweeppp.bandplan");
    CHECK(data->eventName == "band-entered");
    CHECK(data->fields.getInt("matches") == 2);
    REQUIRE(data->fields.find("band") != nullptr);
    REQUIRE(data->fields.find("band")->asHash() != nullptr);
    CHECK(data->fields.find("band")->asHash()->getString("name") == "70cm");
    REQUIRE(data->fields.find("levels") != nullptr);
    REQUIRE(data->fields.find("levels")->asArray() != nullptr);
    CHECK(data->fields.find("levels")->asArray()->size() == 2);
}

TEST_CASE("an Alert event is reserved: it keeps its bytes and invents no body") {
    // Kind 6 is defined and written by nothing. Decoding it as some neighbouring
    // shape would be a guess; keeping the bytes is the honest answer.
    std::vector<std::byte> payload;
    writeU16(payload, static_cast<std::uint16_t>(SessionEvent::Kind::Alert));
    writeU64(payload, 42);
    writeU64(payload, 43);
    writeU32(payload, 1);
    writeU32(payload, 0xDEAD'BEEF);

    ByteReader reader(payload.data(), payload.size());
    auto decoded = decodeEvent(reader);
    REQUIRE(decoded.has_value());
    CHECK(decoded->kindEnum() == SessionEvent::Kind::Alert);
    CHECK(eventKindName(decoded->kind) == "alert");

    const UnknownEventData* body = decoded->as<UnknownEventData>();
    REQUIRE(body != nullptr);
    CHECK(body->body.size() == 4);

    std::vector<std::byte> reencoded;
    encodeEvent(reencoded, *decoded);
    CHECK(reencoded == payload);
}

TEST_CASE("plugin records round-trip and survive an extraction") {
    const ScopedTempDir temp;
    const std::filesystem::path source = temp.file("plugins.sweeps");
    const std::filesystem::path extracted = temp.file("plugins-cut.sweeps");

    const std::vector<std::byte> insideBody{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    const std::vector<std::byte> outsideBody{std::byte{0xAA}};
    const std::vector<std::byte> untimedBody{std::byte{0xFF}, std::byte{0xEE}};

    constexpr std::uint64_t kFirstNs = 1'000'000'000;
    constexpr std::uint64_t kIntervalNs = 20'000'000;
    constexpr std::uint32_t kLines = 400;

    {
        auto writer = SessionWriter::create(source);
        REQUIRE(writer.has_value());

        for (std::uint32_t i = 0; i < kLines; ++i) {
            const TestFrame frame =
                makeFrame(kFirstNs + i * kIntervalNs, 90e6, 9765.625, 2048, -90.0F);
            REQUIRE((*writer)->writeFrame(frame.view()).has_value());
        }

        REQUIRE((*writer)
                    ->writePluginData("org.sweeppp.bandplan", "matches", 3, kFirstNs + kIntervalNs,
                                      insideBody.data(), insideBody.size())
                    .has_value());
        REQUIRE((*writer)
                    ->writePluginData("org.sweeppp.bandplan", "matches", 3,
                                      kFirstNs + (kLines - 1) * kIntervalNs, outsideBody.data(),
                                      outsideBody.size())
                    .has_value());
        REQUIRE((*writer)
                    ->writePluginData("org.sweeppp.other", "settings", 1, 0, untimedBody.data(),
                                      untimedBody.size())
                    .has_value());

        Metadata fields;
        fields.setString("band", "70cm");
        REQUIRE((*writer)
                    ->writePluginEvent("org.sweeppp.bandplan", "band-entered",
                                       kFirstNs + kIntervalNs, kFirstNs + kIntervalNs, fields)
                    .has_value());

        REQUIRE((*writer)->close().has_value());
    }

    auto reader = SessionReader::open(source);
    REQUIRE(reader.has_value());

    const std::vector<PluginRecord>& records = (*reader)->pluginRecords();
    REQUIRE(records.size() == 3);
    CHECK(records[0].pluginId == "org.sweeppp.bandplan");
    CHECK(records[0].recordName == "matches");
    CHECK(records[0].schemaVersion == 3);
    CHECK(records[0].monotonicNs == kFirstNs + kIntervalNs);
    REQUIRE(records[0].bodyBytes == insideBody.size());
    CHECK(std::vector<std::byte>(records[0].body, records[0].body + records[0].bodyBytes) ==
          insideBody);
    CHECK(records[2].monotonicNs == 0);

    const auto pluginEvent = std::find_if(
        (*reader)->events().begin(), (*reader)->events().end(),
        [](const SessionEvent& event) { return event.kindEnum() == SessionEvent::Kind::Plugin; });
    REQUIRE(pluginEvent != (*reader)->events().end());
    REQUIRE(pluginEvent->as<PluginEventData>() != nullptr);
    CHECK(pluginEvent->as<PluginEventData>()->fields.getString("band") == "70cm");

    // Only the first quarter of the session, so the second record's timestamp
    // falls outside it and the untimed one has no timestamp to fall outside of.
    HistoryQuery range;
    range.fromNs = kFirstNs;
    range.toNs = kFirstNs + kLines / 4 * kIntervalNs;
    REQUIRE((*reader)->extract(extracted, range).has_value());

    auto cut = SessionReader::open(extracted);
    REQUIRE(cut.has_value());

    const std::vector<PluginRecord>& copied = (*cut)->pluginRecords();
    REQUIRE(copied.size() == 2);
    CHECK(copied[0].monotonicNs == kFirstNs + kIntervalNs);
    CHECK(std::vector<std::byte>(copied[0].body, copied[0].body + copied[0].bodyBytes) ==
          insideBody);
    // The untimed record is not tied to a moment, so no range excludes it.
    CHECK(copied[1].pluginId == "org.sweeppp.other");
    CHECK(copied[1].monotonicNs == 0);
}

TEST_CASE("a plugin id is bounded, because it is a length-prefixed string from a file") {
    const ScopedTempDir temp;
    auto writer = SessionWriter::create(temp.file("bad-plugin.sweeps"));
    REQUIRE(writer.has_value());

    const std::byte body{0x01};
    CHECK_FALSE((*writer)->writePluginData("", "r", 1, 0, &body, 1).has_value());
    CHECK_FALSE((*writer)
                    ->writePluginData(std::string(kMaxPluginIdBytes + 1, 'a'), "r", 1, 0, &body, 1)
                    .has_value());
    CHECK((*writer)
              ->writePluginData(std::string(kMaxPluginIdBytes, 'a'), "r", 1, 0, &body, 1)
              .has_value());

    Metadata fields;
    CHECK_FALSE((*writer)->writePluginEvent("", "e", 1, 1, fields).has_value());
}

TEST_CASE("spectrumAt reconstructs one instant") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("instant.sweeps");
    const WrittenSession written = writeSession(path, 600, true);

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    // Clicking a waterfall line shows that instant in a linked plot.
    auto spectrum = (*reader)->spectrumAt(written.burstNs, 0);
    REQUIRE(spectrum.has_value());
    CHECK(spectrum->size() == 2048);

    const auto peak = std::max_element(spectrum->begin(), spectrum->end());
    REQUIRE(peak != spectrum->end());
    CHECK(*peak > -40.0F);
}

TEST_CASE("opening a non-session file fails clearly") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("not-a-session.bin");

    {
        std::ofstream out(path, std::ios::binary);
        const std::string junk(1024, 'x');
        out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }

    auto reader = SessionReader::open(path);
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().code() == ErrorCode::Corrupt);
    CHECK(reader.error().message().find("magic") != std::string::npos);
}

TEST_CASE("CRC32 matches the known IEEE check value") {
    // "123456789" -> 0xCBF43926 is the standard CRC-32/ISO-HDLC check value.
    const std::string_view input = "123456789";
    CHECK(crc32(input.data(), input.size()) == 0xCBF4'3926U);
    // And a CRC over nothing is zero, which is what an empty record carries.
    CHECK(crc32(nullptr, 0) == 0U);
}

TEST_CASE("byte encoding round-trips every scalar type") {
    std::vector<std::byte> buffer;
    writeU16(buffer, 0xBEEF);
    writeU32(buffer, 0xDEAD'BEEF);
    writeU64(buffer, 0x0123'4567'89AB'CDEFULL);
    writeF32(buffer, 3.5F);
    writeF64(buffer, -2.718281828459045);
    writeString(buffer, "hello");

    ByteReader reader(buffer.data(), buffer.size());
    CHECK(reader.readU16().value() == 0xBEEF);
    CHECK(reader.readU32().value() == 0xDEAD'BEEF);
    CHECK(reader.readU64().value() == 0x0123'4567'89AB'CDEFULL);
    CHECK(reader.readF32().value() == doctest::Approx(3.5F));
    CHECK(reader.readF64().value() == doctest::Approx(-2.718281828459045));
    CHECK(reader.readString().value() == "hello");
    CHECK(reader.exhausted());

    // Reading past the end is an error, never a wild read -- these bytes may
    // come from a truncated file or a hostile peer.
    CHECK_FALSE(reader.readU32().has_value());
}

TEST_CASE("the little-endian encoding matches the specification's test vectors") {
    // Appendix A.2. An independent implementation is checked against these, so
    // they are part of the contract rather than an internal detail.
    const auto bytes = [](const std::vector<std::byte>& buffer) {
        std::vector<unsigned> out;
        out.reserve(buffer.size());
        for (const std::byte value : buffer) {
            out.push_back(static_cast<unsigned>(value));
        }
        return out;
    };

    std::vector<std::byte> buffer;
    writeU16(buffer, 0xBEEF);
    CHECK(bytes(buffer) == std::vector<unsigned>{0xEF, 0xBE});

    buffer.clear();
    writeU32(buffer, 0xDEAD'BEEF);
    CHECK(bytes(buffer) == std::vector<unsigned>{0xEF, 0xBE, 0xAD, 0xDE});

    buffer.clear();
    writeF32(buffer, 3.5F);
    CHECK(bytes(buffer) == std::vector<unsigned>{0x00, 0x00, 0x60, 0x40});

    buffer.clear();
    writeString(buffer, "hello");
    CHECK(bytes(buffer) ==
          std::vector<unsigned>{0x05, 0x00, 0x00, 0x00, 0x68, 0x65, 0x6C, 0x6C, 0x6F});

    buffer.clear();
    writeString(buffer, "");
    CHECK(bytes(buffer) == std::vector<unsigned>{0x00, 0x00, 0x00, 0x00});
}

TEST_CASE("a hostile string length is rejected rather than allocated") {
    std::vector<std::byte> buffer;
    writeU32(buffer, 0xFFFF'FFFFU); // claims 4 GB of string in a 4-byte buffer

    ByteReader reader(buffer.data(), buffer.size());
    const auto result = reader.readString();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code() == ErrorCode::Corrupt);
}

TEST_CASE("a mid-session frequency change with oversized frames does not corrupt the heap") {
    // Reproduces what a live re-plan does: frames far wider than the stored
    // grid, a segment boundary partway through, and enough lines to push
    // several LOD levels through their decimation and flush tiles across the
    // change. Run under a sanitiser this is the test that would catch a stale
    // buffer sized for the previous grid.
    const ScopedTempDir directory;
    const std::filesystem::path path = directory.file("regrid.sweeps");

    WriterConfig config;
    config.sessionName = "regrid";
    config.binsPerLine = 2048;

    auto writer = SessionWriter::create(path, config);
    REQUIRE(writer.has_value());

    const auto feed = [&writer](double startHz, double binWidthHz, std::size_t bins,
                                std::uint64_t baseNs, int count) {
        TestFrame frame;
        frame.startHz = startHz;
        frame.binWidthHz = binWidthHz;
        frame.config.centerHz = startHz + binWidthHz * static_cast<double>(bins) * 0.5;
        frame.config.sampleRate = binWidthHz * static_cast<double>(bins);
        frame.config.spanHz = frame.config.sampleRate;

        for (int line = 0; line < count; ++line) {
            frame.bins.assign(bins, -90.0F);
            // A narrow carrier, so max-hold decimation has something to carry.
            frame.bins[bins / 3] = -20.0F;
            frame.monotonicNs = baseNs + static_cast<std::uint64_t>(line) * 1'000'000ULL;
            frame.wallNs = frame.monotonicNs;
            REQUIRE((*writer)->writeFrame(frame.view()).has_value());
        }
    };

    // Grid one: 79302 bins, exactly what a 650 Hz RBW sweep over 17 MHz emits.
    feed(414.4e6, 8404.0 / 40.0, 79302, 1'000'000'000ULL, 300);

    // Grid two: different start, different width, different bin count. This is
    // the segment boundary.
    feed(420.2e6, 6985.0 / 40.0, 33043, 2'000'000'000ULL, 300);

    // And back to something wider again, so a later segment is larger than the
    // one before it rather than only smaller.
    feed(410.0e6, 16760.0 / 40.0, 120000, 3'000'000'000ULL, 300);

    REQUIRE((*writer)->close().has_value());

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());
    CHECK((*reader)->segments().size() >= 3);
    CHECK((*reader)->summary().totalLines > 0);
}
