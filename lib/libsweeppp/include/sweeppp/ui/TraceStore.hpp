// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <sweeppp/pipeline/SpectrumFrame.hpp>
#include <vector>

namespace sweeppp::ui {

enum class TraceKind : std::uint8_t { Live, MaxHold, MinHold, Average };

/// The sentinel and its predicate belong to the frame, not to the display: the
/// engine writes it, the recorder stores it and the trace only reads it. Named
/// here as well so the ~40 call sites inside this namespace, and the plugins
/// that qualify it as `sweeppp::ui::measured`, keep working.
using sweeppp::kUnmeasuredDbfs;
using sweeppp::measured;

/// A level as a readout says it, or "-" where there is no reading.
[[nodiscard]] inline std::string levelText(float levelDb) {
    return measured(levelDb) ? std::format("{:.1f} dBFS", static_cast<double>(levelDb))
                             : std::string("-");
}

/// Deliberately not named toString/displayName.
///
/// Declaring those in sweeppp::ui would hide the sweeppp:: ones from every
/// call inside this namespace -- ordinary lookup stops at the first enclosing
/// scope that contains the name, so toString(SdrValue) would silently stop
/// resolving. A `using` declaration does not fix it either, because it only
/// captures the overloads declared before that point.
[[nodiscard]] std::string_view traceKindName(TraceKind kind) noexcept;
[[nodiscard]] std::string_view traceKindLabel(TraceKind kind) noexcept;

/// One trace's data and display state.
struct Trace {
    TraceKind kind = TraceKind::Live;
    bool visible = true;
    std::vector<float> values;

    /// Frames accumulated, for the average.
    std::uint64_t count = 0;

    void reset() {
        values.clear();
        count = 0;
    }
};

/// The per-pixel envelope of a decimated trace.
///
/// A megabin sweep has far more bins than the plot has pixels. Drawing every
/// bin would be slow *and wrong*: whichever bin happened to land on a pixel
/// would win, so a narrow signal would flicker in and out as the view moved by
/// a fraction of a pixel. Keeping the min and max within each pixel column and
/// drawing them as a band means a signal is visible whenever it is present,
/// which is exactly what a spectrum analyser's display does.
struct Envelope {
    std::vector<float> minimum;
    std::vector<float> maximum;
    /// Frequency at the centre of each pixel column.
    std::vector<double> frequency;

    void clear() {
        minimum.clear();
        maximum.clear();
        frequency.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept { return maximum.size(); }
    [[nodiscard]] bool empty() const noexcept { return maximum.empty(); }
};

/// Envelopes already computed, one per trace kind, owned by whoever draws them.
///
/// Owned by the caller rather than by the store because more than one plot
/// draws the same traces at different zooms, and a single slot inside the
/// store was overwritten by each in turn -- recomputing a million-bin walk for
/// every trace on every plot on every frame. One slot per kind is also what
/// keeps Max, Min and Average from evicting each other within one plot.
struct EnvelopeCache {
    struct Slot {
        Envelope envelope;
        double fromHz = 0.0;
        double toHz = 0.0;
        std::size_t pixels = 0;
        std::uint64_t generation = 0;
        bool filled = false;
    };
    std::array<Slot, 4> slots;

    void clear() noexcept {
        for (Slot& slot : slots) {
            slot.filled = false;
        }
    }
};

/// Holds the four traces and produces decimated envelopes for drawing.
class TraceStore {
public:
    /// Feeds a new frame into whichever traces are active.
    void update(const SpectrumFrame& frame);

    /// Clears accumulated traces without discarding the live one -- what the
    /// "reset hold" action does.
    void resetHolds();

    void clear();

    [[nodiscard]] const Trace& trace(TraceKind kind) const;
    [[nodiscard]] Trace& trace(TraceKind kind);

    /// Per-pixel min/max envelope of one trace over a frequency window.
    ///
    /// `pixels` is the plot's width in pixels; the result has at most that
    /// many columns however many bins are involved. Recomputed only when the
    /// traces or the window changed since `cache` last held this kind.
    [[nodiscard]] const Envelope& envelope(TraceKind kind, double fromHz, double toHz,
                                           std::size_t pixels, EnvelopeCache& cache) const;

    [[nodiscard]] double startHz() const noexcept { return m_startHz; }
    [[nodiscard]] double stopHz() const noexcept {
        return m_startHz + m_binWidthHz * static_cast<double>(m_binCount);
    }
    [[nodiscard]] double binWidthHz() const noexcept { return m_binWidthHz; }
    [[nodiscard]] std::size_t binCount() const noexcept { return m_binCount; }

    /// Level at a frequency, from the live trace. Drives the marker readout.
    [[nodiscard]] float levelAt(double hz) const;

    /// Highest bin of the live trace within a window, and where it is. Peak
    /// search.
    [[nodiscard]] bool peakIn(double fromHz, double toHz, double& outHz, float& outDb) const;

    /// Exponential smoothing applied to the live trace, 0 disables. The
    /// "smoothing of pulses" control -- it trades transient fidelity for a
    /// steadier picture, so it is off by default.
    void setSmoothing(float alpha) noexcept { m_smoothing = alpha; }
    [[nodiscard]] float smoothing() const noexcept { return m_smoothing; }

    void setAverageWindow(std::uint32_t frames) noexcept { m_averageWindow = frames; }

    /// Rate the max-hold trace bleeds back down, in dB per second.
    ///
    /// Zero holds forever, which is the right answer for "did anything ever
    /// appear here" and the wrong one for watching a band: an undecayed hold
    /// becomes a record of the whole session with no way to clear it.
    void setMaxHoldDecay(float dbPerSecond) noexcept { m_maxHoldDecayDbPerSecond = dbPerSecond; }
    [[nodiscard]] float maxHoldDecay() const noexcept { return m_maxHoldDecayDbPerSecond; }
    [[nodiscard]] std::uint32_t averageWindow() const noexcept { return m_averageWindow; }

private:
    Trace m_live{.kind = TraceKind::Live};
    Trace m_maxHold{.kind = TraceKind::MaxHold, .visible = false};
    Trace m_minHold{.kind = TraceKind::MinHold, .visible = false};
    Trace m_average{.kind = TraceKind::Average, .visible = false};

    double m_startHz = 0.0;
    double m_binWidthHz = 0.0;
    std::size_t m_binCount = 0;

    float m_smoothing = 0.0F;
    float m_maxHoldDecayDbPerSecond = 0.0F;
    std::uint64_t m_lastFrameNs = 0;
    std::uint32_t m_averageWindow = 16;

    /// Bumped whenever any trace changes, which is what a cached envelope is
    /// checked against.
    std::uint64_t m_generation = 0;

    /// Min and max over runs of 8, 64, 512... bins, per trace.
    ///
    /// Zoomed out over a megabin grid, every plot used to walk every bin on
    /// every new frame, once per plot and once per trace. Built once per
    /// change of the traces and shared, a zoomed-out plot reads a few
    /// thousand entries instead -- and still sees a one-bin carrier, because
    /// an entry is the min and max of what it covers, not a sample of it.
    struct Pyramid {
        std::vector<std::vector<float>> lows;  ///< Level 1 upward.
        std::vector<std::vector<float>> highs; ///< Level 1 upward.
        std::uint64_t generation = 0;
        bool built = false;
    };
    mutable std::array<Pyramid, 4> m_pyramids;

    [[nodiscard]] const Pyramid& pyramid(TraceKind kind) const;
};

} // namespace sweeppp::ui
