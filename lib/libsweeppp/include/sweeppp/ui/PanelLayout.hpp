// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sweeppp::ui {

/// What the panels show.
///
/// Mirror: every panel is a window onto the same grid, each with its own zoom.
/// Spans: each panel is bound to one sweep segment and cannot leave it, which
/// is what gives the FM band and 2.4 GHz a plot each rather than one plot in
/// which the first is a sliver.
enum class PanelMode : std::uint8_t { Mirror, Spans };

/// How the attached panels tile the plot area. Derived from how many there
/// are, plus one choice: whether two sit side by side or stacked.
///
/// Six is three columns by two rows and Nine three by three; a count between
/// two arrangements takes the larger and leaves its last slots empty.
enum class PanelArrangement : std::uint8_t { Single, Columns, Rows, Three, Grid, Six, Nine };

inline constexpr std::size_t kMaxPanels = 9;

/// Where a two-way split may sit, as a fraction of the area it divides.
inline constexpr float kMinSplit = 0.15F;
inline constexpr float kMaxSplit = 0.85F;

/// The narrowest share one part of a three-way split may be squeezed to.
inline constexpr float kMinThird = 0.1F;

/// Where the dividers sit, as fractions of the area they divide.
///
/// A two-way split has one divider per axis and a three-way split two, kept
/// apart so going from a 2×2 grid to a 3×3 one and back finds each where it
/// was left.
struct PanelSplits {
    float x = 0.5F;
    float y = 0.5F;
    std::array<float, 2> thirdsX{1.0F / 3.0F, 2.0F / 3.0F};
    std::array<float, 2> thirdsY{1.0F / 3.0F, 2.0F / 3.0F};
};

/// Every divider brought into reach, with no part narrower than allowed.
[[nodiscard]] PanelSplits clampSplits(const PanelSplits& splits) noexcept;

struct FrequencySpan {
    double startHz = 0.0;
    double stopHz = 0.0;

    [[nodiscard]] bool valid() const noexcept { return stopHz > startHz; }
    [[nodiscard]] double width() const noexcept { return stopHz - startHz; }
    [[nodiscard]] double centre() const noexcept { return (startHz + stopHz) * 0.5; }
    [[nodiscard]] bool contains(double hz) const noexcept { return hz >= startHz && hz <= stopHz; }
};

/// What a view may not leave. `highHz <= lowHz` means unbounded, where the
/// only rule is that a frequency is not negative.
struct ViewLimits {
    double lowHz = 0.0;
    double highHz = 0.0;

    [[nodiscard]] bool bounded() const noexcept { return highHz > lowHz; }
};

struct PanelRect {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

/// A run of grid bins, and the frequencies at their outer edges.
struct BinSlice {
    std::size_t first = 0;
    std::size_t count = 0;
    double startHz = 0.0;
    double stopHz = 0.0;

    [[nodiscard]] bool empty() const noexcept { return count == 0; }
};

/// One panel's own view of the data.
struct PanelView {
    /// Stable while the panel exists; the runtime half is matched to it by id.
    int id = 0;

    /// Visible window; equal values mean "fit".
    double viewStartHz = 0.0;
    double viewStopHz = 0.0;

    // Y axis and waterfall gradient, in dBFS. The defaults and the reasoning
    // behind them are the same as the single view had: the gradient floor sits
    // just under a typical noise floor, so empty spectrum reads dark.
    float yMinDb = -110.0F;
    float yMaxDb = -10.0F;
    float gradientMinDb = -75.0F;
    float gradientMaxDb = -15.0F;

    /// Share of the panel's height the waterfall takes.
    float waterfallFraction = 0.45F;

    /// Never saved: a waterfall restored paused looks like a failed start.
    bool waterfallPaused = false;

    /// Drawn in its own OS window rather than in the main one.
    bool detached = false;

    /// The sweep segment this panel is bound to, in Spans mode.
    FrequencySpan segment;
};

/// The panels, how they are arranged, and which one the toolbar acts on.
///
/// Pure data, saved in a profile. What a panel needs at runtime -- its
/// waterfall texture, its gesture state -- lives with the window and is
/// matched to these by id.
struct PanelLayout {
    PanelMode mode = PanelMode::Mirror;

    /// Two panels stacked rather than side by side.
    bool rowsForTwo = false;

    PanelSplits splits;

    /// The whole-range strip above the panels, in Spans mode.
    bool overview = true;

    /// Never empty.
    std::vector<PanelView> panels{PanelView{.id = 1}};
    int focusedId = 1;
    int nextId = 2;

    /// The focused panel, or the first when the id names none.
    [[nodiscard]] PanelView& focused() noexcept;
    [[nodiscard]] const PanelView& focused() const noexcept;

    [[nodiscard]] PanelView* find(int id) noexcept;
    [[nodiscard]] const PanelView* find(int id) const noexcept;

    /// Appends a copy of `cloneFrom` under a new id, attached and running.
    /// Null at `kMaxPanels`. Invalidates references into `panels`.
    PanelView* add(const PanelView& cloneFrom);

    /// Removes a panel, moving the focus to a neighbour. Refuses the last one.
    bool remove(int id);

    [[nodiscard]] std::size_t attachedCount() const noexcept;

    /// Puts `nextId` one past the highest id present.
    void resetNextId() noexcept;
};

/// The arrangement `attached` panels are drawn in.
[[nodiscard]] PanelArrangement arrangementFor(std::size_t attached, bool rowsForTwo) noexcept;

/// How many panels an arrangement has room for.
[[nodiscard]] std::size_t slotCount(PanelArrangement arrangement) noexcept;

/// Columns and rows of the grid an arrangement is. Three is two columns, the
/// right one split in two, and counts as one row.
[[nodiscard]] std::size_t columnCount(PanelArrangement arrangement) noexcept;
[[nodiscard]] std::size_t rowCount(PanelArrangement arrangement) noexcept;

/// Tiles `area`, in slot order: left to right, then top to bottom; for Three,
/// the wide left panel first. The splits are clamped first.
[[nodiscard]] std::vector<PanelRect> arrangePanels(PanelArrangement arrangement,
                                                   const PanelRect& area, const PanelSplits& splits,
                                                   float gap);

/// The panel's window, or `fit` when it has none of its own.
[[nodiscard]] FrequencySpan resolveView(const PanelView& panel, const FrequencySpan& fit) noexcept;

/// Scales a window by `factor` about `anchorHz`, which stays where it is.
[[nodiscard]] FrequencySpan zoomAbout(const FrequencySpan& view, double anchorHz,
                                      double factor) noexcept;

/// Brings a requested window inside `limits`.
///
/// The width is kept while sliding back inside, so a pan that hits the end
/// stops rather than shrinking; only a window wider than the limits is cut
/// down to them. An inverted or empty request is refused.
[[nodiscard]] std::optional<FrequencySpan> clampView(double fromHz, double toHz,
                                                     const ViewLimits& limits) noexcept;

/// The grid bins whose centres fall inside `span`.
[[nodiscard]] BinSlice segmentBins(double gridStartHz, double binWidthHz, std::size_t binCount,
                                   const FrequencySpan& span) noexcept;

/// Which segment each panel ends up bound to after the plan changed.
struct SegmentBinding {
    /// Per panel, in the order given: an index into the segments, or -1 when
    /// the panel has nothing left to show and should be removed.
    std::vector<int> panelSegment;

    /// Segments no panel took, in plan order.
    std::vector<std::size_t> unclaimed;
};

/// Binds panels to segments by overlap, not by position.
///
/// A plan's segments are not kept sorted, and merging one range into another
/// renumbers what follows, so an index says nothing about which panel showed
/// which band. Each panel in turn keeps the free segment it overlaps most. A
/// panel left overlapping only a segment another panel took is dropped --
/// the second of two whose segments merged. One overlapping nothing at all
/// takes the next leftover segment, and is dropped when there is none.
[[nodiscard]] SegmentBinding rebindSegments(std::span<const FrequencySpan> bound,
                                            std::span<const FrequencySpan> segments);

/// The windows that show `segments` in `groups` panels as large as possible.
///
/// In frequency order, with overlapping segments merged. When there are more
/// segments than panels, the two neighbours with the narrowest gap between
/// them share a window, again and again until they fit -- so what gets drawn
/// as empty space between ranges is the least it can be. Fewer segments than
/// panels gives one window per segment.
[[nodiscard]] std::vector<FrequencySpan> groupSegments(std::span<const FrequencySpan> segments,
                                                       std::size_t groups);

} // namespace sweeppp::ui
