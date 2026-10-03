// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "AppState.hpp"
#include "ContributionOverlay.hpp"
#include "FftBenchmarkRunner.hpp"
#include "UpdateCheck.hpp"
#include "ViewPanel.hpp"
#include "render/HistoryView.hpp"
#include "render/WaterfallRenderer.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <imgui.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sweeppp::ui {

/// Draws the whole application each frame.
///
/// Split by region rather than by widget type, so each method corresponds to
/// something the operator can point at.
class MainWindow {
public:
    explicit MainWindow(AppState& state);

    void draw();

    /// True once the operator has confirmed they want to quit -- the close
    /// prompt may hold it back.
    [[nodiscard]] bool wantsQuit() const noexcept { return m_quitConfirmed; }

    /// Called when the window's close button is pressed. Opens the save prompt
    /// if there is unsaved data.
    void requestQuit();

    /// Runs as a session viewer: the History window is the whole application,
    /// with no toolbar, no radio and no sweep. `path` may be empty, leaving the
    /// operator to open one.
    void enterHistoryViewer(const std::filesystem::path& path);

    /// Opens a session file handed to the application from outside -- dropped
    /// on the window, or opened with it by the desktop.
    ///
    /// The viewer shows it; the instrument hands it to a viewer of its own,
    /// because a recording being read must not displace the sweep on screen.
    void openSessionFile(const std::filesystem::path& path);

    /// Title the window should carry, or empty to leave it alone. Recomputed
    /// only when the open file changes, so it is cheap to poll each frame.
    [[nodiscard]] std::string pendingWindowTitle();

    /// How a snapshot reaches the window's own frame loop.
    ///
    /// A snapshot is a read-back of the finished frame, which exists only
    /// between the render and the swap -- neither of which this class drives.
    /// Handed in rather than reached for, the same way the file-drop handler
    /// goes the other way, so nothing here has to know an AppWindow exists.
    void setFrameCaptureRequest(std::function<void(std::function<void()>)> request);

    /// How a change of text weight reaches the font atlas, which the window
    /// owns and rebuilds between frames.
    void setFontWeightRequest(std::function<void(float)> request);

private:
    /// Acts on a button pressed on a toast, by the id the card carried.
    ///
    /// Ids rather than callbacks, because ToastCenter is deliberately free of
    /// both ImGui and thread affinity; see ToastAction. The set is small and
    /// closed, and lives here so the strings are written twice at most -- once
    /// where the card is raised and once here.
    void handleToastAction(const std::string& id);

    /// Asks about a newer release, once, and raises the card if there is one.
    void pollUpdateCheck();

    /// Copies the plot column to the clipboard, or writes it to a PNG.
    ///
    /// Asks for the filename first, while the frame is still being built: a
    /// modal dialog opened between the render and the swap would freeze on a
    /// window that cannot repaint behind it.
    void takeSnapshot(bool toFile);

    /// Starts a second copy of this binary in viewer mode, on `path` when one
    /// is given and on the running session otherwise.
    void launchHistoryViewer(const std::filesystem::path& path = {});

public:
private:
    void drawToolbar();

    /// The panel launchers, and the popups they open.
    ///
    /// Every settings group lives behind one of these rather than in a
    /// permanently docked column. The plots are the instrument; everything
    /// else is something the operator opens, reads, and dismisses, so it
    /// should not be charging rent on the width of the spectrum.
    void drawPanelLaunchers();

    /// One launcher button plus its popup, with `body` drawn inside.
    ///
    /// Returns the width it consumed, so the caller can decide what still
    /// fits on the bar. `minWidth`/`maxWidth` override the width band the
    /// panels share, for a body that is a table rather than a column of rows.
    float drawPanelLauncher(const char* label, const char* id, const char* tooltip,
                            void (MainWindow::*body)(), float minWidth = 360.0F,
                            float maxWidth = 440.0F);

    /// One row over a panel: its number, the window it shows, and its own
    /// pause, detach and close.
    void drawPanelHeader(PanelView& view, ViewPanel& panel);

    /// Elapsed-time labels down one edge of the waterfall, and the scrolling
    /// they accept.
    ///
    /// Drawn over the pane rather than beside it, so the waterfall keeps the
    /// spectrum's exact horizontal extent and a signal stays at the same x in
    /// both.
    bool drawWaterfallTimeAxis(WaterfallRenderer& renderer, float x, float y, float width,
                               float height, std::uint32_t visibleLines, std::uint32_t maxScroll);

    /// The draggable divider between the spectrum and the waterfall.
    /// `budget` is the height the two panes share; `fraction` the waterfall's.
    void drawPaneSplitter(float budget, float& fraction);

    void drawSpectrum(PanelView& view, ViewPanel& panel);
    void drawWaterfall(PanelView& view, ViewPanel& panel);
    void drawStatusBar();

    // ---- panels (MainWindowViews.cpp) ------------------------------------

    /// Brings `m_panels` in line with the layout: one runtime half per view,
    /// matched by id, created and dropped as views come and go.
    void syncPanels();
    [[nodiscard]] ViewPanel& runtimeFor(const PanelView& view);

    /// The plot area: the overview strip in Spans mode, then the attached
    /// panels in their arrangement with the splitters between them.
    void drawPanels();

    /// One panel -- header, spectrum, splitter, waterfall -- filling the
    /// current window from the cursor down.
    void drawPanelBody(PanelView& view, ViewPanel& panel);

    /// Closes a panel after the frame has drawn, so no list is changed under a
    /// loop still walking it. In Spans its segment leaves the sweep with it.
    void closePanel(int id);

    /// Panels torn off into windows of their own. Drawn after the root window
    /// has ended, since a window cannot be begun inside another's child.
    void drawFloatingPanels();

    /// The splits between arranged panels, drawn over the gaps.
    void drawLayoutSplitters(PanelArrangement arrangement, const std::vector<PanelRect>& rects,
                             const PanelRect& area, float gap);

    /// What a view may not leave, and what it shows with no window of its own:
    /// the radio's reach and the data in Mirror, the bound segment in Spans,
    /// the session in the viewer.
    [[nodiscard]] ViewLimits panelLimits(const PanelView& view) const;
    [[nodiscard]] FrequencySpan panelFit(const PanelView& view) const;
    [[nodiscard]] FrequencySpan panelRange(const PanelView& view) const;

    /// Moves a panel's window, clamped to its limits. The one way any gesture
    /// or button changes a view.
    void setPanelRange(PanelView& view, double fromHz, double toHz);

    /// This frame's line `index` reduced to `bins` columns.
    [[nodiscard]] const std::vector<float>& reducedLine(std::size_t index, std::uint32_t bins);

    /// Hands the theme's colormap to every waterfall that exists.
    void applyWaterfallColorMap();

    /// Marker levels and peak locks, once a frame before any panel draws.
    void refreshMarkers();

    /// Puts every view back to fit when the plan's outer bounds have moved,
    /// and rebinds the Spans panels when its segments have.
    void followPlan();

    /// Clones the focused panel into new slots or drops trailing attached
    /// panels, until the attached ones fill `arrangement`.
    void setArrangement(PanelArrangement arrangement);

    void setPanelMode(PanelMode mode);

    /// Zooms each panel onto one swept range, lowest first, grouping the
    /// closest ranges when there are more of them than panels. Mirror only:
    /// the panels stay free to move afterwards.
    void fitPanelsToRanges();
    void drawPanelsPopup();

    /// Binds each panel to a segment of the plan, by overlap.
    void rebindSpans();

    /// Replaces the plan with `segments`, normalised through `addSegment`.
    void applySegments(const std::vector<SweepSegment>& segments);

    /// The whole range above the Spans panels: a coarse live trace with each
    /// segment marked, and the gestures that add and pick segments.
    void drawOverviewStrip(float height);

    /// The message stack in the top-right corner.
    ///
    /// Frame-level chrome rather than a panel, and drawn by both the
    /// instrument and the viewer: a message raised inside the spectrum, which
    /// the two share, has to land somewhere in either of them.
    void drawToasts();

    /// Raises a message. The clock is the house one, so no call site carries
    /// it, and every message in the window is timed by the same reading.
    void toast(ToastSeverity severity, std::string text);

    void drawPerformancePanel();
    void drawFftBenchmarkWindow();
    void drawHistoryWindow();
    void drawHistoryToolbar();

    /// The centred panel shown when there is no file to draw, and the one
    /// shown over whatever there is while the next file is being read.
    void drawHistoryEmptyCard();
    void drawHistoryLoadingCard();

    /// The same panel over the instrument, while the bus is being probed and
    /// the saved radio opened.
    void drawStartupCard();

    /// And again while a session is being closed and put where it belongs.
    void drawSessionSaveCard();

    /// A centred modal saying that something slow is running, and for how
    /// long. Held back briefly so work that finishes quickly never flashes it,
    /// and dismissed by the same call once `active` goes false.
    void drawProgressCard(const char* id, bool active, const std::string& heading,
                          const std::string& detail, std::uint64_t startedNs);

    /// Asks for a session file and opens whatever is chosen.
    void promptForHistoryFile();

    /// Starts reading `path` on a worker thread. `reload` says this is the
    /// file already open being re-read, which keeps the playhead where it is.
    void beginHistoryOpen(const std::filesystem::path& path, bool reload = false);

    /// Hands a finished read to the view, or its failure to the status bar.
    void pollHistoryOpen();

    /// Keep the window's title on what it is actually showing.
    void updateHistoryTitle();
    void updateInstrumentTitle();
    void setWindowTitle(std::string title);

    void drawHistoryTimeline();
    void drawHistoryTransport();
    void drawHistoryWaterfall();
    void drawHistoryOverview(float height);
    void drawHistoryStripSplitter();
    void drawHistoryStatusBar();
    void drawHistorySettingsPopup();
    void advanceHistoryPlayback();
    void refreshHistorySpectrum();
    void seekHistory(std::uint64_t monotonicNs);
    void drawHistoryAxes(const ImVec2& origin, const ImVec2& size, float axisWidth,
                         float axisHeight);
    void drawClosePrompt();

    /// The Corrections block of the Analysis panel, and its two prompts.
    void drawCorrectionsBlock(bool sweeping);
    void drawLearnPrompt();
    void drawClearCorrectionsPrompt();

    /// A labelled frequency field in MHz with coarse/fine nudge buttons.
    ///
    /// The buttons are the point. Typing is fine for jumping somewhere known,
    /// but walking an edge onto a signal is a repeated small adjustment, and
    /// re-typing a six-digit number to move it 1 MHz is the wrong interaction
    /// for that. `id` distinguishes rows that share a label.
    [[nodiscard]] bool frequencyRow(const char* label, double& valueHz, int id);

    /// Saved ranges, with favourite and delete. Returns true if the plan was
    /// changed by loading one.
    [[nodiscard]] bool drawPresetList(SweepPlan& plan);

    /// Merges a preset's ranges into the plan instead of replacing it.
    void addPresetToPlan(SweepPlan& plan, const SweepPreset& preset);

    /// Saved marker sets, with favourite and delete. The same list as the
    /// ranges have, for the same reason: a bench watches the same handful of
    /// frequencies week after week, and placing them again by hand every
    /// session is the work a preset exists to remove.
    void drawMarkerPresetList(MarkerSet& markers);

    // Panel bodies. Each is the whole content of one launcher's popup.
    void drawSourceSection();

    /// The sweep range: ranges, presets, and the device's own limits. Behind
    /// the range button in the centre of the bar rather than in a settings
    /// panel, because it is the one setting an operator changes constantly.
    void drawRangeSection();

    /// Resolution, window and throughput, simple or advanced.
    ///
    /// One panel rather than a "Sweep" and an "FFT" one: RBW and transform
    /// size are the same quantity from two ends, and separating them let each
    /// half show a number the other half had already overridden.
    void drawAnalysisSection();

    /// The markers: the list, which one the gestures act on, and what is under
    /// each.
    ///
    /// Its own launcher rather than a block in Display, because a marker is
    /// something an operator places and reads constantly while watching a
    /// sweep, not a preference set once. Display keeps only where the readout
    /// card sits, which is a drawing choice.
    void drawMarkersSection();
    void drawDisplaySection();
    void drawThemeSection();

    /// Interface scale and text size.
    ///
    /// Not in Display, which is about what is drawn on the plots, and not in
    /// Theme, which travels in a profile: how big the interface is belongs to
    /// the screen it is on, not to a measurement configuration.
    void drawAppearanceSection();
    void drawProfilesSection();
    void drawContributorsSection();
    void drawPluginsSection();

    /// The antenna library: what the operator owns, and what each covers.
    ///
    /// In the menu rather than in the device panel because it outlives any one
    /// radio -- the same discone is on the bench whichever receiver is plugged
    /// in. The device panel is where one gets *assigned* to a connector.
    void drawAntennasSection();

    /// A window rather than a popover, for the same reason the channel editor
    /// is one: an antenna is typed in over a minute with a datasheet open
    /// beside it, and a popover closes the moment attention moves.
    void drawAntennaEditor();

    /// Opens the editor on `antenna`, or on a blank entry when it is null.
    void beginEditingAntenna(const Antenna* antenna);

    /// The open radio's connectors and what is on each, in the device panel.
    ///
    /// Bespoke rather than generated, and that is consistent rather than an
    /// exception: the "no per-*device* UI code" rule is about not knowing one
    /// radio from another, and a receive port is a first-class `ISdrDevice`
    /// concept exactly as `info()` and `healthReadings()` are.
    void drawDeviceAntennas();

    /// Which connector the radio is listening on right now, in the status bar.
    ///
    /// Live rather than configured: a routed sweep moves it several times a
    /// pass, and "which antenna measured this" is the question an operator has
    /// while looking at the trace, not while setting the bench up.
    void drawRxPortChip(const ChromeTheme& chrome);

    /// One assignment row: a label, a combo, and the caption under it.
    ///
    /// Shared by a connector and by a switcher input, because from the
    /// operator's side they are the same question -- what is screwed onto
    /// this -- and two copies would answer it two ways.
    ///
    /// `port` is null for a switcher input, which has no bias-T of its own.
    void drawAntennaRow(const char* label, const struct SdrRxPort* port,
                        std::string_view assignedId, bool biasTeeOn, bool deviceHasBiasTee,
                        const std::function<void(std::string_view)>& assign,
                        const std::function<void(std::string_view)>& assignSwitcher);

    // Popovers.
    void drawChartSettingsPopup();
    void drawWaterfallSettingsPopup();
    void drawGeneralSettingsPopup();

    /// The application settings without a popup around them, so the merged
    /// menu can host the same controls.
    void drawGeneralSettingsBody();

    /// Display, theme, profiles, plugins and application settings, together.
    void drawMenuSection();
    void drawGradientEditor();

    /// Builds the SDR panel entirely from `parameters()`. There is no
    /// per-device UI code anywhere in the project; this is the one function
    /// that renders any radio.
    void drawDeviceParameters();

    /// Whether a pane's clicks reach the markers at all.
    ///
    /// The viewer's waterfall says which *line* to plot, so an unmodified click
    /// there means a time rather than a frequency, and the right button has
    /// nothing to place. Panning and the band selections are identical either
    /// way, so this is a flag rather than a second copy.
    enum class MarkerClicks { Enabled, Disabled };

    /// Wheel zoom, left-drag pan, the shift-drag band selections, and the right
    /// button's markers.
    ///
    /// Shared by both panes rather than living with the spectrum. They show the
    /// same frequencies at the same x, so a gesture that works on one and not
    /// the other is arbitrary from the operator's side -- and the waterfall is
    /// often the pane a signal is spotted in first.
    void handleFrequencyGestures(const struct SpectrumLayout& layout, bool hovered,
                                 FrequencyPane pane, PanelView& view, ViewPanel& panel,
                                 MarkerClicks markers = MarkerClicks::Enabled);

    /// Custom-drawn spectrum layer over the ImPlot host: envelope band,
    /// gradient fill, markers, Y handles and the gradient bar.
    void drawSpectrumOverlay(const struct SpectrumLayout& layout, PanelView& view,
                             ViewPanel& panel);

    /// The card over the waterfall: what is under the selected marker, and the
    /// measurements between it and the previous visible one.
    ///
    /// On the waterfall rather than the spectrum because the spectrum's top
    /// corners are where the trace itself usually is, and because the anchor
    /// setting can then put it anywhere along a pane whose content is a picture
    /// rather than a line to be read.
    void drawMarkerReadout(const ImVec2& origin, const ImVec2& size);

    /// Publishes a `MarkerEvent` for each marker that has moved since the last
    /// frame.
    ///
    /// A per-frame diff rather than a call at each gesture site: the markers
    /// are mutated in place in several places, and a diff both catches all of
    /// them and debounces a drag into one event per resting position. Only the
    /// frequency is compared -- the level is re-read from the live trace every
    /// frame, so comparing it would publish continuously while a signal
    /// breathes.
    void publishMarkerChanges();

    AppState& m_state;

    /// The runtime half of each panel in the layout, in no particular order.
    std::vector<ViewPanel> m_panels;

    /// This frame's waterfall lines, taken once and fanned out to every panel
    /// that is not paused.
    std::vector<AppState::WaterfallLine> m_frameLines;

    /// `m_frameLines` reduced to a texture width, by width, made on first use
    /// this frame and shared by every Mirror panel that width.
    std::map<std::uint32_t, std::vector<std::vector<float>>> m_reducedLines;
    std::vector<float> m_reduceScratch;

    /// The plan generation and segments the views were last fitted and bound
    /// to. Empty until the first frame, which adopts whatever the saved
    /// layout brought rather than resetting it.
    std::optional<std::uint64_t> m_viewResetSeen;
    std::vector<SweepSegment> m_boundSegments;
    bool m_segmentsBound = false;

    /// A panel whose close button was pressed this frame, or zero.
    int m_closePanelId = 0;

    /// Screen rectangle the snapshot is cut from -- every attached panel --
    /// in ImGui points relative to the main viewport, captured each frame.
    ImVec4 m_snapshotRect{};
    std::function<void(std::function<void()>)> m_frameCaptureRequest;
    std::function<void(float)> m_fontWeightRequest;

    UpdateCheck m_updateCheck;
    /// The release the card is offering, kept so its button has somewhere to
    /// send the operator after the card itself is gone.
    std::string m_updateUrl;

    /// Opens the device panel once, unprompted, when nothing is connected.
    ///
    /// With no radio open there is nothing to draw and no other obvious first
    /// move, so the panel that lists the devices should be the thing already
    /// in front of the operator. Only on the first frame it becomes true --
    /// re-opening it every frame would make the panel impossible to dismiss.
    bool m_devicePromptShown = false;

    /// Whether the device panel is offering the device list rather than the
    /// open device's settings. Set when nothing is open, or by "Change
    /// device".
    bool m_deviceChooserMode = false;

    /// Whether a radio was open last frame, so the frame one arrives on can be
    /// told from every frame after it.
    bool m_deviceWasOpen = false;

    /// Where each marker was when it was last published, by id, for the diff in
    /// `publishMarkerChanges`.
    ///
    /// Keyed on the id rather than held as positions: markers are deleted from
    /// the middle of the list, and a positional diff would read the shuffle
    /// that follows as every marker after it having moved.
    std::map<int, double> m_publishedMarkers;

    /// What the operator is doing with the contribution flags: which one is
    /// picked out, which "…" list is open, and whether a flag took this
    /// frame's click.
    ///
    /// The last is what keeps a marker off it: the contributions are drawn at
    /// the top of `drawSpectrumOverlay` and the click gestures handled at the
    /// bottom of it, so the flag gets to answer first.
    ContributionInteraction m_contributions;

    /// Range editor state: several ranges rather than one, and whether the
    /// single-range editor is showing start/stop or centre/span.
    bool m_multipleRanges = false;
    int m_rangeEntryMode = 0;

    /// 0 simple, 1 advanced.
    int m_advancedAnalysis = 0;
    std::string m_newPresetName;
    std::string m_newMarkerPresetName;
    std::string m_newProfileName;
    std::string m_newThemeName;

    /// The instant picked out of a session, which the linked spectrum plots.
    ///
    /// Time only. The frequency being examined is the spectrum's marker, the
    /// same one the instrument uses; this used to carry a second copy of it and
    /// the two disagreed about which one the overview strip should follow.
    struct HistoryPick {
        std::uint64_t monotonicNs = 0;
    };

    /// A session file being read on a worker thread.
    ///
    /// Opening walks the whole file -- and rebuilds the index from that walk
    /// when the session ended abruptly -- which on a long recording is seconds
    /// of work. Done on the UI thread it froze the window with nothing on
    /// screen to say why, so it happens off it and the frame keeps running.
    struct HistoryOpenResult {
        std::unique_ptr<session::SessionReader> reader;
        std::string error;
    };

    struct HistoryLoad {
        std::filesystem::path path;
        /// Re-reading the file already shown, which keeps the playhead: a
        /// growing session is reloaded to see its tail, not to start over.
        bool reload = false;
        std::uint64_t startedNs = 0;
        std::future<HistoryOpenResult> future;
    };

    HistoryView m_history;

    /// The viewer's one panel. Bounded by the session rather than by any one
    /// segment of it, so the zoom holds as the playhead crosses boundaries.
    /// Never saved.
    PanelView m_historyView;
    ViewPanel m_historyPanel;
    bool m_historyLevelsAdopted = false;
    /// True when this process is a viewer rather than the instrument.
    bool m_historyViewerMode = false;
    std::optional<HistoryLoad> m_historyLoad;
    std::string m_historyMessage;
    std::optional<HistoryPick> m_historySelection;
    std::vector<float> m_historySpectrum;
    std::uint32_t m_historySpectrumSegment = 0;

    bool m_historyPlaying = false;
    float m_historySpeed = 1.0F;
    std::uint64_t m_historyLastTickNs = 0;

    /// Frequency width the overview strip aggregates around the marker.
    ///
    /// Wide enough to hold a carrier and its neighbours, so moving the marker
    /// visibly re-aims the strip; zero means the whole recorded span.
    double m_historyBandHz = 50e6;
    float m_historyOverviewHeight = 72.0F;

    bool m_showPerformance = false;
    bool m_showHistory = false;
    bool m_showGradientEditor = false;
    bool m_showFftBenchmark = false;
    bool m_showLearnPrompt = false;
    bool m_showClearCorrectionsPrompt = false;

    bool m_showAntennaEditor = false;
    Antenna m_editingAntenna;

    /// The id the editor was opened on, empty for a new entry.
    ///
    /// Kept separately so a rename removes the old entry rather than leaving
    /// the library holding both -- and so editing a shipped antenna knows
    /// which id the copy has to carry to shadow it.
    std::string m_editingAntennaOriginalId;

    /// The switchers on the bench, and when they were last looked for.
    ///
    /// Cached rather than asked per frame: `enumerate` is a bus scan per
    /// driver, and the panel is drawn sixty times a second.
    std::vector<RfPathInfo> m_switcherList;
    std::uint64_t m_switcherListNs = 0;

    /// Which backend is fastest is a property of this machine, so it is
    /// measured here rather than asserted in a document. Owned by the window
    /// and not by AppState: nothing outside the panel acts on the result, and
    /// the run must be abandoned when the window goes away.
    FftBenchmarkRunner m_fftBenchmark;

    /// How long to spend per measurement: 0 quick, 1 normal, 2 thorough.
    int m_fftBenchDepth = 1;

    /// How far up the size ladder to go, as an index into the tops offered.
    /// The reason it is adjustable rather than fixed is resolution: reaching
    /// a 1 kHz RBW needs a transform two decades longer than the default
    /// ladder's top, and a backend's ranking is not the same there.
    int m_fftBenchTop = 0;

    /// Which thread counts to measure at. One thread isolates the transform;
    /// the pipeline's worker count is the one that predicts a sweep, and the
    /// two do not always rank the backends the same way -- which is the whole
    /// reason both are offered.
    bool m_fftBenchOneThread = true;
    bool m_fftBenchAllWorkers = true;

    /// Which column the results table shows.
    int m_fftBenchMetric = 0;

    /// What the window is called, and whether the change has reached GLFW yet.
    /// Recomputed every frame and compared, so closing a file or a radio
    /// renames the window as surely as opening one does.
    std::string m_windowTitle;
    bool m_windowTitleDirty = false;

    bool m_closeRequested = false;
    bool m_quitConfirmed = false;
    /// Set when quitting is waiting on a session being written.
    bool m_quitWhenSessionSaved = false;

    /// The overview strip's own window, independent of every panel's; zero
    /// means fit. And its shift-drag in progress.
    FrequencySpan m_overviewView;
    bool m_overviewSelecting = false;
    float m_overviewSelectStartX = 0.0F;
    EnvelopeCache m_overviewEnvelopes;

    /// Which marker's row in the panel has its editor open, or zero.
    ///
    /// Behind a button rather than always shown: typing an exact frequency is
    /// the rare case -- a marker is normally placed by pointing at a signal --
    /// and a field with six nudge buttons beside it is taller than the list it
    /// belongs to.
    int m_editingMarker = 0;

    int m_editingColorMapStop = -1;
    ColorMap m_editingColorMap;
    std::string m_newColorMapName = "custom";

    /// Where an Appearance slider's value lives while it is being dragged.
    ///
    /// Not in the settings, because those are applied the moment they change
    /// and applying a scale rescales the panel the slider is in -- the row
    /// moves, the grab moves out from under the pointer, and the same hand
    /// position now means a different number. Held here until the gesture
    /// ends, so the interface resizes once, when the operator has decided.
    std::optional<float> m_pendingUiScalePercent;
    std::optional<float> m_pendingFontSize;
    std::optional<float> m_pendingFontWeight;

    /// Whether the pointer was over the toast stack last frame, which holds
    /// every timer.
    ///
    /// Carried across frames rather than read inside `update`, because a
    /// window's hover state is only knowable once it has been submitted --
    /// which is after the cards have already been decided for this frame.
    bool m_toastHovered = false;
};

} // namespace sweeppp::ui
