// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AppState.hpp"

#include "BarChrome.hpp"
#include "FileDialog.hpp"
#include "Icons.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
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

} // namespace

AppState::AppState() = default;

AppState::~AppState() {
    // The radio goes before the plugin that handed it out.
    //
    // A plugin driver's device carries a vtable living in the plugin's image,
    // and `shutdown()` withdraws every facet whether or not one is still open
    // -- so deactivating first would leave this object holding a radio it
    // could not even destroy. Closing here also means the plugin receives the
    // device-closed event on a channel that is still live, which it did not
    // when the order was the other way round.
    if (m_remote) {
        m_remote->disconnect();
    }
    if (m_local) {
        m_local->closeDevice();
    }

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

    installLoggingSubscribers(m_events);

    // After discovery, so a switcher or a backend a plugin provides is there
    // to be opened. Everything the engine publishes lands on the display bus.
    m_local = std::make_unique<LocalInstrument>(m_displayBus, m_events, m_telemetry,
                                                InstrumentPaths::fromConfig(), **backend);
    m_displayBus.subscribe(this);
    m_servers = remote::ServerList::load(Paths::instance().configDir() / "servers.toml");

    m_themes = discoverThemes();
    setTheme(m_view.themeName);

    m_presets = SweepPresetStore::load(Paths::instance().configDir() / "sweep-presets.toml");
    m_markerPresets =
        MarkerPresetStore::load(Paths::instance().configDir() / "marker-presets.toml");
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
    if (m_startupProfile && m_startupProfile->deviceDriver == "remote") {
        startup.server = endpointFor(m_startupProfile->deviceId);
        startup.label = m_startupProfile->deviceLabel.empty() ? m_startupProfile->deviceId
                                                              : m_startupProfile->deviceLabel;
    } else if (m_startupProfile) {
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
    m_reconnect.cancel();
    if (!m_remote && m_local->holds(driver, id)) {
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

void AppState::beginConnectServer(const remote::RemoteEndpoint& endpoint, const std::string& label,
                                  bool reconnect) {
    if (m_viewerMode || m_startup) {
        return;
    }
    if (!reconnect) {
        m_reconnect.cancel();
    }
    if (m_remote && m_remote->profileId() == endpoint.address() && m_remote->linkUp()) {
        clearError();
        return;
    }

    DeviceStartup startup;
    startup.server = endpoint;
    startup.label = label.empty() ? endpoint.address() : label;
    startup.enumerate = false;
    startup.reconnect = reconnect;
    startDeviceWorker(std::move(startup));
}

std::string AppState::reconnectStatus() const {
    if (!m_reconnect.active()) {
        return {};
    }
    if (m_reconnect.attempting() || m_reconnect.attempt() == 0) {
        return std::format("Reconnecting to {} (attempt {})", m_reconnect.label(),
                           std::max(m_reconnect.attempt(), 1));
    }
    const std::uint64_t now = monotonicNs();
    const std::uint64_t waitNs = m_reconnect.nextAtNs() > now ? m_reconnect.nextAtNs() - now : 0;
    return std::format("Lost {}; trying again in {:.0f} s (attempt {} failed)", m_reconnect.label(),
                       std::ceil(nsToSeconds(waitNs)), m_reconnect.attempt());
}

void AppState::stopReconnecting() {
    m_reconnect.cancel();
    m_reconnectProfile.reset();
    clearError();
}

void AppState::loseRemote() {
    // Read before the link is torn down, which takes the radio's settings
    // with it.
    const remote::RemoteInstrument::LostState lost = m_remote->lostState();
    Profile snapshot = currentProfile("reconnect", PluginState::Omit);
    snapshot.deviceDriver = "remote";
    snapshot.deviceId = m_remote->profileId();
    snapshot.deviceLabel = m_remote->serverName();
    snapshot.deviceParameters = lost.parameters;
    const remote::RemoteEndpoint endpoint = m_remote->endpoint();
    const std::string label = m_remote->serverName();

    dropRemote();

    m_reconnectProfile = std::move(snapshot);
    m_reconnectWasRunning = lost.running;
    m_reconnect.begin(endpoint, label, monotonicNs());
}

remote::RemoteEndpoint AppState::endpointFor(const std::string& address) const {
    if (const remote::SavedServer* saved = m_servers.find(address)) {
        return saved->endpoint;
    }
    auto parsed = remote::RemoteEndpoint::parse(address);
    return parsed ? *parsed : remote::RemoteEndpoint{.host = address};
}

void AppState::setLinkResolution(std::uint32_t maxBins) {
    if (!m_remote) {
        return;
    }
    m_remote->setLinkResolution(maxBins);
    if (remote::SavedServer* saved = m_servers.find(m_remote->profileId())) {
        saved->maxBins = maxBins;
        saveServers();
    }
}

std::vector<remote::mdns::DiscoveredServer> AppState::discoveredServers() {
    m_browserWantedNs = monotonicNs();
    if (!m_browser) {
        auto started = remote::mdns::Browser::start();
        if (!started) {
            logInfo("remote", "not looking for servers: {}", started.error().describe());
            return {};
        }
        m_browser = std::move(*started);
    }
    return m_browser->servers();
}

void AppState::saveServers() {
    if (auto saved = m_servers.save(Paths::instance().configDir() / "servers.toml"); !saved) {
        logWarn("remote", "{}", saved.error().describe());
    }
}

void AppState::startDeviceWorker(DeviceStartup startup) {
    // The radio in hand is released before another is claimed, on this thread,
    // before the worker exists.
    //
    // A device this process still holds cannot be opened again -- the driver is
    // asked for a handle to something it has already given out, and on a
    // BladeRF that is where it stops. Adopting the new one first and closing
    // the old one after reads better and is exactly backwards.
    if ((!startup.driver.empty() || startup.server) && (device() != nullptr || m_remote)) {
        closeDevice();
    }

    startup.startedNs = monotonicNs();
    startup.opening = std::make_shared<std::atomic_bool>(!startup.enumerate);

    startup.future =
        std::async(std::launch::async, [this, driver = startup.driver, id = startup.id,
                                        server = startup.server, reconnect = startup.reconnect,
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
            if (server) {
                // The buses are members and outlive the worker: the future is
                // waited for before any of them is destroyed.
                opening->store(true, std::memory_order_relaxed);
                // Shorter when retrying: the operator did not ask for this
                // one, and the toolbar waits on it.
                auto connected = remote::RemoteInstrument::connect(
                    *server, m_displayBus, m_events,
                    reconnect ? std::chrono::seconds(3) : std::chrono::seconds(5));
                if (connected) {
                    result.remote = std::move(*connected);
                } else {
                    result.error = connected.error().message();
                }
                return result;
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
    const bool wantedDevice = !m_startup->driver.empty() || m_startup->server.has_value();
    const bool reconnect = m_startup->reconnect;
    m_startup.reset();

    if (reconnect && !result.remote) {
        m_reconnect.failed(monotonicNs());
        logInfo("remote", "reconnect failed: {}", result.error);
        if (m_reconnect.active()) {
            setError(reconnectStatus());
        }
        m_startupProfile.reset();
        return;
    }
    if (reconnect) {
        m_reconnect.succeeded();
    }

    if (result.enumerated) {
        m_devices = std::move(result.devices);
    }

    if (result.device || result.remote) {
        if (result.remote) {
            m_remote = std::move(result.remote);
            m_remote->begin();
            if (const remote::SavedServer* saved = m_servers.find(m_remote->profileId());
                saved != nullptr && saved->maxBins > 0) {
                m_remote->setLinkResolution(saved->maxBins);
            }
            // A server that kept the radio running through the drop still has
            // this desktop's setup; putting it back would only restart it.
            if (reconnect && m_remote->running()) {
                m_startupProfile.reset();
            }
        } else {
            m_local->adoptDevice(std::move(result.device));
        }
        clearError();

        if (m_startupProfile) {
            for (const auto& [key, value] : m_startupProfile->deviceParameters) {
                if (auto applied = instrument().setDeviceParameter(key, value); !applied) {
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

    if (reconnect && result.remote) {
        if (m_reconnectWasRunning && !instrument().running()) {
            (void)start();
        }
        m_reconnectProfile.reset();
        m_toasts.success(std::format("Reconnected to {}", label), monotonicNs());
    }
}

std::string AppState::deviceStartupLabel() const {
    if (!m_startup) {
        return {};
    }
    if (m_startup->server) {
        return std::format("Connecting to {}", m_startup->label);
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
    if (m_startup->server) {
        return std::format("Signing in to {} and reading its state", m_startup->server->address());
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
    profile.sweepPlan = instrument().sweepPlan();
    profile.sweeping = instrument().sweeping();
    profile.pipeline = instrument().pipelineConfig();
    profile.corrections = instrument().correctionSettings();
    profile.view = m_view;

    if (const DeviceDescriptor* open = device()) {
        profile.deviceDriver = instrument().profileDriver();
        profile.deviceId = instrument().profileId();
        profile.deviceLabel = instrument().displayLabel();

        // Every parameter the driver declares, read back rather than
        // remembered. A value the device coerced -- a sample rate it rounded,
        // a gain it clamped -- is saved as what it actually is, so reloading
        // the profile is idempotent instead of drifting a little each time.
        for (const SdrParameter& parameter : open->parameters) {
            if (parameter.readOnly) {
                continue;
            }
            if (auto value = instrument().parameter(parameter.key)) {
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

    if (auto applied = instrument().applyPipelineConfig(profile.pipeline); !applied) {
        logWarn("profile", "{}", applied.error().describe());
    }
    if (auto applied = instrument().setSweeping(profile.sweeping); !applied) {
        logWarn("profile", "{}", applied.error().describe());
    }
    instrument().setCorrectionSettings(profile.corrections);

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
        const bool remote = profile.deviceDriver == "remote";
        const bool alreadyOpen =
            remote ? m_remote && m_remote->profileId() == profile.deviceId
                   : !m_remote && m_local->holds(profile.deviceDriver, profile.deviceId);
        if (!alreadyOpen) {
            m_startupProfile = profile;
            if (devices == DeviceHandling::Now && remote) {
                beginConnectServer(endpointFor(profile.deviceId), profile.deviceLabel);
            } else if (devices == DeviceHandling::Now) {
                beginOpenDevice(profile.deviceDriver, profile.deviceId, profile.deviceLabel);
            }
        }
    }

    if (device() != nullptr) {
        for (const auto& [key, value] : profile.deviceParameters) {
            if (auto applied = instrument().setDeviceParameter(key, value); !applied) {
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

void AppState::beginRefreshDevices() {
    if (m_viewerMode || m_startup) {
        return;
    }

    DeviceStartup refresh;
    refresh.enumerate = true;
    startDeviceWorker(std::move(refresh));
}

void AppState::closeDevice() {
    m_reconnect.cancel();
    if (m_remote) {
        dropRemote();
        return;
    }
    m_local->closeDevice();
}

void AppState::dropRemote() {
    // What the server was doing becomes what the local instrument would do,
    // so opening a radio here next picks up the same plan and settings.
    m_local->adoptConfiguration(m_remote->sweepPlan(), m_remote->pipelineConfig(),
                                m_remote->sweeping(), m_remote->correctionSettings());
    m_remote->disconnect();
    for (InstrumentNotice& notice : m_remote->takeNotices()) {
        if (notice.kind == InstrumentNotice::Kind::Condition) {
            setError(std::move(notice.text));
        }
    }
    m_remote.reset();
}

void AppState::recordPlanChange(const SweepPlan& plan) {
    const std::vector<SweepSegment> previousSegments = instrument().sweepPlan().segments;

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
}

Status AppState::applySweepPlan(const SweepPlan& plan) {
    recordPlanChange(plan);
    return instrument().applySweepPlan(plan);
}

Status AppState::sweepRange(const SweepPlan& plan) {
    recordPlanChange(plan);
    return instrument().sweepRange(plan);
}

Status AppState::start() {
    if (auto started = instrument().start(); !started) {
        setError(started.error().describe());
        return started;
    }
    followStart();
    clearError();
    return ok();
}

void AppState::stop() {
    instrument().stop();
}

void AppState::followStart() {
    // Not running is not a run: a server connected to while idle reports
    // whatever generation it reached, which says nothing about this window.
    const std::uint64_t generation = instrument().startGeneration();
    if (!instrument().running() || generation == m_startGenerationSeen) {
        return;
    }
    m_startGenerationSeen = generation;

    // What was accumulated describes the last run.
    m_traces.clear();

    // Retention begins with acquisition, not when the operator asks for it.
    // "Save what I have been watching" cannot be answered by a writer created
    // at the moment of asking -- it would record only the future.
    if (auto session = beginSession(); !session) {
        logWarn("app", "session not being retained: {}", session.error().describe());
    }
    m_hasUnsavedSession = true;
}

void AppState::deliverNotices() {
    const std::uint64_t now = monotonicNs();
    for (InstrumentNotice& notice : instrument().takeNotices()) {
        switch (notice.kind) {
        case InstrumentNotice::Kind::Info:
            m_toasts.info(std::move(notice.text), now);
            break;
        case InstrumentNotice::Kind::Success:
            m_toasts.success(std::move(notice.text), now);
            break;
        case InstrumentNotice::Kind::Warning:
            m_toasts.warning(std::move(notice.text), now);
            break;
        case InstrumentNotice::Kind::Error:
            m_toasts.error(std::move(notice.text), now);
            break;
        case InstrumentNotice::Kind::Condition:
            setError(std::move(notice.text));
            break;
        case InstrumentNotice::Kind::ClearCondition:
            clearError();
            break;
        }
    }
}

void AppState::sampleHealth() {
    // Matched by label rather than by index: a driver may report a reading
    // conditionally, and the histories have to follow the reading they belong
    // to rather than a position that shifts.
    const std::vector<SdrHealthReading> readings = instrument().health();
    std::vector<HealthTrace> updated;
    updated.reserve(readings.size());

    for (const SdrHealthReading& reading : readings) {
        const auto existing = std::ranges::find_if(m_health, [&reading](const HealthTrace& trace) {
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
}

Status AppState::goBack() {
    std::vector<SweepSegment> segments = m_rangeHistory.goBack();
    if (segments.empty()) {
        return ok();
    }

    SweepPlan plan = instrument().sweepPlan();
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

    SweepPlan plan = instrument().sweepPlan();
    plan.segments = std::move(segments);

    m_navigatingHistory = true;
    auto applied = applySweepPlan(plan);
    m_navigatingHistory = false;
    return applied;
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
    const std::lock_guard lock(m_frameMutex);
    m_pendingFrame = frame;
}

void AppState::pumpFrames() {
    const std::uint64_t now = monotonicNs();
    instrument().tick(now);
    deliverNotices();
    if (m_remote && !m_remote->linkUp()) {
        loseRemote();
    }
    // Not asked for in a while: the chooser is closed, so stop asking the
    // network.
    if (m_browser && now - m_browserWantedNs > 10'000'000'000ULL) {
        m_browser.reset();
    }
    if (m_reconnect.active() && !m_startup && m_reconnect.due(now)) {
        m_startupProfile = m_reconnectProfile;
        beginConnectServer(m_reconnect.endpoint(), m_reconnect.label(), true);
    }

    // A restart the instrument made on its own -- a plan change while running,
    // a backend switch -- is a new run as much as Start is.
    followStart();

    SpectrumFramePtr frame;
    {
        const std::lock_guard lock(m_frameMutex);
        frame = std::move(m_pendingFrame);
        m_pendingFrame.reset();
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
        const bool advanceWaterfall = !instrument().sweeping() || frame->passComplete;

        if (advanceWaterfall) {
            const std::lock_guard lock(m_frameMutex);
            // Bounded: if the UI stalls, old lines are dropped rather than
            // queued without limit.
            if (m_pendingWaterfallLines.size() < 64) {
                m_pendingWaterfallLines.push_back(WaterfallLine{
                    .dbfs = frame->binsDbfs,
                    .startHz = frame->startHz,
                    .binWidthHz = frame->binWidthHz,
                    .ns = frame->hostTimeNs != 0 ? frame->hostTimeNs : monotonicNs()});
            }
        }
    }

    // Telemetry at ~4 Hz, independent of frame rate.
    if (now - m_lastTelemetrySampleNs > 250'000'000ULL) {
        if (const TelemetrySnapshot* engine = instrument().engineTelemetry()) {
            m_stats = m_telemetry.sampleWith(engine->stream, engine->process);
        } else {
            m_stats = m_telemetry.sample();
        }
        m_lastTelemetrySampleNs = now;

        // The running configuration, for plugins that read it. On this timer
        // rather than per frame: it flattens the whole profile to a few dozen
        // strings, which is nothing four times a second and real work sixty.
        PluginManager::instance().setProfile(currentProfile("current", PluginState::Omit));

        sampleHealth();
    }

    m_telemetry.render().framesRendered.fetch_add(1, std::memory_order_relaxed);
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
    if (device() != nullptr) {
        const double centerHz =
            asDouble(instrument().parameter("center_hz").value_or(SdrValue{100e6}));
        const double sampleRate =
            asDouble(instrument().parameter("sample_rate").value_or(SdrValue{20e6}));
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
    if (const DeviceDescriptor* open = device()) {
        return {std::max(0.0, open->info.minFrequencyHz), open->info.maxFrequencyHz};
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
