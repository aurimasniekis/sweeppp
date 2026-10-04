// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AppState.hpp"

#include "BarChrome.hpp"
#include "FileDialog.hpp"
#include "Icons.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <imgui.h>
#include <implot.h>
#include <sweeppp/backends/sdr/SyntheticDevice.hpp>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/Log.hpp>
#include <sweeppp/core/Paths.hpp>
#include <sweeppp/plugin/PluginHost.hpp>

namespace sweeppp::ui {
namespace {

ImVec4 toImVec4(const Color& color) {
    return {color.r, color.g, color.b, color.a};
}

ImVec2 scaled(const ImVec2& value, float scale) {
    return {value.x * scale, value.y * scale};
}

/// The one parameter key the sweep plan owns as well as the driver.
constexpr std::string_view kSampleRateKey = "sample_rate";

} // namespace

AppState::AppState() = default;

AppState::~AppState() {
    stop();

    // The radio goes before the plugin that handed it out.
    //
    // A plugin driver's device carries a vtable living in the plugin's image,
    // and `shutdown()` withdraws every facet whether or not one is still open
    // -- so deactivating first would leave this object holding a radio it
    // could not even destroy. Closing here also means the plugin receives the
    // device-closed event on a channel that is still live, which it did not
    // when the order was the other way round.
    closeDevice();

    // Then the plugins: a frame processor is subscribed to `m_displayBus`,
    // which is a member of this object, so its subscription has to go while
    // the bus is still alive.
    PluginManager::instance().shutdown();

    // A working file must not outlive the process, and data nobody decided
    // about must not be deleted by one.
    //
    // Reaching here with an unhandled session means the prompt never ran -- a
    // crash, a signal, a close path that bypassed it. Keeping it under a name
    // that says what happened is the only option that cannot lose an hour of
    // sweeping; the file is recoverable by scan even though it was never
    // closed cleanly.
    if (hasUnsavedSession()) {
        endSession(Paths::instance().sessionsDir() /
                   std::format("recovered-{}.sweeps", formatWallClockCompact(wallClockNs())));
    } else {
        endSession(std::nullopt);
    }
}

void AppState::setError(std::string message) {
    // The previous card goes first, so a condition that changes its wording --
    // "no such device" becoming "claimed by another process" -- replaces what
    // it said rather than stacking a second copy of the same trouble.
    m_toasts.dismiss(m_errorToast);
    m_errorToast = m_toasts.error(std::move(message), monotonicNs());
}

void AppState::clearError() {
    m_toasts.dismiss(m_errorToast);
    m_errorToast = 0;
}

Status AppState::initialise() {
    registerBuiltinSdrDevices();

    if (auto ensured = Paths::instance().ensureConfigTree(); !ensured) {
        logWarn("app", "{}", ensured.error().describe());
    }

    // Plugins after the built-in drivers, and deliberately: a plugin claiming
    // a driver name a built-in already holds is refused and listed with the
    // reason, which only works if the built-ins got there first. The display
    // bus is handed over before discovery because a frame processor attaches
    // during activation.
    //
    // Discovery must also stay ahead of both the FFT acquisition below and
    // `beginDeviceStartup()`, which is load-bearing rather than tidy: every
    // transform and every radio bar the synthetic one and a file arrives
    // through a plugin, so a profile naming `hackrf` has nothing to open --
    // and there is no FFT at all -- until the plugins providing them have
    // registered.
    PluginManager::instance().setFrameBus(&m_displayBus);
    PluginManager::instance().attachEvents(m_events);

    // The switches behind a plugin's own toolbar button. Bound to `m_view`
    // rather than copied out of it, so the button and the B and C keys are two
    // ways of reaching one flag instead of two flags that agree until they do
    // not.
    //
    // A spot rides with the channels, because that is how the spectrum paints
    // it: a beacon is a channel of no width.
    PluginManager::instance().setChrome(PluginManager::Chrome{
        .contributionsShown =
            [this](sweeppp_contribution_type_t type) {
                return type == SWEEPPP_CONTRIBUTION_BAND ? m_view.showBandContributions
                                                         : m_view.showChannelContributions;
            },
        .setContributionsShown =
            [this](sweeppp_contribution_type_t type, bool shown) {
                (type == SWEEPPP_CONTRIBUTION_BAND ? m_view.showBandContributions
                                                   : m_view.showChannelContributions) = shown;
            },
        .iconsAvailable = [] { return icon::available(); },
        // The application's own native dialog, lent to plugins. It lives here
        // rather than in the library because the library also builds headless
        // and a second copy of NFD in one process is two ways of driving the
        // same panel.
        .saveFile = [](std::string_view title, std::string_view suggestedName,
                       std::string_view extension) -> std::string {
            const auto chosen =
                saveFileDialog(Paths::instance().sessionsDir(), std::string(suggestedName),
                               std::string(title), std::string(extension));
            return chosen ? chosen->string() : std::string{};
        },
    });

    PluginManager::instance().discover();

    auto backend = FftBackendManager::instance().acquireOrDefault({});
    if (!backend) {
        return std::unexpected(backend.error());
    }
    m_backend = *backend;

    installLoggingSubscribers(m_events);

    m_pipeline = std::make_unique<Pipeline>(m_pipelineBus, m_telemetry, m_events);
    m_sweepEngine = std::make_unique<SweepEngine>(m_displayBus, m_telemetry, m_events);

    // The learners read the sweep through the engine's hook and advance on
    // its passes. Both are wired once, here, and decide per frame whether
    // anything is listening.
    m_sweepEngine->setStepObserver(
        [this](const SpectrumFrame& frame, const SweepStep&) { observeStep(frame); });
    m_events.subscribe<SweepPassEvent>(
        [this](const SweepPassEvent&) { m_passesSeen.fetch_add(1, std::memory_order_relaxed); });

    // Two buses in series: the pipeline emits per-step frames, the sweep
    // engine stitches them, and everything downstream binds to the stitched
    // one. In fixed-tune mode the engine is bypassed and the pipeline
    // publishes straight to the display bus.
    m_displayBus.subscribe(this);

    m_themes = discoverThemes();
    setTheme(m_view.themeName);

    // Fixed-tune defaults.
    //
    // 50% overlap with a Hann window is the textbook pairing: the two halves
    // sum to unity, so no sample is weighted out and a short burst cannot fall
    // between transforms and be missed entirely. It costs twice the transforms
    // per second, which at these sizes is nothing next to being blind for half
    // the time. Averaging stays at one -- averaging is a deliberate trade of
    // responsiveness for noise floor, and a live display should start
    // responsive.
    m_pipelineConfig = PipelineConfig{.fftSize = 4096,
                                      .window = WindowType::Hann,
                                      .overlap = 0.5,
                                      .averageCount = 1,
                                      .targetFrameRate = 60.0};

    m_sweepPlan.segments = {SweepSegment{.startHz = 88e6, .stopHz = 108e6}};
    m_sweepPlan.sampleRate = 20e6;
    m_sweepPlan.rbwHz = 25e3;
    m_sweepPlan.applyMode(SweepMode::Fast);

    m_presets = SweepPresetStore::load(Paths::instance().configDir() / "sweep-presets.toml");
    m_markerPresets =
        MarkerPresetStore::load(Paths::instance().configDir() / "marker-presets.toml");
    reloadAntennas();
    m_assignments = AntennaAssignments::load(Paths::instance().antennasDir() / "assignments.toml");
    refreshSwitchers();

    // Overrides the defaults above. The radio it names is not opened here --
    // see loadSettings and beginDeviceStartup.
    loadSettings();

    // The bus probe and the radio, on a worker thread. Started last, so
    // everything it will hand back to has been built, and started here rather
    // than by the caller so no path can forget it and come up with an empty
    // device list.
    beginDeviceStartup();
    return ok();
}

void AppState::beginDeviceStartup() {
    // The viewer claims no hardware at all: probing the bus is the first half
    // of claiming a radio, and the instrument may be using the very device
    // this recording came from.
    if (m_viewerMode || m_startup) {
        return;
    }

    DeviceStartup startup;
    startup.enumerate = true;
    if (m_startupProfile) {
        startup.driver = m_startupProfile->deviceDriver;
        startup.id = m_startupProfile->deviceId;

        // What the radio called itself last time, so the panel names the radio
        // rather than its driver while waiting for it. Profiles written before
        // the label was kept have only the driver to fall back on.
        startup.label = m_startupProfile->deviceLabel.empty() ? m_startupProfile->deviceDriver
                                                              : m_startupProfile->deviceLabel;
    }
    startDeviceWorker(std::move(startup));
}

void AppState::beginOpenDevice(const std::string& driver, const std::string& id,
                               const std::string& label) {
    if (m_viewerMode || m_startup || driver.empty()) {
        return;
    }

    // Clicking the radio that is already open is not a request to cycle it.
    // Reopening means releasing a working device and claiming it again, which
    // is seconds of waiting to arrive exactly where the operator already was.
    if (m_device && m_device->info().driver == driver && m_device->info().id == id) {
        clearError();
        return;
    }

    DeviceStartup startup;
    startup.driver = driver;
    startup.id = id;
    startup.label = label.empty() ? driver : label;
    startup.enumerate = false;
    startDeviceWorker(std::move(startup));
}

void AppState::startDeviceWorker(DeviceStartup startup) {
    // The radio in hand is released before another is claimed, on this thread,
    // before the worker exists.
    //
    // A device this process still holds cannot be opened again -- the driver is
    // asked for a handle to something it has already given out, and on a
    // BladeRF that is where it stops. Adopting the new one first and closing
    // the old one after reads better and is exactly backwards.
    if (!startup.driver.empty() && m_device) {
        stop();
        closeDevice();
    }

    startup.startedNs = monotonicNs();
    startup.opening = std::make_shared<std::atomic_bool>(!startup.enumerate);

    startup.future =
        std::async(std::launch::async, [driver = startup.driver, id = startup.id,
                                        enumerate = startup.enumerate, opening = startup.opening] {
            StartupResult result;

            // The manager holds its own lock and the registry was filled before
            // this thread existed, so both calls are the worker's to make. Nothing
            // else touches the hardware while it runs: the window blocks the
            // controls that would.
            if (enumerate) {
                result.devices = SdrDeviceManager::instance().enumerateAll();
                result.enumerated = true;
            }
            if (driver.empty()) {
                return result;
            }

            opening->store(true, std::memory_order_relaxed);
            auto device = SdrDeviceManager::instance().open(driver, id);
            if (device) {
                result.device = std::move(*device);
            } else {
                result.error = device.error().message();
            }
            return result;
        });

    m_startup = std::move(startup);
}

void AppState::pollDeviceStartup() {
    if (!m_startup ||
        m_startup->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    StartupResult result = m_startup->future.get();
    const std::string label = m_startup->label;
    const bool wantedDevice = !m_startup->driver.empty();
    m_startup.reset();

    if (result.enumerated) {
        m_devices = std::move(result.devices);
    }

    if (result.device) {
        adoptDevice(std::move(result.device));
        clearError();

        if (m_startupProfile) {
            for (const auto& [key, value] : m_startupProfile->deviceParameters) {
                if (auto applied = m_device->setParameter(key, value); !applied) {
                    logWarn("profile", "{}: {}", key, applied.error().describe());
                }
            }

            // Re-applied now that there is a radio to apply it against. It was
            // applied once already, without one, so the window came up showing
            // the range that was saved; what the device clamps is only knowable
            // here.
            if (auto applied = applySweepPlan(m_startupProfile->sweepPlan); !applied) {
                setError(applied.error().message());
            }
        }
    } else if (wantedDevice) {
        // A radio unplugged between runs is the common case, and it is not a
        // failure of anything the operator just did -- so it is said once and
        // everything else stays restored.
        std::string trouble = std::format("{} is not available: {}", label, result.error);
        logInfo("device", "{}", trouble);
        setError(std::move(trouble));
    }

    m_startupProfile.reset();
}

std::string AppState::deviceStartupLabel() const {
    if (!m_startup) {
        return {};
    }
    if (m_startup->opening && m_startup->opening->load(std::memory_order_relaxed)) {
        return std::format("Opening {}", m_startup->label);
    }
    return "Looking for radios";
}

std::string AppState::deviceStartupDetail() const {
    if (!m_startup) {
        return {};
    }
    if (m_startup->opening && m_startup->opening->load(std::memory_order_relaxed)) {
        return "Claiming the radio and starting its firmware";
    }
    return "Checking what is connected";
}

std::uint64_t AppState::deviceStartupStartedNs() const noexcept {
    return m_startup ? m_startup->startedNs : 0;
}

Profile AppState::currentProfile(const std::string& name, PluginState plugins) const {
    Profile profile;
    profile.name = name;
    profile.sweepPlan = m_sweepPlan;
    profile.sweeping = m_sweeping;
    profile.pipeline = m_pipelineConfig;
    profile.corrections = m_correctionSettings;
    profile.view = m_view;

    if (m_device) {
        profile.deviceDriver = m_device->info().driver;
        profile.deviceId = m_device->info().id;
        profile.deviceLabel = m_device->info().label;

        // Every parameter the driver declares, read back rather than
        // remembered. A value the device coerced -- a sample rate it rounded,
        // a gain it clamped -- is saved as what it actually is, so reloading
        // the profile is idempotent instead of drifting a little each time.
        for (const SdrParameter& parameter : m_device->parameters()) {
            if (parameter.readOnly) {
                continue;
            }
            if (auto value = m_device->getParameter(parameter.key)) {
                profile.deviceParameters.emplace_back(parameter.key, *value);
            }
        }
    }

    // Whatever the plugins want kept with this setup. Asked here rather than
    // remembered from the last load, for the same reason the device parameters
    // are read back rather than remembered: a profile should record what is
    // true now, so saving and reloading is idempotent instead of drifting.
    if (plugins == PluginState::Include) {
        profile.pluginValues = PluginManager::instance().collectProfileValues();
    }

    return profile;
}

Status AppState::applyProfile(const Profile& profile, DeviceHandling devices) {
    stop();

    // Display first, and unconditionally: it depends on nothing else, so an
    // absent radio must not cost the operator their colours and levels too.
    m_view = profile.view;
    m_applyingProfile = true;
    setTheme(m_view.themeName);

    m_pipelineConfig = profile.pipeline;
    m_sweeping = profile.sweeping;
    setCorrectionSettings(profile.corrections);

    std::string trouble;

    // The viewer restores everything else the profile carries -- colours,
    // levels, band plan -- but never the radio: it is reading a file, and the
    // device it was recorded with may well be in use by the instrument.
    //
    // Whichever way, the radio is opened on the worker: it takes seconds, and
    // a profile switch must not stop the window drawing any more than a start
    // may. Everything below still happens -- the plan is applied without a
    // device, the window immediately shows the profile it was given, and the
    // worker re-applies parameters and plan against the radio when it lands.
    if (!profile.deviceDriver.empty() && !m_viewerMode) {
        const bool alreadyOpen = m_device && m_device->info().driver == profile.deviceDriver &&
                                 m_device->info().id == profile.deviceId;
        if (!alreadyOpen) {
            m_startupProfile = profile;
            if (devices == DeviceHandling::Now) {
                beginOpenDevice(profile.deviceDriver, profile.deviceId, profile.deviceLabel);
            }
        }
    }

    if (m_device) {
        for (const auto& [key, value] : profile.deviceParameters) {
            if (auto applied = m_device->setParameter(key, value); !applied) {
                logWarn("profile", "{}: {}", key, applied.error().describe());
            }
        }
    }

    // Last, because the plan is applied against the device that is now open
    // and adopts whatever it accepted.
    if (auto applied = applySweepPlan(profile.sweepPlan); !applied && trouble.empty()) {
        trouble = applied.error().message();
    }
    m_applyingProfile = false;

    // Plugins last, once everything they might read is in place, and
    // unconditionally: a plugin's part of a profile is no less restored
    // because the radio it named is not plugged in.
    PluginManager::instance().applyProfileValues(profile.pluginValues);
    PluginManager::instance().setProfile(profile);

    if (!trouble.empty()) {
        setError(trouble);
        return fail(ErrorCode::Unavailable, "{}", trouble);
    }
    clearError();
    return ok();
}

std::vector<std::string> AppState::profileNames() const {
    std::vector<std::string> names;

    std::error_code ec;
    const std::filesystem::path directory = Paths::instance().profilesDir();
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".toml") {
            names.push_back(entry.path().stem().string());
        }
    }

    std::ranges::sort(names);
    return names;
}

void AppState::loadSettings() {
    auto profile = Profile::load(Paths::instance().settingsFile());
    if (!profile) {
        // A first run has no settings file, which is not a problem worth
        // reporting; anything else is.
        if (profile.error().code() != ErrorCode::NotFound) {
            logWarn("settings", "{}", profile.error().describe());
        }
        return;
    }

    // Deferred: this runs before the window exists, and opening the radio here
    // is what kept it from appearing. The profile is kept for the worker,
    // which needs its parameters and its sweep plan once the device is in.
    if (auto applied = applyProfile(*profile, DeviceHandling::Deferred); !applied) {
        // The radio being absent is the common case here -- it was unplugged
        // between runs. Everything else was restored, so this is a note rather
        // than a failure.
        logInfo("settings", "{}", applied.error().message());
    }

    m_startupProfile = std::move(*profile);
}

void AppState::saveSettings() {
    // The viewer never writes settings back. It deliberately opens no radio, so
    // the profile it would save names none -- and saving that over the
    // instrument's own settings would cost the operator the device, sweep plan
    // and parameters they had set up, just for having looked at a recording.
    if (m_viewerMode) {
        return;
    }

    const Profile profile = currentProfile("session");
    if (auto saved = profile.save(Paths::instance().settingsFile()); !saved) {
        logWarn("settings", "{}", saved.error().describe());
    }
}

void AppState::savePresets() {
    // A failed save is worth a line in the log but not worth interrupting the
    // operator: the presets are still correct for this session, and the thing
    // they were in the middle of matters more than the file.
    const std::filesystem::path path = Paths::instance().configDir() / "sweep-presets.toml";
    if (auto saved = m_presets.save(path); !saved) {
        logWarn("presets", "{}", saved.error().describe());
    }
}

void AppState::saveMarkerPresets() {
    // As with the sweep presets: a failed write is a line in the log, not an
    // interruption. The sets are still correct for this session.
    const std::filesystem::path path = Paths::instance().configDir() / "marker-presets.toml";
    if (auto saved = m_markerPresets.save(path); !saved) {
        logWarn("markers", "{}", saved.error().describe());
    }
}

void AppState::reloadAntennas() {
    std::vector<std::string> problems;
    const std::vector<std::filesystem::path> directories = Paths::instance().searchPath("antennas");
    m_antennas = AntennaLibrary::discover(directories, &problems);

    // One malformed row is worth saying so about and nothing more: the
    // operator hand-edits this file, and losing the other nine antennas
    // because the tenth has its stop below its start would be the worse
    // failure by far.
    for (const std::string& problem : problems) {
        logWarn("antennas", "{}", problem);
    }
}

void AppState::saveAntennas() {
    const std::filesystem::path path = Paths::instance().antennasDir() / "custom.toml";
    if (auto saved = m_antennas.saveUserFile(path); !saved) {
        logWarn("antennas", "{}", saved.error().describe());
        return;
    }

    // Read back, so what the editor lists is what the file holds -- including
    // which entries the shipped set still owns after a copy over one was
    // deleted.
    reloadAntennas();
    applyAntennaChange();
}

void AppState::saveAntennaAssignments() {
    const std::filesystem::path path = Paths::instance().antennasDir() / "assignments.toml";
    if (auto saved = m_assignments.save(path); !saved) {
        logWarn("antennas", "{}", saved.error().describe());
    }
    applyAntennaChange();
}

void AppState::applyAntennaChange() {
    // A box that has just been named has to be opened before anything can be
    // routed through it, and one that has just been detached has to be let go.
    refreshSwitchers();

    // Only a routed sweep is planned against the antennas. With routing off
    // they change the readouts and nothing else, and cycling acquisition to
    // apply a change the sweep does not read would be an unexplained gap in
    // the waterfall.
    if (!m_sweepPlan.antennaRouting || !m_device || m_viewerMode) {
        return;
    }

    // The same path a range edit takes: reconfigure when stopped, cycle
    // acquisition when running. Nothing here is a new mechanism -- an antenna
    // moving is a plan change, because the plan is what routes through it.
    if (auto applied = applySweepPlan(m_sweepPlan); !applied) {
        setError(applied.error().describe());
    }
}

std::vector<RfLeg> AppState::rfPath() const {
    if (!m_device) {
        return {};
    }
    return resolveRfPath(m_device->info(), m_device->rxPorts(), m_antennas, m_assignments,
                         m_openPaths);
}

std::vector<std::pair<double, double>> AppState::antennaCoverage() const {
    const std::vector<RfLeg> legs = rfPath();
    return coveredRanges(legs);
}

IRfPath* AppState::switcher(std::string_view key) const {
    const auto match = std::ranges::find_if(
        m_openPaths, [key](const OpenRfPath& open) { return open.key == key; });
    return match != m_openPaths.end() ? match->path : nullptr;
}

void AppState::refreshSwitchers() {
    // Every box an assignment names, whichever radio it was filed against: a
    // switcher stays on the bench when the receiver is unplugged, and the
    // Antennas rows have to keep describing the chain either way.
    std::vector<std::string> wanted;
    for (const AntennaAssignment& entry : m_assignments.entries()) {
        if (entry.switcher.empty() || entry.device.empty()) {
            continue;
        }
        if (std::ranges::find(wanted, entry.switcher) == wanted.end()) {
            wanted.push_back(entry.switcher);
        }
    }

    // Kept rather than reopened. Reopening on every assignment edit would
    // click every relay in the box each time the operator changed a combo, and
    // on hardware that counts its own cycles that is wear for nothing.
    std::vector<std::unique_ptr<IRfPath>> keep;
    std::vector<OpenRfPath> open;

    for (const std::string& key : wanted) {
        const auto existing =
            std::ranges::find_if(m_switchers, [&key](const std::unique_ptr<IRfPath>& path) {
                return path && rfPathKey(path->info()) == key;
            });
        if (existing != m_switchers.end()) {
            open.push_back(OpenRfPath{.key = key, .path = existing->get()});
            keep.push_back(std::move(*existing));
            continue;
        }

        auto opened = RfPathManager::instance().openSpecifier(key);
        if (!opened) {
            // Listed as missing rather than dropped: the assignment survives so
            // reconnecting the box restores the whole chain, and the sweep
            // refuses to route through what it cannot drive.
            logWarn("antennas", "switcher '{}' is assigned but not connected: {}", key,
                    opened.error().describe());
            continue;
        }
        open.push_back(OpenRfPath{.key = key, .path = opened->get()});
        keep.push_back(std::move(*opened));
    }

    m_switchers = std::move(keep);
    m_openPaths = std::move(open);
}

std::string AppState::deviceAntennaKey() const {
    return m_device ? AntennaAssignments::deviceKey(m_device->info()) : std::string{};
}

const Antenna* AppState::antennaOnPort(std::string_view portId) const {
    if (!m_device) {
        return nullptr;
    }
    const std::string_view id =
        m_assignments.antennaFor(AntennaAssignments::deviceKey(m_device->info()), portId);
    return id.empty() ? nullptr : m_antennas.find(id);
}

void AppState::beginRefreshDevices() {
    if (m_viewerMode || m_startup) {
        return;
    }

    DeviceStartup refresh;
    refresh.enumerate = true;
    startDeviceWorker(std::move(refresh));
}

void AppState::adoptDevice(std::unique_ptr<ISdrDevice> device) {
    stop();
    closeDevice();
    m_device = std::move(device);

    // A freshly opened radio sweeps its whole range by default.
    //
    // The first thing an operator wants from a new device is to see what is
    // out there, and a fixed tune at some arbitrary default centre shows one
    // sample rate's worth of it. Sweeping the full range is both the more
    // useful starting point and the one that immediately shows the device is
    // working. Any narrower interest is a zoom away; the reverse -- guessing
    // which 20 MHz the operator meant -- is not.
    const SdrDeviceInfo& info = m_device->info();
    m_sweeping = true;
    m_sweepPlan.segments = {
        SweepSegment{.startHz = info.minFrequencyHz, .stopHz = info.maxFrequencyHz}};

    // The fastest rate the device offers, because sweep rate scales with it:
    // a wider step covers the same span in fewer retunes, and retuning is what
    // a wide sweep spends most of its time doing.
    const std::vector<double> rates = m_device->supportedSampleRates();
    if (const auto fastest = std::ranges::max_element(rates); fastest != rates.end()) {
        m_sweepPlan.sampleRate = *fastest;
    }

    m_events.publish(DeviceOpenedEvent{.monotonicNs = monotonicNs(),
                                       .deviceId = info.id,
                                       .label = info.label,
                                       .serial = info.serial});

    loadCalibration();
}

void AppState::closeDevice() {
    if (!m_device) {
        return;
    }
    m_events.publish(DeviceClosedEvent{.monotonicNs = monotonicNs(),
                                       .deviceId = m_device->info().id,
                                       .reason = "closed by operator"});
    m_device.reset();
    loadCalibration();
}

Status AppState::applyPipelineConfig(const PipelineConfig& config) {
    m_pipelineConfig = config;
    if (!m_pipeline) {
        return ok();
    }

    if (!running()) {
        return m_pipeline->configure(*m_backend, config);
    }

    // A sweep is a second thread driving this pipeline, and reconfigure()
    // stops it. The sweep loop reads framesPerBlock() and calls
    // cycleDeviceStream() on the pipeline while stop() is clearing the pool
    // and the device out from under it -- a check-then-use across two threads,
    // which is a null dereference at best. Stop the pair and start it again,
    // which is what re-planning has always done.
    if (m_sweepEngine && m_sweepEngine->running()) {
        return restart();
    }

    return m_pipeline->reconfigure(config);
}

Status AppState::restart() {
    stop();
    if (auto started = start(); !started) {
        setError(started.error().describe());
        return started;
    }
    adoptEffectivePlan();
    return ok();
}

Status AppState::setSweeping(bool enabled) {
    if (enabled == m_sweeping) {
        return ok();
    }
    m_sweeping = enabled;

    // Not a display preference. The flag decides what start() builds -- the
    // engine between the two buses, or the display forwarder -- and it also
    // gates the waterfall, which only advances on a completed pass while it is
    // set. Writing it under a running fixed tune therefore froze the waterfall
    // for good: nothing in that mode ever sets passComplete, so no row was
    // ever added again while the spectrum carried on updating.
    return running() ? restart() : ok();
}

Status AppState::sweepRange(const SweepPlan& plan) {
    // One restart, not two. Dragging out a range is a single request even
    // though it changes two things, and applying them separately would rebuild
    // acquisition twice -- each rebuild a stream stop and start on the radio.
    m_sweeping = true;
    return applySweepPlan(plan);
}

Status AppState::applySweepPlan(const SweepPlan& plan) {
    // The plan is kept whatever it says, so the editor always shows what was
    // typed, but only a *valid* one is allowed near the radio.
    //
    // Editing a range passes through states that are briefly nonsense -- a
    // stop below its start, a span of zero -- and every keystroke arrives
    // here. Acting on those would stop a running sweep on the way to a
    // perfectly good range, which is a much worse outcome than a moment of
    // the display not matching the fields.
    const std::vector<SweepSegment> previousSegments = m_sweepPlan.segments;

    m_sweepPlan = plan;

    // Moving the sweep resets the views onto it; see viewResetGeneration.
    if (std::abs(plan.lowestHz() - m_viewResetLowHz) > 1.0 ||
        std::abs(plan.highestHz() - m_viewResetHighHz) > 1.0) {
        m_viewResetLowHz = plan.lowestHz();
        m_viewResetHighHz = plan.highestHz();
        if (!m_applyingProfile) {
            ++m_viewResetGeneration;
        }
    }

    // Only ranges that are actually usable become places to come back to. A
    // half-typed one is not somewhere the operator was.
    if (plan.validate() && !m_navigatingHistory) {
        // The first change needs somewhere to go back *to*.
        //
        // The range a session opens on -- the device's full range, or the
        // startup default -- is assigned directly rather than applied, so it
        // never reaches the history on its own. Seeding it here means the very
        // first narrowing gesture is undoable, instead of "back" doing nothing
        // until the second one.
        if (m_rangeHistory.size() == 0 && !previousSegments.empty()) {
            m_rangeHistory.reset(previousSegments);
        }
        m_rangeHistory.record(plan.segments, monotonicNs());
    }

    if (!m_device || !m_sweepEngine) {
        return ok();
    }
    if (auto valid = plan.validate(); !valid) {
        return valid;
    }

    // Applied to a running sweep by cycling acquisition, not by making the
    // operator stop and start.
    //
    // Retuning the plan is the single most common thing to want mid-sweep --
    // narrowing onto something that just appeared is the whole job -- and the
    // radio, the session and the history all survive it. What cannot survive
    // is reconfiguring underneath a running engine: the plan sets the FFT
    // size, the grid and the block size, so the pipeline has to be rebuilt
    // around it. Every grid-affecting change opens a new segment in the
    // session file, which is exactly what the format is for.
    if (!running()) {
        if (auto configured = m_sweepEngine->configure(plan, *m_backend, *m_device, m_antennas,
                                                       m_assignments, m_openPaths);
            !configured) {
            return configured;
        }
        adoptEffectivePlan();
        return ok();
    }

    return restart();
}

std::string_view AppState::fftBackendName() const noexcept {
    return m_backend != nullptr ? m_backend->name() : std::string_view{};
}

Status AppState::setFftBackend(std::string_view name) {
    if (m_backend != nullptr && m_backend->name() == name) {
        return ok();
    }

    // Acquired before anything is torn down. A backend that cannot instantiate
    // demotes itself in the registry on first use, and finding that out after
    // stopping acquisition would leave the operator with a stopped instrument
    // and the old backend still selected.
    auto acquired = FftBackendManager::instance().acquire(name);
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    m_backend = *acquired;

    logInfo("fft", "backend is now {}", m_backend->name());

    // The pipeline's plan and the sweep schedule are both built from the
    // backend -- the schedule because `snapSize` decides the FFT size, and so
    // the grid. Both have to be rebuilt, which is what start() does.
    if (running()) {
        return restart();
    }

    if (m_sweeping && m_device) {
        if (auto configured = m_sweepEngine->configure(m_sweepPlan, *m_backend, *m_device,
                                                       m_antennas, m_assignments, m_openPaths);
            !configured) {
            return configured;
        }
        m_pipelineConfig.fftSize = m_sweepEngine->schedule().fftSize;
        adoptEffectivePlan();
    }
    return applyPipelineConfig(m_pipelineConfig);
}

Status AppState::setDeviceParameter(const std::string& key, const SdrValue& value) {
    if (!m_device) {
        return fail(ErrorCode::NotFound, "no device is open");
    }

    if (key == kSampleRateKey && m_sweeping) {
        SweepPlan plan = m_sweepPlan;
        plan.sampleRate = asDouble(value);
        return applySweepPlan(plan);
    }

    return m_device->setParameter(key, value);
}

bool AppState::parameterNeedsStop(const SdrParameter& parameter) const noexcept {
    if (!parameter.requiresStop) {
        return false;
    }
    return !(m_sweeping && parameter.key == kSampleRateKey);
}

Status AppState::goBack() {
    std::vector<SweepSegment> segments = m_rangeHistory.goBack();
    if (segments.empty()) {
        return ok();
    }

    SweepPlan plan = m_sweepPlan;
    plan.segments = std::move(segments);

    m_navigatingHistory = true;
    auto applied = applySweepPlan(plan);
    m_navigatingHistory = false;
    return applied;
}

Status AppState::goForward() {
    std::vector<SweepSegment> segments = m_rangeHistory.goForward();
    if (segments.empty()) {
        return ok();
    }

    SweepPlan plan = m_sweepPlan;
    plan.segments = std::move(segments);

    m_navigatingHistory = true;
    auto applied = applySweepPlan(plan);
    m_navigatingHistory = false;
    return applied;
}

void AppState::adoptEffectivePlan() {
    // The plan the engine is running, not the one that was asked for.
    //
    // Radios quantise and clamp: a request for 100 MS/s on a device that
    // stops at 20 becomes 20, and the engine plans the grid against what it
    // actually got. Leaving the requested figure in the UI's copy would leave
    // the panel showing a sample rate nothing is using, and the predicted
    // sweep rate derived from it would be wrong by the same factor -- a
    // display that quietly disagrees with the instrument is worse than one
    // that shows an unwelcome number.
    if (m_sweepEngine && !m_sweepEngine->schedule().steps.empty()) {
        m_sweepPlan = m_sweepEngine->plan();
    }

    reportUnroutedRanges();
}

void AppState::reportUnroutedRanges() {
    if (!m_sweepEngine) {
        return;
    }

    const std::vector<std::pair<double, double>>& unrouted = m_sweepEngine->schedule().unroutedHz;

    // Once per change, not once per plan. An operator dragging a range out on
    // the spectrum re-plans on every frame, and a toast per frame would bury
    // the display under the very warning it is trying to deliver.
    if (unrouted == m_reportedUnroutedHz) {
        return;
    }
    m_reportedUnroutedHz = unrouted;

    if (unrouted.empty()) {
        return;
    }

    // A toast as well as the line in the Analysis panel, because the panel is
    // a popover: the plan can grow a hole while nothing that would say so is
    // on screen, and finding out from a flat trace an hour later is not
    // finding out.
    std::string ranges;
    for (const auto& [fromHz, toHz] : unrouted) {
        if (!ranges.empty()) {
            ranges += ", ";
        }
        ranges += std::format("{} - {}", toml_util::formatFrequencyShort(fromHz),
                              toml_util::formatFrequencyShort(toHz));
    }

    m_toasts.warning(std::format("no assigned antenna covers {}; it will be swept through "
                                 "whichever connector is selected",
                                 ranges),
                     monotonicNs());
}

Status AppState::start() {
    if (!m_device) {
        return fail(ErrorCode::Unavailable, "no device is open");
    }
    if (running()) {
        return ok();
    }

    m_telemetry.reset();
    m_traces.clear();

    // Whatever sat between the buses last run is replaced, not stacked --
    // otherwise switching between sweep and fixed tune would leave both
    // attached and publish every frame twice.
    if (m_pipelineSubscription != 0) {
        m_pipelineBus.unsubscribe(m_pipelineSubscription);
        m_pipelineSubscription = 0;
    }

    if (m_sweeping) {
        if (auto configured = m_sweepEngine->configure(m_sweepPlan, *m_backend, *m_device,
                                                       m_antennas, m_assignments, m_openPaths);
            !configured) {
            setError(configured.error().describe());
            return configured;
        }

        // In sweep mode the engine sits between the two buses.
        m_pipelineConfig.fftSize = m_sweepEngine->schedule().fftSize;

        // Every step's frame must reach the engine. The pipeline's frame-rate
        // cap is a *display* concern, and applying it here would silently drop
        // most steps' measurements before they could be stitched -- the engine
        // throttles its own stitched output instead.
        m_pipelineConfig.targetFrameRate = 0.0;

        m_pipelineSubscription = m_pipelineBus.subscribe(m_sweepEngine.get());
    } else {
        // Fixed tune: forwarded straight through, so the display cap applies
        // to the pipeline directly.
        m_pipelineConfig.targetFrameRate = 60.0;
        m_pipelineSubscription = m_pipelineBus.subscribe(&m_displayForwarder);
    }

    if (auto configured = m_pipeline->configure(*m_backend, m_pipelineConfig); !configured) {
        // Through stop(), so the subscription made above does not outlive the
        // failed start. Stopping a pipeline that never started is a no-op.
        stop();
        setError(configured.error().describe());
        return configured;
    }
    pushCorrectionSettings();

    const double sampleRate =
        asDouble(m_device->getParameter("sample_rate").value_or(SdrValue{20e6}));
    const double centerHz = asDouble(m_device->getParameter("center_hz").value_or(SdrValue{100e6}));
    m_pipeline->setTuning(centerHz, sampleRate, sampleRate);

    // Block size is a sweep-rate decision, not a constant.
    //
    // A block is stamped with one centre frequency, so it must not span a
    // retune. At 20 MS/s a 262144-frame block covers 13 ms, while a sweep step
    // may dwell for well under a millisecond -- one block would then straddle
    // a dozen retunes and every sample in it would be attributed to whichever
    // frequency happened to be set when it was filled.
    //
    // Sizing it to the step's own collection window keeps one block inside one
    // step. Fixed tune has no such constraint and prefers large blocks, which
    // cost fewer wakeups per sample.
    std::size_t framesPerBlock = 262'144;
    if (m_sweeping) {
        const std::uint32_t fftSize = m_sweepEngine->schedule().fftSize;
        const std::uint32_t averages = std::max(m_pipelineConfig.averageCount, 1U);
        framesPerBlock =
            std::clamp<std::size_t>(static_cast<std::size_t>(fftSize) * averages, 2048, 262'144);
    }

    const StreamConfig streamConfig{
        .framesPerBlock = framesPerBlock, .blockCount = 64, .format = m_device->nativeFormat()};

    if (auto started = m_pipeline->start(*m_device, streamConfig); !started) {
        stop();
        setError(started.error().describe());
        return started;
    }

    if (m_sweeping) {
        if (auto started = m_sweepEngine->start(*m_device, *m_pipeline); !started) {
            stop();
            setError(started.error().describe());
            return started;
        }
    }

    // Here rather than only in applySweepPlan, so pressing Start also settles
    // the panel onto what the radio agreed to.
    adoptEffectivePlan();
    beginAutoSpurs();

    // Retention begins with acquisition, not when the operator asks for it.
    // "Save what I have been watching" cannot be answered by a writer created
    // at the moment of asking -- it would record only the future.
    if (auto session = beginSession(); !session) {
        logWarn("app", "session not being retained: {}", session.error().describe());
    }

    m_hasUnsavedSession = true;
    clearError();
    return ok();
}

void AppState::stop() {
    // A learn measures a running radio; without one there is nothing to
    // finish, and the settings it borrowed go back.
    abortLearning();
    endAutoSpurs();

    if (m_sweepEngine) {
        m_sweepEngine->stop();
    }
    if (m_pipeline) {
        m_pipeline->stop();
    }

    // Detached here rather than only on the way back in.
    //
    // start() also clears this before subscribing, which was enough to keep
    // the bus from fanning out twice but left whatever ran last attached for
    // as long as the instrument was stopped -- including the sweep engine,
    // which is torn down and rebuilt around a device that may be closed in
    // between. A subscriber outliving what it points at is the failure the
    // bus's own unsubscribe ordering exists to prevent.
    if (m_pipelineSubscription != 0) {
        m_pipelineBus.unsubscribe(m_pipelineSubscription);
        m_pipelineSubscription = 0;
    }
}

bool AppState::running() const noexcept {
    return m_pipeline && m_pipeline->running();
}

Status AppState::beginSession() {
    if (m_writer) {
        return ok();
    }

    // A dot-prefixed working name, so an interrupted run leaves something
    // obviously provisional rather than a file that looks like a saved
    // recording. The container is recoverable by scan, so even a session
    // ended by a power cut opens with everything that reached the disk.
    const std::filesystem::path path =
        Paths::instance().sessionsDir() /
        std::format(".session-{}.sweeps", formatWallClockCompact(wallClockNs()));

    auto writer =
        session::SessionRecorder::create(path, session::RecorderConfig{{.sessionName = "session"}});
    if (!writer) {
        setError(writer.error().describe());
        return std::unexpected(writer.error());
    }

    m_writer = std::move(*writer);
    m_writer->attachEvents(m_events);
    m_writerSubscription = m_displayBus.subscribe(m_writer.get());

    // Plugins record into the same file as the sweep they are analysing.
    // Handed over here and taken back in detachWriter, so a plugin's write can
    // never reach a recorder that is being closed on a worker thread.
    PluginManager::instance().setSessionRecorder(m_writer.get());

    logInfo("app", "session recording to {}", path.string());
    return ok();
}

namespace {

/// Closes a detached writer and puts its file where it was asked to go.
/// Returns the error to report, or empty on success.
///
/// Takes the writer by value because it runs on a worker: the recorder is
/// destroyed here, on whichever thread finished with it, and nothing on the UI
/// side can still reach it.
[[nodiscard]] std::string finishSession(std::unique_ptr<session::SessionRecorder> writer,
                                        std::optional<std::filesystem::path> keepAs) {
    const std::filesystem::path working = writer->path();

    std::string error;
    if (auto closed = writer->close(); !closed) {
        error = closed.error().describe();
    }
    writer.reset();

    std::error_code ec;
    if (!keepAs) {
        std::filesystem::remove(working, ec);
        logInfo("app", "session discarded");
        return error;
    }

    std::filesystem::create_directories(keepAs->parent_path(), ec);

    // Rename first: it is atomic within a filesystem and costs nothing however
    // large the session is. Only if that fails -- a different volume, most
    // likely -- fall back to copying, which for a long session may be
    // gigabytes and must not silently leave the original behind.
    ec.clear();
    std::filesystem::rename(working, *keepAs, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(working, *keepAs,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error =
                std::format("could not save the session to {}: {}", keepAs->string(), ec.message());
            logWarn("app", "{}", error);
            return error;
        }
        std::error_code removeEc;
        std::filesystem::remove(working, removeEc);
    }

    logInfo("app", "session saved to {}", keepAs->string());
    return error;
}

} // namespace

std::unique_ptr<session::SessionRecorder> AppState::detachWriter() {
    // Detached before it is closed, not after. unsubscribe() waits for any
    // delivery already in progress, so past this point no publishing thread can
    // still be inside the writer -- which is what makes closing it safe, here
    // or on a worker.
    if (m_writerSubscription != 0) {
        m_displayBus.unsubscribe(m_writerSubscription);
        m_writerSubscription = 0;
    }

    // Before the writer moves out, so no plugin can still be handed a recorder
    // that is about to be closed on another thread.
    PluginManager::instance().setSessionRecorder(nullptr);

    return std::move(m_writer);
}

void AppState::endSession(const std::optional<std::filesystem::path>& keepAs) {
    if (!m_writer) {
        return;
    }

    if (std::string error = finishSession(detachWriter(), keepAs); !error.empty()) {
        setError(std::move(error));
        return;
    }
    m_hasUnsavedSession = false;
}

void AppState::beginEndSession(const std::optional<std::filesystem::path>& keepAs) {
    if (!m_writer) {
        return;
    }

    // One at a time, and there is only ever one: the writer is taken below, so
    // a second call finds nothing to close.
    const std::uint64_t lines = sessionLines();
    m_sessionSaveError.clear();
    m_sessionSave = SessionSave{
        .keepAs = keepAs,
        .lines = lines,
        .startedNs = monotonicNs(),
        .future = std::async(std::launch::async, [writer = detachWriter(), keepAs]() mutable {
            return finishSession(std::move(writer), keepAs);
        })};
}

void AppState::pollEndSession() {
    if (!m_sessionSave) {
        return;
    }
    if (m_sessionSave->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    m_sessionSaveError = m_sessionSave->future.get();
    if (m_sessionSaveError.empty()) {
        m_hasUnsavedSession = false;
    } else {
        setError(m_sessionSaveError);
    }
    m_sessionSave.reset();
}

std::string AppState::sessionSaveLabel() const {
    if (!m_sessionSave) {
        return {};
    }
    return m_sessionSave->keepAs
               ? std::format("Saving {}", m_sessionSave->keepAs->filename().string())
               : std::string("Discarding the session");
}

std::string AppState::sessionSaveDetail() const {
    if (!m_sessionSave) {
        return {};
    }
    return m_sessionSave->keepAs ? std::format("Closing the index and moving {} lines into place",
                                               m_sessionSave->lines)
                                 : std::string("Closing the working file");
}

std::uint64_t AppState::sessionSaveStartedNs() const noexcept {
    return m_sessionSave ? m_sessionSave->startedNs : 0;
}

std::filesystem::path AppState::sessionPath() const {
    return m_writer ? m_writer->path() : std::filesystem::path{};
}

std::uint64_t AppState::sessionLines() const noexcept {
    return m_writer ? m_writer->linesWritten() : 0;
}

void AppState::onFrame(const SpectrumFramePtr& frame) noexcept {
    // Runs on a pipeline thread. Parks the frame and returns -- anything more
    // would put UI work on the acquisition path.
    {
        const std::lock_guard lock(m_frameMutex);
        m_pendingFrame = frame;
        // Bounded like the lines they become: a stalled UI drops passes
        // rather than holding every grid.
        if (frame->passComplete && m_pendingPasses.size() < 64) {
            m_pendingPasses.push_back(frame);
        }
    }

    // What a learn takes from this bus: at a fixed tune, every frame; while
    // sweeping, the first pass completed wholly through the LO-offset set.
    // One pass over the bins, or a pointer copy -- nothing heavier.
    const std::lock_guard lock(m_learnMutex);
    if (!m_learn) {
        return;
    }
    if (m_learn->phase == LearnRun::Phase::Fixed) {
        m_learn->learner.addFrame(frame->binsDbfs, frame->config.centerHz);
    } else if (m_learn->phase == LearnRun::Phase::Absolute && frame->passComplete &&
               frame->sweepPass > m_learn->installedAtPass) {
        // The grid as one wide frame: its span for the sample rate and its
        // middle for the centre, so the offsets come back as frequencies.
        CorrectionLearner& grid = m_learn->gridLearner;
        if (grid.binCount() != frame->binCount()) {
            grid.begin(frame->binCount(),
                       frame->binWidthHz * static_cast<double>(frame->binCount()), false);
        }
        grid.addFrame(frame->binsDbfs, frame->centerHz());
    }
}

void AppState::pumpFrames() {
    advanceLearning();
    updateAutoSpurs();

    SpectrumFramePtr frame;
    std::vector<SpectrumFramePtr> passes;
    {
        const std::lock_guard lock(m_frameMutex);
        frame = std::move(m_pendingFrame);
        m_pendingFrame.reset();
        passes.swap(m_pendingPasses);
        if (frame) {
            m_latestFrame = frame;
        }
    }

    if (frame) {
        m_traces.setSmoothing(m_view.smoothing);
        m_traces.setMaxHoldDecay(m_view.maxHoldDecayDbPerSec);
        m_traces.setAverageWindow(static_cast<std::uint32_t>(m_view.averageWindow));
        m_traces.trace(TraceKind::MaxHold).visible = m_view.showMaxHold;
        m_traces.trace(TraceKind::MinHold).visible = m_view.showMinHold;
        m_traces.trace(TraceKind::Average).visible = m_view.showAverage;

        m_traces.update(*frame);

        // One waterfall line per completed pass when sweeping.
        //
        // A sweep publishes the whole grid every time a step lands, so the
        // display can update as the sweep moves. Those partials are the right
        // thing for the spectrum trace and the wrong thing for the waterfall:
        // each one is a snapshot of a sweep in progress, and writing them all
        // draws the sweep's own progress as a staircase of half-filled rows
        // rather than the spectrum over time.
        //
        // A row is a pass. That is what makes the time axis mean something on
        // a swept display -- and on a wide span it is genuinely slow, which is
        // the truth about how often that spectrum was actually measured.
        // At a fixed tune every frame is a row.
        if (!m_sweeping) {
            passes = {frame};
        }
    }

    // Every pass that completed since the last frame, in order, whatever
    // partials arrived after it.
    if (!passes.empty()) {
        const std::lock_guard lock(m_frameMutex);
        for (const SpectrumFramePtr& pass : passes) {
            // Bounded: if the UI stalls, old lines are dropped rather than
            // queued without limit.
            if (m_pendingWaterfallLines.size() >= 64) {
                break;
            }
            m_pendingWaterfallLines.push_back(
                WaterfallLine{.dbfs = pass->binsDbfs,
                              .startHz = pass->startHz,
                              .binWidthHz = pass->binWidthHz,
                              .ns = pass->hostTimeNs != 0 ? pass->hostTimeNs : monotonicNs()});
        }
    }

    // Telemetry at ~4 Hz, independent of frame rate.
    const std::uint64_t now = monotonicNs();
    if (now - m_lastTelemetrySampleNs > 250'000'000ULL) {
        m_stats = m_telemetry.sample();
        m_lastTelemetrySampleNs = now;

        // The running configuration, for plugins that read it. On this timer
        // rather than per frame: it flattens the whole profile to a few dozen
        // strings, which is nothing four times a second and real work sixty.
        PluginManager::instance().setProfile(currentProfile("current", PluginState::Omit));

        // Whether the learned floor still applies, on the same cadence: a
        // gain change reaches the device through several paths, and this is
        // the one place that sees the result of all of them.
        refreshFloorStaleness();

        // Device health is read here rather than by the panel that shows it.
        //
        // Each of these is a USB control transfer, so polling them per frame
        // would put a hundred round trips a second on the same bus carrying
        // the samples -- and the panel would only be open sometimes, making
        // the load depend on what the operator happened to have on screen.
        if (m_device) {
            const std::vector<ISdrDevice::HealthReading> readings = m_device->healthReadings();

            // Matched by label rather than by index: a driver may report a
            // reading conditionally, and the histories have to follow the
            // reading they belong to rather than a position that shifts.
            std::vector<HealthTrace> updated;
            updated.reserve(readings.size());

            for (const ISdrDevice::HealthReading& reading : readings) {
                const auto existing =
                    std::ranges::find_if(m_health, [&reading](const HealthTrace& trace) {
                        return trace.reading.label == reading.label;
                    });

                HealthTrace trace;
                if (existing != m_health.end()) {
                    trace.history = existing->history;
                }
                trace.reading = reading;
                trace.history.push(reading.numeric);
                updated.push_back(std::move(trace));
            }

            m_health = std::move(updated);
        } else if (!m_health.empty()) {
            m_health.clear();
        }
    }

    m_telemetry.render().framesRendered.fetch_add(1, std::memory_order_relaxed);
}

// ---- receiver corrections ---------------------------------------------------

CalibrationContext AppState::currentContext() const {
    if (!m_device) {
        return {};
    }
    return calibrationContextFor(*m_device, m_correctionSettings.dcRemoval);
}

CorrectionSettings AppState::effectiveCorrectionSettings() const noexcept {
    CorrectionSettings settings = m_correctionSettings;
    if (m_learn) {
        // A learn measures the receiver as it is, so the corrections it is
        // about to replace stay out of the data. The scan for fixed spurs
        // runs through the LO-offset set it just produced, because what it
        // looks for is what that set leaves behind.
        const bool throughLoSet = m_learn->phase == LearnRun::Phase::Absolute;
        settings.flatten = throughLoSet;
        settings.spurMask = throughLoSet;
    }
    return settings;
}

void AppState::pushCorrectionSettings() {
    if (m_pipeline) {
        m_pipeline->setCorrectionSettings(effectiveCorrectionSettings());
    }
}

void AppState::setCorrectionSettings(const CorrectionSettings& settings) {
    const bool dcRemovalWas = m_correctionSettings.dcRemoval;
    const bool autoSpursWere = m_correctionSettings.autoSpurs;
    m_correctionSettings = settings;
    pushCorrectionSettings();

    // DC removal is part of the context a floor was learned in.
    if (dcRemovalWas != settings.dcRemoval) {
        installCorrections();
    }
    if (autoSpursWere != settings.autoSpurs) {
        if (settings.autoSpurs) {
            beginAutoSpurs();
        } else {
            endAutoSpurs();
        }
    }
}

const CorrectionSet* AppState::corrections() const noexcept {
    return m_corrections ? &*m_corrections : nullptr;
}

std::size_t AppState::spurCount() const noexcept {
    return m_corrections ? m_corrections->spurs.size() : 0;
}

std::size_t AppState::automaticSpurCount() const noexcept {
    if (!m_corrections) {
        return 0;
    }
    return static_cast<std::size_t>(std::ranges::count_if(
        m_corrections->spurs, [](const SpurEntry& e) { return e.automatic; }));
}

void AppState::loadCalibration() {
    m_corrections.reset();
    m_floorStaleReason.clear();

    if (m_device) {
        const std::filesystem::path path = CorrectionSet::pathFor(m_device->info());
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            if (auto loaded = CorrectionSet::load(path)) {
                m_corrections = std::move(*loaded);
                logInfo("calibration", "{}: floor over {} points, {} spurs, learned {}",
                        path.filename().string(), m_corrections->floor.levelDb.size(),
                        m_corrections->spurs.size(), m_corrections->learnedAt);
            } else {
                logWarn("calibration", "{}: {}", path.string(), loaded.error().describe());
            }
        }
    }

    installCorrections();
}

void AppState::installCorrections() {
    if (!m_pipeline) {
        return;
    }
    if (!m_corrections || !m_device) {
        m_floorStaleReason.clear();
        m_pipeline->setCorrections(nullptr);
        return;
    }

    // A copy, so the set the pipeline reads never changes under it and the
    // stale floor can be left out of what it sees while staying on record.
    auto installed = std::make_shared<CorrectionSet>(*m_corrections);
    m_floorStaleReason.clear();
    if (!installed->floor.empty()) {
        m_floorStaleReason = installed->context.firstDifference(currentContext());
        if (!m_floorStaleReason.empty()) {
            installed->floor = {};
        }
    }
    m_pipeline->setCorrections(std::move(installed));
}

void AppState::refreshFloorStaleness() {
    if (!m_corrections || m_corrections->floor.empty() || !m_device) {
        return;
    }
    if (m_corrections->context.firstDifference(currentContext()) != m_floorStaleReason) {
        installCorrections();
    }
}

Status AppState::startLearning() {
    if (!running() || !m_device) {
        return fail(ErrorCode::Unavailable, "start acquisition before learning");
    }
    if (m_learn) {
        return fail(ErrorCode::AlreadyExists, "a learn is already running");
    }

    const bool sweeping = m_sweeping && m_sweepEngine->running();
    const std::size_t fftSize =
        sweeping ? m_sweepEngine->schedule().fftSize : m_pipeline->config().fftSize;
    const double sampleRate =
        sweeping ? m_sweepEngine->plan().sampleRate
                 : asDouble(m_device->getParameter(kSampleRateKey).value_or(SdrValue{0.0}));
    if (fftSize == 0 || sampleRate <= 0.0) {
        return fail(ErrorCode::Unavailable, "no grid to learn against");
    }

    {
        const std::lock_guard lock(m_learnMutex);
        LearnRun& run = m_learn.emplace();
        run.phase = sweeping ? LearnRun::Phase::LoOffsets : LearnRun::Phase::Fixed;
        run.savedSettings = m_correctionSettings;
        run.context = currentContext();
        run.passesHandled = m_passesSeen.load(std::memory_order_relaxed);
        run.learner.begin(fftSize, sampleRate, sweeping);
    }
    pushCorrectionSettings();

    m_toasts.info(sweeping ? std::format("Learning receiver corrections over the next {} "
                                         "passes. Keep the antenna disconnected.",
                                         learnPasses())
                           : std::format("Learning receiver corrections from the next {} "
                                         "frames. Keep the antenna disconnected.",
                                         kLearnFrames),
                  monotonicNs());
    return ok();
}

std::string AppState::learningLabel() const {
    const std::lock_guard lock(m_learnMutex);
    if (!m_learn) {
        return {};
    }
    switch (m_learn->phase) {
    case LearnRun::Phase::LoOffsets:
        return "learning the floor and LO-offset spurs";
    case LearnRun::Phase::Absolute:
        return std::format("scanning for fixed spurs, pass {} of {}",
                           m_learn->gridLearner.frameCount() + 1, LearnParameters{}.absolutePasses);
    case LearnRun::Phase::Fixed:
        return std::format("learning, {} of {} frames", m_learn->learner.frameCount(),
                           kLearnFrames);
    }
    return {};
}

std::size_t AppState::learnPasses() noexcept {
    // One for the floor and the LO-offset spurs, the rest through them, plus
    // the one under way when the button is pressed.
    return LearnParameters{}.absolutePasses + 2;
}

void AppState::cancelLearning() {
    if (m_learn) {
        abortLearning();
        m_toasts.info("Learning cancelled", monotonicNs());
    }
}

void AppState::observeStep(const SpectrumFrame& frame) noexcept {
    const std::lock_guard lock(m_learnMutex);
    if (m_learn) {
        if (m_learn->phase == LearnRun::Phase::LoOffsets) {
            m_learn->learner.addFrame(frame.binsDbfs, frame.config.centerHz);
        }
    } else if (m_autoLearner) {
        m_autoLearner->addFrame(frame.binsDbfs, frame.config.centerHz);
    }
}

void AppState::advanceLearning() {
    if (!m_learn) {
        return;
    }
    LearnRun& run = *m_learn;

    switch (run.phase) {
    case LearnRun::Phase::LoOffsets: {
        const std::uint64_t passes = m_passesSeen.load(std::memory_order_relaxed);
        if (passes == run.passesHandled) {
            return;
        }
        run.passesHandled = passes;

        // The pass that was under way when the learn began only contributed
        // its remainder. Half the steps is enough for the per-bin statistics
        // -- every step measures every local bin -- and less than that waits
        // for the next pass.
        CorrectionLearner learner;
        {
            const std::lock_guard lock(m_learnMutex);
            learner = run.learner;
        }
        const std::size_t steps = m_sweepEngine->schedule().steps.size();
        if (learner.frameCount() < std::max<std::size_t>(1, steps / 2)) {
            return;
        }

        run.floor = learner.floorShape();
        run.loSpurs = learner.loSpurs();

        // Installed now, so the next pass is measured through them and what
        // it still shows is at a fixed frequency.
        CorrectionSet interim;
        interim.context = run.context;
        interim.floor = run.floor;
        interim.spurs = run.loSpurs;
        m_corrections = std::move(interim);
        installCorrections();

        if (!m_sweepEngine->plan().continuous) {
            m_toasts.warning("One-shot plan: fixed-frequency spurs were not scanned. Learn on a "
                             "continuous sweep to find them.",
                             monotonicNs());
            finishLearning({});
            return;
        }

        {
            const std::lock_guard lock(m_learnMutex);
            run.phase = LearnRun::Phase::Absolute;
            run.installedAtPass = m_sweepEngine->passCount();
            run.gridLearner = {};
        }
        pushCorrectionSettings();
        m_toasts.info(std::format("Floor and {} LO-offset spur(s) learned; scanning the next "
                                  "{} passes for fixed spurs",
                                  run.loSpurs.size(), LearnParameters{}.absolutePasses),
                      monotonicNs());
        return;
    }

    case LearnRun::Phase::Absolute: {
        CorrectionLearner grid;
        {
            const std::lock_guard lock(m_learnMutex);
            if (run.gridLearner.frameCount() < LearnParameters{}.absolutePasses) {
                return;
            }
            grid = run.gridLearner;
        }
        finishLearning(grid.loSpurs());
        return;
    }

    case LearnRun::Phase::Fixed: {
        CorrectionLearner learner;
        {
            const std::lock_guard lock(m_learnMutex);
            if (run.learner.frameCount() < kLearnFrames) {
                return;
            }
            learner = run.learner;
        }
        run.floor = learner.floorShape();
        run.loSpurs = learner.loSpurs();
        finishLearning({});
        return;
    }
    }
}

void AppState::finishLearning(const std::vector<SpurEntry>& absoluteSpurs) {
    LearnRun run;
    {
        const std::lock_guard lock(m_learnMutex);
        run = std::move(*m_learn);
        m_learn.reset();
    }

    CorrectionSet set;
    set.context = run.context;
    set.floor = std::move(run.floor);
    set.spurs = std::move(run.loSpurs);
    const std::size_t loCount = set.spurs.size();
    set.spurs.insert(set.spurs.end(), absoluteSpurs.begin(), absoluteSpurs.end());
    set.learnedAt = formatWallClockIso8601(wallClockNs());

    std::string savedAs;
    if (m_device) {
        const std::filesystem::path path = CorrectionSet::pathFor(m_device->info());
        if (auto saved = set.save(path); !saved) {
            m_toasts.error(
                std::format("could not save the calibration: {}", saved.error().describe()),
                monotonicNs());
        } else {
            savedAs = path.filename().string();
        }
    }

    m_corrections = std::move(set);

    // Learning is a request to use the result: the switches come back as
    // they were, with these two on.
    m_correctionSettings = run.savedSettings;
    m_correctionSettings.flatten = true;
    m_correctionSettings.spurMask = true;
    pushCorrectionSettings();
    installCorrections();
    beginAutoSpurs();

    m_toasts.success(
        std::format("Corrections learned: floor over {} points, {} LO-offset and "
                    "{} fixed spur(s){}",
                    m_corrections->floor.levelDb.size(), loCount, absoluteSpurs.size(),
                    savedAs.empty() ? std::string{} : std::format(", saved as {}", savedAs)),
        monotonicNs());
}

void AppState::abortLearning() {
    if (!m_learn) {
        return;
    }
    CorrectionSettings saved;
    {
        const std::lock_guard lock(m_learnMutex);
        saved = m_learn->savedSettings;
        m_learn.reset();
    }
    m_correctionSettings = saved;
    pushCorrectionSettings();

    // An interim set may have gone in after the first phase; what is on disk
    // is what stands.
    loadCalibration();
}

void AppState::beginAutoSpurs() {
    if (!m_correctionSettings.autoSpurs || !m_sweeping || !running() || !m_sweepEngine ||
        !m_sweepEngine->running()) {
        return;
    }
    const std::lock_guard lock(m_learnMutex);
    m_autoLearner.emplace();
    m_autoLearner->begin(m_sweepEngine->schedule().fftSize, m_sweepEngine->plan().sampleRate, true);
    m_autoPassesHandled = m_passesSeen.load(std::memory_order_relaxed);
}

void AppState::endAutoSpurs() {
    const std::lock_guard lock(m_learnMutex);
    m_autoLearner.reset();
}

void AppState::updateAutoSpurs() {
    if (!m_autoLearner || m_learn) {
        return;
    }
    const std::uint64_t passes = m_passesSeen.load(std::memory_order_relaxed);
    if (passes == m_autoPassesHandled) {
        return;
    }
    m_autoPassesHandled = passes;

    // One pass at a time: the minimum over a pass is what tells a spur from
    // a signal, and the next pass starts from nothing.
    CorrectionLearner learner;
    {
        const std::lock_guard lock(m_learnMutex);
        learner = *m_autoLearner;
        m_autoLearner->begin(learner.binCount(), learner.sampleRate(), true);
    }
    if (learner.frameCount() == 0 || learner.binCount() == 0) {
        return;
    }

    const std::vector<SpurEntry> found = learner.loSpurs();
    if (found.empty()) {
        return;
    }

    if (!m_corrections) {
        m_corrections.emplace();
        m_corrections->context = currentContext();
    }

    // Additive: the data this saw was already masked, so anything it found
    // is something the mask does not yet cover.
    const double binWidthHz = learner.sampleRate() / static_cast<double>(learner.binCount());
    std::size_t added = 0;
    for (SpurEntry spur : found) {
        const bool covered =
            std::ranges::any_of(m_corrections->spurs, [&](const SpurEntry& existing) {
                return existing.kind == SpurKind::LoOffset &&
                       std::abs(existing.hz - spur.hz) <=
                           (existing.widthHz + spur.widthHz) * 0.5 + binWidthHz;
            });
        if (covered) {
            continue;
        }
        spur.automatic = true;
        m_corrections->spurs.push_back(spur);
        ++added;
    }

    if (added > 0) {
        installCorrections();
        logInfo("calibration", "{} LO-offset spur(s) added automatically", added);
    }
}

void AppState::clearAutoSpurs() {
    if (!m_corrections) {
        return;
    }
    std::erase_if(m_corrections->spurs, [](const SpurEntry& e) { return e.automatic; });
    installCorrections();
}

void AppState::clearCorrections() {
    abortLearning();
    if (m_device) {
        const std::filesystem::path path = CorrectionSet::pathFor(m_device->info());
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            m_toasts.error(std::format("could not remove {}: {}", path.string(), ec.message()),
                           monotonicNs());
        }
    }
    m_corrections.reset();
    m_floorStaleReason.clear();
    installCorrections();
    beginAutoSpurs();
}

std::vector<AppState::WaterfallLine> AppState::takePendingWaterfallLines() {
    const std::lock_guard lock(m_frameMutex);
    return std::exchange(m_pendingWaterfallLines, {});
}

SpectrumFramePtr AppState::latestFrame() const {
    const std::lock_guard lock(m_frameMutex);
    return m_latestFrame;
}

FrequencySpan AppState::fitRange() const {
    if (m_traces.binCount() > 0) {
        return {m_traces.startHz(), m_traces.stopHz()};
    }

    // No data yet. Prefer what the open device is tuned to, so the axis is
    // already right when the first frame lands.
    if (m_device != nullptr) {
        const double centerHz =
            asDouble(m_device->getParameter("center_hz").value_or(SdrValue{100e6}));
        const double sampleRate =
            asDouble(m_device->getParameter("sample_rate").value_or(SdrValue{20e6}));
        if (sampleRate > 0.0) {
            return {centerHz - sampleRate * 0.5, centerHz + sampleRate * 0.5};
        }
    }

    // Nothing open at all. FM broadcast: present everywhere, and a range whose
    // axis labels read sensibly -- unlike a 0..1 Hz placeholder, which renders
    // as "0 .. 1e-06 MHz" and looks like a fault.
    return {88e6, 108e6};
}

ViewLimits AppState::viewLimits() const {
    if (m_device) {
        return {std::max(0.0, m_device->info().minFrequencyHz), m_device->info().maxFrequencyHz};
    }
    if (m_traces.binCount() > 0) {
        return {std::max(0.0, m_traces.startHz()), m_traces.stopHz()};
    }
    return {};
}

void AppState::setTheme(const std::string& name) {
    const auto match = std::ranges::find_if(
        m_themes, [&name](const Theme& candidate) { return candidate.name() == name; });
    if (match != m_themes.end()) {
        m_theme = *match;
    } else if (!m_themes.empty()) {
        m_theme = m_themes.front();
    }

    m_view.themeName = m_theme.name();
    m_stylePending = true;
}

void AppState::reloadThemes() {
    m_themes = discoverThemes();
}

void AppState::refreshTheme() {
    m_stylePending = true;
}

void AppState::applyPendingStyle() {
    if (std::exchange(m_stylePending, false)) {
        applyThemeToImGui();
    }
}

float AppState::effectiveUiScale() const noexcept {
    // A configured scale wins; zero means follow the display.
    //
    // The display's own number is not a preference and is not stored: it is a
    // property of the screen the window opened on, and writing it into the
    // config would freeze one machine's answer into a file that may be shared
    // with another over a home directory.
    return m_appSettings.uiScale > 0.0F ? m_appSettings.uiScale : m_displayUiScale;
}

void AppState::setDisplayUiScale(float scale) {
    m_displayUiScale = scale > 0.0F ? scale : 1.0F;
}

void AppState::setBaseFontSize(float points) noexcept {
    m_baseFontSize = points > 0.0F ? points : 15.0F;
}

void AppState::adoptAppSettings(AppSettings settings) {
    m_appSettings = settings;
}

Status AppState::saveAppSettings() const {
    return m_appSettings.save(Paths::instance().configDir() / "app.toml");
}

void AppState::applyThemeToImGui() {
    ImGuiStyle& style = ImGui::GetStyle();
    const ChromeTheme& chrome = m_theme.chrome();

    // Every metric back to ImGui's own default before anything is written,
    // because the scale at the end of this function multiplies in place.
    //
    // ScaleAllSizes reaches all fifty-odd of them and the theme below assigns
    // a dozen; the rest were therefore multiplied afresh on every theme change
    // and on every frame of a scale drag, and they compound. WindowMinSize is
    // the one that shows: nudge the scale past 100% for a second and it
    // outgrows the display, at which point ImGui enforces it on the root
    // window, the window becomes larger than the screen, and the status bar,
    // the waterfall and most of the spectrum are pushed off the bottom of it.
    // Nothing brought them back either -- returning the scale to 100% leaves
    // the compounded figures exactly where they were.
    style = ImGuiStyle();

    // StyleColorsDark/Light only touch Colors, so the font scale set up at
    // window creation survives a theme switch.
    if (chrome.dark) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
    }

    // Compact metrics. An instrument panel has dozens of controls competing
    // for one screen, and generous desktop-app padding costs roughly a third
    // of a settings panel's vertical space for nothing.
    style.WindowRounding = chrome.rounding;
    style.ChildRounding = chrome.rounding;
    style.FrameRounding = 2.0F;
    style.PopupRounding = chrome.rounding;
    style.GrabRounding = 2.0F;
    style.TabRounding = chrome.rounding;
    style.ScrollbarRounding = 2.0F;
    style.WindowBorderSize = chrome.borderSize;
    // No border on framed widgets. ImGui applies FrameBorderSize to buttons
    // and collapsing headers as well as to inputs, so switching it on puts a
    // box around every element in the panel -- which is noise, not clarity.
    // Depth comes from fill instead: buttons raised, inputs inset.
    style.FrameBorderSize = 0.0F;
    // Panels and popups keep an edge. There are few of them, and the outline
    // is what separates a panel from the plots under glare.
    style.ChildBorderSize = chrome.borderSize;
    style.PopupBorderSize = chrome.borderSize;
    style.WindowPadding = ImVec2(6.0F, 6.0F);
    style.FramePadding = ImVec2(6.0F, 3.0F);
    style.ItemSpacing = ImVec2(6.0F, 3.0F);
    style.ItemInnerSpacing = ImVec2(5.0F, 3.0F);
    style.IndentSpacing = 14.0F;
    style.ScrollbarSize = 11.0F;
    style.GrabMinSize = 9.0F;
    style.CellPadding = ImVec2(4.0F, 2.0F);
    // Sub-section headings: no left stub before the text, so a heading reads
    // as a heading rather than as a stray dash.
    style.SeparatorTextBorderSize = 1.0F;
    style.SeparatorTextAlign = ImVec2(0.0F, 0.5F);
    style.SeparatorTextPadding = ImVec2(0.0F, 6.0F);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = toImVec4(chrome.windowBackground);
    colors[ImGuiCol_ChildBg] = toImVec4(chrome.panelBackground);
    colors[ImGuiCol_PopupBg] = toImVec4(chrome.panelBackground);
    colors[ImGuiCol_MenuBarBg] = toImVec4(chrome.headerBackground);
    colors[ImGuiCol_Text] = toImVec4(chrome.text);
    colors[ImGuiCol_TextDisabled] = toImVec4(chrome.textDim);
    colors[ImGuiCol_Border] = toImVec4(chrome.border);
    colors[ImGuiCol_Separator] = toImVec4(chrome.separator);
    colors[ImGuiCol_SeparatorHovered] = toImVec4(chrome.accent);
    colors[ImGuiCol_SeparatorActive] = toImVec4(chrome.accentHover);
    colors[ImGuiCol_Button] = toImVec4(chrome.buttonBackground);
    colors[ImGuiCol_ButtonHovered] = toImVec4(chrome.buttonHover);
    colors[ImGuiCol_ButtonActive] = toImVec4(chrome.buttonActive);
    // Inputs, combos and sliders read as wells sunk into the panel; buttons
    // above read as raised. The two are then unmistakable at a glance without
    // either carrying an outline.
    colors[ImGuiCol_FrameBg] = toImVec4(chrome.inputBackground);
    colors[ImGuiCol_FrameBgHovered] = toImVec4(chrome.inputHover);
    colors[ImGuiCol_FrameBgActive] = toImVec4(chrome.inputActive);
    colors[ImGuiCol_Header] = toImVec4(chrome.headerBackground);
    colors[ImGuiCol_HeaderHovered] = toImVec4(chrome.buttonHover);
    colors[ImGuiCol_HeaderActive] = toImVec4(chrome.buttonActive);
    colors[ImGuiCol_TitleBg] = toImVec4(chrome.headerBackground);
    colors[ImGuiCol_TitleBgActive] = toImVec4(chrome.headerBackground);
    colors[ImGuiCol_TitleBgCollapsed] = toImVec4(chrome.panelBackground);
    colors[ImGuiCol_CheckMark] = toImVec4(chrome.accent);
    colors[ImGuiCol_SliderGrab] = toImVec4(chrome.accent);
    colors[ImGuiCol_SliderGrabActive] = toImVec4(chrome.accentHover);
    colors[ImGuiCol_Tab] = toImVec4(chrome.panelBackground);
    colors[ImGuiCol_TabHovered] = toImVec4(chrome.buttonHover);
    colors[ImGuiCol_ScrollbarBg] = toImVec4(chrome.panelBackground);
    colors[ImGuiCol_ScrollbarGrab] = toImVec4(chrome.buttonBackground);
    colors[ImGuiCol_ScrollbarGrabHovered] = toImVec4(chrome.buttonHover);
    colors[ImGuiCol_ScrollbarGrabActive] = toImVec4(chrome.buttonActive);

    // ImGui defaults BorderShadow to transparent black but Border to a value
    // from the base style; both are set explicitly so the theme's border
    // colour is the one actually drawn.
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_TableBorderStrong] = toImVec4(chrome.border);
    colors[ImGuiCol_TableBorderLight] = toImVec4(chrome.separator);
    colors[ImGuiCol_TableHeaderBg] = toImVec4(chrome.headerBackground);
    colors[ImGuiCol_TextSelectedBg] = toImVec4(chrome.accent.withAlpha(0.35F));
    colors[ImGuiCol_NavCursor] = toImVec4(chrome.accent);
    colors[ImGuiCol_DragDropTarget] = toImVec4(chrome.accent);
    colors[ImGuiCol_TabSelected] = toImVec4(chrome.buttonActive);
    colors[ImGuiCol_TabDimmed] = toImVec4(chrome.panelBackground);
    colors[ImGuiCol_TabDimmedSelected] = toImVec4(chrome.buttonBackground);
    colors[ImGuiCol_PlotLines] = toImVec4(chrome.accent);
    colors[ImGuiCol_PlotHistogram] = toImVec4(chrome.accent);

    // The scale goes on last, and it has to be here.
    //
    // Everything above assigns absolute pixel metrics from scratch, so a scale
    // applied any earlier -- at start-up, or in the panel that changes it --
    // is wiped by the next theme switch or gradient edit. This function is
    // also the live-apply hook, called from setTheme and refreshTheme, so
    // "last thing in here" is both the only correct place and the one that
    // makes the slider move the interface as it is dragged.
    const float scale = effectiveUiScale();
    style.ScaleAllSizes(scale);

    // Text, through FontScaleMain and not FontSizeBase.
    //
    // FontSizeBase looks like the field for this and is not: ImGui rewrites it
    // from the context every time the current font size is recomputed, which
    // happens at every Begin(). A value assigned here -- from inside a frame,
    // because this runs from the panel that changed it -- was therefore
    // overwritten before the next window was drawn, and the text never changed
    // size at all. That is what made the Appearance sliders look as though
    // they only reached a few paddings. FontScaleMain is the documented global
    // factor, and it persists.
    //
    // The size the operator asked for is expressed as a ratio against the size
    // the atlas was built at, which is what the preferences held when the
    // window opened. 1.92 rasterises on demand, so the glyphs are baked at the
    // final size rather than stretched.
    const float fontRatio = m_baseFontSize > 0.0F ? m_appSettings.fontSize / m_baseFontSize : 1.0F;
    style.FontScaleMain = scale * fontRatio;

    // The bars, the pane divider and the popovers push their own metrics over
    // the style, so ScaleAllSizes never reaches them; this is where they read
    // the scale from.
    bar::setScale(scale);

    // ImPlot keeps a style of its own that ImGui's scaling does not touch, and
    // the plots are most of the window. Rebuilt from its defaults rather than
    // multiplied in place, for the same reason the block above is: this
    // function runs again on every theme change and a cumulative scale would
    // compound.
    ImPlotStyle& plots = ImPlot::GetStyle();
    const ImPlotStyle defaults;
    plots.PlotPadding = scaled(defaults.PlotPadding, scale);
    plots.LabelPadding = scaled(defaults.LabelPadding, scale);
    plots.LegendPadding = scaled(defaults.LegendPadding, scale);
    plots.LegendInnerPadding = scaled(defaults.LegendInnerPadding, scale);
    plots.LegendSpacing = scaled(defaults.LegendSpacing, scale);
    plots.MousePosPadding = scaled(defaults.MousePosPadding, scale);
    plots.AnnotationPadding = scaled(defaults.AnnotationPadding, scale);
    plots.MajorTickLen = scaled(defaults.MajorTickLen, scale);
    plots.MinorTickLen = scaled(defaults.MinorTickLen, scale);
    plots.PlotMinSize = scaled(defaults.PlotMinSize, scale);
}

} // namespace sweeppp::ui
