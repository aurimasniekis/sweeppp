// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "Commands.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <ostream>
#include <sstream>
#include <sweeps/Clock.hpp>
#include <sweeps/Config.hpp>
#include <sweeps/Metadata.hpp>
#include <sweeps/Text.hpp>
#include <sweeps/WindowType.hpp>
#include <vector>

namespace fs = std::filesystem;

namespace sweeps::cli {
namespace {

/// Opens a session, reporting the failure on `err` in the shape every command
/// uses.
///
/// The reader is given a log sink rather than the silent default, because
/// several of the things it has to say -- a damaged index, a truncated tail, an
/// unreadable manifest -- are the reason the output looks the way it does. A
/// blank session name with no explanation is the failure mode that matters here.
std::unique_ptr<SessionReader> openOrReport(const fs::path& path, std::ostream& err) {
    Log log([&err](LogLevel level, std::string_view category, std::string message) {
        if (level >= LogLevel::Warn) {
            err << "sweeps: " << category << ": " << message << '\n';
        }
    });

    auto reader = SessionReader::open(path, std::move(log));
    if (!reader) {
        err << "sweeps: " << reader.error().describe() << '\n';
        return nullptr;
    }
    return std::move(*reader);
}

/// A stream that never depends on the ambient locale, so a comma-decimal
/// system cannot turn a CSV into something no parser accepts.
void useClassicLocale(std::ostream& out) {
    out.imbue(std::locale::classic());
}

/// One event as a JSON object, which is what makes the stream `jq`-able.
Metadata eventObject(const SessionEvent& event) {
    Metadata object;
    object.setInt("monotonic_ns", static_cast<std::int64_t>(event.monotonicNs));
    object.setInt("wall_ns", static_cast<std::int64_t>(event.wallNs));
    object.setInt("segment", event.segmentId);
    object.setString("kind", std::string(eventKindName(event.kind)));
    // The raw id as well as the name: an unrecognised kind renders as "unknown",
    // and the number is the only thing that says *which* unknown.
    object.setInt("kind_id", event.kind);
    object.setHash("body", eventBodyMetadata(event));
    return object;
}

std::string offsetLabel(std::uint64_t eventNs, std::uint64_t referenceNs) {
    // Signed: some events -- the first throttle transition, the segment
    // opening -- are stamped before the first stored line, and an unsigned
    // subtraction would print them as about five million hours.
    const double seconds = (static_cast<double>(eventNs) - static_cast<double>(referenceNs)) * 1e-9;
    return (seconds < 0.0 ? "-" : "+") + formatDuration(std::abs(seconds));
}

} // namespace

int runInfo(const InfoOptions& options, std::ostream& out, std::ostream& err) {
    const std::unique_ptr<SessionReader> reader = openOrReport(options.path, err);
    if (!reader) {
        return 1;
    }

    const SessionSummary& summary = reader->summary();

    out << options.path.string() << '\n';
    out << "  name              " << summary.name << '\n';
    out << "  written by        " << summary.appVersion << '\n';
    // Major *and* minor: reporting only the major would hide exactly what the
    // minor version exists to communicate -- that a file may carry additive
    // content this build does not know about.
    out << "  format version    " << summary.versionString()
        << (summary.newerMinorVersion ? " (newer than this build; some content skipped)" : "")
        << '\n';
    out << "  created           " << formatWallClockIso8601(summary.createdWallNs) << '\n';
    out << "  size              " << formatBytes(summary.fileBytes) << '\n';
    out << "  duration          " << formatDuration(summary.durationSeconds()) << '\n';
    out << "  lines             " << summary.totalLines << '\n';
    out << "  tiles             " << summary.totalTiles << '\n';
    out << "  frequency         " << formatFrequencyShort(summary.lowestHz) << " .. "
        << formatFrequencyShort(summary.highestHz) << '\n';

    if (summary.recoveredByScan) {
        // Not hidden: an operator should know the session ended abruptly, and
        // that recovery is the reason it opened at all.
        out << "  recovery          index missing or damaged; " << summary.totalTiles
            << " recovered by scan";
        if (summary.truncatedBytes > 0) {
            out << " (" << summary.truncatedBytes << " bytes truncated)";
        }
        out << '\n';
    }

    out << "\nSegments (" << reader->segments().size() << ")\n";
    for (const SegmentInfo& segment : reader->segments()) {
        out << "  [" << segment.id << "] " << formatFrequencyShort(segment.grid.startHz) << " .. "
            << formatFrequencyShort(segment.grid.stopHz()) << " | " << segment.grid.binCount
            << " bins of " << formatFrequencyShort(segment.grid.binWidthHz) << " | "
            << segment.lineCount << " lines | " << segment.reason << '\n';
        out << "       FFT " << segment.config.fftSize << ' ' << toString(segment.config.window)
            << " | RBW " << formatFrequencyShort(segment.config.rbwHz) << " | rate "
            << formatFrequencyShort(segment.config.sampleRate) << '\n';
    }

    const std::vector<SessionEvent>& events = reader->events();
    out << "\nEvents (" << events.size() << ")\n";

    const std::size_t shown =
        options.maxEvents == 0 ? events.size() : std::min(events.size(), options.maxEvents);
    for (std::size_t i = 0; i < shown; ++i) {
        const SessionEvent& event = events[i];
        out << "  " << std::setw(11) << std::right
            << offsetLabel(event.monotonicNs, summary.firstLineNs) << ' ' << std::setw(12)
            << std::left << eventKindName(event.kind) << ' ' << eventBodyMetadata(event).toJson(0)
            << '\n';
        out << std::right;
    }
    if (shown < events.size()) {
        out << "  ... and " << events.size() - shown << " more\n";
    }

    return 0;
}

int runVerify(const VerifyOptions& options, std::ostream& out, std::ostream& err) {
    const std::unique_ptr<SessionReader> reader = openOrReport(options.path, err);
    if (!reader) {
        return 1;
    }

    auto records = reader->verify();
    if (!records) {
        err << "sweeps: " << options.path.string() << ": " << records.error().describe() << '\n';
        return 1;
    }

    const SessionSummary& summary = reader->summary();
    if (!options.quiet) {
        out << options.path.string() << ": " << *records << " records, " << summary.totalTiles
            << " tiles, all checksums good\n";
        if (summary.recoveredByScan) {
            // Every record present is intact, but the file still ended without
            // an index. Saying so is the difference between "verified" and
            // "verified, and also incomplete".
            out << "  note: no usable index; the session was not closed cleanly";
            if (summary.truncatedBytes > 0) {
                out << " (" << summary.truncatedBytes << " bytes truncated)";
            }
            out << '\n';
        }
    }
    return 0;
}

int runExtract(const ExtractOptions& options, std::ostream& out, std::ostream& err) {
    if (options.output.empty()) {
        err << "sweeps: extract needs an output path\n";
        return 1;
    }

    const std::unique_ptr<SessionReader> reader = openOrReport(options.input, err);
    if (!reader) {
        return 1;
    }

    const SessionSummary& summary = reader->summary();

    HistoryQuery range;
    range.fromNs = summary.firstLineNs + secondsToNs(options.fromSeconds);
    range.toNs = options.toSeconds > 0.0 ? summary.firstLineNs + secondsToNs(options.toSeconds)
                                         : summary.lastLineNs;
    range.fromHz = options.startHz > 0.0 ? options.startHz : summary.lowestHz;
    range.toHz = options.stopHz > 0.0 ? options.stopHz : summary.highestHz;

    sweeps::ExtractOptions writeOptions;
    writeOptions.applicationVersion = options.applicationVersion;

    if (auto extracted = reader->extract(options.output, range, writeOptions); !extracted) {
        err << "sweeps: " << extracted.error().describe() << '\n';
        return 1;
    }

    out << "extracted " << formatFrequencyShort(range.fromHz) << " .. "
        << formatFrequencyShort(range.toHz) << " over " << formatDuration(options.fromSeconds)
        << " .. "
        << formatDuration(options.toSeconds > 0.0 ? options.toSeconds : summary.durationSeconds())
        << " to " << options.output.string() << '\n';
    // Extraction granularity is the tile, so a range that clips a tile keeps
    // the whole tile. Saying so keeps the output from being read as an exact
    // cut.
    out << "note: the unit of extraction is a tile, so the result may cover slightly "
           "more than requested\n";
    return 0;
}

int runEvents(const EventsOptions& options, std::ostream& out, std::ostream& err) {
    const std::unique_ptr<SessionReader> reader = openOrReport(options.path, err);
    if (!reader) {
        return 1;
    }

    if (!options.kind.empty()) {
        if (auto parsed = eventKindFromString(options.kind); !parsed) {
            err << "sweeps: " << parsed.error().describe() << '\n';
            return 1;
        }
    }

    // JSON Lines, one object per event. Bodies are heterogeneous now, so no
    // fixed CSV header can describe them -- a column set wide enough for every
    // kind would be mostly empty on every row, which is the shape the flat event
    // record had and the reason it was replaced.
    for (const SessionEvent& event : reader->events()) {
        if (!options.kind.empty() && eventKindName(event.kind) != options.kind) {
            continue;
        }
        out << eventObject(event).toJson(0) << '\n';
    }

    return 0;
}

int runManifest(const ManifestOptions& options, std::ostream& out, std::ostream& err) {
    const std::unique_ptr<SessionReader> reader = openOrReport(options.path, err);
    if (!reader) {
        return 1;
    }

    const Metadata& manifest = reader->manifest();
    if (manifest.empty()) {
        err << "sweeps: " << options.path.string() << " has no manifest metadata\n";
        return 1;
    }

    // JSON rather than a flat listing: values nest, and a `key = value` table
    // has nothing to say about a hash inside an array. It also means `| jq` is
    // the whole of the tooling story for anyone outside C++.
    out << manifest.toJson(2) << '\n';
    return 0;
}

int runPlugins(const PluginsOptions& options, std::ostream& out, std::ostream& err) {
    const std::unique_ptr<SessionReader> reader = openOrReport(options.path, err);
    if (!reader) {
        return 1;
    }

    const std::vector<PluginRecord>& records = reader->pluginRecords();
    const std::uint64_t firstLineNs = reader->summary().firstLineNs;

    out << "plugin,record,schema,offset,bytes\n";

    std::size_t shown = 0;
    for (const PluginRecord& record : records) {
        if (!options.pluginId.empty() && record.pluginId != options.pluginId) {
            continue;
        }
        out << record.pluginId << ',' << record.recordName << ',' << record.schemaVersion << ','
            << (record.monotonicNs == 0 ? std::string("-")
                                        : offsetLabel(record.monotonicNs, firstLineNs))
            << ',' << record.bodyBytes << '\n';
        ++shown;
    }

    if (shown == 0) {
        err << "sweeps: " << options.path.string() << " carries no plugin records";
        if (!options.pluginId.empty()) {
            err << " from '" << options.pluginId << "'";
        }
        err << '\n';
    }
    return 0;
}

int runDump(const DumpOptions& options, std::ostream& out, std::ostream& err) {
    const std::unique_ptr<SessionReader> reader = openOrReport(options.path, err);
    if (!reader) {
        return 1;
    }

    const SessionSummary& summary = reader->summary();

    HistoryQuery request;
    request.fromNs =
        options.fromSeconds > 0.0 ? summary.firstLineNs + secondsToNs(options.fromSeconds) : 0;
    request.toNs = options.toSeconds > 0.0 ? summary.firstLineNs + secondsToNs(options.toSeconds)
                                           : std::numeric_limits<std::uint64_t>::max();
    request.fromHz = options.startHz > 0.0 ? options.startHz : 0.0;
    request.toHz = options.stopHz > 0.0 ? options.stopHz : std::numeric_limits<double>::max();
    request.segmentId = options.segmentId;
    // The LOD is chosen by capping the line budget, so an explicit level is
    // expressed as "ask for so many lines that only that level fits".
    request.maxLines = std::numeric_limits<std::uint32_t>::max();

    useClassicLocale(out);
    out << "# " << options.path.string() << '\n';
    out << "# levels are dBFS, dequantised at " << kDbPerStep << " dB per step\n";
    out << "segment,lod,line,monotonic_ns,start_hz,bin_width_hz,bins,levels_db...\n";
    out << std::setprecision(std::numeric_limits<double>::max_digits10);

    std::uint64_t emitted = 0;

    for (const SegmentInfo& segment : reader->segments()) {
        if (options.segmentId && *options.segmentId != segment.id) {
            continue;
        }

        std::uint32_t lod = 0;
        if (options.lod) {
            lod = *options.lod;
            if (lod >= kLodLevels) {
                err << "sweeps: lod must be 0.." << kLodLevels - 1 << '\n';
                return 1;
            }
            if (!reader->hasTilesAtLod(segment.id, lod)) {
                err << "sweeps: segment " << segment.id << " has no tiles at lod " << lod << '\n';
                return 1;
            }
        } else {
            for (std::uint32_t candidate = 0; candidate < kLodLevels; ++candidate) {
                if (reader->hasTilesAtLod(segment.id, candidate)) {
                    lod = candidate;
                    break;
                }
            }
        }

        HistoryQuery segmentRequest = request;
        segmentRequest.segmentId = segment.id;
        // Named outright. Left to the line-budget heuristic, an unbounded time
        // range makes it pick the coarsest level regardless of what was asked
        // for, and an export must give back the level it says it did.
        segmentRequest.lod = lod;

        auto tiles = reader->query(segmentRequest);
        if (!tiles) {
            err << "sweeps: " << tiles.error().describe() << '\n';
            return 1;
        }

        // A waterfall row spans every frequency block of its time block, so the
        // tiles are regrouped before anything is printed.
        std::map<std::uint32_t, std::vector<const HistoryTile*>> byTimeBlock;
        for (const HistoryTile& tile : *tiles) {
            if (tile.lod != lod) {
                continue;
            }
            byTimeBlock[tile.timeBlock].push_back(&tile);
        }

        for (auto& block : byTimeBlock) {
            std::vector<const HistoryTile*>& row = block.second;
            std::sort(row.begin(), row.end(), [](const HistoryTile* a, const HistoryTile* b) {
                return a->freqBlock < b->freqBlock;
            });

            std::uint32_t lines = 0;
            for (const HistoryTile* tile : row) {
                lines = std::max(lines, tile->lines);
            }

            for (std::uint32_t line = 0; line < lines; ++line) {
                if (options.maxLines != 0 && emitted >= options.maxLines) {
                    out << "# output stopped at " << options.maxLines
                        << " lines; raise --max-lines for more\n";
                    return 0;
                }

                const HistoryTile& first = *row.front();
                // Timestamps are interpolated across the tile's own extent,
                // which is what the format stores -- there is no per-line
                // timestamp to recover.
                const std::uint64_t span =
                    first.lastLineNs > first.firstLineNs ? first.lastLineNs - first.firstLineNs : 0;
                const std::uint64_t timeNs =
                    first.lines > 1 ? first.firstLineNs + span * line / (first.lines - 1)
                                    : first.firstLineNs;

                std::uint32_t bins = 0;
                for (const HistoryTile* tile : row) {
                    bins += line < tile->lines ? tile->bins : 0;
                }

                out << segment.id << ',' << lod << ',' << line << ',' << timeNs << ','
                    << segment.grid.startHz +
                           segment.grid.binWidthHz *
                               static_cast<double>(row.front()->freqBlock * kTileBins)
                    << ',' << segment.grid.binWidthHz << ',' << bins;

                for (const HistoryTile* tile : row) {
                    if (line >= tile->lines) {
                        continue;
                    }
                    for (std::uint32_t bin = 0; bin < tile->bins; ++bin) {
                        out << ',' << tile->dbAt(line, bin);
                    }
                }
                out << '\n';
                ++emitted;
            }
        }
    }

    return 0;
}

int runVersion(std::ostream& out) {
    out << "libsweepsfile " << libraryVersion() << '\n';
    out << ".sweeps format " << kMajorVersion << '.' << kMinorVersion << '\n';
    return 0;
}

void printUsage(std::ostream& out) {
    out << R"(sweeps -- read and inspect .sweeps spectrum session files

Usage:
  sweeps info     <file> [--events N]
  sweeps verify   <file> [--quiet]
  sweeps extract  <file> -o <out> [--from S] [--to S] [--start HZ] [--stop HZ]
  sweeps events   <file> [--kind KIND]
  sweeps manifest <file>
  sweeps plugins  <file> [--plugin ID]
  sweeps dump     <file> [--segment N] [--lod N] [--from S] [--to S]
                         [--start HZ] [--stop HZ] [--max-lines N]
  sweeps version

Commands:
  info      Metadata, segments and the first events.
  verify    Walk every record and check every CRC. Exit 1 on damage.
  extract   Copy a time and frequency range into a new standalone file.
            Tiles are copied byte for byte, never re-encoded.
  events    The event stream as JSON Lines, one object per event.
  manifest  The session metadata, as JSON.
  plugins   The plugin records a session carries.
  dump      Dequantised levels as CSV, one row per waterfall line.
  version   Library and format versions.

Times are seconds from the session's first stored line. Frequencies are hertz.
)";
}

} // namespace sweeps::cli
