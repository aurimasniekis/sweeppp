// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "AppSettings.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sweeppp/core/EventBus.hpp>
#include <sweeppp/core/Telemetry.hpp>
#include <sweeppp/correction/CorrectionLearner.hpp>
#include <sweeppp/correction/Corrections.hpp>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/history/IFrameSource.hpp>
#include <sweeppp/history/SessionRecorder.hpp>
#include <sweeppp/pipeline/FrameBus.hpp>
#include <sweeppp/pipeline/Pipeline.hpp>
#include <sweeppp/profile/Profile.hpp>
#include <sweeppp/rf/Antenna.hpp>
#include <sweeppp/rf/AntennaAssignments.hpp>
#include <sweeppp/rf/IRfPath.hpp>
#include <sweeppp/rf/RfRouting.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <sweeppp/sweep/RangeHistory.hpp>
#include <sweeppp/sweep/SweepEngine.hpp>
#include <sweeppp/sweep/SweepPreset.hpp>
#include <sweeppp/ui/MarkerPreset.hpp>
#include <sweeppp/ui/Theme.hpp>
#include <sweeppp/ui/ToastCenter.hpp>
#include <sweeppp/ui/TraceStore.hpp>
#include <sweeppp/ui/ViewSettings.hpp>
#include <vector>

namespace sweeppp::ui {

/// Everything the UI reads and the engine writes.
///
/// The UI thread never touches the pipeline's internals directly: it takes
/// frames from the bus through this class's consumer, and drives the engine
/// through explicit methods. That keeps every cross-thread interaction in one
/// reviewable place.
class AppState final : public IFrameConsumer {
public:
    AppState();
    ~AppState() override;

    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;

    /// Runs the state without a radio, for the session viewer.
    ///
    /// Must be set before `initialise`, which is where the saved profile would
    /// otherwise reopen whatever device it was captured with. A viewer reading
    /// a file has no business claiming the hardware -- doing so blocks the
    /// instrument itself from opening it.
    void setViewerMode(bool value) noexcept { m_viewerMode = value; }
    [[nodiscard]] bool viewerMode() const noexcept { return m_viewerMode; }

    [[nodiscard]] Status initialise();

    // ---- frame intake ----------------------------------------------------

    void onFrame(const SpectrumFramePtr& frame) noexcept override;
    [[nodiscard]] std::string_view consumerName() const noexcept override { return "ui"; }

    /// Moves the newest frame into the traces. Called once per rendered frame
    /// on the UI thread -- the consumer callback only parks a pointer, so a
    /// slow redraw can never stall the pipeline.
    void pumpFrames();

    // ---- devices ---------------------------------------------------------

    [[nodiscard]] const std::vector<SdrDeviceInfo>& availableDevices() const noexcept {
        return m_devices;
    }

    /// Re-probes the bus on the worker. Slow enough to be worth saying so:
    /// enumeration walks every driver and every device each one claims.
    void beginRefreshDevices();

    void closeDevice();
    [[nodiscard]] ISdrDevice* device() const noexcept { return m_device.get(); }

    // ---- startup ---------------------------------------------------------
    //
    // Finding the radios and opening the saved one are the two slowest things
    // the application does, and both used to run inside `initialise()` --
    // before the window was created. A BladeRF is enumerated over USB and
    // opened with a firmware handshake, so the operator got several seconds of
    // nothing at all: no window, no cursor change, no way to tell a slow start
    // from a hang. They now run on a worker thread while the window is up.

    /// Starts the bus probe, and the saved profile's radio behind it.
    /// Does nothing in viewer mode, which deliberately claims no hardware.
    void beginDeviceStartup();

    /// Opens a radio the operator picked, on the same worker.
    ///
    /// `label` is what the panel calls it, which is what the operator clicked
    /// on -- the driver string is the file format's name for it, not theirs.
    void beginOpenDevice(const std::string& driver, const std::string& id,
                         const std::string& label);

    /// Adopts a finished open. Called once per frame on the UI thread.
    void pollDeviceStartup();

    [[nodiscard]] bool deviceStartupRunning() const noexcept { return m_startup.has_value(); }

    /// What is happening, and what it involves -- the two lines of the panel
    /// that says so.
    [[nodiscard]] std::string deviceStartupLabel() const;
    [[nodiscard]] std::string deviceStartupDetail() const;

    /// When it began, so the UI can hold its panel back long enough that a
    /// fast open never flashes it.
    [[nodiscard]] std::uint64_t deviceStartupStartedNs() const noexcept;

    // ---- run control -----------------------------------------------------

    [[nodiscard]] Status start();
    void stop();
    [[nodiscard]] bool running() const noexcept;

    /// Stops and starts, adopting whatever the radio agreed to.
    ///
    /// The one way to apply a change that cannot be made underneath a running
    /// sweep. Several settings are in that class -- the plan, the FFT backend,
    /// anything that resizes a block -- and each used to spell this out.
    [[nodiscard]] Status restart();

    [[nodiscard]] Status applyPipelineConfig(const PipelineConfig& config);
    [[nodiscard]] const PipelineConfig& pipelineConfig() const noexcept { return m_pipelineConfig; }

    [[nodiscard]] Status applySweepPlan(const SweepPlan& plan);

    /// The backend the transform is actually running on.
    ///
    /// Not `FftBackendManager::suggestedDefault()`, which is only what a fresh
    /// install would preselect and never changes: with more than one backend
    /// installed the two disagree the moment the operator picks the other one.
    [[nodiscard]] std::string_view fftBackendName() const noexcept;

    /// Switches the transform to another registered backend.
    ///
    /// A grid-affecting change, not a preference: backends differ in the sizes
    /// they accept, so the FFT size and with it the whole frequency grid can
    /// move underneath a switch. Applied by cycling acquisition, the same way
    /// a sweep plan change is, rather than by making the operator stop and
    /// start.
    [[nodiscard]] Status setFftBackend(std::string_view name);

    /// Applies a device parameter from the UI.
    ///
    /// Most go straight to the driver. The sample rate does not: while
    /// sweeping it *is* the step width, so it belongs to the plan, and setting
    /// it on the device alone would last until the engine next configured and
    /// wrote the plan's own rate over it.
    [[nodiscard]] Status setDeviceParameter(const std::string& key, const SdrValue& value);

    /// Whether the UI must hold this parameter until acquisition stops.
    ///
    /// Weaker than the driver's own `requiresStop`: what the plan owns is
    /// applied by cycling acquisition around it, which is a stop, so it stays
    /// editable mid-sweep.
    [[nodiscard]] bool parameterNeedsStop(const SdrParameter& parameter) const noexcept;

    /// Replaces the stored plan with the one the engine is actually running,
    /// after the device has clamped anything it could not honour.
    void adoptEffectivePlan();

    /// Warns once when a re-plan leaves spectrum no assigned antenna covers.
    void reportUnroutedRanges();

    // ---- range history ---------------------------------------------------
    //
    // Where the radio has been pointed, as a back/forward stack.
    //
    // Narrowing onto a signal is a one-way gesture -- a drag replaces the
    // range outright -- so getting back to the wider view meant retyping the
    // numbers that were on screen a moment earlier. Only the *range* is
    // tracked: resolution and mode are settings an operator adjusts and
    // leaves, while the range is navigated.

    [[nodiscard]] bool canGoBack() const noexcept { return m_rangeHistory.canGoBack(); }
    [[nodiscard]] bool canGoForward() const noexcept { return m_rangeHistory.canGoForward(); }

    /// Steps the sweep range back or forward. No-ops at either end.
    [[nodiscard]] Status goBack();
    [[nodiscard]] Status goForward();
    [[nodiscard]] const SweepPlan& sweepPlan() const noexcept { return m_sweepPlan; }

    // ---- profiles --------------------------------------------------------

    /// Whether building a profile also asks the plugins for their state.
    ///
    /// `Omit` exists because a plugin's save hook is a statement that a profile
    /// is being written, not a poll: the snapshot published for plugins to
    /// *read* is refreshed several times a second, and running every plugin's
    /// contribution hook at that rate would be both wasteful and a lie about
    /// what is happening.
    enum class PluginState : std::uint8_t { Include, Omit };

    /// The current configuration, as a profile.
    [[nodiscard]] Profile currentProfile(const std::string& name,
                                         PluginState plugins = PluginState::Include) const;

    /// Who starts the radio a profile names.
    ///
    /// Never the calling thread either way -- opening blocks for seconds and
    /// the window has to keep drawing. `Now` starts the worker here; `Deferred`
    /// leaves it to `initialise`, which probes the bus in the same pass.
    /// Either way the profile's display, analysis and ranges are restored
    /// immediately, so the window comes up looking like the one that closed.
    enum class DeviceHandling : std::uint8_t { Now, Deferred };

    /// Applies a profile: device, plan, analysis and display.
    ///
    /// Best-effort by design. A profile saved with a radio that is not plugged
    /// in now should still restore the resolution, the ranges and the
    /// colours -- refusing the lot because one part cannot be honoured would
    /// make profiles useless on any machine but the one they were written on.
    Status applyProfile(const Profile& profile, DeviceHandling devices = DeviceHandling::Now);

    /// Where the application's own state is kept between runs.
    void loadSettings();
    void saveSettings();

    /// Named profiles found in the config directory, by file stem.
    [[nodiscard]] std::vector<std::string> profileNames() const;

    /// Saved frequency ranges. Mutated in place by the UI; call
    /// `savePresets()` afterwards to make the change outlive the session.
    [[nodiscard]] SweepPresetStore& presets() noexcept { return m_presets; }
    void savePresets();

    /// Saved marker sets, in their own file rather than in a profile: a
    /// profile restores one whole session, while these are picked up and put
    /// down against whatever is already on screen.
    [[nodiscard]] MarkerPresetStore& markerPresets() noexcept { return m_markerPresets; }
    void saveMarkerPresets();

    /// What the operator has told the application is screwed onto a connector.
    ///
    /// Mutated in place by the editor; `saveAntennas()` writes the user file
    /// and reloads from disk, so the list and the file cannot disagree about
    /// which entries are shipped and which are the operator's own.
    [[nodiscard]] AntennaLibrary& antennas() noexcept { return m_antennas; }
    [[nodiscard]] const AntennaLibrary& antennas() const noexcept { return m_antennas; }
    void saveAntennas();
    void reloadAntennas();

    /// Which antenna is on which connector, for every radio this installation
    /// has seen. Not part of a profile: loading a saved job must not unscrew
    /// the horn from RX2.
    [[nodiscard]] AntennaAssignments& antennaAssignments() noexcept { return m_assignments; }
    [[nodiscard]] const AntennaAssignments& antennaAssignments() const noexcept {
        return m_assignments;
    }
    void saveAntennaAssignments();

    /// Re-resolves the RF path and, when a routed sweep is configured or
    /// running, re-plans it.
    ///
    /// Called after anything that changes what is in front of the tuner: an
    /// assignment, a fallback port, an edit to the library. The sweep is
    /// planned against the antennas, so leaving it on the old plan would have
    /// the panel describing one bench and the radio measuring another until
    /// something else happened to re-plan.
    void applyAntennaChange();

    /// The key the open radio's assignments are filed under, or empty when
    /// there is no radio.
    [[nodiscard]] std::string deviceAntennaKey() const;

    /// The antenna on `portId`, or null. An empty `portId` is the single
    /// implicit input a one-connector radio has.
    [[nodiscard]] const Antenna* antennaOnPort(std::string_view portId) const;

    /// Every antenna the open radio can hear through, with the band each
    /// reaches. Empty when there is no radio or nothing is assigned.
    [[nodiscard]] std::vector<RfLeg> rfPath() const;

    /// The frequencies the assigned antennas cover, merged and ascending.
    [[nodiscard]] std::vector<std::pair<double, double>> antennaCoverage() const;

    /// The antenna switchers this installation has open.
    [[nodiscard]] std::span<const OpenRfPath> switchers() const noexcept { return m_openPaths; }

    /// The switcher filed under `key`, or null.
    [[nodiscard]] IRfPath* switcher(std::string_view key) const;

    /// Opens every switcher an assignment names and closes the rest.
    ///
    /// Called after the assignments change and after a radio is adopted. A box
    /// nobody has assigned is left alone: opening it would claim a serial port
    /// or a USB handle to answer a question nothing is asking.
    void refreshSwitchers();

    // ---- receiver corrections --------------------------------------------
    //
    // DC removal, floor flattening and a spur mask, applied by the pipeline to
    // every frame, and the learn that produces the floor and the spurs. The
    // switches travel in a profile; what they apply is a file per radio,
    // loaded when the radio is adopted.

    [[nodiscard]] const CorrectionSettings& correctionSettings() const noexcept {
        return m_correctionSettings;
    }

    /// Takes effect on the next block, with no restart.
    void setCorrectionSettings(const CorrectionSettings& settings);

    /// What was learned for the open radio, or null when nothing has been.
    [[nodiscard]] const CorrectionSet* corrections() const noexcept;

    /// Why the learned floor is not being applied: the first setting that
    /// differs from when it was learned, or empty while it applies.
    [[nodiscard]] const std::string& floorStaleReason() const noexcept {
        return m_floorStaleReason;
    }

    [[nodiscard]] std::size_t spurCount() const noexcept;
    [[nodiscard]] std::size_t automaticSpurCount() const noexcept;

    /// Learns the floor and the spurs from what the receiver shows with no
    /// signal in. Needs acquisition running, and the antenna off.
    [[nodiscard]] Status startLearning();
    [[nodiscard]] bool learning() const noexcept { return m_learn.has_value(); }
    [[nodiscard]] std::string learningLabel() const;
    void cancelLearning();

    /// Drops the spurs found automatically this session.
    void clearAutoSpurs();

    /// Forgets everything learned for the open radio, file included.
    void clearCorrections();

    /// The learn's own numbers, for the prompt that starts one.
    [[nodiscard]] static std::size_t learnPasses() noexcept;
    static constexpr std::size_t kLearnFrames = 200;

    [[nodiscard]] bool sweeping() const noexcept { return m_sweeping; }

    /// Switches between sweeping the planned range and sitting on one centre
    /// frequency, restarting acquisition when it is running.
    ///
    /// Not a bare field write: the flag decides how the buses are wired and
    /// whether the waterfall advances per frame or per pass, and neither can
    /// be changed underneath a running pipeline.
    Status setSweeping(bool enabled);

    /// Sweeps `plan`, whether or not the radio was sweeping before. One
    /// restart, where setting the two separately would cost two.
    Status sweepRange(const SweepPlan& plan);
    [[nodiscard]] const SweepEngine& sweepEngine() const noexcept { return *m_sweepEngine; }

    // ---- recording -------------------------------------------------------

    /// Starts retaining the session to a working file, if it is not already.
    ///
    /// Called when acquisition starts. Everything measured is written from
    /// that moment, because "save what I have been watching" cannot be
    /// answered after the fact -- a writer created at the moment the operator
    /// asks would record nothing but the future.
    [[nodiscard]] Status beginSession();

    /// Closes the session, keeping it at `keepAs` or deleting it.
    ///
    /// Deleting is the *explicit* discard path, never a default: an operator
    /// who has been sweeping for an hour is asked first.
    ///
    /// Blocks until the file is written. Use beginEndSession() anywhere a
    /// frame still has to be drawn afterwards.
    void endSession(const std::optional<std::filesystem::path>& keepAs);

    /// The same, on a worker, for the paths with a window still up. Closing an
    /// hour of sweeping means flushing an index and then moving the file, and
    /// a move that crosses a volume is a copy of every byte.
    ///
    /// The writer is detached before the worker starts, so from the caller's
    /// next line the session is already closed: sessionOpen() is false and
    /// nothing can still be written to it.
    void beginEndSession(const std::optional<std::filesystem::path>& keepAs);

    /// Adopts a finished save. Called once per frame on the UI thread.
    void pollEndSession();

    [[nodiscard]] bool sessionSaveRunning() const noexcept { return m_sessionSave.has_value(); }
    [[nodiscard]] std::string sessionSaveLabel() const;
    [[nodiscard]] std::string sessionSaveDetail() const;
    [[nodiscard]] std::uint64_t sessionSaveStartedNs() const noexcept;

    /// Why the last save failed, empty when it did not. Kept as its own field
    /// rather than only raised as a message, so a caller waiting on this save
    /// can ask about it directly instead of reading whatever went wrong last.
    [[nodiscard]] const std::string& sessionSaveError() const noexcept {
        return m_sessionSaveError;
    }

    /// Where the session is being written, while one is open.
    [[nodiscard]] std::filesystem::path sessionPath() const;

    /// Lines written so far -- zero means there is genuinely nothing to save.
    [[nodiscard]] std::uint64_t sessionLines() const noexcept;

    /// True while a session is being written.
    ///
    /// Deliberately not called "recording": retention now runs for as long as
    /// acquisition does, so this is true almost always, and a caller reading
    /// it as "the operator asked to record" will conclude the opposite of the
    /// truth. Whether anything has been *kept* is hasUnsavedSession().
    [[nodiscard]] bool sessionOpen() const noexcept { return m_writer != nullptr; }
    [[nodiscard]] const session::SessionRecorder* writer() const noexcept { return m_writer.get(); }

    // ---- state -----------------------------------------------------------

    [[nodiscard]] TraceStore& traces() noexcept { return m_traces; }
    [[nodiscard]] const TraceStore& traces() const noexcept { return m_traces; }
    [[nodiscard]] ViewSettings& view() noexcept { return m_view; }
    [[nodiscard]] const ViewSettings& view() const noexcept { return m_view; }
    [[nodiscard]] Telemetry& telemetry() noexcept { return m_telemetry; }

    /// Zeroes the counters and histories, and the copy the panels read.
    ///
    /// Both, because they are refreshed on a 4 Hz timer rather than per frame:
    /// resetting only the telemetry would leave the display showing the old
    /// figures for a quarter of a second, which reads as the button not having
    /// worked. The snapshot is taken rather than resampled -- sampling
    /// immediately after a reset would compute rates over a zero interval.
    void resetTelemetry() noexcept {
        m_telemetry.reset();
        m_stats = m_telemetry.snapshot();
    }
    [[nodiscard]] const TelemetrySnapshot& stats() const noexcept { return m_stats; }

    /// A device reading with the history needed to plot it.
    struct HealthTrace {
        ISdrDevice::HealthReading reading;
        RollingHistory<240> history;
    };

    /// Device readings, sampled with the rest of the telemetry.
    [[nodiscard]] const std::vector<HealthTrace>& health() const noexcept { return m_health; }
    [[nodiscard]] FrameBus& displayBus() noexcept { return m_displayBus; }
    [[nodiscard]] EventBus& events() noexcept { return m_events; }

    [[nodiscard]] const Theme& theme() const noexcept { return m_theme; }
    [[nodiscard]] const std::vector<Theme>& themes() const noexcept { return m_themes; }
    void setTheme(const std::string& name);
    /// Re-reads the theme folders, so a theme saved this session is listed.
    void reloadThemes();
    /// Re-applies the current theme, for the gradient editor's live preview.
    void refreshTheme();

    /// Rebuilds the ImGui style, if the theme or the appearance has changed.
    ///
    /// Called at the top of a frame, before anything is drawn, and it has to
    /// be: every control that changes either of them is drawn inside a
    /// PushStyleVar scope, and the matching pop puts the padding it saved back
    /// over whatever the rebuild wrote. Rebuilding where the change is made
    /// therefore leaves the style half old and half new for good.
    void applyPendingStyle();

    // ---- appearance ------------------------------------------------------

    [[nodiscard]] AppSettings& appSettings() noexcept { return m_appSettings; }
    [[nodiscard]] const AppSettings& appSettings() const noexcept { return m_appSettings; }

    /// Takes the preferences read before the window existed, so the file is
    /// read once: the font atlas needs the size at window-creation time, which
    /// is before this object is initialised.
    void adoptAppSettings(AppSettings settings);
    [[nodiscard]] Status saveAppSettings() const;

    /// The multiplier actually in force -- the operator's, or the display's
    /// when they have not chosen one.
    [[nodiscard]] float effectiveUiScale() const noexcept;

    /// What the display asks for, handed in by the window that measured it.
    ///
    /// On macOS this is 1: the framebuffer is already twice the window's
    /// points and ImGui rasterises against that on its own. On Windows and X11
    /// at 150% it is 1.5, because there the window's coordinates *are* pixels
    /// and nothing scales the interface at all -- which is why the application
    /// rendered at 100% on a 150% display.
    void setDisplayUiScale(float scale);

    /// The size the font atlas was built at, which the live font size is
    /// expressed as a ratio against. Handed in by the window that built it.
    void setBaseFontSize(float points) noexcept;

    /// The placed markers. Part of the view settings, so they travel in a
    /// profile without this class having to carry them separately.
    [[nodiscard]] MarkerSet& markers() noexcept { return m_view.markers; }
    [[nodiscard]] const MarkerSet& markers() const noexcept { return m_view.markers; }

    /// Newest frame's metadata, for the info row and overlays.
    [[nodiscard]] SpectrumFramePtr latestFrame() const;

    /// What a Mirror panel's view is bounded by: what the *radio* can reach,
    /// not what is currently being swept.
    ///
    /// Bounding it to the sweep looks tidier and is a trap: selecting a band
    /// on the plot is how a sweep range gets set, so a view locked to the plan
    /// makes it impossible to ever select a range outside it. Past the edge of
    /// the sweep nothing is drawn, which reads correctly as "not measured".
    /// With no radio open, the data on screen is the only bound there is.
    [[nodiscard]] ViewLimits viewLimits() const;

    /// The window a panel with none of its own shows: the data, or before
    /// there is any, what the radio is tuned to, or failing that the FM band.
    [[nodiscard]] FrequencySpan fitRange() const;

    /// Bumped whenever the plan's outer bounds move, which is when the panels
    /// go back to fit.
    ///
    /// The view is a zoom into the swept span, so once that span moves the
    /// old window is at best a fraction of the new one and at worst nowhere
    /// near it -- re-planning from the FM band to 2.4 GHz would otherwise
    /// leave the display parked over frequencies no longer being measured.
    /// A profile being applied does not bump it: it brings its own views.
    [[nodiscard]] std::uint64_t viewResetGeneration() const noexcept {
        return m_viewResetGeneration;
    }

    /// Every message the operator is shown, from here and from the window
    /// alike. The state layer owns it because it is the object both sides
    /// already share.
    [[nodiscard]] ToastCenter& toasts() noexcept { return m_toasts; }
    [[nodiscard]] const ToastCenter& toasts() const noexcept { return m_toasts; }

    /// Set when something the operator should see goes wrong.
    ///
    /// A *latched condition* rather than an event: only one is up at a time,
    /// and the pair holds the card's id so clearing takes back what setting
    /// raised. The radio coming back should retract "not available", not leave
    /// it sitting in the corner contradicting a device that is plainly open.
    void setError(std::string message);
    void clearError();

    /// True once a session has data the operator has not chosen to keep or
    /// discard -- drives the prompt on close.
    /// True when a session holds data the operator has not decided about.
    [[nodiscard]] bool hasUnsavedSession() const noexcept {
        return m_hasUnsavedSession && sessionLines() > 0;
    }
    void markSessionHandled() { m_hasUnsavedSession = false; }

    /// One waterfall row: a completed pass when sweeping, a frame otherwise,
    /// with the grid it was measured on so each panel can cut its own slice.
    struct WaterfallLine {
        std::vector<float> dbfs;
        double startHz = 0.0;
        double binWidthHz = 0.0;
        std::uint64_t ns = 0; ///< Monotonic time it was measured.
    };

    /// Lines not yet handed to the panels, so the renderers can take them on
    /// the GL thread. Taken once per frame and fanned out; each panel decides
    /// for itself whether it is paused.
    [[nodiscard]] std::vector<WaterfallLine> takePendingWaterfallLines();

private:
    void applyThemeToImGui();

    /// Republishes pipeline frames onto the display bus when not sweeping.
    ///
    /// In sweep mode the SweepEngine occupies this position, stitching steps
    /// into a whole-span frame. Fixed tune needs no stitching, but everything
    /// downstream still binds to the display bus -- so the two modes differ
    /// only in what sits between the buses, not in what consumers see.
    class DisplayForwarder final : public IFrameConsumer {
    public:
        explicit DisplayForwarder(FrameBus& target) : m_target(target) {}

        void onFrame(const SpectrumFramePtr& frame) noexcept override { m_target.publish(frame); }
        [[nodiscard]] std::string_view consumerName() const noexcept override {
            return "display-forwarder";
        }

    private:
        FrameBus& m_target;
    };

    FrameBus m_pipelineBus; ///< raw per-step frames from the pipeline
    FrameBus m_displayBus;  ///< stitched frames the UI and recorder consume
    DisplayForwarder m_displayForwarder{m_displayBus};
    FrameBus::SubscriptionId m_pipelineSubscription = 0;
    Telemetry m_telemetry;
    EventBus m_events;

    std::unique_ptr<Pipeline> m_pipeline;
    std::unique_ptr<SweepEngine> m_sweepEngine;
    std::unique_ptr<ISdrDevice> m_device;
    std::unique_ptr<session::SessionRecorder> m_writer;
    IFftBackend* m_backend = nullptr;

    PipelineConfig m_pipelineConfig;
    SweepPlan m_sweepPlan;
    SweepPresetStore m_presets;
    MarkerPresetStore m_markerPresets;
    AntennaLibrary m_antennas;
    AntennaAssignments m_assignments;

    /// Owned; `m_openPaths` borrows from these, so the two are only ever
    /// rebuilt together.
    std::vector<std::unique_ptr<IRfPath>> m_switchers;
    std::vector<OpenRfPath> m_openPaths;

    /// What the last warning said, so a re-plan that changes nothing does not
    /// raise it again.
    std::vector<std::pair<double, double>> m_reportedUnroutedHz;

    SweepRangeHistory m_rangeHistory;

    /// Set while replaying history, so stepping back does not itself get
    /// recorded as a new place to come back to.
    bool m_navigatingHistory = false;
    bool m_sweeping = false;

    AppSettings m_appSettings;
    float m_displayUiScale = 1.0F;
    float m_baseFontSize = 15.0F;
    bool m_stylePending = false;
    bool m_hasUnsavedSession = false;
    FrameBus::SubscriptionId m_writerSubscription = 0;

    /// Unsubscribes the recorder from the display bus and hands it over, so
    /// the caller holds the only reference to it.
    [[nodiscard]] std::unique_ptr<session::SessionRecorder> detachWriter();

    /// A session being closed on a worker. The writer is moved in here, so
    /// nothing on the UI thread can reach it while it is being finished.
    struct SessionSave {
        std::optional<std::filesystem::path> keepAs;
        std::uint64_t lines = 0;
        std::uint64_t startedNs = 0;
        std::future<std::string> future; ///< The error, or empty on success.
    };
    std::optional<SessionSave> m_sessionSave;
    std::string m_sessionSaveError;

    TraceStore m_traces;
    ViewSettings m_view;
    TelemetrySnapshot m_stats;
    std::vector<HealthTrace> m_health;
    Theme m_theme;
    std::vector<Theme> m_themes;

    /// The outer bounds of the plan last applied, which a view reset is
    /// judged against. Not `m_sweepPlan`'s own: adopting a radio writes its
    /// full range there directly, moments before the saved plan is applied
    /// again, and comparing with that would throw away the saved views.
    double m_viewResetLowHz = 0.0;
    double m_viewResetHighHz = 0.0;
    std::uint64_t m_viewResetGeneration = 0;
    bool m_applyingProfile = false;

    std::vector<SdrDeviceInfo> m_devices;
    ToastCenter m_toasts;

    /// The card the latched error condition currently has up, or zero.
    ToastId m_errorToast = 0;
    bool m_viewerMode = false;

    /// The bus probe, and the radio behind it, running off the UI thread.
    ///
    /// Only the enumeration and the open itself go to the worker. Everything
    /// that follows -- publishing the event, restoring the profile's
    /// parameters, applying the sweep plan -- happens in `pollDeviceStartup`
    /// on the UI thread, because it touches the buses and the engine.
    struct StartupResult {
        std::vector<SdrDeviceInfo> devices;
        /// Whether `devices` is an answer rather than an empty default -- an
        /// open with no probe behind it must not blank the device list.
        bool enumerated = false;
        std::unique_ptr<ISdrDevice> device;
        std::string error;
    };

    /// Installs an opened radio: stops what was running, publishes the event,
    /// and takes the device's own full range as the plan.
    ///
    /// The one way a device is ever installed. Opening it is the worker's job
    /// and this is what it hands back to, so a radio restored from a profile
    /// and one picked from the list end up in the same state.
    void adoptDevice(std::unique_ptr<ISdrDevice> device);

    struct DeviceStartup {
        /// The radio being opened, empty when this is only a bus probe.
        std::string driver;
        std::string id;
        /// What to call it on screen.
        std::string label;
        /// Whether the bus is probed first. Only startup needs that; an
        /// operator opening a device picked it from a list already read.
        bool enumerate = true;
        std::uint64_t startedNs = 0;
        /// Raised by the worker when it stops probing and starts opening, so
        /// the panel can say which of the two is taking the time.
        std::shared_ptr<std::atomic_bool> opening;
        std::future<StartupResult> future;
    };

    /// Spawns the worker for a prepared request. The one place a device is
    /// ever opened off the UI thread.
    void startDeviceWorker(DeviceStartup startup);

    std::optional<DeviceStartup> m_startup;

    /// Held from `initialise` until the startup lands, because the parameters
    /// and the sweep plan it carries are applied to the device that arrives.
    std::optional<Profile> m_startupProfile;

    // ---- corrections ------------------------------------------------------

    [[nodiscard]] CalibrationContext currentContext() const;

    /// The switches as the pipeline should see them: a learn in progress
    /// overrides the flatten and mask switches for its own phases.
    [[nodiscard]] CorrectionSettings effectiveCorrectionSettings() const noexcept;
    void pushCorrectionSettings();

    /// Reads the open radio's calibration file, or clears it.
    void loadCalibration();

    /// Hands the pipeline the set, minus the floor when the context has
    /// moved since it was learned.
    void installCorrections();
    void refreshFloorStaleness();

    /// A learn, from the button to the saved file.
    struct LearnRun {
        /// Sweep: one pass for the floor and the LO-offset spurs, then
        /// several through them for whatever stands at a fixed frequency.
        /// Fixed tune: one phase, everything absolute.
        enum class Phase : std::uint8_t { LoOffsets, Absolute, Fixed };
        Phase phase = Phase::LoOffsets;
        CorrectionSettings savedSettings;
        CalibrationContext context;
        CorrectionLearner learner;
        FloorShape floor;
        std::vector<SpurEntry> loSpurs;
        std::uint64_t passesHandled = 0;
        /// The pass in progress when the LO-offset set went in; only a pass
        /// completed after it was measured through the set throughout.
        std::uint64_t installedAtPass = 0;
        /// The stitched grid's own learner, one frame per completed pass.
        CorrectionLearner gridLearner;
    };

    /// Called per frame on the UI thread, where the heavy end of a learn runs.
    void advanceLearning();
    void finishLearning(const std::vector<SpurEntry>& absoluteSpurs);
    void abortLearning();

    /// Each accepted step frame, on the bus thread.
    void observeStep(const SpectrumFrame& frame) noexcept;

    void beginAutoSpurs();
    void endAutoSpurs();
    void updateAutoSpurs();

    CorrectionSettings m_correctionSettings;
    std::optional<CorrectionSet> m_corrections;
    std::string m_floorStaleReason;

    /// Guards the two learners and `m_learn` itself, which the bus thread
    /// reads and the UI thread replaces.
    mutable std::mutex m_learnMutex;
    std::optional<LearnRun> m_learn;
    std::optional<CorrectionLearner> m_autoLearner;
    std::uint64_t m_autoPassesHandled = 0;

    /// Sweep passes completed, counted on the sweep thread and polled by the
    /// UI thread, so a phase change never runs on the thread that raised it.
    std::atomic<std::uint64_t> m_passesSeen{0};

    /// The consumer callback parks the newest frame here and returns; the UI
    /// thread picks it up. Nothing expensive happens on the publishing thread.
    mutable std::mutex m_frameMutex;
    SpectrumFramePtr m_pendingFrame;
    SpectrumFramePtr m_latestFrame;
    std::vector<WaterfallLine> m_pendingWaterfallLines;

    std::uint64_t m_lastTelemetrySampleNs = 0;
};

} // namespace sweeppp::ui
