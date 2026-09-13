// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// What has been transmitting here, and how often -- as a plugin.
//
// The spectrum says what is there now. This says what has been there: a burst
// that fires every two minutes is invisible unless somebody happens to be
// looking when it does, and an operator watching a band should not have to be
// the thing that stays awake.
//
// Three facets, and the split is the point:
//
//   FRAME_PROCESSOR  scans every published frame on a thread the host owns.
//                    The first user of that facet in this tree.
//   UI_EXTENSION     the button in the status bar and the window behind it.
//   CONTRIBUTOR      publishes only the ticked rows, so the host paints their
//                    spans with the same code that paints a band plan.
//
// Everything with a decision in it lives in `Detector`, which has no ABI in it
// and is covered by the suite next door. This file is glue: the vtables, the
// two threads and their one mutex, the drawing, and the writing.
//
// It links `sweeppp::sweeppp`, and only for values: toml_util, Result,
// PluginSettings, Clock, RollingHistory. It must never reach a singleton --
// see the rule at the top of <sweeppp/plugin/Plugin.hpp>.
#include "Detector.hpp"

#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/Result.hpp>
#include <sweeppp/core/Telemetry.hpp>
#include <sweeppp/core/Toml.hpp>
#include <sweeppp/plugin/Plugin.hpp>
#include <sweeppp/plugin/PluginSettings.hpp>

#if defined(SWEEPPP_PLUGIN_HAS_UI)
#include <imgui.h>
#include <implot.h>
#include <sweeppp/plugin/PluginChrome.hpp>
#endif

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace detections {
namespace {

namespace plugin = sweeppp::plugin;
#if defined(SWEEPPP_PLUGIN_HAS_UI)
namespace chrome = sweeppp::plugin::chrome;
#endif
namespace toml_util = sweeppp::toml_util;

constexpr std::string_view kPluginId = "org.sweeppp.detections";

/// What the hold can be set to. Logarithmic between them in the slider,
/// because the difference between five seconds and thirty matters as much as
/// the one between five minutes and thirty.
constexpr double kHoldMinSeconds = 2.0;
constexpr double kHoldMaxSeconds = 3600.0;

/// How often the worker folds its counters into a sample for the UI.
constexpr std::uint64_t kTickIntervalNs = 250'000'000ULL;

/// The three spans the graph switches between, as a count of ticks per sample.
///
/// 240 samples per ring, so: 240 x 0.25 s = 1 minute, 240 x 1 s = 4 minutes,
/// 240 x 7.5 s = 30 minutes.
constexpr std::size_t kSpanCount = 3;
constexpr std::array<std::uint32_t, kSpanCount> kSpanTicks{1, 4, 30};

/// The house sparkline length, from `sweeppp::AppState::HealthTrace`.
constexpr std::size_t kHistoryLength = 240;

/// Which of a row's two series the graph column is showing.
enum class GraphSeries : std::uint8_t { Activity, PeakLevel };

/// How the two tables are ordered.
///
/// First seen by default, and never frequency: a detection's frequency is a
/// live measurement that moves a bin at a time, so ordering on it makes rows
/// swap places while they are being read. First seen never changes once it is
/// set, which makes it the only ordering that holds still.
enum class SortOrder : std::uint8_t { FirstSeen, LastSeen, Frequency, Level, Sightings };

[[nodiscard]] std::string_view sortKey(SortOrder order) noexcept {
    switch (order) {
    case SortOrder::LastSeen:
        return "last_seen";
    case SortOrder::Frequency:
        return "frequency";
    case SortOrder::Level:
        return "level";
    case SortOrder::Sightings:
        return "sightings";
    case SortOrder::FirstSeen:
        break;
    }
    return "first_seen";
}

[[nodiscard]] SortOrder sortFrom(std::string_view key) noexcept {
    if (key == "last_seen") {
        return SortOrder::LastSeen;
    }
    if (key == "frequency") {
        return SortOrder::Frequency;
    }
    if (key == "level") {
        return SortOrder::Level;
    }
    if (key == "sightings") {
        return SortOrder::Sightings;
    }
    return SortOrder::FirstSeen;
}

/// A number as a settings file or an input box carries it.
///
/// `std::strtod` rather than `std::from_chars`: the floating-point overload of
/// the latter is not in this deployment target's library, and the difference
/// that matters here -- locale sensitivity -- is settled by the values only
/// ever being written by `std::format`, which is locale-independent.
[[nodiscard]] std::optional<double> toDouble(std::string_view text) {
    const std::string owned(text);
    char* end = nullptr;
    const double value = std::strtod(owned.c_str(), &end);
    if (end == owned.c_str() || end != owned.c_str() + owned.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string_view modeKey(ThresholdMode mode) noexcept {
    return mode == ThresholdMode::Absolute ? "absolute" : "above_noise";
}

[[nodiscard]] ThresholdMode modeFrom(std::string_view key) noexcept {
    return key == "absolute" ? ThresholdMode::Absolute : ThresholdMode::AboveNoise;
}

[[nodiscard]] std::string_view seriesKey(GraphSeries series) noexcept {
    return series == GraphSeries::PeakLevel ? "peak" : "activity";
}

[[nodiscard]] GraphSeries seriesFrom(std::string_view key) noexcept {
    return key == "peak" ? GraphSeries::PeakLevel : GraphSeries::Activity;
}

/// The colour a ticked detection is drawn in.
constexpr float kSpanColor[4]{0.98F, 0.58F, 0.16F, 1.0F};

#if defined(SWEEPPP_PLUGIN_HAS_UI)
/// Where a level sits between the weakest and the strongest in its own graph,
/// as a colour: cold and dim at the bottom, hot at the top.
///
/// The peak-level sparkline is a plot of one quantity, so drawing it in one
/// flat colour throws away the axis a reader can take in without measuring.
/// Four stops rather than a smooth hue sweep, because a continuous rainbow has
/// no ordering anyone can read off it.
[[nodiscard]] ImU32 levelColor(float t) {
    constexpr std::array<ImVec4, 4> kStops{
        ImVec4(0.30F, 0.44F, 0.72F, 1.0F), ImVec4(0.28F, 0.72F, 0.68F, 1.0F),
        ImVec4(0.95F, 0.72F, 0.24F, 1.0F), ImVec4(0.92F, 0.33F, 0.28F, 1.0F)};

    const float clamped = std::clamp(t, 0.0F, 1.0F) * static_cast<float>(kStops.size() - 1);
    const auto low = static_cast<std::size_t>(clamped);
    const std::size_t high = std::min(low + 1, kStops.size() - 1);
    const float mix = clamped - static_cast<float>(low);

    return ImGui::GetColorU32(ImVec4(kStops[low].x + (kStops[high].x - kStops[low].x) * mix,
                                     kStops[low].y + (kStops[high].y - kStops[low].y) * mix,
                                     kStops[low].z + (kStops[high].z - kStops[low].z) * mix, 1.0F));
}
#endif

/// One band, and the settings that make sense in it.
///
/// A single set of settings cannot serve the whole spectrum, and the reason is
/// physical rather than a matter of taste: an NFM channel at 150 MHz is 12.5
/// kHz wide and a Wi-Fi carrier at 2.4 GHz is twenty *megahertz* of OFDM whose
/// level moves several dB between adjacent bins. A merge gap that keeps the
/// first two apart shreds the second into a dozen rows, and one that holds the
/// second together swallows a whole repeater band.
struct RangeProfile {
    std::string name;
    double startHz = 0.0;
    double stopHz = 0.0;

    /// The three that are genuinely a property of what transmits here, and
    /// only those. The threshold is deliberately NOT among them: it is the
    /// control an operator sets while watching the plot and goes on watching,
    /// and having it jump because the sweep moved is a change they did not
    /// make and cannot see the reason for. It stays one value for the plugin,
    /// like the hold.
    float hysteresisDb = 6.0F;
    std::size_t minWidthBins = 3;
    std::size_t mergeGapBins = 8;
};

/// What each band gets before the operator has said otherwise.
///
/// Sized from what actually transmits there. They are a starting point rather
/// than a claim to be right: whatever is changed while a band is swept is
/// saved back into that band's entry, so the second visit starts where the
/// first one finished.
[[nodiscard]] std::vector<RangeProfile> defaultRanges() {
    const auto profile = [](std::string_view name, double startHz, double stopHz,
                            float hysteresisDb, std::size_t minWidthBins,
                            std::size_t mergeGapBins) {
        return RangeProfile{.name = std::string(name),
                            .startHz = startHz,
                            .stopHz = stopHz,
                            .hysteresisDb = hysteresisDb,
                            .minWidthBins = minWidthBins,
                            .mergeGapBins = mergeGapBins};
    };

    return {
        // SSB is three kilohertz wide and CW is a hundred hertz, so almost
        // nothing here should be joined to its neighbour.
        profile("HF", 0.0, 30e6, 6.0F, 2, 3),
        // NFM at 12.5 kHz through broadcast FM at 200.
        profile("VHF", 30e6, 300e6, 6.0F, 3, 6),
        // ISM at 433 and 868, PMR, DVB-T at 8 MHz.
        profile("UHF", 300e6, 1e9, 6.0F, 3, 12),
        // LTE carriers from 1.4 to 20 MHz, and GPS wider still.
        profile("L band", 1e9, 2.3e9, 8.0F, 4, 24),
        // The band this plugin was first pointed at. Wi-Fi is 20 MHz of OFDM
        // and Bluetooth hops across all of it, so both the gap and the
        // hysteresis are wide enough to hold one channel together.
        profile("2.4 GHz ISM", 2.3e9, 2.5e9, 10.0F, 8, 64),
        // Wi-Fi 5 and 6: 20 MHz channels bonded up to 160.
        profile("5 GHz", 2.5e9, 6e9, 10.0F, 8, 96),
        profile("Microwave", 6e9, 40e9, 8.0F, 4, 32),
    };
}

/// The band a swept range sits in: the one holding its centre.
///
/// Its centre rather than its edges, because a sweep almost always straddles a
/// boundary somewhere and the band it is mostly in is the one whose settings
/// it wants.
[[nodiscard]] std::size_t rangeFor(const std::vector<RangeProfile>& ranges, double startHz,
                                   double stopHz) {
    const double centre = (startHz + stopHz) * 0.5;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (centre >= ranges[i].startHz && centre < ranges[i].stopHz) {
            return i;
        }
    }
    return ranges.empty() ? 0 : ranges.size() - 1;
}

/// A range as the settings file carries it. Semicolons, because the one plural
/// the settings API has is an array of strings.
[[nodiscard]] std::string encodeRange(const RangeProfile& range) {
    std::string name = range.name;
    std::erase(name, ';');
    return std::format("{};{};{};{};{};{}", name, range.startHz, range.stopHz, range.hysteresisDb,
                       range.minWidthBins, range.mergeGapBins);
}

[[nodiscard]] std::optional<RangeProfile> decodeRangeProfile(std::string_view text) {
    std::array<std::string_view, 6> parts{};
    std::size_t count = 0;
    std::size_t at = 0;
    while (count < parts.size()) {
        const std::size_t next = text.find(';', at);
        parts[count++] = text.substr(at, next == std::string_view::npos ? next : next - at);
        if (next == std::string_view::npos) {
            break;
        }
        at = next + 1;
    }
    if (count < parts.size()) {
        return std::nullopt;
    }

    const auto number = [](std::string_view field, double fallback) {
        return toDouble(field).value_or(fallback);
    };

    RangeProfile range;
    range.name = std::string(parts[0]);
    range.startHz = number(parts[1], 0.0);
    range.stopHz = number(parts[2], 0.0);
    range.hysteresisDb = static_cast<float>(number(parts[3], 6.0));
    range.minWidthBins = static_cast<std::size_t>(std::max(number(parts[4], 3.0), 1.0));
    range.mergeGapBins = static_cast<std::size_t>(std::max(number(parts[5], 8.0), 0.0));
    return range;
}

#if defined(SWEEPPP_PLUGIN_HAS_UI)
/// The bar button's glyph, in the host's merged icon font: radar, F0437.
constexpr const char* kIcon = "\xF3\xB0\x90\xB7";

/// The row actions: eye-off F0209 for "stop looking here", close F0156 for
/// "forget this one".
constexpr const char* kIgnoreIcon = "\xF3\xB0\x88\x89";
constexpr const char* kForgetIcon = "\xF3\xB0\x85\x96";

/// The two switches that live in the bar as one button each, because both are
/// two-state and both are flipped far more often than they are read: pulse
/// F05BC and chart-line F012A for the graph, waves F078D and ruler F0596 for
/// the threshold. A tooltip carries the words, and the line under the
/// threshold slider says which mode is in force in any case.
constexpr const char* kActivityIcon = "\xF3\xB0\x96\xBC";
constexpr const char* kLevelIcon = "\xF3\xB0\x84\xAA";
constexpr const char* kAboveNoiseIcon = "\xF3\xB0\x9E\x8D";
constexpr const char* kAbsoluteIcon = "\xF3\xB0\x96\x96";
constexpr const char* kAddIcon = "\xF3\xB0\x90\x95";

/// The button's own id, qualified because the status bar shares one ImGui id
/// stack between the host's controls and every plugin's -- see the note on
/// SWEEPPP_UI_SPOT_STATUS_ACTION. Everything inside the window is scoped by the
/// window itself and by the per-row PushID.
constexpr const char* kButtonId = "##org.sweeppp.detections.button";

constexpr std::array<const char*, kSpanCount> kSpanLabels{"1 min", "4 min", "30 min"};
constexpr std::array<const char*, 5> kSortLabels{"First seen", "Last seen", "Frequency", "Level",
                                                 "Sightings"};
#endif

// One `sweeppp_field_t`, filled in. The wrapper has no field builder of its
// own -- a plugin event's fields are the only place they appear -- so the one
// plugin that writes them brings a small overload set with it.
[[nodiscard]] sweeppp_field_t field(std::string_view key, double value) noexcept {
    sweeppp_value_t held{};
    held.struct_size = sizeof(held);
    held.type = SWEEPPP_VALUE_FLOAT;
    held.number = value;
    return sweeppp_field_t{
        .struct_size = sizeof(sweeppp_field_t), .key = plugin::str(key), .value = held};
}

[[nodiscard]] sweeppp_field_t field(std::string_view key, std::int64_t value) noexcept {
    sweeppp_value_t held{};
    held.struct_size = sizeof(held);
    held.type = SWEEPPP_VALUE_INT;
    held.integer = value;
    return sweeppp_field_t{
        .struct_size = sizeof(sweeppp_field_t), .key = plugin::str(key), .value = held};
}

[[nodiscard]] sweeppp_field_t field(std::string_view key, std::string_view value) noexcept {
    sweeppp_value_t held{};
    held.struct_size = sizeof(held);
    held.type = SWEEPPP_VALUE_STRING;
    held.text = plugin::str(value);
    return sweeppp_field_t{
        .struct_size = sizeof(sweeppp_field_t), .key = plugin::str(key), .value = held};
}

/// An ignore range as the settings file carries it: the settings API holds
/// string arrays and nothing plural besides, and `parseFrequency` reads a bare
/// number back as readily as it reads "868.3 MHz".
[[nodiscard]] std::string encodeRange(const IgnoreRange& range) {
    return std::format("{}:{}:{}", range.startHz, range.stopHz, range.note);
}

[[nodiscard]] std::optional<IgnoreRange> decodeRange(std::string_view text) {
    const std::size_t first = text.find(':');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t second = text.find(':', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }

    const auto start = toml_util::parseFrequency(text.substr(0, first));
    const auto stop = toml_util::parseFrequency(text.substr(first + 1, second - first - 1));
    if (!start || !stop) {
        return std::nullopt;
    }

    // The note is whatever is left, colons and all: splitting on the first two
    // separators is what lets an operator write "868 MHz, 2nd harmonic".
    return IgnoreRange{
        .startHz = *start, .stopHz = *stop, .note = std::string(text.substr(second + 1))};
}

#if defined(SWEEPPP_PLUGIN_HAS_UI)
/// A frequency an operator typed, in the unit they were thinking in.
///
/// Megahertz for a bare number, and whatever the suffix says otherwise --
/// "2400", "2.4G" and "2400 MHz" are all the same span. `parseFrequency` alone
/// reads a bare number as hertz, which is right for a config file written by a
/// machine and wrong for a box next to a plot labelled in MHz.
[[nodiscard]] std::optional<double> parseMegahertz(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    const bool bare = std::ranges::all_of(
        text, [](char c) { return (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-'; });
    if (bare) {
        const std::optional<double> value = toDouble(text);
        return value ? std::optional<double>(*value * 1e6) : std::nullopt;
    }

    const auto parsed = toml_util::parseFrequency(text);
    return parsed ? std::optional<double>(*parsed) : std::nullopt;
}
#endif

/// One tracked signal as the UI holds it: the last sample, the operator's tick,
/// and the six rings the graph column draws from.
///
/// Two series at three spans rather than one ring resampled, because the
/// coarser spans accumulate across their ticks -- present-fraction averaged,
/// peak level maxed -- so a burst between two 7.5 second marks still shows on
/// the 30-minute graph instead of falling between two samples.
struct Row {
    TickSample sample;

    /// Paint this one's span on the plot. What the contributor publishes.
    bool painted = false;

    /// What the span is called where it is drawn -- its centre frequency,
    /// rebuilt as the signal moves. A serial number would be shorter and would
    /// say nothing: the frequency is the thing an operator is looking for, and
    /// it is already the only identity a detection has that means anything off
    /// this screen.
    std::string label;
    std::string description;

    std::array<sweeppp::RollingHistory<kHistoryLength>, kSpanCount> activity;
    std::array<sweeppp::RollingHistory<kHistoryLength>, kSpanCount> level;

    std::array<float, kSpanCount> pendingActivity{};
    std::array<float, kSpanCount> pendingLevel{};
    std::array<std::uint32_t, kSpanCount> pendingTicks{};

    void feed(const TickSample& incoming) {
        sample = incoming;
        for (std::size_t span = 0; span < kSpanCount; ++span) {
            pendingActivity[span] += incoming.presentFraction;
            pendingLevel[span] = pendingTicks[span] == 0
                                     ? incoming.peakDbfs
                                     : std::max(pendingLevel[span], incoming.peakDbfs);
            ++pendingTicks[span];

            if (pendingTicks[span] < kSpanTicks[span]) {
                continue;
            }

            activity[span].push(pendingActivity[span] / static_cast<float>(pendingTicks[span]));
            level[span].push(pendingLevel[span]);
            pendingActivity[span] = 0.0F;
            pendingLevel[span] = 0.0F;
            pendingTicks[span] = 0;
        }
    }
};

#if defined(SWEEPPP_PLUGIN_HAS_UI)

/// Width of the label column every settings row in this panel shares.
///
/// The host's own panels are laid out this way and the helper that does it
/// lives in an anonymous namespace in `MainWindowPanels.cpp`, which a plugin
/// cannot reach. Reimplemented here rather than left to ImGui's default, which
/// puts a widget's label to its *right* -- so a column of controls comes out
/// with its captions trailing behind it, none of them aligned with each other.
constexpr float kLabelWidth = 104.0F;

/// A row's label, leaving the cursor where that row's control goes.
///
/// Measured from where the row actually starts rather than from the window's
/// left edge, because `SameLine`'s offset ignores the current indent -- a row
/// drawn inside `Indent()` would otherwise put its control one indent early,
/// over the tail of its own label.
void rowLabel(const char* label, const char* tooltip = nullptr) {
    const float rowStart = ImGui::GetCursorPosX();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0F);
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // A label wider than the column pushes its control right rather than
    // having it drawn on top of the last few characters.
    const float used = ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(rowStart + std::max(kLabelWidth, used));
}

/// A dim line under a row, aligned to the control column rather than the
/// margin: it explains that row's value, not the section.
void rowCaption(const std::string& text) {
    ImGui::Indent(kLabelWidth);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::Unindent(kLabelWidth);
}

/// How long ago, as a row's "last seen" column says it.
///
/// Quantised, and that is the whole point of the function. `formatDuration` is
/// built for latency readouts and carries three decimals, so an age recomputed
/// from the clock every frame reads as a blur of digits rather than as a
/// number -- and the eye is dragged to whichever row is churning hardest,
/// which is exactly the wrong row to be looking at.
///
/// The step grows with the magnitude, so the column changes at most every five
/// seconds near the bottom and once a minute above that. Nothing is lost: to
/// the second is not a precision anyone reads "when did this last transmit" to.
[[nodiscard]] std::string ageText(std::uint64_t nowNs, std::uint64_t thenNs) {
    if (thenNs == 0 || nowNs <= thenNs) {
        return "now";
    }

    const double seconds = sweeppp::nsToSeconds(nowNs - thenNs);
    if (seconds < 3.0) {
        return "now";
    }
    if (seconds < 60.0) {
        return std::format("{} s", static_cast<int>(seconds / 5.0) * 5);
    }
    if (seconds < 3600.0) {
        return std::format("{} min", static_cast<int>(seconds / 60.0));
    }
    return std::format("{} h", static_cast<int>(seconds / 3600.0));
}

/// The same age in a sentence rather than in a column.
[[nodiscard]] std::string sinceText(std::uint64_t nowNs, std::uint64_t thenNs) {
    const std::string age = ageText(nowNs, thenNs);
    return age == "now" ? std::string("just now") : std::format("{} ago", age);
}

/// How often it comes back, as the interval column says it.
///
/// Zero is the model's way of saying there is no interval -- it has never
/// stopped, or it has only ever been seen once -- and "every 0 s" would be a
/// different claim entirely.
[[nodiscard]] std::string intervalText(const TickSample& sample) {
    if (sample.meanIntervalSeconds <= 0.0) {
        return sample.appearances <= 1 && sample.hits > 1 ? "continuous" : "-";
    }
    return std::format("every {}", sweeppp::formatDuration(sample.meanIntervalSeconds));
}

#endif

// ------------------------------------------------------------------ plugin

class DetectionsPlugin final : public plugin::Plugin {
public:
    [[nodiscard]] bool activate(plugin::Host& host) override;
    void deactivate() override;
    void saveProfile(const plugin::ProfileWriter& writer) override;
    void loadProfile(const plugin::ProfileReader& reader) override;

    // ---- frames ----------------------------------------------------------

    /// One published frame, on the host's worker thread for this facet.
    void onFrame(const sweeppp_frame_t& frame);

    // ---- contributions ---------------------------------------------------
    //
    // Answered from `m_rows`, which the UI thread owns and which the contributor
    // is only ever asked about from that same thread -- the plot, the marker
    // readout and the inset all query while drawing. So no lock, which is what
    // keeps a detector falling behind from ever stalling the spectrum.

    [[nodiscard]] std::uint32_t datasetCount() const { return 1; }
    [[nodiscard]] std::string_view datasetName(std::uint32_t) const { return "Detections"; }
    [[nodiscard]] std::string_view datasetDescription(std::uint32_t) const {
        return "Signals seen over the threshold, as they are ticked in the panel";
    }
    [[nodiscard]] std::uint32_t activeDataset() const { return 0; }
    [[nodiscard]] bool selectDataset(std::uint32_t index) { return index == 0; }

    [[nodiscard]] std::uint32_t contributionsIn(double fromHz, double toHz,
                                                sweeppp_contribution_t* out,
                                                std::uint32_t capacity) const;
    [[nodiscard]] std::uint32_t contributionsAt(double hz, sweeppp_contribution_t* out,
                                                std::uint32_t capacity) const;

    /// The operator ctrl-clicked this span on the plot: untick its row, which
    /// is the same gesture the panel's checkbox is, reached from the plot.
    [[nodiscard]] bool hide(const sweeppp_contribution_t& which);

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    // ---- ui --------------------------------------------------------------

    /// The button in the status bar, after the host's own.
    ///
    /// Dispatched every frame whether or not the window is open, which is what
    /// makes it the place the samples are folded in: the graphs must not lose
    /// their history to the panel having been closed.
    void drawStatusAction();
    void drawWindow();

    /// The ticked detections, drawn on the spectrum.
    ///
    /// Public, and it has to be: `makeUiVtable` detects the slots a plugin
    /// fills with a `requires` expression evaluated outside the class, so a
    /// private one is not merely inaccessible -- it is invisible, the vtable
    /// entry is left null, and the facet silently draws nothing.
    void drawOverlay(const plugin::PlotContext& context);
#endif

private:
    /// What the UI asks the worker to do to the detector it owns.
    struct Command {
        enum class Kind : std::uint8_t { Forget, ClearHistory };
        Kind kind = Kind::Forget;
        int id = 0;
    };

    void persist();
    void applyConfig();

    /// Pushes the band being edited into the detector, when it is also the one
    /// being swept. Editing a band the sweep is not in changes nothing now.
    void commitRange();

    /// Copies a band's three tuning values into the running config. The
    /// threshold and the hold are the plugin's, not the band's.
    void adoptRange(const RangeProfile& range);

    /// Adopts the tuning of whichever band the sweep has moved into.
    void followRange(double startHz, double stopHz);

    /// Writes whatever the operator just changed back into the band it was
    /// changed in, so the next visit starts where this one finished.
    void rememberRange();

    void applyIgnored();
    void queue(Command command);

    /// Writes the transitions this scan produced, on the worker thread.
    void writeTransitions(std::vector<Transition> transitions);

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    void foldSamples();
    void drawMenuBar();
    void drawSections();
    void drawRanges();
    void drawTuning(RangeProfile& range);
    void drawIgnoreRanges();
    void drawTable(const char* id, bool wantActive, float height);
    void drawSparkline(const Row& row);
    [[nodiscard]] std::vector<std::size_t> ordered(bool wantActive) const;
    void drawFooter();

    [[nodiscard]] sweeppp::Status exportCsv(const std::filesystem::path& path) const;
    [[nodiscard]] sweeppp::Status exportToml(const std::filesystem::path& path) const;
    void exportTo(std::string_view extension);

    /// Monotonic and wall clock taken together, so an age measured on the one
    /// can be written as a time of day on the other.
    [[nodiscard]] std::uint64_t wallOf(std::uint64_t monotonicNs) const;
#endif

    plugin::Host m_host;
    sweeppp::PluginSettings m_settings;

    // ---- the worker's own ------------------------------------------------

    Detector m_detector;
    std::uint64_t m_lastTickNs = 0;

    // ---- shared ----------------------------------------------------------
    //
    // One mutex, held for a handful of scalars at a time. The worker takes it
    // once per frame to pick up whatever the operator changed, and once per
    // tick to hand over the samples; the UI takes it only when something is
    // actually changed. `AppState::HealthTrace` is the host's own answer to
    // the same problem, and this is the same shape.

    mutable std::mutex m_stateMutex;
    std::vector<TickSample> m_pending;
    bool m_pendingValid = false;
    DetectorConfig m_sharedConfig;
    std::vector<IgnoreRange> m_sharedIgnored;
    bool m_configDirty = false;
    bool m_ignoredDirty = false;
    std::vector<Command> m_commands;
    float m_floorDbfs = 0.0F;
    float m_thresholdDbfs = 0.0F;
    double m_binWidthHz = 0.0;
    double m_gridStartHz = 0.0;
    double m_gridStopHz = 0.0;
    std::uint64_t m_droppedFrames = 0;

    // ---- the UI thread's own ---------------------------------------------

    DetectorConfig m_config;
    std::vector<IgnoreRange> m_ignored;
    std::vector<Row> m_rows;

    /// The bands, and which of them the sweep is currently in. `m_config` is
    /// always that band's settings: changing a control changes the band.
    std::vector<RangeProfile> m_ranges;

    /// Which band the editor below the list is working on -- not necessarily
    /// the one being swept. Two ideas, because they are two questions: what
    /// the detector is using now, and what the operator is typing into.
    std::size_t m_editingRange = 0;

    /// How long a signal stays in the Active list after its last sighting.
    ///
    /// One value for the whole plugin rather than one per band: it is not a
    /// property of the radio spectrum, it is how long the operator wants to go
    /// on seeing something that has stopped. Short, and a busy band churns
    /// rows between the two tables faster than they can be read.
    double m_holdSeconds = 120.0;

    /// Past the end until the first frame says which band is being swept, so
    /// that first frame applies a band's settings even when it turns out to be
    /// the first one in the list. Every reader is already guarded on it, which
    /// is what makes "no band yet" a state rather than a special case.
    std::size_t m_activeRange = std::numeric_limits<std::size_t>::max();
    bool m_followRanges = true;

    /// This plugin's own draw switch, and deliberately not the host's channel
    /// one: a detection is not a channel, and an operator who wants the flags
    /// off does not thereby want to stop seeing what is transmitting.
    bool m_showOnPlot = true;

    bool m_windowOpen = false;
    GraphSeries m_series = GraphSeries::Activity;
    SortOrder m_sort = SortOrder::FirstSeen;
    std::size_t m_span = 1;

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    /// The floor as the last frame reported it, for the level graph's colour.
    /// Read once per frame in the controls, which are drawn before the tables.
    float m_uiFloorDbfs = 0.0F;

    /// ImPlot wants a pointer and a count, and a `RollingHistory` is a ring.
    /// One buffer, refilled per sparkline, rather than one per row per frame.
    std::vector<float> m_scratch;

    std::array<char, 64> m_draftBandName{};
    std::array<char, 32> m_draftStart{};
    std::array<char, 32> m_draftStop{};
    std::array<char, 64> m_draftNote{};
    std::string m_message;
#endif
};

// ------------------------------------------------------------- the vtables

/// The frame processor's vtable, written by hand.
///
/// There is no `makeFrameProcessorVtable<T>`: this is the first user of the
/// facet in the tree, and one hand-written vtable is a smaller thing to add
/// than a helper with a single caller. The try/catch is not decoration -- it
/// is what `detail::guard` does for every UI slot, and an exception unwinding
/// out of a C callback is undefined behaviour on the way back into the host.
const sweeppp_frame_processor_vtable_t& frameVtable() {
    static const sweeppp_frame_processor_vtable_t value{
        .struct_size = sizeof(sweeppp_frame_processor_vtable_t),
        .on_frame =
            [](void* instance, const sweeppp_frame_t* frame) {
                if (instance == nullptr || frame == nullptr) {
                    return;
                }
                try {
                    static_cast<DetectionsPlugin*>(instance)->onFrame(*frame);
                } catch (...) {
                    // Swallowed rather than reported: this runs on the host's
                    // worker with no way to log that is safe from here, and a
                    // dropped scan is a smaller failure than a dropped host.
                }
            },
    };
    return value;
}

const sweeppp_contributor_vtable_t& contributorVtable() {
    // RENDER_NONE: the host keeps asking what is at a frequency -- the marker
    // readout and the hover list still name a detection -- but the painting is
    // this plugin's own. A detection is not an allocation or a channel, and
    // drawn as one it is indistinguishable from the band plan sitting under
    // it; what it wants is the look of something the operator selected.
    static const sweeppp_contributor_vtable_t value =
        plugin::makeContributorVtable<DetectionsPlugin>(SWEEPPP_CONTRIBUTION_RENDER_NONE);
    return value;
}

#if defined(SWEEPPP_PLUGIN_HAS_UI)
const sweeppp_ui_vtable_t& uiVtable() {
    // A button in the status bar and a window, and nothing else.
    //
    // A window rather than a popover, and the difference is what this is for: a
    // popover closes the moment the operator clicks the spectrum, which is
    // exactly what they do while watching a list that is telling them where to
    // look.
    static const sweeppp_ui_vtable_t value = plugin::makeUiVtable<DetectionsPlugin>(
        SWEEPPP_UI_SPOT_BIT(SWEEPPP_UI_SPOT_STATUS_ACTION) |
            SWEEPPP_UI_SPOT_BIT(SWEEPPP_UI_SPOT_WINDOW) |
            SWEEPPP_UI_SPOT_BIT(SWEEPPP_UI_SPOT_SPECTRUM_OVERLAY),
        // No `governs`: this button opens a window rather than switching a
        // kind of contribution, so there is nothing for a keystroke to route
        // here -- what the ticked rows paint follows the channel switch the
        // channel list's button already owns.
        SWEEPPP_UI_LAYER_UNDER);
    return value;
}

constexpr std::size_t kFacetCount = 3;
#else
constexpr std::size_t kFacetCount = 2;
#endif

std::span<const sweeppp_facet_t> facets() {
    static const std::array<sweeppp_facet_t, kFacetCount> value{
        plugin::facet(SWEEPPP_FACET_FRAME_PROCESSOR, "detector", "Signal detector",
                      "Scans every published frame for signals over the threshold", &frameVtable()),
        plugin::facet(SWEEPPP_FACET_CONTRIBUTOR, "detections", "Detected signals",
                      "The detections the operator has ticked, as spans on the plot",
                      &contributorVtable()),
#if defined(SWEEPPP_PLUGIN_HAS_UI)
        plugin::facet(SWEEPPP_FACET_UI_EXTENSION, "panel", "Detections button and panel",
                      "The bar's detections button, the list behind it, and the ticked spans "
                      "on the spectrum",
                      &uiVtable()),
#endif
    };
    return value;
}

// --------------------------------------------------------------- lifetime

bool DetectionsPlugin::activate(plugin::Host& host) {
    m_host = host;

    std::string settingsProblem;
    m_settings = sweeppp::PluginSettings::load(host.settingsPath(), &settingsProblem);
    if (!settingsProblem.empty()) {
        host.warn("{}", settingsProblem);
    }

    m_config.mode = modeFrom(m_settings.getString("mode", modeKey(m_config.mode)));
    m_config.absoluteDbfs = m_settings.getFloat("absolute_dbfs", m_config.absoluteDbfs);
    m_config.marginDb = m_settings.getFloat("margin_db", m_config.marginDb);
    m_config.hysteresisDb = m_settings.getFloat("hysteresis_db", m_config.hysteresisDb);
    m_config.minWidthBins = static_cast<std::size_t>(std::max<std::int64_t>(
        m_settings.getInt("min_width_bins", static_cast<std::int64_t>(m_config.minWidthBins)), 1));
    m_config.mergeGapBins = static_cast<std::size_t>(std::max<std::int64_t>(
        m_settings.getInt("merge_gap_bins", static_cast<std::int64_t>(m_config.mergeGapBins)), 0));
    m_holdSeconds =
        std::clamp(m_settings.getDouble("hold_s", m_holdSeconds), kHoldMinSeconds, kHoldMaxSeconds);
    m_config.dropAfterSeconds = m_holdSeconds;

    for (const std::string& encoded : m_settings.getStringArray("ranges")) {
        if (std::optional<RangeProfile> range = decodeRangeProfile(encoded)) {
            m_ranges.push_back(std::move(*range));
        }
    }
    if (m_ranges.empty()) {
        m_ranges = defaultRanges();
    }
    m_followRanges = m_settings.getBool("follow_ranges", m_followRanges);
    m_showOnPlot = m_settings.getBool("show_on_plot", m_showOnPlot);

    m_series = seriesFrom(m_settings.getString("graph_series", seriesKey(m_series)));
    m_sort = sortFrom(m_settings.getString("sort", sortKey(m_sort)));
    m_span = std::min(static_cast<std::size_t>(std::max<std::int64_t>(
                          m_settings.getInt("graph_span", static_cast<std::int64_t>(m_span)), 0)),
                      kSpanCount - 1);
    m_windowOpen = m_settings.getBool("window_open", m_windowOpen);

    for (const std::string& encoded : m_settings.getStringArray("ignore")) {
        if (std::optional<IgnoreRange> range = decodeRange(encoded)) {
            m_ignored.push_back(std::move(*range));
        } else {
            host.warn("ignoring an unreadable ignore range: {}", encoded);
        }
    }

    // Straight into the detector rather than through the shared copy: nothing
    // else is running yet, so there is no worker to hand it to.
    m_detector.setConfig(m_config);
    m_detector.setIgnored(m_ignored);
    m_sharedConfig = m_config;
    m_sharedIgnored = m_ignored;

    // UNAVAILABLE here means the host has no frame bus at all -- the listing
    // one the CLI stands up to describe what is installed -- which is not a
    // failure and already carries its own reason into that listing. Anything
    // else is worth saying, because the panel would come up empty for ever
    // with nothing to explain why.
    if (const sweeppp_plugin_status_t status = host.registerFacet(facets()[0], this);
        status != SWEEPPP_PLUGIN_OK && status != SWEEPPP_PLUGIN_ERR_UNAVAILABLE) {
        host.warn("the detector facet could not register: nothing will be scanned");
    }

    if (const sweeppp_plugin_status_t status = host.registerFacet(facets()[1], this);
        status != SWEEPPP_PLUGIN_OK) {
        host.warn("the detections data facet could not register");
    }

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    // A refusal here is reported and survivable, following the channel list:
    // the detector keeps scanning and the session keeps its events, because
    // detection without drawing is still worth having.
    if (std::string reason; !host.adoptImGui(reason)) {
        host.warn("not drawing: {}", reason);
        host.reportFacet(facets()[2], reason);
        return true;
    }

    if (const sweeppp_plugin_status_t status = host.registerFacet(facets()[2], this);
        status != SWEEPPP_PLUGIN_OK) {
        host.warn("the detections panel facet could not register");
    }
#endif

    return true;
}

void DetectionsPlugin::deactivate() {
    // Nothing to stop: the worker is the host's, and it has already withdrawn
    // every facet by the time this runs. The settings were written when they
    // changed rather than here, because a plugin that saves only on the way out
    // loses everything to the one exit that never runs it.
    const std::lock_guard lock(m_stateMutex);
    m_pending.clear();
    m_pendingValid = false;
    m_commands.clear();
}

void DetectionsPlugin::saveProfile(const plugin::ProfileWriter& writer) {
    // The survey setup, and not the detections themselves: a profile is how
    // the instrument was arranged, not what it happened to hear.
    writer.set("detections.mode", modeKey(m_config.mode));
    writer.set("detections.absolute_dbfs", static_cast<double>(m_config.absoluteDbfs));
    writer.set("detections.margin_db", static_cast<double>(m_config.marginDb));
    writer.set("detections.hysteresis_db", static_cast<double>(m_config.hysteresisDb));
    writer.set("detections.min_width_bins", static_cast<std::int64_t>(m_config.minWidthBins));
}

void DetectionsPlugin::loadProfile(const plugin::ProfileReader& reader) {
    m_config.mode = modeFrom(reader.getString("detections.mode", modeKey(m_config.mode)));
    m_config.absoluteDbfs = static_cast<float>(
        reader.getDouble("detections.absolute_dbfs", static_cast<double>(m_config.absoluteDbfs)));
    m_config.marginDb = static_cast<float>(
        reader.getDouble("detections.margin_db", static_cast<double>(m_config.marginDb)));
    m_config.hysteresisDb = static_cast<float>(
        reader.getDouble("detections.hysteresis_db", static_cast<double>(m_config.hysteresisDb)));
    m_config.minWidthBins = static_cast<std::size_t>(
        std::max<std::int64_t>(reader.getInt("detections.min_width_bins",
                                             static_cast<std::int64_t>(m_config.minWidthBins)),
                               1));

    applyConfig();
    persist();
}

// ---------------------------------------------------------------- settings

void DetectionsPlugin::persist() {
    m_settings.set("mode", modeKey(m_config.mode));
    m_settings.set("absolute_dbfs", static_cast<double>(m_config.absoluteDbfs));
    m_settings.set("margin_db", static_cast<double>(m_config.marginDb));
    m_settings.set("hysteresis_db", static_cast<double>(m_config.hysteresisDb));
    m_settings.set("min_width_bins", static_cast<std::int64_t>(m_config.minWidthBins));
    m_settings.set("merge_gap_bins", static_cast<std::int64_t>(m_config.mergeGapBins));
    m_settings.set("hold_s", m_holdSeconds);
    m_settings.set("graph_series", seriesKey(m_series));
    m_settings.set("sort", sortKey(m_sort));
    m_settings.set("follow_ranges", m_followRanges);
    m_settings.set("show_on_plot", m_showOnPlot);

    std::vector<std::string> ranges;
    ranges.reserve(m_ranges.size());
    for (const RangeProfile& range : m_ranges) {
        ranges.push_back(encodeRange(range));
    }
    m_settings.set("ranges", std::span<const std::string>(ranges));
    m_settings.set("graph_span", static_cast<std::int64_t>(m_span));
    m_settings.set("window_open", m_windowOpen);

    std::vector<std::string> encoded;
    encoded.reserve(m_ignored.size());
    for (const IgnoreRange& range : m_ignored) {
        encoded.push_back(encodeRange(range));
    }
    m_settings.set("ignore", std::span<const std::string>(encoded));

    if (const sweeppp::Status saved = m_settings.save(); !saved) {
        m_host.warn("{}", saved.error().describe());
    }
}

void DetectionsPlugin::followRange(double startHz, double stopHz) {
    if (!m_followRanges || m_ranges.empty() || stopHz <= startHz) {
        return;
    }

    const std::size_t wanted = rangeFor(m_ranges, startHz, stopHz);
    if (wanted == m_activeRange) {
        return;
    }

    m_activeRange = wanted;
    m_editingRange = wanted;
    adoptRange(m_ranges[wanted]);
    applyConfig();
    m_host.info("tuning for {}", m_ranges[wanted].name);
}

void DetectionsPlugin::commitRange() {
    if (m_editingRange < m_ranges.size() && m_editingRange == m_activeRange) {
        adoptRange(m_ranges[m_editingRange]);
        applyConfig();
    }
    persist();
}

void DetectionsPlugin::adoptRange(const RangeProfile& range) {
    m_config.hysteresisDb = range.hysteresisDb;
    m_config.minWidthBins = range.minWidthBins;
    m_config.mergeGapBins = range.mergeGapBins;
}

void DetectionsPlugin::rememberRange() {
    if (m_activeRange < m_ranges.size()) {
        RangeProfile& range = m_ranges[m_activeRange];
        range.hysteresisDb = m_config.hysteresisDb;
        range.minWidthBins = m_config.minWidthBins;
        range.mergeGapBins = m_config.mergeGapBins;
    }
}

void DetectionsPlugin::applyConfig() {
    rememberRange();
    const std::lock_guard lock(m_stateMutex);
    m_sharedConfig = m_config;
    m_configDirty = true;
}

void DetectionsPlugin::applyIgnored() {
    const std::lock_guard lock(m_stateMutex);
    m_sharedIgnored = m_ignored;
    m_ignoredDirty = true;
}

void DetectionsPlugin::queue(Command command) {
    const std::lock_guard lock(m_stateMutex);
    m_commands.push_back(command);
}

// ------------------------------------------------------------- the worker

void DetectionsPlugin::onFrame(const sweeppp_frame_t& frame) {
    {
        const std::lock_guard lock(m_stateMutex);
        if (m_configDirty) {
            m_detector.setConfig(m_sharedConfig);
            m_configDirty = false;
        }
        if (m_ignoredDirty) {
            m_detector.setIgnored(m_sharedIgnored);
            m_ignoredDirty = false;
        }
        for (const Command& command : m_commands) {
            if (command.kind == Command::Kind::Forget) {
                m_detector.forget(command.id);
            } else {
                m_detector.clearHistory();
            }
        }
        m_commands.clear();
    }

    m_detector.onFrame(frame.host_time_ns, frame.sequence, frame.start_hz, frame.bin_width_hz,
                       std::span<const float>(frame.bins_dbfs, frame.bin_count),
                       frame.pass_complete != 0);

    writeTransitions(m_detector.takeTransitions());

    if (m_lastTickNs != 0 && frame.host_time_ns - m_lastTickNs < kTickIntervalNs) {
        return;
    }
    m_lastTickNs = frame.host_time_ns;

    std::vector<TickSample> samples = m_detector.tick(frame.host_time_ns);
    writeTransitions(m_detector.takeTransitions());

    const std::lock_guard lock(m_stateMutex);
    m_pending = std::move(samples);
    m_pendingValid = true;
    m_floorDbfs = m_detector.noiseFloorDbfs();
    m_thresholdDbfs = m_detector.thresholdDbfs();
    m_binWidthHz = m_detector.binWidthHz();
    m_gridStartHz = frame.start_hz;
    m_gridStopHz = frame.start_hz + frame.bin_width_hz * static_cast<double>(frame.bin_count);
    m_droppedFrames = m_detector.droppedFrames();
}

void DetectionsPlugin::writeTransitions(std::vector<Transition> transitions) {
    // Transitions only, never per scan. `publishMarkerChanges` learned this the
    // hard way: a level that breathes publishes continuously, and a session
    // full of one event per frame is a session nobody can read.
    if (transitions.empty() || !m_host.sessionOpen()) {
        return;
    }

    for (const Transition& transition : transitions) {
        const Detection& found = transition.detection;

        // A number rather than a name, and it is the only thing here that is
        // not a measurement: what joins an `appeared` to the `gone` that ends
        // it. Reading the file back, the frequency beside it is what says
        // which signal this was.
        if (transition.kind == Transition::Kind::Appeared) {
            const std::array fields{
                field("id", static_cast<std::int64_t>(found.id)),
                field("center_hz", found.centerHz),
                field("start_hz", found.startHz),
                field("stop_hz", found.stopHz),
                field("peak_dbfs", static_cast<double>(found.peakDbfs)),
                field("threshold_dbfs", static_cast<double>(transition.thresholdDbfs)),
                field("mode", modeKey(m_detector.config().mode)),
            };
            (void)m_host.writeSessionEvent("detection.appeared", fields);
            continue;
        }

        const double durationSeconds =
            found.lastSeenNs > found.firstSeenNs
                ? sweeppp::nsToSeconds(found.lastSeenNs - found.firstSeenNs)
                : 0.0;
        const std::array fields{
            field("id", static_cast<std::int64_t>(found.id)),
            field("center_hz", found.centerHz),
            field("duration_s", durationSeconds),
            field("hits", static_cast<std::int64_t>(found.hits)),
            field("appearances", static_cast<std::int64_t>(found.appearances)),
            field("strongest_dbfs", static_cast<double>(found.strongestDbfs)),
        };
        (void)m_host.writeSessionEvent("detection.gone", fields);
    }
}

// --------------------------------------------------------- contributions

std::uint32_t DetectionsPlugin::contributionsIn(double fromHz, double toHz,
                                                sweeppp_contribution_t* out,
                                                std::uint32_t capacity) const {
    std::uint32_t total = 0;
    for (const Row& row : m_rows) {
        if (!row.painted || row.sample.stopHz < fromHz || row.sample.startHz > toHz) {
            continue;
        }
        // Written while there is room, counted always: the full count is what
        // lets the host size a buffer with one call and fill it with a second.
        if (total < capacity) {
            out[total] = plugin::contribution(SWEEPPP_CONTRIBUTION_CHANNEL, row.label, "Detections",
                                              row.sample.startHz, row.sample.stopHz, kSpanColor,
                                              row.description);
        }
        ++total;
    }
    return total;
}

std::uint32_t DetectionsPlugin::contributionsAt(double hz, sweeppp_contribution_t* out,
                                                std::uint32_t capacity) const {
    return contributionsIn(hz, hz, out, capacity);
}

bool DetectionsPlugin::hide(const sweeppp_contribution_t& which) {
    // Matched on the span rather than on the label, because the label is a
    // frequency and a frequency moves: by the time a click arrives the row may
    // already be reading a bin further along. The span it was drawn at is what
    // the operator actually pointed at.
    const auto found = std::ranges::find_if(m_rows, [&which](const Row& row) {
        return row.painted && row.sample.startHz <= which.stop_hz &&
               row.sample.stopHz >= which.start_hz;
    });
    if (found == m_rows.end()) {
        return false;
    }
    found->painted = false;
    return true;
}

#if defined(SWEEPPP_PLUGIN_HAS_UI)

// --------------------------------------------------------------------- ui

void DetectionsPlugin::foldSamples() {
    std::vector<TickSample> samples;
    {
        const std::lock_guard lock(m_stateMutex);
        if (!m_pendingValid) {
            return;
        }
        samples = std::move(m_pending);
        m_pending.clear();
        m_pendingValid = false;
    }

    // Matched by id, never by position: a detection dropped from the middle of
    // the detector's list would otherwise shift every row below it onto
    // somebody else's history.
    std::vector<Row> updated;
    updated.reserve(samples.size());

    for (const TickSample& sample : samples) {
        const auto existing =
            std::ranges::find(m_rows, sample.id, [](const Row& row) { return row.sample.id; });

        Row row;
        if (existing != m_rows.end()) {
            row = std::move(*existing);
        }

        row.feed(sample);
        row.label = toml_util::formatFrequency(sample.centerHz, 3);
        row.description = std::format(
            "{:.1f} dBFS, {} wide, seen {} times", static_cast<double>(sample.peakDbfs),
            toml_util::formatFrequencyShort(sample.stopHz - sample.startHz), sample.hits);
        updated.push_back(std::move(row));
    }

    m_rows = std::move(updated);
}

void DetectionsPlugin::drawStatusAction() {
    foldSamples();

    // The sweep may have moved to another band since the last frame. Done here
    // rather than on the worker because the settings, the bands and the panel
    // are all this thread's, and the worker is handed the result like any
    // other change the operator could have made.
    {
        double startHz = 0.0;
        double stopHz = 0.0;
        {
            const std::lock_guard lock(m_stateMutex);
            startHz = m_gridStartHz;
            stopHz = m_gridStopHz;
        }
        followRange(startHz, stopHz);
    }

    const auto active = static_cast<std::size_t>(
        std::ranges::count_if(m_rows, [](const Row& row) { return row.sample.active; }));

    std::string label = m_host.icon(kIcon, "Detections");
    if (active > 0) {
        label += std::format(" {}", active);
    }
    label += kButtonId;

    if (ImGui::Button(label.c_str())) {
        m_windowOpen = !m_windowOpen;
        persist();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("What has been transmitting here, and how often.\n\n"
                          "%zu on air now, %zu heard earlier.",
                          active, m_rows.size() - active);
    }

    // The bar lays its controls out in a row and the host has no way to know
    // this button was submitted, so the plugin leaves the cursor where the next
    // one goes.
    ImGui::SameLine();
}

void DetectionsPlugin::drawMenuBar() {
    float floorDbfs = 0.0F;
    float thresholdDbfs = 0.0F;
    double binWidthHz = 0.0;
    std::uint64_t dropped = 0;
    {
        const std::lock_guard lock(m_stateMutex);
        floorDbfs = m_floorDbfs;
        thresholdDbfs = m_thresholdDbfs;
        binWidthHz = m_binWidthHz;
        dropped = m_droppedFrames;
    }
    m_uiFloorDbfs = floorDbfs;
    const bool measured = binWidthHz > 0.0;

    if (!ImGui::BeginMenuBar()) {
        return;
    }

    // Every control the panel has, on one row painted with `ImGuiCol_MenuBarBg`
    // -- which the application sets from its theme's header colour, the same
    // one the toolbar and the status bar carry. What is left below it is the
    // two lists, which is what the window is for.
    //
    // The counts and the band are in the title instead: they are what the
    // panel currently IS, not something to operate.
    //
    // Each either/or is BOTH of its options, with the one in force lit --
    // never one button showing the current state. A lone icon cannot say
    // whether it is what you have or what you would get by pressing it, and
    // the reader has no way to find out but to press it and see.

    if (chrome::toolbarToggle(m_host.icon(kIcon, "On plot").append("##onplot").c_str(),
                              m_showOnPlot)) {
        m_showOnPlot = !m_showOnPlot;
        persist();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draw the signals you have ticked onto the spectrum.\n\n"
                          "Separate from the channel flags: hiding those does not hide these.");
    }

    ImGui::Separator();

    const bool absolute = m_config.mode == ThresholdMode::Absolute;

    if (chrome::toolbarToggle(m_host.icon(kAboveNoiseIcon, "Above noise").append("##noise").c_str(),
                              !absolute) &&
        absolute) {
        m_config.mode = ThresholdMode::AboveNoise;
        applyConfig();
        persist();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("List anything a set amount louder than the background noise.\n\n"
                          "Use this when you do not know how strong the signal is. It keeps "
                          "working when you change the gain or move to a quieter band.");
    }

    if (chrome::toolbarToggle(m_host.icon(kAbsoluteIcon, "Fixed").append("##absolute").c_str(),
                              absolute) &&
        !absolute) {
        m_config.mode = ThresholdMode::Absolute;
        applyConfig();
        persist();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("List anything louder than a level you set.\n\n"
                          "Use this when you know how strong the signal you are looking for "
                          "is. It does not follow the noise, so a gain change will change what "
                          "shows up.");
    }

    // The resulting threshold is baked into the slider's own label, so the one
    // number that decides what is on the list is on the control that sets it.
    // ImGui printf-formats this with the value, and there is exactly one `%` in
    // it either way.
    ImGui::SetNextItemWidth(196.0F);
    if (absolute) {
        const std::string label =
            measured ? std::format("%.1f dBFS \u00b7 noise {:.1f}", static_cast<double>(floorDbfs))
                     : std::string("%.1f dBFS");
        if (ImGui::SliderFloat("##absolute", &m_config.absoluteDbfs, -140.0F, 0.0F,
                               label.c_str())) {
            applyConfig();
            persist();
        }
    } else {
        const std::string label = measured ? std::format("+%.1f dB \u2192 {:.1f} dBFS",
                                                         static_cast<double>(thresholdDbfs))
                                           : std::string("+%.1f dB over the noise");
        if (ImGui::SliderFloat("##margin", &m_config.marginDb, 1.0F, 40.0F, label.c_str())) {
            applyConfig();
            persist();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(measured ? "How loud something has to be to appear in the list.\n\n"
                                     "The background noise is at %.1f dBFS, so anything louder "
                                     "than %.1f dBFS is being listed.\n\n"
                                     "Drag to change it, or ctrl-click to type a number."
                                   : "How loud something has to be to appear in the list.\n\n"
                                     "Nothing has been swept yet, so the background noise is "
                                     "not known.\n\nDrag to change it, or ctrl-click to type "
                                     "a number.",
                          static_cast<double>(floorDbfs), static_cast<double>(thresholdDbfs));
    }

    ImGui::Separator();

    const bool activity = m_series == GraphSeries::Activity;

    if (chrome::toolbarToggle(m_host.icon(kActivityIcon, "When").append("##activity").c_str(),
                              activity) &&
        !activity) {
        m_series = GraphSeries::Activity;
        persist();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Each row's graph shows WHEN the signal was on air.\n\nHigh means "
                          "it was there, low means it was not -- so a signal that comes and "
                          "goes reads as a row of blocks.");
    }

    if (chrome::toolbarToggle(m_host.icon(kLevelIcon, "Level").append("##level").c_str(),
                              !activity) &&
        activity) {
        m_series = GraphSeries::PeakLevel;
        persist();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Each row's graph shows HOW STRONG the signal was.\n\nThe colour is "
                          "the strength too, and means the same on every row: blue is close to "
                          "the noise, red is far above it.");
    }

    ImGui::SetNextItemWidth(80.0F);
    if (ImGui::BeginCombo("##span", kSpanLabels[m_span])) {
        for (std::size_t i = 0; i < kSpanCount; ++i) {
            if (ImGui::Selectable(kSpanLabels[i], i == m_span)) {
                m_span = i;
                persist();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How far back the graphs go.");
    }

    ImGui::Separator();

    ImGui::SetNextItemWidth(108.0F);
    if (ImGui::BeginCombo("##sort", kSortLabels[static_cast<std::size_t>(m_sort)])) {
        for (std::size_t i = 0; i < kSortLabels.size(); ++i) {
            if (ImGui::Selectable(kSortLabels[i], static_cast<std::size_t>(m_sort) == i)) {
                m_sort = static_cast<SortOrder>(i);
                persist();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The order of both lists.\n\nFirst seen keeps rows where they are. "
                          "Frequency makes them shuffle as you watch, because a signal's "
                          "measured frequency wanders slightly.");
    }

    ImGui::SetNextItemWidth(100.0F);
    if (ImGui::SliderScalar("##hold", ImGuiDataType_Double, &m_holdSeconds, &kHoldMinSeconds,
                            &kHoldMaxSeconds, "hold %.0f s", ImGuiSliderFlags_Logarithmic)) {
        m_config.dropAfterSeconds = m_holdSeconds;
        applyConfig();
        persist();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How long a signal stays in the Active list after it was last heard. "
                          "After that it moves down to History, keeping everything it "
                          "learned.\n\nSet this too short and rows keep hopping between the "
                          "two lists.\n\nCtrl-click to type a number.");
    }

    if (dropped > 0) {
        ImGui::Separator();
        ImGui::TextDisabled("%llu lost", static_cast<unsigned long long>(dropped));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sweep++ could not hand every measurement to the detector, so "
                              "some transmissions were missed. The counts below are short by "
                              "roughly this much.");
        }
    }

    ImGui::EndMenuBar();
}

void DetectionsPlugin::drawSections() {
    // Below the fold, and in the scrolling half of the window: the threshold
    // and the graph switches above are read and changed while watching, and a
    // control that can scroll out from under the eye that is using it is a
    // control in the wrong place.
    //
    // The band list and the tuning are one section rather than two. They were
    // two, and the table was then a read-only copy of the four numbers the
    // other one edited -- so the same value appeared twice on screen, in
    // different words, with no way to tell which of them you were changing.
    if (ImGui::CollapsingHeader("Bands and tuning")) {
        drawRanges();
    }
    if (ImGui::CollapsingHeader("Ignore ranges")) {
        drawIgnoreRanges();
    }
}

void DetectionsPlugin::drawRanges() {
    ImGui::Indent(6.0F);

    if (ImGui::Checkbox("Follow the swept range", &m_followRanges)) {
        persist();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Use each band's own settings as you sweep into it.\n\n"
                          "Turn this off to keep one band's settings whatever you sweep.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0F);
        ImGui::TextUnformatted(
            "Different parts of the spectrum need different settings. A narrow radio channel "
            "is about 12 kHz wide; a Wi-Fi signal is 20 MHz. Settings that keep two radio "
            "channels apart would chop that Wi-Fi signal into a dozen pieces.\n\n"
            "So each band below has its own, and Sweep++ uses the one your sweep is in -- "
            "marked in the list.\n\nClick any band to edit it, including the ones that came "
            "with Sweep++. The threshold is not one of these: it stays where you put it "
            "wherever you sweep.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    ImGui::Spacing();

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("##ranges", 4, flags)) {
        const auto column = [&style](float content) {
            return content + style.CellPadding.x * 2.0F;
        };

        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed,
                                column(ImGui::CalcTextSize("2.4 GHz ISM  \u25b8").x));
        ImGui::TableSetupColumn("##span", ImGuiTableColumnFlags_WidthFixed,
                                column(ImGui::CalcTextSize("888.8 MHz - 888.8 MHz").x));
        ImGui::TableSetupColumn("##width", ImGuiTableColumnFlags_WidthFixed,
                                column(ImGui::CalcTextSize("width 888, gap 888 bins").x));
        ImGui::TableSetupColumn("##hysteresis", ImGuiTableColumnFlags_WidthStretch);

        std::size_t pick = m_ranges.size();

        for (std::size_t i = 0; i < m_ranges.size(); ++i) {
            const RangeProfile& range = m_ranges[i];
            const bool swept = i == m_activeRange;
            const bool editing = i == m_editingRange;

            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            // Every row is selectable, the shipped ones included. What a row
            // click chooses is what the editor below is pointed at -- not what
            // the detector is using, which the sweep decides.
            ImGui::TableNextColumn();
            const std::string label = swept ? std::format("{} \u25b8", range.name) : range.name;
            if (ImGui::Selectable(label.c_str(), editing, ImGuiSelectableFlags_SpanAllColumns)) {
                pick = i;
            }
            if (swept && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("This is the band you are sweeping now, so these are the "
                                  "settings in use.");
            }

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s - %s", toml_util::formatFrequencyShort(range.startHz).c_str(),
                                toml_util::formatFrequencyShort(range.stopHz).c_str());

            ImGui::TableNextColumn();
            ImGui::TextDisabled("width %zu, gap %zu bins", range.minWidthBins, range.mergeGapBins);

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.1f dB hysteresis", static_cast<double>(range.hysteresisDb));

            ImGui::PopID();
        }

        ImGui::EndTable();

        if (pick < m_ranges.size()) {
            m_editingRange = pick;
            m_draftBandName.fill('\0');
            const std::string& name = m_ranges[pick].name;
            std::memcpy(m_draftBandName.data(), name.data(),
                        std::min(name.size(), m_draftBandName.size() - 1));
        }
    }

    if (ImGui::Button(m_host.icon(kAddIcon, "+").append("##addband").c_str())) {
        double startHz = 0.0;
        double stopHz = 0.0;
        {
            const std::lock_guard lock(m_stateMutex);
            startHz = m_gridStartHz;
            stopHz = m_gridStopHz;
        }

        RangeProfile fresh;
        fresh.name = "New band";
        // Seeded from what is being swept, which is very nearly always the
        // band the operator is adding one for.
        fresh.startHz = stopHz > startHz ? startHz : 0.0;
        fresh.stopHz = stopHz > startHz ? stopHz : 1e9;
        fresh.hysteresisDb = m_config.hysteresisDb;
        fresh.minWidthBins = m_config.minWidthBins;
        fresh.mergeGapBins = m_config.mergeGapBins;

        m_ranges.push_back(std::move(fresh));
        std::ranges::stable_sort(m_ranges, {}, &RangeProfile::startHz);
        m_editingRange = static_cast<std::size_t>(
            std::ranges::find(m_ranges, "New band", &RangeProfile::name) - m_ranges.begin());
        m_draftBandName.fill('\0');
        std::memcpy(m_draftBandName.data(), "New band", 8);
        persist();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add a band covering what you are sweeping now.");
    }

    if (m_editingRange >= m_ranges.size()) {
        ImGui::Unindent(6.0F);
        return;
    }

    RangeProfile& editing = m_ranges[m_editingRange];

    ImGui::SeparatorText(editing.name.c_str());

    rowLabel("Name");
    ImGui::SetNextItemWidth(220.0F);
    if (ImGui::InputText("##bandname", m_draftBandName.data(), m_draftBandName.size())) {
        editing.name = m_draftBandName.data();
        std::erase(editing.name, ';');
        persist();
    }

    // Megahertz, like the ignore editor: every frequency an operator has in
    // their head for a band edge is already in MHz, and nine digits typed
    // twice is how the hertz version felt.
    rowLabel("Covers");
    double startMHz = editing.startHz / 1e6;
    double stopMHz = editing.stopHz / 1e6;

    ImGui::SetNextItemWidth(104.0F);
    bool moved = ImGui::DragScalar("##bandstart", ImGuiDataType_Double, &startMHz, 1.0, nullptr,
                                   nullptr, "%.3f MHz");
    ImGui::SameLine();
    ImGui::TextDisabled("-");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(104.0F);
    moved |= ImGui::DragScalar("##bandstop", ImGuiDataType_Double, &stopMHz, 1.0, nullptr, nullptr,
                               "%.3f MHz");
    if (moved) {
        editing.startHz = std::max(startMHz, 0.0) * 1e6;
        editing.stopHz = std::max(stopMHz, startMHz) * 1e6;
        persist();
    }

    drawTuning(editing);

    const std::vector<RangeProfile> shipped = defaultRanges();
    const auto original = std::ranges::find(shipped, editing.name, &RangeProfile::name);

    ImGui::BeginDisabled(original == shipped.end());
    if (ImGui::Button("Restore defaults")) {
        editing = *original;
        commitRange();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(original == shipped.end()
                              ? "You added this band, so there is nothing to put back."
                              : "Put this band's settings and its edges back to how they came "
                                "with Sweep++. No other band is changed.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(m_ranges.size() <= 1);
    if (ImGui::Button("Delete this band")) {
        m_ranges.erase(m_ranges.begin() + static_cast<std::ptrdiff_t>(m_editingRange));
        m_editingRange = 0;
        m_activeRange = std::numeric_limits<std::size_t>::max();
        m_draftBandName.fill('\0');
        persist();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(m_ranges.size() <= 1
                              ? "The last band cannot be deleted -- the settings have to live "
                                "somewhere."
                              : "Delete this band. Sweeps that fell in it will use whichever "
                                "band covers them next.");
    }

    ImGui::Unindent(6.0F);
}

void DetectionsPlugin::drawTuning(RangeProfile& range) {
    double binWidthHz = 0.0;
    {
        const std::lock_guard lock(m_stateMutex);
        binWidthHz = m_binWidthHz;
    }

    constexpr float kValueWidth = 128.0F;

    /// The same setting in the unit the plot is labelled in, beside the one it
    /// is stored in. A count of bins is what makes the default correct at any
    /// resolution; hertz is what the operator can find on the screen.
    const auto inHertz = [binWidthHz](std::size_t count) {
        if (binWidthHz <= 0.0) {
            return;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            "%s", toml_util::formatFrequencyShort(binWidthHz * static_cast<double>(count)).c_str());
    };

    bool changed = false;

    rowLabel("Minimum width", "Ignore anything narrower than this.\n\nRaise it to skip the "
                              "one-bin spikes that are noise rather than a transmission.");
    ImGui::SetNextItemWidth(kValueWidth);
    int minWidth = static_cast<int>(range.minWidthBins);
    if (ImGui::DragInt("##minwidth", &minWidth, 0.2F, 1, 4096, "%d bins")) {
        range.minWidthBins = static_cast<std::size_t>(std::max(minWidth, 1));
        changed = true;
    }
    inHertz(range.minWidthBins);

    rowLabel("Merge gap", "Join two nearby pieces into one signal when the gap between them "
                          "is smaller than this.\n\nA real transmission often dips in the "
                          "middle; without this it gets listed twice. Set it to zero to never "
                          "join.\n\nGaps you have told it to ignore are never bridged.");
    ImGui::SetNextItemWidth(kValueWidth);
    int mergeGap = static_cast<int>(range.mergeGapBins);
    if (ImGui::DragInt("##mergegap", &mergeGap, 0.2F, 0, 4096, "%d bins")) {
        range.mergeGapBins = static_cast<std::size_t>(std::max(mergeGap, 0));
        changed = true;
    }
    inHertz(range.mergeGapBins);

    rowLabel("Hysteresis", "How far a signal has to drop before it counts as having "
                           "stopped.\n\nWi-Fi and mobile signals jump up and down by a few dB "
                           "across their own width. Without this, one transmission gets listed "
                           "as many.");
    ImGui::SetNextItemWidth(kValueWidth);
    changed |= ImGui::DragFloat("##hysteresis", &range.hysteresisDb, 0.1F, 0.0F, 40.0F, "%.1f dB");

    // Both widths above are counts of bins, so the panel says what a bin
    // currently is. Without it they are numbers with no scale attached, and
    // the operator has no way to know that on this grid one carrier is spread
    // over four thousand of them.
    if (binWidthHz > 0.0) {
        rowCaption(std::format("one bin is {}", toml_util::formatFrequencyShort(binWidthHz)));
    }

    if (changed) {
        commitRange();
    }
}

void DetectionsPlugin::drawIgnoreRanges() {
    ImGui::Indent(6.0F);

    if (m_ignored.empty()) {
        ImGui::TextDisabled("Nothing ignored.");
    } else {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float square = ImGui::GetFrameHeight();
        const auto column = [&style](float content) {
            return content + style.CellPadding.x * 2.0F;
        };

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##ignored", 4, flags)) {
            ImGui::TableSetupColumn("##span", ImGuiTableColumnFlags_WidthFixed,
                                    column(ImGui::CalcTextSize("8888.888 MHz - 8888.888 MHz").x));
            ImGui::TableSetupColumn("##width", ImGuiTableColumnFlags_WidthFixed,
                                    column(ImGui::CalcTextSize("888.8 MHz").x));
            ImGui::TableSetupColumn("##note", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, column(square));

            std::size_t remove = m_ignored.size();
            for (std::size_t i = 0; i < m_ignored.size(); ++i) {
                const IgnoreRange& range = m_ignored[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::Text("%s - %s", toml_util::formatFrequency(range.startHz, 3).c_str(),
                            toml_util::formatFrequency(range.stopHz, 3).c_str());

                ImGui::TableNextColumn();
                ImGui::TextDisabled(
                    "%s", toml_util::formatFrequencyShort(range.stopHz - range.startHz).c_str());

                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", range.note.c_str());

                ImGui::TableNextColumn();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0F, 0.0F));
                if (ImGui::Button(m_host.icon(kForgetIcon, "x").append("##drop").c_str(),
                                  ImVec2(square, square))) {
                    remove = i;
                }
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Stop ignoring this range and start watching it "
                                      "again.");
                }

                ImGui::PopID();
            }

            ImGui::EndTable();

            if (remove < m_ignored.size()) {
                m_ignored.erase(m_ignored.begin() + static_cast<std::ptrdiff_t>(remove));
                applyIgnored();
                persist();
            }
        }
    }

    // Megahertz unless a suffix says otherwise. A bare number here used to be
    // hertz, which meant ignoring the 2.4 GHz band was nine digits typed twice
    // -- and every frequency an operator has in their head for this is already
    // in MHz.
    constexpr float kFieldWidth = 112.0F;

    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::InputTextWithHint("##start", "start MHz", m_draftStart.data(), m_draftStart.size());
    ImGui::SameLine();
    ImGui::TextDisabled("-");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::InputTextWithHint("##stop", "stop MHz", m_draftStop.data(), m_draftStop.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0F);
    ImGui::InputTextWithHint("##note", "note", m_draftNote.data(), m_draftNote.size());

    ImGui::SameLine();
    if (ImGui::Button("Add##ignore")) {
        const std::optional<double> start = parseMegahertz(m_draftStart.data());
        const std::optional<double> stop = parseMegahertz(m_draftStop.data());
        if (!start || !stop) {
            m_message = "start and stop must be frequencies in MHz, or carry their own unit";
        } else {
            m_ignored.push_back(IgnoreRange{
                .startHz = *start, .stopHz = *stop, .note = std::string(m_draftNote.data())});
            m_draftStart[0] = '\0';
            m_draftStop[0] = '\0';
            m_draftNote[0] = '\0';
            m_message.clear();
            applyIgnored();
            persist();
        }
    }

    // The span on screen is the one being looked at, so it is the one worth
    // being able to exclude without reading two numbers off the axis first.
    double fromHz = 0.0;
    double toHz = 0.0;
    {
        const std::lock_guard lock(m_stateMutex);
        fromHz = m_gridStartHz;
        toHz = m_gridStopHz;
    }
    if (toHz > fromHz) {
        ImGui::SameLine();
        if (ImGui::Button("Add the swept span")) {
            m_ignored.push_back(IgnoreRange{
                .startHz = fromHz, .stopHz = toHz, .note = std::string(m_draftNote.data())});
            m_draftNote[0] = '\0';
            applyIgnored();
            persist();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Ignore everything you are sweeping now: %s to %s.",
                              toml_util::formatFrequencyShort(fromHz).c_str(),
                              toml_util::formatFrequencyShort(toHz).c_str());
        }
    }

    ImGui::Unindent(6.0F);
}

void DetectionsPlugin::drawSparkline(const Row& row) {
    const sweeppp::RollingHistory<kHistoryLength>& history =
        m_series == GraphSeries::Activity ? row.activity[m_span] : row.level[m_span];

    const std::size_t count = history.count();
    if (count == 0) {
        ImGui::TextDisabled("-");
        return;
    }

    m_scratch.resize(count);
    float lowest = history.at(0);
    float highest = lowest;
    for (std::size_t i = 0; i < count; ++i) {
        const float value = history.at(i);
        m_scratch[i] = value;
        lowest = std::min(lowest, value);
        highest = std::max(highest, value);
    }

    // A single sample is still a reading, and drawing nothing for it is what
    // made a row's graph vanish and come back: the coarse spans push once
    // every four and every thirty ticks, so a row that has just appeared has
    // one point for seconds at a time. Doubled into a flat line rather than
    // left blank, so the row keeps its shape while the ring fills.
    if (count == 1) {
        m_scratch.push_back(m_scratch.front());
    }
    const auto plotted = static_cast<int>(m_scratch.size());

    // The axis is the whole ring, not the part of it that is filled. Scaling
    // to `count` stretched five samples across the full width and drew them as
    // a confident thirty-minute trace, which is a graph telling you about
    // twenty-nine minutes it never saw. Anchored at the right instead, so the
    // line is short when the history is short and grows leftwards into the
    // window as it fills -- and two rows are then on the same time axis.
    const auto span = static_cast<double>(kHistoryLength - 1);
    const double firstX = span - static_cast<double>(plotted - 1);

    // Activity is a fraction on a fixed axis, so two rows are comparable by
    // height. A level is dBFS on the row's own extremes, computed here rather
    // than from `RollingHistory::max()` -- which seeds from 0.0F and reports
    // zero for a series that is negative all the way along.
    // Padded, so a flat line at nothing-at-all or at present-throughout is
    // drawn inside the box rather than along its border, where it is
    // indistinguishable from no line at all. The two values an activity
    // sparkline spends most of its time at are exactly 0 and exactly 1.
    double minimum = -0.06;
    double maximum = 1.06;
    if (m_series == GraphSeries::PeakLevel) {
        // At least a few decibels of scale even when the level barely moves,
        // or a signal sitting steady at -55 is drawn as a mountain range of
        // quantisation noise.
        const float middle = (highest + lowest) * 0.5F;
        const float half = std::max((highest - lowest) * 0.6F, 2.0F);
        minimum = static_cast<double>(middle - half);
        maximum = static_cast<double>(middle + half);
    }

    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));

    if (ImPlot::BeginPlot("##spark", ImVec2(-1, 22),
                          ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations,
                          ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxesLimits(0, span, minimum, maximum, ImPlotCond_Always);
        if (m_series == GraphSeries::PeakLevel) {
            // Drawn segment by segment into the plot's own draw list, because
            // `PlotLine` takes one colour for the whole series and the whole
            // point here is that the colour is the value. Two hundred and
            // forty short lines is nothing to a draw list; two hundred and
            // forty `PlotLine` calls would not be.
            // Coloured against the noise floor rather than against the row's
            // own extremes. Per-row normalisation gave every row the full
            // sweep of the ramp no matter how little its level actually moved,
            // so a signal steady to within a decibel came out a rainbow and
            // two rows' colours meant different things. Decibels over the
            // floor mean the same on every row, which is the only thing that
            // makes a shared colour scale worth having.
            constexpr float kColorSpanDb = 40.0F;
            ImDrawList* draw = ImPlot::GetPlotDrawList();

            for (int i = 1; i < plotted; ++i) {
                const auto previous = static_cast<std::size_t>(i) - 1;
                const auto current = static_cast<std::size_t>(i);
                const ImPlotPoint from(firstX + static_cast<double>(i - 1),
                                       static_cast<double>(m_scratch[previous]));
                const ImPlotPoint to(firstX + static_cast<double>(i),
                                     static_cast<double>(m_scratch[current]));
                const float level = (m_scratch[current] + m_scratch[previous]) * 0.5F;
                draw->AddLine(ImPlot::PlotToPixels(from), ImPlot::PlotToPixels(to),
                              levelColor((level - m_uiFloorDbfs) / kColorSpanDb), 1.4F);
            }
        } else {
            ImPlotSpec spec;
            spec.LineColor = ImVec4(kSpanColor[0], kSpanColor[1], kSpanColor[2],
                                    row.sample.active ? 1.0F : 0.55F);
            spec.LineWeight = 1.2F;
            ImPlot::PlotLine("##v", m_scratch.data(), plotted, 1.0, firstX, spec);
        }
        ImPlot::EndPlot();
    }

    ImGui::PopStyleColor();
    ImPlot::PopStyleVar();
}

std::vector<std::size_t> DetectionsPlugin::ordered(bool wantActive) const {
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].sample.active == wantActive) {
            visible.push_back(i);
        }
    }

    // Sorted on a key that does not move, or the rows swap places under the
    // pointer. First seen is the only one that never changes once set, which
    // is why it is the default; frequency is available and is the worst of
    // them, because it is a live measurement that drifts a bin at a time.
    const std::vector<Row>& rows = m_rows;
    const SortOrder order = m_sort;
    std::ranges::stable_sort(visible, [&rows, order](std::size_t a, std::size_t b) {
        const TickSample& left = rows[a].sample;
        const TickSample& right = rows[b].sample;
        switch (order) {
        case SortOrder::LastSeen:
            return left.lastSeenNs > right.lastSeenNs;
        case SortOrder::Frequency:
            return left.centerHz < right.centerHz;
        case SortOrder::Level:
            return left.peakDbfs > right.peakDbfs;
        case SortOrder::Sightings:
            return left.hits > right.hits;
        case SortOrder::FirstSeen:
            break;
        }
        // Oldest first: a list that grows downwards keeps everything already
        // read where it was, which a list that grows upwards does not.
        return left.firstSeenNs < right.firstSeenNs;
    });

    return visible;
}

void DetectionsPlugin::drawTable(const char* id, bool wantActive, float height) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float square = ImGui::GetFrameHeight();

    const std::vector<std::size_t> visible = ordered(wantActive);

    // Always scrolling, and always exactly the height it was given. That is
    // what keeps the footer on the bottom edge of the window instead of below
    // however many rows happen to be listed -- and it is also what lets ImGui
    // clip the rows that are off screen, which matters when each one carries a
    // plot of its own.
    const float rowHeight = square + style.CellPadding.y * 2.0F;
    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;

    if (!ImGui::BeginTable(id, 8, flags, ImVec2(0.0F, std::max(height, rowHeight)))) {
        return;
    }

    // No header row: "D1", "433.920 MHz" and "-52.3 dBFS" say what they are.
    //
    // Widths reserved from the widest value each column can hold rather than
    // from what is in it. These change several times a second, and a column
    // sized to its own text drags the whole row sideways every time a digit
    // appears. A column is also wider than its content by its own padding, and
    // a fixed width that forgets that clips half of the last button.
    const auto column = [&style](float content) { return content + style.CellPadding.x * 2.0F; };

    ImGui::TableSetupColumn("##paint", ImGuiTableColumnFlags_WidthFixed, column(square));
    ImGui::TableSetupColumn("##frequency", ImGuiTableColumnFlags_WidthFixed,
                            column(ImGui::CalcTextSize("8888.888 MHz").x));
    ImGui::TableSetupColumn("##width", ImGuiTableColumnFlags_WidthFixed,
                            column(ImGui::CalcTextSize("888.8 kHz").x));
    ImGui::TableSetupColumn("##level", ImGuiTableColumnFlags_WidthFixed,
                            column(ImGui::CalcTextSize("-888.8 dBFS").x));
    ImGui::TableSetupColumn("##last", ImGuiTableColumnFlags_WidthFixed,
                            column(ImGui::CalcTextSize("888 min").x));
    ImGui::TableSetupColumn("##interval", ImGuiTableColumnFlags_WidthFixed,
                            column(ImGui::CalcTextSize("every 888.8 min").x));
    ImGui::TableSetupColumn("##graph", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed,
                            column(square * 2.0F + style.ItemSpacing.x));

    // Recorded and applied after the table: erasing mid-iteration would
    // invalidate the row the rest of this pass is still drawing.
    int forget = 0;
    int ignore = 0;

    const std::uint64_t nowNs = sweeppp::monotonicNs();

    // Inside the table, as a row of its own. Placed after it -- which is what
    // this used to do -- the cursor had to be walked back up over the table's
    // whole height, and it landed in the middle of the controls above.
    if (visible.empty()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextDisabled(wantActive ? "Nothing over the threshold yet."
                                       : "Nothing has gone quiet yet.");
    }

    for (const std::size_t index : visible) {
        Row& row = m_rows[index];
        const TickSample& sample = row.sample;
        ImGui::PushID(sample.id);
        ImGui::TableNextRow();

        // The tick is submitted before the row's selectable so it wins the hit
        // test over its own few pixels: painting a span must not also be a
        // click on the row behind it.
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("##paint", &row.painted) && row.painted) {
            // Ticking one is the operator saying "show me this", and the host
            // draws a channel-typed contribution only while it is painting
            // channel contributions at all. Leaving that switch alone would
            // make the tick do nothing whatsoever, with nothing on screen to
            // say why -- so the tick turns it on, which is what was meant.
            if (!m_host.contributionsShown(SWEEPPP_CONTRIBUTION_CHANNEL)) {
                m_host.setContributionsShown(SWEEPPP_CONTRIBUTION_CHANNEL, true);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Show this signal on the spectrum.");
        }

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.label.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("First heard %s.\nSeen in %llu sweeps, over %llu separate "
                              "spells.\nStrongest it ever got: %.1f dBFS.\n\n"
                              "Covers %s to %s.",
                              sinceText(nowNs, sample.firstSeenNs).c_str(),
                              static_cast<unsigned long long>(sample.hits),
                              static_cast<unsigned long long>(sample.appearances),
                              static_cast<double>(sample.strongestDbfs),
                              toml_util::formatFrequency(sample.startHz, 3).c_str(),
                              toml_util::formatFrequency(sample.stopHz, 3).c_str());
        }

        ImGui::TableNextColumn();
        ImGui::TextDisabled(
            "%s", toml_util::formatFrequencyShort(sample.stopHz - sample.startHz).c_str());

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(
            std::format("{:.1f} dBFS", static_cast<double>(sample.peakDbfs)).c_str());

        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", ageText(nowNs, sample.lastSeenNs).c_str());

        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", intervalText(sample).c_str());

        ImGui::TableNextColumn();
        drawSparkline(row);

        ImGui::TableNextColumn();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0F, 0.0F));
        if (ImGui::Button(m_host.icon(kIgnoreIcon, "x").append("##ignore").c_str(),
                          ImVec2(square, square))) {
            ignore = sample.id;
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Stop watching this frequency range.\n\nIt is added to the "
                              "ignore list, where you can take it off again.");
        }

        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0F, 0.0F));
        if (ImGui::Button(m_host.icon(kForgetIcon, "-").append("##forget").c_str(),
                          ImVec2(square, square))) {
            forget = sample.id;
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove this from the list.\n\nIf the signal comes back it "
                              "will be listed again as a new one.");
        }

        ImGui::PopID();
    }

    ImGui::EndTable();

    if (ignore != 0) {
        const auto found =
            std::ranges::find(m_rows, ignore, [](const Row& row) { return row.sample.id; });
        if (found != m_rows.end()) {
            m_ignored.push_back(IgnoreRange{.startHz = found->sample.startHz,
                                            .stopHz = found->sample.stopHz,
                                            .note = found->label});
            applyIgnored();
        }
        forget = ignore;
        persist();
    }

    if (forget != 0) {
        queue(Command{.kind = Command::Kind::Forget, .id = forget});
        std::erase_if(m_rows, [forget](const Row& row) { return row.sample.id == forget; });
    }
}

void DetectionsPlugin::drawFooter() {
    if (ImGui::Button("Export CSV")) {
        exportTo("csv");
    }
    ImGui::SameLine();
    if (ImGui::Button("Export TOML")) {
        exportTo("toml");
    }

    ImGui::SameLine();
    const auto history = static_cast<std::size_t>(
        std::ranges::count_if(m_rows, [](const Row& row) { return !row.sample.active; }));
    ImGui::BeginDisabled(history == 0);
    if (ImGui::Button("Clear history")) {
        queue(Command{.kind = Command::Kind::ClearHistory});
        std::erase_if(m_rows, [](const Row& row) { return !row.sample.active; });
    }
    ImGui::EndDisabled();

    if (!m_message.empty()) {
        // Just what happened. The operator chose the file, so they know where
        // it went -- the copy-path button that used to be here existed only
        // because the plugin had picked the directory itself.
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_message.c_str());
    }
}

std::uint64_t DetectionsPlugin::wallOf(std::uint64_t monotonicNs) const {
    const std::uint64_t nowMono = sweeppp::monotonicNs();
    const std::uint64_t nowWall = sweeppp::wallClockNs();
    if (monotonicNs == 0 || monotonicNs > nowMono) {
        return nowWall;
    }
    const std::uint64_t age = nowMono - monotonicNs;
    return age > nowWall ? 0 : nowWall - age;
}

sweeppp::Status DetectionsPlugin::exportCsv(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return sweeppp::fail(sweeppp::ErrorCode::IoError, "cannot write {}", path.string());
    }

    out << "id,first_seen,last_seen,center_hz,start_hz,stop_hz,width_hz,peak_dbfs,"
           "strongest_dbfs,hits,appearances,mean_interval_s,active\n";

    for (const Row& row : m_rows) {
        const TickSample& sample = row.sample;
        out << sample.id << ',' << sweeppp::formatWallClockIso8601(wallOf(sample.firstSeenNs))
            << ',' << sweeppp::formatWallClockIso8601(wallOf(sample.lastSeenNs)) << ','
            << std::format("{:.3f},{:.3f},{:.3f},{:.3f},{:.2f},{:.2f},{},{},{:.3f},{}\n",
                           sample.centerHz, sample.startHz, sample.stopHz,
                           sample.stopHz - sample.startHz, static_cast<double>(sample.peakDbfs),
                           static_cast<double>(sample.strongestDbfs), sample.hits,
                           sample.appearances, sample.meanIntervalSeconds,
                           sample.active ? "true" : "false");
    }

    out.flush();
    return out ? sweeppp::ok()
               : sweeppp::fail(sweeppp::ErrorCode::IoError, "failed while writing {}",
                               path.string());
}

sweeppp::Status DetectionsPlugin::exportToml(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return sweeppp::fail(sweeppp::ErrorCode::IoError, "cannot write {}", path.string());
    }

    float threshold = 0.0F;
    {
        const std::lock_guard lock(m_stateMutex);
        threshold = m_thresholdDbfs;
    }

    out << "[export]\n"
        << std::format("created = \"{}\"\n",
                       sweeppp::formatWallClockIso8601(sweeppp::wallClockNs()))
        << std::format("mode = \"{}\"\n", modeKey(m_config.mode))
        << std::format("threshold_dbfs = {:.2f}\n\n", static_cast<double>(threshold));

    for (const Row& row : m_rows) {
        const TickSample& sample = row.sample;
        out << "[[detection]]\n"
            << std::format("id = {}\n", sample.id)
            << std::format("first_seen = \"{}\"\n",
                           sweeppp::formatWallClockIso8601(wallOf(sample.firstSeenNs)))
            << std::format("last_seen = \"{}\"\n",
                           sweeppp::formatWallClockIso8601(wallOf(sample.lastSeenNs)))
            << std::format("center_hz = {:.3f}\n", sample.centerHz)
            << std::format("start_hz = {:.3f}\n", sample.startHz)
            << std::format("stop_hz = {:.3f}\n", sample.stopHz)
            << std::format("width_hz = {:.3f}\n", sample.stopHz - sample.startHz)
            << std::format("peak_dbfs = {:.2f}\n", static_cast<double>(sample.peakDbfs))
            << std::format("strongest_dbfs = {:.2f}\n", static_cast<double>(sample.strongestDbfs))
            << std::format("hits = {}\n", sample.hits)
            << std::format("appearances = {}\n", sample.appearances)
            << std::format("mean_interval_s = {:.3f}\n", sample.meanIntervalSeconds)
            << std::format("active = {}\n\n", sample.active ? "true" : "false");
    }

    out.flush();
    return out ? sweeppp::ok()
               : sweeppp::fail(sweeppp::ErrorCode::IoError, "failed while writing {}",
                               path.string());
}

void DetectionsPlugin::exportTo(std::string_view extension) {
    if (m_rows.empty()) {
        m_message = "nothing to export yet";
        return;
    }

    const std::string suggested = std::format(
        "detections-{}.{}", sweeppp::formatWallClockCompact(sweeppp::wallClockNs()), extension);

    std::filesystem::path path;
    if (m_host.canChooseFile()) {
        path = m_host.chooseFile("Export detections", suggested, extension);
        if (path.empty()) {
            // Cancelled. Not a failure, and not something to leave a message
            // about -- the operator knows what they just did.
            m_message.clear();
            return;
        }
    } else {
        // A host with no dialog -- headless, or one older than the call. Better
        // to write somewhere findable and say where than to refuse.
        path = m_host.dataDir() / suggested;
    }

    const sweeppp::Status written = extension == "csv" ? exportCsv(path) : exportToml(path);
    if (!written) {
        m_message = written.error().describe();
        m_host.warn("{}", m_message);
        return;
    }

    m_message = std::format("wrote {} to {}", m_rows.size(), path.filename().string());
    m_host.info("wrote {} detections to {}", m_rows.size(), path.string());
}

void DetectionsPlugin::drawOverlay(const plugin::PlotContext& context) {
    if (!m_showOnPlot) {
        return;
    }
    ImDrawList* draw = context.drawList();
    if (draw == nullptr) {
        return;
    }

    const float top = context.top();
    const float bottom = context.bottom();
    const float alpha = std::clamp(context.overlayAlpha(), 0.05F, 1.0F);

    const ImU32 band =
        ImGui::GetColorU32(ImVec4(kSpanColor[0], kSpanColor[1], kSpanColor[2], alpha * 0.30F));
    const ImU32 edge =
        ImGui::GetColorU32(ImVec4(kSpanColor[0], kSpanColor[1], kSpanColor[2], 0.95F));

    for (const Row& row : m_rows) {
        if (!row.painted) {
            continue;
        }
        const TickSample& sample = row.sample;
        if (sample.stopHz < context.fromHz() || sample.startHz > context.toHz()) {
            continue;
        }

        const float left = context.xForHz(sample.startHz);
        const float right = context.xForHz(sample.stopHz);
        const float centre = context.xForHz(sample.centerHz);

        // Deliberately not a labelled flag: that is what the band plan and the
        // channel list look like, and a detection sitting among them as a
        // sixth flag is a detection nobody can pick out. This reads as
        // something the operator selected -- a filled span between two hard
        // edges, a line down the middle of it, and a caret on the top rail.
        if (right - left >= 1.0F) {
            draw->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), band);
        }
        draw->AddLine(ImVec2(left, top), ImVec2(left, bottom), edge, 1.0F);
        draw->AddLine(ImVec2(right, top), ImVec2(right, bottom), edge, 1.0F);
        draw->AddLine(ImVec2(centre, top), ImVec2(centre, bottom), edge, 1.4F);

        constexpr float kCaret = 5.0F;
        draw->AddTriangleFilled(ImVec2(centre - kCaret, top), ImVec2(centre + kCaret, top),
                                ImVec2(centre, top + kCaret * 1.6F), edge);

        // The label goes beside the caret rather than under it, so two
        // detections close together do not write over each other's stems.
        const std::string label = toml_util::formatFrequencyShort(sample.centerHz);
        draw->AddText(ImVec2(centre + kCaret + 3.0F, top + 1.0F), edge, label.c_str());
    }
}

void DetectionsPlugin::drawWindow() {
    if (!m_windowOpen) {
        return;
    }

    // `io.IniFilename` is null, so nothing about this window is remembered
    // between runs and an unsized one collapses its sparklines to a sliver.
    ImGui::SetNextWindowSize(ImVec2(820.0F, 660.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(620.0F, 360.0F), ImVec2(FLT_MAX, FLT_MAX));

    // Everything after "###" is the window's identity and nothing before it
    // is: the label can say what the panel currently holds without ImGui
    // treating each new count as a different window and losing its position.
    const auto active = static_cast<std::size_t>(
        std::ranges::count_if(m_rows, [](const Row& row) { return row.sample.active; }));

    std::string title = "Detections";
    if (m_activeRange < m_ranges.size()) {
        title += std::format(" \u00b7 {}", m_ranges[m_activeRange].name);
    }
    if (!m_rows.empty()) {
        title +=
            std::format(" \u00b7 {} active \u00b7 {} in history", active, m_rows.size() - active);
    }
    title += "###org.sweeppp.detections";

    const bool wasOpen = m_windowOpen;

    // The bar's metrics, pushed around the Begin because that is where ImGui
    // decides how tall a window's menu bar and title bar are. Popped straight
    // after the bar, so everything below it goes back to panel proportions --
    // a bar control is aimed at while the eye is on the plot, a table row is
    // read.
    bool visible = false;
    {
        const chrome::BarMetrics metrics;
        visible = ImGui::Begin(title.c_str(), &m_windowOpen, ImGuiWindowFlags_MenuBar);
        if (visible) {
            drawMenuBar();
        }
    }

    if (!visible) {
        ImGui::End();
        if (wasOpen != m_windowOpen) {
            persist();
        }
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();

    // Reserved before anything below is laid out, so the export and clear
    // buttons sit on the window's bottom edge whatever is above them.
    // Scrolling to reach a button is the one thing a footer must never ask
    // for: it is where you go when you have finished reading.
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y + 1.0F;

    if (ImGui::BeginChild("##body", ImVec2(0.0F, -footerHeight))) {
        drawSections();

        const float separators =
            (ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y) * 2.0F;
        const float rowHeight = ImGui::GetFrameHeight() + style.CellPadding.y * 2.0F;

        float remaining = ImGui::GetContentRegionAvail().y - separators;
        remaining = std::max(remaining, rowHeight * 4.0F);

        // Each table asks for what it holds and gets no more. Handing the
        // leftover to History meant two empty lists were drawn as one short
        // box above one enormous one, which reads as a fault rather than as an
        // empty list.
        const auto count = [this](bool wantActive) {
            return static_cast<float>(std::ranges::count_if(
                m_rows, [wantActive](const Row& row) { return row.sample.active == wantActive; }));
        };

        const float activeHeight =
            std::clamp((count(true) + 0.4F) * rowHeight, rowHeight * 1.6F, remaining * 0.6F);
        const float historyHeight = std::clamp((count(false) + 0.4F) * rowHeight, rowHeight * 1.6F,
                                               remaining - activeHeight);

        ImGui::SeparatorText("Active");
        drawTable("##active", true, activeHeight);

        ImGui::SeparatorText("History");
        drawTable("##history", false, historyHeight);
    }
    ImGui::EndChild();

    ImGui::Separator();
    drawFooter();

    ImGui::End();

    if (wasOpen != m_windowOpen) {
        persist();
    }
}

#endif

// -------------------------------------------------------------- manifest

const sweeppp_plugin_desc_t& describe() {
    static const std::array kAuthors{plugin::author("Sweep++")};

    static const std::array kLinks{
        plugin::link(SWEEPPP_LINK_REPOSITORY, "https://github.com/aurimasniekis/sweeppp"),
        plugin::link(SWEEPPP_LINK_DOCUMENTATION,
                     "https://github.com/aurimasniekis/sweeppp/blob/main/docs/plugins.md"),
    };

    static const sweeppp_plugin_desc_t desc{
        .struct_size = sizeof(sweeppp_plugin_desc_t),
        .manifest =
            sweeppp_manifest_t{
                .struct_size = sizeof(sweeppp_manifest_t),
                .id = plugin::str(kPluginId),
                .version = plugin::str("1.0.0"),
                .name = plugin::str("Detections"),
                .description = plugin::str(
                    "Watches every frame for signals over a threshold and keeps what it "
                    "finds: where they are, how wide, when they were first and last seen, "
                    "and how often they come back."),
                .authors = kAuthors.data(),
                .author_count = static_cast<std::uint32_t>(kAuthors.size()),
                .links = kLinks.data(),
                .link_count = static_cast<std::uint32_t>(kLinks.size()),
                .dependencies = nullptr,
                .dependency_count = 0,
                .min_host_version = plugin::str("0.1.0"),
                // Everything it registers can be withdrawn live: the host stops
                // the worker before `deactivate`, and the facets hold nothing.
                .requires_restart_to_disable = 0,
            },
        .facets = facets().data(),
        .facet_count = static_cast<std::uint32_t>(facets().size()),
        // Filled in by SWEEPPP_PLUGIN_MAIN.
        .activate = nullptr,
        .deactivate = nullptr,
    };
    return desc;
}

} // namespace
} // namespace detections

SWEEPPP_PLUGIN_MAIN(detections::DetectionsPlugin, detections::describe)
