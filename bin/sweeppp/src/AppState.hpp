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
#include <sweeppp/correction/Corrections.hpp>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/history/IFrameSource.hpp>
#include <sweeppp/history/SessionRecorder.hpp>
#include <sweeppp/instrument/LocalInstrument.hpp>
#include <sweeppp/pipeline/FrameBus.hpp>
#include <sweeppp/pipeline/Pipeline.hpp>
#include <sweeppp/profile/Profile.hpp>
#include <sweeppp/remote/RemoteInstrument.hpp>
#include <sweeppp/remote/ServerList.hpp>
#include <sweeppp/rf/Antenna.hpp>
#include <sweeppp/rf/AntennaAssignments.hpp>
#include <sweeppp/rf/IRfPath.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <sweeppp/sweep/RangeHistory.hpp>
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

    /// Closes the radio, or disconnects from the server serving it.
    void closeDevice();

    /// The radio, the pipeline and the sweep engine, wherever they run.
    [[nodiscard]] Instrument& instrument() noexcept {
        return m_remote ? static_cast<Instrument&>(*m_remote) : *m_local;
    }
    [[nodiscard]] const Instrument& instrument() const noexcept {
        return m_remote ? static_cast<const Instrument&>(*m_remote) : *m_local;
    }

    /// The server the instrument is on, or null when it is in this process.
    [[nodiscard]] const remote::RemoteInstrument* remoteInstrument() const noexcept {
        return m_remote.get();
    }

    /// Saved servers. Mutated in place by the UI; `saveServers()` keeps it.
    [[nodiscard]] remote::ServerList& servers() noexcept { return m_servers; }
    void saveServers();

    /// The open radio as values, or null.
    [[nodiscard]] const DeviceDescriptor* device() const noexcept { return instrument().device(); }

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

    /// Connects to a server, on the same worker. `label` is what the operator
    /// calls it.
    void beginConnectServer(const remote::RemoteEndpoint& endpoint, const std::string& label);

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
    //
    // What adds something to the instrument's own: a start also opens the
    // session, and a plan change is also a place in the range history.

    [[nodiscard]] Status start();
    void stop();
    [[nodiscard]] bool running() const noexcept { return instrument().running(); }
    [[nodiscard]] bool sweeping() const noexcept { return instrument().sweeping(); }
    [[nodiscard]] const SweepPlan& sweepPlan() const noexcept { return instrument().sweepPlan(); }

    [[nodiscard]] Status applySweepPlan(const SweepPlan& plan);

    /// Sweeps `plan`, whether or not the radio was sweeping before. One
    /// restart, where setting the two separately would cost two.
    Status sweepRange(const SweepPlan& plan);

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
    void resetTelemetry() {
        instrument().resetTelemetry();
        m_telemetry.reset();
        m_stats = m_telemetry.snapshot();
    }
    [[nodiscard]] const TelemetrySnapshot& stats() const noexcept { return m_stats; }

    /// A device reading with the history needed to plot it.
    struct HealthTrace {
        SdrHealthReading reading;
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

    FrameBus m_displayBus; ///< what the instrument publishes: the UI and recorder consume it
    Telemetry m_telemetry;
    EventBus m_events;

    /// After the buses it publishes onto, so it is destroyed before them.
    std::unique_ptr<LocalInstrument> m_local;

    /// While connected to a server, the instrument in use; the local one
    /// waits with no radio, and takes the configuration back when this goes.
    std::unique_ptr<remote::RemoteInstrument> m_remote;
    remote::ServerList m_servers;

    /// The instrument's start generation as last seen; a new one is a new run.
    std::uint64_t m_startGenerationSeen = 0;

    std::unique_ptr<session::SessionRecorder> m_writer;

    SweepPresetStore m_presets;
    MarkerPresetStore m_markerPresets;

    SweepRangeHistory m_rangeHistory;

    /// Set while replaying history, so stepping back does not itself get
    /// recorded as a new place to come back to.
    bool m_navigatingHistory = false;

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
        std::unique_ptr<remote::RemoteInstrument> remote;
        std::string error;
    };

    struct DeviceStartup {
        /// The radio being opened, empty when this is only a bus probe.
        std::string driver;
        std::string id;
        /// Or the server being connected to, instead of a radio.
        std::optional<remote::RemoteEndpoint> server;
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

    /// What a plan change means here, before the instrument sees it: a place
    /// in the range history, and a view reset when its bounds moved.
    void recordPlanChange(const SweepPlan& plan);

    /// A new run, noticed: the traces start over and the session opens.
    void followStart();

    /// Leaves the server, handing what it was doing to the local instrument.
    void dropRemote();

    /// A server's address as an endpoint, with its token if it is saved.
    [[nodiscard]] remote::RemoteEndpoint endpointFor(const std::string& address) const;

    /// Hands what the instrument raised to the toasts and the latched error.
    void deliverNotices();

    /// Device readings into their histories, at the telemetry cadence.
    void sampleHealth();

    /// The consumer callback parks the newest frame here and returns; the UI
    /// thread picks it up. Nothing expensive happens on the publishing thread.
    mutable std::mutex m_frameMutex;
    SpectrumFramePtr m_pendingFrame;
    SpectrumFramePtr m_latestFrame;
    std::vector<WaterfallLine> m_pendingWaterfallLines;

    std::uint64_t m_lastTelemetrySampleNs = 0;
};

} // namespace sweeppp::ui
