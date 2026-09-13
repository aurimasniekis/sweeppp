// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What has been transmitting here, and how often.
//
// A value type with no ABI in it and no singleton behind it, exactly as
// `channels::ChannelSet` is: the plugin file is glue, and everything with a
// decision in it is here, where a doctest binary can reach it without
// `dlopen`.
//
// Single-threaded by contract. The host's frame worker owns one of these and
// is the only thread that touches it; what crosses to the UI is `TickSample`,
// which is scalars and nothing else.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <sweeppp/core/Telemetry.hpp>
#include <vector>

namespace detections {

/// What counts as "over the threshold".
///
/// Two answers because the two questions are different. An absolute floor is
/// what an operator has when they know the level a signal of interest arrives
/// at; a margin over the measured noise is what they have when they do not,
/// and it is the one that survives a gain change.
enum class ThresholdMode : std::uint8_t { Absolute, AboveNoise };

struct DetectorConfig {
    ThresholdMode mode = ThresholdMode::AboveNoise;
    float absoluteDbfs = -70.0F; ///< Absolute mode.
    float marginDb = 10.0F;      ///< AboveNoise mode: this far over the floor.

    /// A run stays open until the level falls this far below the threshold.
    ///
    /// A Schmitt trigger, and the thing that holds a real signal together. An
    /// OFDM carrier -- Wi-Fi, LTE -- is not a smooth hump: its level moves
    /// several dB between adjacent bins and between frames, so a bare
    /// comparison against one threshold cuts a single 20 MHz transmission into
    /// a dozen fragments, each of which then becomes its own row. Opening high
    /// and closing low is what stops that, and it costs one comparison.
    float hysteresisDb = 6.0F;

    /// Widths and gaps in BINS, not hertz.
    ///
    /// Deliberately, and it is not a unit preference. The bin width changes
    /// under the operator's hand -- 4.9 kHz on a narrow span, 500 kHz on a
    /// wide one -- so no fixed number of hertz is a sane default at both ends:
    /// a gap that merges a Wi-Fi carrier's nulls at one RBW swallows the whole
    /// band at another. In bins these defaults mean the same thing everywhere.
    std::size_t minWidthBins = 3; ///< Narrower runs are spurs. 1 = keep one bin.
    std::size_t mergeGapBins = 8; ///< A gap this narrow does not split a run.

    double dropAfterSeconds = 5.0; ///< Not seen for this long -> history.
    std::size_t maxTracked = 512;  ///< Oldest history entries fall off past this.
};

/// A span the detector does not look in.
///
/// The answer to a transmitter the operator already knows about sitting in the
/// middle of the band they are watching: without it, the one signal they are
/// not interested in is the one row that is always at the top.
struct IgnoreRange {
    double startHz = 0.0;
    double stopHz = 0.0;
    std::string note;
};

/// One run of bins over the threshold, in one frame.
struct Peak {
    double startHz = 0.0;
    double stopHz = 0.0;
    double peakHz = 0.0; ///< Centre of the loudest bin in the run.
    float peakDbfs = 0.0F;
};

/// A signal being tracked.
struct Detection {
    int id = 0; ///< Stable name: D1, D2. Never reused while alive.

    double startHz = 0.0;
    double stopHz = 0.0;
    double centerHz = 0.0; ///< The loudest bin -- the frequency an operator reads.

    float peakDbfs = 0.0F;      ///< In the scan it was last seen in.
    float strongestDbfs = 0.0F; ///< The loudest it has ever been.

    std::uint64_t firstSeenNs = 0;
    std::uint64_t lastSeenNs = 0;

    std::uint64_t hits = 0;        ///< Scans it appeared in.
    std::uint64_t appearances = 0; ///< Onsets: how many separate spells.

    /// Mean gap between onsets. 0 means continuous, or only ever seen once --
    /// which is the same statement about the interval: there isn't one.
    double meanIntervalSeconds = 0.0;

    bool active = true; ///< False once it has aged out into the history table.
};

/// One tracked signal as it crosses to the UI thread. Scalars only: no
/// histories, no strings, nothing that owns memory the other side will free.
struct TickSample {
    int id = 0;
    double startHz = 0.0;
    double stopHz = 0.0;
    double centerHz = 0.0;
    float peakDbfs = 0.0F;
    float strongestDbfs = 0.0F;
    std::uint64_t firstSeenNs = 0;
    std::uint64_t lastSeenNs = 0;
    std::uint64_t hits = 0;
    std::uint64_t appearances = 0;
    double meanIntervalSeconds = 0.0;
    bool active = true;

    /// Share of the scans in this tick's window the signal was present in.
    /// What the activity sparkline plots.
    float presentFraction = 0.0F;
};

/// A detection starting or ending, for whoever is writing the session.
///
/// Returned rather than published through a callback: the model has no host in
/// it, and a transition is a fact about the scan that produced it rather than
/// an event with a subscriber.
struct Transition {
    enum class Kind : std::uint8_t { Appeared, Gone };

    Kind kind = Kind::Appeared;
    Detection detection;
    float thresholdDbfs = 0.0F;
};

class Detector {
public:
    void setConfig(const DetectorConfig& config);
    [[nodiscard]] const DetectorConfig& config() const noexcept { return m_config; }

    void setIgnored(std::vector<IgnoreRange> ranges);
    [[nodiscard]] const std::vector<IgnoreRange>& ignored() const noexcept { return m_ignored; }

    /// One frame. `bins` is dBFS, ascending, borrowed for the call.
    ///
    /// Not every frame is scanned: a sweep publishes the whole grid every time
    /// a step lands, and counting those partial re-publications as separate
    /// sightings would turn "how often it appears" into a count of how fast
    /// the sweep is running.
    void onFrame(std::uint64_t monotonicNs, double startHz, double binWidthHz,
                 std::span<const float> bins, bool passComplete);

    /// The same, with the frame's own sequence number, so a gap left by the
    /// host's bounded queue is counted rather than silently under-reported.
    void onFrame(std::uint64_t monotonicNs, std::uint64_t sequence, double startHz,
                 double binWidthHz, std::span<const float> bins, bool passComplete);

    /// Folds the per-tick counters into one sample per tracked signal, and
    /// ages out whatever has not been seen for `dropAfterSeconds`.
    [[nodiscard]] std::vector<TickSample> tick(std::uint64_t monotonicNs);

    /// Onsets and drop-outs since this was last called.
    [[nodiscard]] std::vector<Transition> takeTransitions();

    [[nodiscard]] const std::vector<Detection>& detections() const noexcept { return m_detections; }

    /// The estimated noise floor, smoothed across frames. Meaningful once at
    /// least one frame with measured bins has arrived.
    [[nodiscard]] float noiseFloorDbfs() const noexcept { return m_floor.value(); }

    /// What a bin is actually being compared against, in the current mode.
    [[nodiscard]] float thresholdDbfs() const noexcept;

    /// The grid the last scanned frame was on. What tells the panel how wide
    /// one bin is, which is the unit the width and gap settings are really in.
    [[nodiscard]] double binWidthHz() const noexcept { return m_gridBinWidthHz; }

    /// Frames the host's queue dropped before they reached us -- counted from
    /// gaps in the frame sequence, which is the only evidence a plugin has.
    [[nodiscard]] std::uint64_t droppedFrames() const noexcept { return m_droppedFrames; }

    void forget(int id);

    /// Drops everything that has aged out, keeping what is still active.
    void clearHistory();

private:
    /// A tracked signal and the bookkeeping that is nobody else's business.
    struct Track {
        Detection detection;

        /// The last sixteen gaps between onsets, for the mean. A ring rather
        /// than a running total because a signal that fired every two seconds
        /// this morning and every hour since should read as hourly.
        std::array<double, 16> intervals{};
        std::size_t intervalCount = 0;
        std::size_t intervalNext = 0;
        std::uint64_t lastOnsetNs = 0;

        bool present = false;  ///< Seen in the most recent completed scan.
        bool previous = false; ///< Seen in the one before it.

        std::uint32_t scansPresent = 0; ///< Since the last `tick`.
    };

    void scan(std::uint64_t monotonicNs, double startHz, double binWidthHz,
              std::span<const float> bins);
    void estimateFloor(std::span<const float> bins);
    [[nodiscard]] std::vector<Peak> findPeaks(double startHz, double binWidthHz,
                                              std::span<const float> bins, float threshold) const;
    [[nodiscard]] bool isIgnored(double hz) const noexcept;
    void age(std::uint64_t monotonicNs);
    void trim();
    void republish();

    DetectorConfig m_config;
    std::vector<IgnoreRange> m_ignored;

    std::vector<Track> m_tracks;

    /// The public halves of `m_tracks`, republished after every change.
    ///
    /// A copy rather than a second source of truth: the alternative is two
    /// vectors kept in step by hand, and the one thing that is certain about
    /// two vectors kept in step by hand is that one day they are not.
    std::vector<Detection> m_detections;

    std::vector<Transition> m_transitions;

    int m_nextId = 1;

    // The frame grid this detector last scanned, for spotting a re-plan.
    double m_gridStartHz = 0.0;
    double m_gridBinWidthHz = 0.0;
    std::size_t m_gridBinCount = 0;

    /// Smoothed across frames: a per-frame median bucket jitters by a decibel
    /// or two, and a threshold that jitters with it turns one signal into a
    /// row that appears and disappears several times a second.
    sweeppp::Ewma m_floor{0.25F};
    bool m_floorPrimed = false;

    std::uint64_t m_lastPassCompleteNs = 0;
    std::uint64_t m_lastSequence = 0;
    bool m_haveSequence = false;
    std::uint64_t m_droppedFrames = 0;

    std::uint32_t m_scansSinceTick = 0;
};

} // namespace detections
