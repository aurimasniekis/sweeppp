// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Detector.hpp"

#include <algorithm>
#include <cmath>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/ui/TraceStore.hpp>
#include <sweeppp/ui/ViewSettings.hpp>
#include <utility>

namespace detections {
namespace {

/// The histogram the noise floor is read off, in 1 dB buckets over the range
/// every level in this application is held within.
///
/// A median found by counting into fixed buckets rather than by sorting: one
/// pass, no allocation, and the answer is wanted to a decibel rather than to a
/// hundredth of one.
constexpr float kHistogramFloorDbfs = sweeppp::ui::kScaleFloorDbfs;
constexpr float kHistogramCeilingDbfs = sweeppp::ui::kScaleCeilingDbfs;
constexpr std::size_t kBucketCount =
    static_cast<std::size_t>(kHistogramCeilingDbfs - kHistogramFloorDbfs);

/// How long since a completed pass still means "the radio is sweeping".
///
/// The same rule the host applies to its waterfall: while a sweep is running,
/// only a completed pass is a measurement of the whole span, and everything
/// between two of them is a re-publication of bins that have not been
/// re-measured.
constexpr std::uint64_t kSweepingWindowNs = 5'000'000'000ULL;

/// One run of qualifying bins, before it becomes a `Peak`.
struct Run {
    std::size_t firstBin = 0;
    std::size_t lastBin = 0;
    std::size_t peakBin = 0;
    float peakDbfs = 0.0F;
};

[[nodiscard]] double overlapHz(double aStart, double aStop, double bStart, double bStop) noexcept {
    return std::min(aStop, bStop) - std::max(aStart, bStart);
}

} // namespace

void Detector::setConfig(const DetectorConfig& config) {
    m_config = config;
    if (m_config.maxTracked == 0) {
        m_config.maxTracked = 1;
    }
    trim();
}

void Detector::setIgnored(std::vector<IgnoreRange> ranges) {
    for (IgnoreRange& range : ranges) {
        if (range.startHz > range.stopHz) {
            std::swap(range.startHz, range.stopHz);
        }
    }
    m_ignored = std::move(ranges);
}

float Detector::thresholdDbfs() const noexcept {
    return m_config.mode == ThresholdMode::Absolute ? m_config.absoluteDbfs
                                                    : m_floor.value() + m_config.marginDb;
}

bool Detector::isIgnored(double hz) const noexcept {
    return std::ranges::any_of(m_ignored, [hz](const IgnoreRange& range) {
        return hz >= range.startHz && hz <= range.stopHz;
    });
}

void Detector::onFrame(std::uint64_t monotonicNs, double startHz, double binWidthHz,
                       std::span<const float> bins, bool passComplete) {
    // No sequence number, so nothing is counted as dropped. What the tests
    // call, and what the overload below forwards to once it has counted.
    if (passComplete) {
        m_lastPassCompleteNs = monotonicNs;
    }
    if (bins.empty() || binWidthHz <= 0.0) {
        return;
    }

    const bool sweeping =
        m_lastPassCompleteNs != 0 && monotonicNs - m_lastPassCompleteNs <= kSweepingWindowNs;
    if (sweeping && !passComplete) {
        return;
    }

    scan(monotonicNs, startHz, binWidthHz, bins);
}

void Detector::onFrame(std::uint64_t monotonicNs, std::uint64_t sequence, double startHz,
                       double binWidthHz, std::span<const float> bins, bool passComplete) {
    // Counted before the cadence gate and before the empty-frame check: a
    // frame the host's queue dropped is a frame that never arrived at all, and
    // whether we would have scanned it is beside the point.
    if (m_haveSequence && sequence > m_lastSequence + 1) {
        m_droppedFrames += sequence - m_lastSequence - 1;
    }
    m_lastSequence = sequence;
    m_haveSequence = true;

    onFrame(monotonicNs, startHz, binWidthHz, bins, passComplete);
}

void Detector::estimateFloor(std::span<const float> bins) {
    std::array<std::uint32_t, kBucketCount> histogram{};
    std::size_t measured = 0;

    for (const float level : bins) {
        if (!sweeppp::ui::measured(level)) {
            continue;
        }
        const float clamped = std::clamp(level, kHistogramFloorDbfs, kHistogramCeilingDbfs);
        const auto bucket = static_cast<std::size_t>(clamped - kHistogramFloorDbfs);
        ++histogram[std::min(bucket, kBucketCount - 1)];
        ++measured;
    }

    if (measured == 0) {
        return;
    }

    const std::size_t half = measured / 2;
    std::size_t seen = 0;
    std::size_t median = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        seen += histogram[i];
        if (seen > half) {
            median = i;
            break;
        }
    }

    // The bucket's centre, not its edge: the estimate is a decibel wide and
    // reporting its lower bound biases every threshold half a decibel low.
    m_floor.push(kHistogramFloorDbfs + static_cast<float>(median) + 0.5F);
    m_floorPrimed = true;
}

std::vector<Peak> Detector::findPeaks(double startHz, double binWidthHz,
                                      std::span<const float> bins, float threshold) const {
    // Open high, close low. Everything between the two is inside a run that is
    // already open and outside one that is not.
    const float stay = threshold - std::max(m_config.hysteresisDb, 0.0F);

    const auto binCenter = [startHz, binWidthHz](std::size_t bin) {
        return startHz + binWidthHz * (static_cast<double>(bin) + 0.5);
    };

    // Whether the detector is allowed to look at this bin at all.
    //
    // `measured` rather than a comparison against -200: smoothing and
    // averaging lift an untouched bin a little, and a bin nothing has been
    // measured into is not a quiet reading, it is no reading at all. An
    // ignored bin is the same kind of nothing, arrived at on purpose.
    const auto lookable = [&](std::size_t bin) {
        return sweeppp::ui::measured(bins[bin]) && !isIgnored(binCenter(bin));
    };

    std::vector<Run> runs;
    bool open = false;

    for (std::size_t i = 0; i < bins.size(); ++i) {
        if (!lookable(i)) {
            open = false;
            continue;
        }

        const float level = bins[i];

        if (open) {
            if (level < stay) {
                open = false;
                continue;
            }
            Run& run = runs.back();
            run.lastBin = i;
            if (level > run.peakDbfs) {
                run.peakDbfs = level;
                run.peakBin = i;
            }
            continue;
        }

        if (level < threshold) {
            continue;
        }

        // Opening. Walk back over the shoulder this signal already has above
        // the closing level, so its two edges are measured the same way -- a
        // run that starts at the threshold and ends at the threshold minus the
        // hysteresis would report a width that depends on which side of the
        // peak the scan happened to arrive from.
        std::size_t first = i;
        const std::size_t limit = runs.empty() ? 0 : runs.back().lastBin + 1;
        while (first > limit && lookable(first - 1) && bins[first - 1] >= stay) {
            --first;
        }

        runs.push_back(Run{.firstBin = first, .lastBin = i, .peakBin = i, .peakDbfs = level});
        open = true;
    }

    // Runs a narrow gap apart are one signal with a null in it, not two.
    if (m_config.mergeGapBins > 0 && runs.size() > 1) {
        std::vector<Run> merged;
        merged.reserve(runs.size());
        merged.push_back(runs.front());

        for (std::size_t i = 1; i < runs.size(); ++i) {
            Run& into = merged.back();
            const Run& next = runs[i];

            bool joinable = next.firstBin - into.lastBin - 1 <= m_config.mergeGapBins;

            // Never across a bin the scan was not allowed to look at. Two runs
            // either side of an unmeasured stretch or an ignore range are not
            // known to be one signal -- and saying they are would let an
            // ignore range be bridged by the very setting meant to join a
            // signal to itself.
            for (std::size_t bin = into.lastBin + 1; joinable && bin < next.firstBin; ++bin) {
                joinable = lookable(bin);
            }

            if (joinable) {
                into.lastBin = next.lastBin;
                if (next.peakDbfs > into.peakDbfs) {
                    into.peakDbfs = next.peakDbfs;
                    into.peakBin = next.peakBin;
                }
            } else {
                merged.push_back(next);
            }
        }

        runs = std::move(merged);
    }

    const std::size_t minimum = std::max<std::size_t>(m_config.minWidthBins, 1);

    std::vector<Peak> peaks;
    peaks.reserve(runs.size());
    for (const Run& run : runs) {
        if (run.lastBin - run.firstBin + 1 < minimum) {
            continue;
        }
        // Bin edges, so a one-bin run is one bin wide rather than nothing
        // wide, and two adjacent runs meet rather than overlap.
        peaks.push_back(Peak{.startHz = startHz + binWidthHz * static_cast<double>(run.firstBin),
                             .stopHz = startHz + binWidthHz * static_cast<double>(run.lastBin + 1),
                             .peakHz = binCenter(run.peakBin),
                             .peakDbfs = run.peakDbfs});
    }

    return peaks;
}

void Detector::scan(std::uint64_t monotonicNs, double startHz, double binWidthHz,
                    std::span<const float> bins) {
    // A re-plan is a different grid, not different signals. The detections are
    // keyed in hertz rather than in bins, so they survive it; what does not
    // survive is the floor estimate -- a different span has a different noise
    // floor -- and the record of what was present in the previous scan, which
    // was a statement about a measurement that is no longer being repeated.
    const bool gridChanged =
        startHz != m_gridStartHz || binWidthHz != m_gridBinWidthHz || bins.size() != m_gridBinCount;
    if (gridChanged) {
        m_gridStartHz = startHz;
        m_gridBinWidthHz = binWidthHz;
        m_gridBinCount = bins.size();
        m_floor.reset();
        m_floorPrimed = false;
        for (Track& track : m_tracks) {
            track.present = false;
            track.previous = false;
        }
    }

    estimateFloor(bins);
    if (m_config.mode == ThresholdMode::AboveNoise && !m_floorPrimed) {
        return;
    }

    const float threshold = thresholdDbfs();
    const std::vector<Peak> peaks = findPeaks(startHz, binWidthHz, bins, threshold);

    for (Track& track : m_tracks) {
        track.previous = track.present;
        track.present = false;
    }

    ++m_scansSinceTick;

    for (const Peak& peak : peaks) {
        const double peakWidth = peak.stopHz - peak.startHz;
        const double centerTolerance = std::max(binWidthHz * 2.0, peakWidth * 0.5);

        // Overlap first, and only then proximity. A signal that has grown or
        // shrunk still overlaps where it was; one that has moved a bin between
        // passes has not, and matching it by centre is what keeps a drifting
        // carrier one row rather than a new row per sweep.
        Track* best = nullptr;
        double bestOverlap = 0.0;
        double bestDistance = 0.0;

        for (Track& track : m_tracks) {
            if (track.present) {
                continue;
            }
            const double shared = overlapHz(peak.startHz, peak.stopHz, track.detection.startHz,
                                            track.detection.stopHz);
            if (shared <= 0.0) {
                continue;
            }
            if (best == nullptr || shared > bestOverlap) {
                best = &track;
                bestOverlap = shared;
            }
        }

        if (best == nullptr) {
            for (Track& track : m_tracks) {
                if (track.present) {
                    continue;
                }
                const double distance = std::abs(track.detection.centerHz - peak.peakHz);
                if (distance > centerTolerance) {
                    continue;
                }
                if (best == nullptr || distance < bestDistance) {
                    best = &track;
                    bestDistance = distance;
                }
            }
        }

        if (best == nullptr) {
            Track fresh;
            fresh.detection.id = m_nextId++;
            fresh.detection.firstSeenNs = monotonicNs;
            fresh.detection.strongestDbfs = peak.peakDbfs;
            m_tracks.push_back(fresh);
            best = &m_tracks.back();
        }

        Detection& found = best->detection;
        found.startHz = peak.startHz;
        found.stopHz = peak.stopHz;
        found.centerHz = peak.peakHz;
        found.peakDbfs = peak.peakDbfs;
        found.strongestDbfs = std::max(found.strongestDbfs, peak.peakDbfs);
        found.lastSeenNs = monotonicNs;
        ++found.hits;
        found.active = true;

        best->present = true;
        ++best->scansPresent;

        if (!best->previous) {
            ++found.appearances;
            if (best->lastOnsetNs != 0) {
                const double gap = sweeppp::nsToSeconds(monotonicNs - best->lastOnsetNs);
                best->intervals[best->intervalNext] = gap;
                best->intervalNext = (best->intervalNext + 1) % best->intervals.size();
                best->intervalCount = std::min(best->intervalCount + 1, best->intervals.size());

                double total = 0.0;
                for (std::size_t i = 0; i < best->intervalCount; ++i) {
                    total += best->intervals[i];
                }
                found.meanIntervalSeconds = total / static_cast<double>(best->intervalCount);
            }
            best->lastOnsetNs = monotonicNs;

            m_transitions.push_back(Transition{.kind = Transition::Kind::Appeared,
                                               .detection = found,
                                               .thresholdDbfs = threshold});
        }
    }

    age(monotonicNs);
    trim();
    republish();
}

void Detector::age(std::uint64_t monotonicNs) {
    const auto dropAfterNs =
        static_cast<std::uint64_t>(std::max(m_config.dropAfterSeconds, 0.0) * 1e9);

    for (Track& track : m_tracks) {
        if (!track.detection.active) {
            continue;
        }
        if (monotonicNs <= track.detection.lastSeenNs ||
            monotonicNs - track.detection.lastSeenNs <= dropAfterNs) {
            continue;
        }

        track.detection.active = false;
        track.present = false;
        track.previous = false;
        m_transitions.push_back(Transition{.kind = Transition::Kind::Gone,
                                           .detection = track.detection,
                                           .thresholdDbfs = thresholdDbfs()});
    }
}

void Detector::trim() {
    // Least recently seen first, and inactive before active: the cap is there
    // to stop a threshold set into the noise from growing a list without end,
    // and what such a list is full of is entries nothing has been seen at
    // since.
    while (m_tracks.size() > m_config.maxTracked) {
        const auto worst = std::ranges::min_element(m_tracks, [](const Track& a, const Track& b) {
            if (a.detection.active != b.detection.active) {
                return !a.detection.active;
            }
            return a.detection.lastSeenNs < b.detection.lastSeenNs;
        });
        m_tracks.erase(worst);
    }
}

void Detector::republish() {
    m_detections.clear();
    m_detections.reserve(m_tracks.size());
    for (const Track& track : m_tracks) {
        m_detections.push_back(track.detection);
    }
}

std::vector<TickSample> Detector::tick(std::uint64_t monotonicNs) {
    age(monotonicNs);
    republish();

    std::vector<TickSample> samples;
    samples.reserve(m_tracks.size());

    for (Track& track : m_tracks) {
        // Nothing was scanned in this window -- a wide sweep takes longer than
        // a tick to complete a pass -- so the honest answer is what the last
        // scan found rather than a zero that would punch a hole in the
        // activity graph of a signal that never went away.
        const float fraction = m_scansSinceTick > 0 ? static_cast<float>(track.scansPresent) /
                                                          static_cast<float>(m_scansSinceTick)
                                                    : (track.present ? 1.0F : 0.0F);
        track.scansPresent = 0;

        const Detection& found = track.detection;
        samples.push_back(TickSample{.id = found.id,
                                     .startHz = found.startHz,
                                     .stopHz = found.stopHz,
                                     .centerHz = found.centerHz,
                                     .peakDbfs = found.peakDbfs,
                                     .strongestDbfs = found.strongestDbfs,
                                     .firstSeenNs = found.firstSeenNs,
                                     .lastSeenNs = found.lastSeenNs,
                                     .hits = found.hits,
                                     .appearances = found.appearances,
                                     .meanIntervalSeconds = found.meanIntervalSeconds,
                                     .active = found.active,
                                     .presentFraction = fraction});
    }

    m_scansSinceTick = 0;
    return samples;
}

std::vector<Transition> Detector::takeTransitions() {
    return std::exchange(m_transitions, {});
}

void Detector::forget(int id) {
    std::erase_if(m_tracks, [id](const Track& track) { return track.detection.id == id; });
    republish();
}

void Detector::clearHistory() {
    std::erase_if(m_tracks, [](const Track& track) { return !track.detection.active; });
    republish();
}

} // namespace detections
