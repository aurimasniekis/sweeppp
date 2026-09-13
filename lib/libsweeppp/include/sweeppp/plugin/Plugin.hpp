// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/// Writing a Sweep++ plugin as an ordinary C++ class.
///
/// Header-only, and it depends on `PluginAbi.h` and the standard library and
/// nothing else -- deliberately, so that a plugin can use it without linking
/// libsweeppp at all. The C ABI stays the only real boundary; this is sugar
/// over it, not a second one.
///
/// The shape of a plugin:
///
///     class MyPlugin : public sweeppp::plugin::Plugin {
///     public:
///         bool activate(sweeppp::plugin::Host& host) override { ... }
///         void deactivate() override { ... }
///     };
///
///     const sweeppp_plugin_desc_t& describe() { ... }   // static storage
///     SWEEPPP_PLUGIN_MAIN(MyPlugin, describe)
///
/// Two rules the wrapper cannot enforce for you:
///
///   1. Everything reachable from the descriptor -- strings, arrays, vtables --
///      must have static storage duration. The `str()`, `author()` and
///      `facet()` helpers below are `constexpr` so a `static constexpr` table
///      is the natural way to write one.
///
///   2. A plugin may link libsweeppp for VALUES and must never touch a
///      SINGLETON. `Paths::instance()`, `SdrDeviceManager::instance()`,
///      `FftBackendManager::instance()` and `Log` are per-image state: the
///      plugin's copy is not the host's, and writing to it looks like it
///      worked. Use `Host` for all of those.

#include "sweeppp/plugin/PluginAbi.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(SWEEPPP_PLUGIN_HAS_UI)
#include <imgui.h>
// For ImGuiItemFlags_MixedValue alone, which is what `drawTick` below needs to
// draw a parent whose children disagree as a dash. `PushItemFlag` itself is
// public.
#include <imgui_internal.h>
#include <implot.h>
#endif

namespace sweeppp::plugin {

// ---------------------------------------------------------------- strings

[[nodiscard]] constexpr sweeppp_str_t str(std::string_view text) noexcept {
    return sweeppp_str_t{.data = text.empty() ? "" : text.data(), .len = text.size()};
}

[[nodiscard]] inline std::string_view view(sweeppp_str_t text) noexcept {
    return text.data != nullptr ? std::string_view(text.data, text.len) : std::string_view{};
}

namespace detail {

/// Containment on the plugin's side of the boundary.
///
/// The host catches too, but an exception unwinding through a C frame is
/// undefined behaviour rather than something a `catch` can promise to hold --
/// so it is stopped here, before it reaches one.
template <typename Fn>
void guard(Fn&& body) noexcept {
    try {
        body();
    } catch (...) {
        // Nothing useful to do: the host owns the reporting, and this facet
        // has already failed. Swallowing beats terminating.
    }
}

} // namespace detail

// ----------------------------------------------------------------- events

/// A string that is part of a type.
///
/// An event's identity crosses the ABI as text -- it has to, `typeid` cannot --
/// but nothing above this line should ever handle that text. Held in a
/// structural literal type so it can be a `static constexpr` member and, if a
/// plugin prefers, a template argument: `host.subscribe<RetuneEvent>(...)`
/// then names the event by type and the string never appears at a call site.
template <std::size_t N>
struct EventName {
    char value[N]{};

    consteval EventName(const char (&text)[N]) // NOLINT(google-explicit-constructor)
    {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = text[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(static_cast<const char*>(value), N - 1);
    }

    constexpr operator std::string_view() const noexcept // NOLINT(google-explicit-constructor)
    {
        return view();
    }
};

template <std::size_t N>
EventName(const char (&)[N]) -> EventName<N>;

/// How an event type is identified and versioned.
///
/// The primary template reads the two statics from the type itself, which is
/// how a plugin declares an event of its own:
///
///     struct PlanChanged {
///         std::uint32_t struct_size;          // first, always
///         sweeppp_str_t name;
///         static constexpr sweeppp::plugin::EventName kEventType{
///             "org.sweeppp.bandplan.plan_changed"};
///         static constexpr std::uint32_t kSchemaVersion = 1;
///     };
///
/// The host's own events are plain C structs and cannot carry statics, so they
/// are specialised below instead.
template <typename Event>
struct EventTraits {
    static constexpr std::string_view name = Event::kEventType;
    static constexpr std::uint32_t schemaVersion = Event::kSchemaVersion;
};

#define SWEEPPP_DECLARE_EVENT_TRAITS(Type, Name)                                                   \
    template <>                                                                                    \
    struct EventTraits<Type> {                                                                     \
        static constexpr std::string_view name = Name;                                             \
        static constexpr std::uint32_t schemaVersion = 1;                                          \
    }

SWEEPPP_DECLARE_EVENT_TRAITS(sweeppp_device_opened_event_t, SWEEPPP_EVENT_DEVICE_OPENED);
SWEEPPP_DECLARE_EVENT_TRAITS(sweeppp_device_closed_event_t, SWEEPPP_EVENT_DEVICE_CLOSED);
SWEEPPP_DECLARE_EVENT_TRAITS(sweeppp_device_error_event_t, SWEEPPP_EVENT_DEVICE_ERROR);
SWEEPPP_DECLARE_EVENT_TRAITS(sweeppp_parameter_changed_event_t, SWEEPPP_EVENT_PARAMETER_CHANGED);
SWEEPPP_DECLARE_EVENT_TRAITS(sweeppp_retune_event_t, SWEEPPP_EVENT_RETUNE);
SWEEPPP_DECLARE_EVENT_TRAITS(sweeppp_sweep_pass_event_t, SWEEPPP_EVENT_SWEEP_PASS);
SWEEPPP_DECLARE_EVENT_TRAITS(sweeppp_marker_event_t, SWEEPPP_EVENT_MARKER);
SWEEPPP_DECLARE_EVENT_TRAITS(sweeppp_annotation_event_t, SWEEPPP_EVENT_ANNOTATION);
SWEEPPP_DECLARE_EVENT_TRAITS(sweeppp_throttle_changed_event_t, SWEEPPP_EVENT_THROTTLE_CHANGED);
SWEEPPP_DECLARE_EVENT_TRAITS(sweeppp_fields_event_t, SWEEPPP_EVENT_FIELDS);

#undef SWEEPPP_DECLARE_EVENT_TRAITS

/// What a type needs to cross the event channel.
///
/// A concept rather than a comment, so getting it wrong is a message naming
/// the missing piece instead of a page of substitution failures. The
/// `struct_size` requirement is the load-bearing one: it is what lets a
/// payload grow without the subscriber that has not been rebuilt reading past
/// the end of it.
template <typename Event>
concept EventPayload = requires(Event event) {
    { event.struct_size } -> std::same_as<std::uint32_t&>;
    { EventTraits<Event>::name } -> std::convertible_to<std::string_view>;
    { EventTraits<Event>::schemaVersion } -> std::convertible_to<std::uint32_t>;
} && std::is_standard_layout_v<Event> && std::is_trivially_copyable_v<Event>;

// --------------------------------------------------------------- manifest

[[nodiscard]] constexpr sweeppp_author_t author(std::string_view name,
                                                std::string_view email = {}) noexcept {
    return sweeppp_author_t{
        .struct_size = sizeof(sweeppp_author_t), .name = str(name), .email = str(email)};
}

[[nodiscard]] constexpr sweeppp_link_t link(sweeppp_link_type_t type,
                                            std::string_view url) noexcept {
    return sweeppp_link_t{.struct_size = sizeof(sweeppp_link_t), .type = type, .url = str(url)};
}

[[nodiscard]] constexpr sweeppp_dependency_t dependency(std::string_view pluginId,
                                                        std::string_view minVersion = {},
                                                        std::string_view maxVersion = {},
                                                        bool optional = false) noexcept {
    return sweeppp_dependency_t{.struct_size = sizeof(sweeppp_dependency_t),
                                .plugin_id = str(pluginId),
                                .min_version = str(minVersion),
                                .max_version = str(maxVersion),
                                .optional = optional ? 1 : 0};
}

[[nodiscard]] constexpr sweeppp_facet_t facet(sweeppp_facet_kind_t kind, std::string_view id,
                                              std::string_view name, std::string_view description,
                                              const void* vtable) noexcept {
    return sweeppp_facet_t{.struct_size = sizeof(sweeppp_facet_t),
                           .kind = kind,
                           .id = str(id),
                           .name = str(name),
                           .description = str(description),
                           .vtable = vtable};
}

// ---------------------------------------------------------------- logging

/// Logs under a category of its own.
///
/// A driver's messages belong under "hackrf", not under the plugin's
/// reverse-DNS id: the log is read by an operator looking for what their radio
/// did, and "hackrf" is the word they will grep for -- the same one the driver
/// used when it was built in. `Host::as()` makes one.
///
/// Through the host, never through libsweeppp's own `Log`. A plugin linking
/// libsweeppp gets a private copy of the log's sink and level, so `logInfo()`
/// there writes to a sink the application never installed: the message goes to
/// stderr instead of the log panel and the log file, and looks like it vanished.
class Logger {
public:
    Logger() = default;

    Logger(const sweeppp_host_api_t* api, std::string_view category) noexcept
        : m_api(api), m_category(category) {}

    template <typename... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args) const {
        emit(SWEEPPP_LOG_TRACE, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const {
        emit(SWEEPPP_LOG_DEBUG, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const {
        emit(SWEEPPP_LOG_INFO, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const {
        emit(SWEEPPP_LOG_WARN, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const {
        emit(SWEEPPP_LOG_ERROR, std::format(fmt, std::forward<Args>(args)...));
    }

private:
    void emit(sweeppp_log_level_t level, std::string message) const {
        if (m_api != nullptr && m_api->log != nullptr) {
            m_api->log(m_api->host, level, str(m_category), str(message));
        }
    }

    const sweeppp_host_api_t* m_api = nullptr;
    std::string_view m_category;
};

// ------------------------------------------------------------------- host

/// The host, as a plugin sees it.
///
/// A non-owning view of `sweeppp_host_api_t` plus the plugin's own id, which
/// every host call needs and no plugin should have to repeat.
class Host {
public:
    Host() = default;

    Host(const sweeppp_host_api_t* api, std::string_view pluginId) : m_api(api), m_id(pluginId) {}

    [[nodiscard]] bool valid() const noexcept { return m_api != nullptr; }
    [[nodiscard]] const sweeppp_host_api_t* api() const noexcept { return m_api; }
    [[nodiscard]] std::string_view pluginId() const noexcept { return m_id; }

    /// The application's own release, for comparing against a `min_host_version`
    /// at runtime rather than only at load.
    [[nodiscard]] std::string_view hostVersion() const noexcept {
        return m_api != nullptr ? view(m_api->host_version) : std::string_view{};
    }

    // ---- logging ---------------------------------------------------------

    /// A logger writing under `category` rather than under this plugin's id.
    ///
    /// What a device driver wants: its messages should read "hackrf: ..." the
    /// way they did when it was built in, not "org.sweeppp.sdr-hackrf: ...".
    /// Keep the result in a member -- it is two pointers.
    [[nodiscard]] Logger as(std::string_view category) const noexcept {
        return Logger(m_api, category);
    }

    template <typename... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args) const {
        as(m_id).trace(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const {
        as(m_id).debug(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const {
        as(m_id).info(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const {
        as(m_id).warn(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const {
        as(m_id).error(fmt, std::forward<Args>(args)...);
    }

    // ---- paths -----------------------------------------------------------

    [[nodiscard]] std::filesystem::path path(sweeppp_path_kind_t kind) const {
        if (m_api == nullptr || m_api->path == nullptr) {
            return {};
        }
        // Copied immediately: the host's answer is valid only until this
        // thread's next host call.
        return std::filesystem::path(std::string(view(m_api->path(m_api->host, str(m_id), kind))));
    }

    [[nodiscard]] std::filesystem::path settingsPath() const {
        return path(SWEEPPP_PATH_PLUGIN_SETTINGS);
    }

    [[nodiscard]] std::filesystem::path dataDir() const {
        return path(SWEEPPP_PATH_PLUGIN_DATA_DIR);
    }

    [[nodiscard]] std::filesystem::path resourcesDir() const {
        return path(SWEEPPP_PATH_RESOURCES_DIR);
    }

    [[nodiscard]] std::filesystem::path binaryDir() const {
        return path(SWEEPPP_PATH_PLUGIN_BINARY_DIR);
    }

    // ---- facets ----------------------------------------------------------

    /// Registers a facet the manifest declared. Returns the host's status, so
    /// a plugin can carry on with the facets that did register -- losing a
    /// driver-name race is not a reason to withdraw an overlay.
    [[nodiscard]] sweeppp_plugin_status_t registerFacet(const sweeppp_facet_t& declared,
                                                        void* instance) const {
        if (m_api == nullptr) {
            return SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
        }

        switch (declared.kind) {
        case SWEEPPP_FACET_SDR_DEVICE:
            return call(m_api->register_sdr_driver, declared, instance);
        case SWEEPPP_FACET_FFT_BACKEND:
            return call(m_api->register_fft_backend, declared, instance);
        case SWEEPPP_FACET_RF_PATH:
            return call(m_api->register_rf_path, declared, instance);
        case SWEEPPP_FACET_CONTRIBUTOR:
            return call(m_api->register_contributor, declared, instance);
        case SWEEPPP_FACET_UI_EXTENSION:
            return call(m_api->register_ui_extension, declared, instance);
        case SWEEPPP_FACET_FRAME_PROCESSOR:
            if (m_api->frame_subscribe == nullptr) {
                return SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
            }
            return m_api->frame_subscribe(m_api->host, str(m_id), &declared, instance) != 0
                       ? SWEEPPP_PLUGIN_OK
                       : SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
        case SWEEPPP_FACET_KIND_FORCE_INT32:
            break;
        }
        return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
    }

    /// Says why a declared facet is not being offered, so the listing carries
    /// the reason instead of an unexplained "inactive".
    void reportFacet(const sweeppp_facet_t& declared, std::string_view reason) const {
        if (m_api != nullptr && m_api->report_facet != nullptr) {
            m_api->report_facet(m_api->host, str(m_id), declared.kind, declared.id, str(reason));
        }
    }

    // ---- the running configuration ---------------------------------------
    //
    // Flat dotted keys over typed values -- "sweep.start", "display.grid" --
    // which is the shape a session manifest has and the shape the profile is
    // written to disk in. Read-only: a plugin that wants something changed
    // asks on the event channel, where the request is addressed and refusable.

    [[nodiscard]] std::vector<std::string> profileKeys() const {
        if (m_api == nullptr || m_api->profile_keys == nullptr) {
            return {};
        }

        std::vector<sweeppp_str_t> raw(m_api->profile_keys(m_api->host, nullptr, 0));
        if (raw.empty()) {
            return {};
        }
        raw.resize(
            m_api->profile_keys(m_api->host, raw.data(), static_cast<std::uint32_t>(raw.size())));

        std::vector<std::string> keys;
        keys.reserve(raw.size());
        for (const sweeppp_str_t& key : raw) {
            keys.emplace_back(view(key));
        }
        return keys;
    }

    [[nodiscard]] bool profileBool(std::string_view key, bool fallback) const {
        const sweeppp_value_t value = profileValue(key);
        return value.type == SWEEPPP_VALUE_BOOL ? value.boolean != 0 : fallback;
    }

    [[nodiscard]] std::int64_t profileInt(std::string_view key, std::int64_t fallback) const {
        const sweeppp_value_t value = profileValue(key);
        return value.type == SWEEPPP_VALUE_INT ? value.integer : fallback;
    }

    [[nodiscard]] double profileDouble(std::string_view key, double fallback) const {
        const sweeppp_value_t value = profileValue(key);
        if (value.type == SWEEPPP_VALUE_FLOAT) {
            return value.number;
        }
        // An integer where a float was expected is the usual shape of a
        // hand-written config: `overlap = 0` is an int in TOML and a double
        // everywhere it is used.
        return value.type == SWEEPPP_VALUE_INT ? static_cast<double>(value.integer) : fallback;
    }

    [[nodiscard]] std::string profileString(std::string_view key,
                                            std::string_view fallback = {}) const {
        const sweeppp_value_t value = profileValue(key);
        return value.type == SWEEPPP_VALUE_STRING ? std::string(view(value.text))
                                                  : std::string(fallback);
    }

    // ---- the session -----------------------------------------------------

    /// Whether anything written below will be kept. False before Start, which
    /// is a normal state rather than a failure.
    [[nodiscard]] bool sessionOpen() const {
        return m_api != nullptr && m_api->session_open != nullptr &&
               m_api->session_open(m_api->host) != 0;
    }

    /// An opaque record under this plugin's id, in the same `.sweeps` file as
    /// the sweep it describes. Copied before this returns.
    [[nodiscard]] sweeppp_plugin_status_t writeSessionRecord(std::string_view recordName,
                                                             std::uint32_t schemaVersion,
                                                             const void* body,
                                                             std::size_t bodySize) const {
        if (m_api == nullptr || m_api->session_write_record == nullptr) {
            return SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
        }
        return m_api->session_write_record(m_api->host, str(m_id), str(recordName), schemaVersion,
                                           body, bodySize);
    }

    /// A plugin event on the session's own time axis, so a viewer can put it
    /// on the timeline beside retunes and markers.
    [[nodiscard]] sweeppp_plugin_status_t
    writeSessionEvent(std::string_view eventName,
                      std::span<const sweeppp_field_t> fields = {}) const {
        if (m_api == nullptr || m_api->session_write_event == nullptr) {
            return SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
        }
        return m_api->session_write_event(m_api->host, str(m_id), str(eventName), fields.data(),
                                          static_cast<std::uint32_t>(fields.size()));
    }

    // ---- events ----------------------------------------------------------
    //
    // Typed on both sides, in the shape libsweeppp's own `EventBus` has:
    // `subscribe<RetuneEvent>` and `publish(event)`, with the type name and
    // the payload struct travelling together so a subscriber never casts on
    // faith.
    //
    // An event type is any struct with `struct_size` first and a
    //
    //     static constexpr std::string_view kEventType = "my.plugin.thing";
    //     static constexpr std::uint32_t kSchemaVersion = 1;
    //
    // which is how a plugin declares one of its own for other plugins to
    // listen to. The host's types are declared with the same shape below.

    /// Publishes one of this plugin's own events.
    ///
    /// The type name comes from the type, not from the call site. The host
    /// refuses a name that does not begin with this plugin's id, so a plugin
    /// cannot publish `sweeppp.retune`.
    template <EventPayload Event>
    sweeppp_plugin_status_t publish(Event payload) const {
        if (m_api == nullptr || m_api->publish_event == nullptr) {
            return SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
        }

        payload.struct_size = sizeof(Event);

        sweeppp_event_t event{};
        event.struct_size = sizeof(sweeppp_event_t);
        event.type = str(EventTraits<Event>::name);
        event.schema_version = EventTraits<Event>::schemaVersion;
        event.payload = &payload;
        event.payload_size = sizeof(Event);

        return m_api->publish_event(m_api->host, str(m_id), &event);
    }

    /// Listens for one event type, named by the type.
    ///
    /// `handler` is a plain function pointer taking `(void* user, const
    /// Event&)`, not a `std::function`: the callback crosses back into the
    /// host, and nothing that owns memory may cross this boundary in either
    /// direction. The payload's size is checked before the cast, so a
    /// publisher built against a smaller version of the struct is dropped
    /// rather than read past the end of.
    template <EventPayload Event>
    [[nodiscard]] std::uint64_t subscribe(void (*handler)(void* user, const Event& event),
                                          void* user = nullptr) const {
        if (m_api == nullptr || m_api->subscribe_event == nullptr || handler == nullptr) {
            return 0;
        }

        // The handler and the user pointer travel as one, so the trampoline
        // stays a non-capturing lambda and therefore a function pointer.
        struct Binding {
            void (*handler)(void*, const Event&);
            void* user;
        };
        // Never freed, and bounded by the number of subscriptions a plugin
        // makes: it has to outlive every delivery, and the host has no way to
        // say when the last one has happened.
        auto* binding = new Binding{handler, user};

        return m_api->subscribe_event(
            m_api->host, str(EventTraits<Event>::name),
            [](void* bound, const sweeppp_event_t* event) {
                const auto* target = static_cast<const Binding*>(bound);
                if (event == nullptr || event->payload == nullptr ||
                    event->payload_size < sizeof(Event)) {
                    return;
                }
                // A schema version says the layout changed in a way growing it
                // cannot express, so a mismatch is dropped rather than
                // reinterpreted.
                if (event->schema_version != EventTraits<Event>::schemaVersion) {
                    return;
                }

                const auto* payload = static_cast<const Event*>(event->payload);
                if (payload->struct_size < sizeof(Event)) {
                    // Published by something built against a smaller version
                    // of this struct. Reading it would read past the end.
                    return;
                }
                detail::guard([&] { target->handler(target->user, *payload); });
            },
            binding);
    }

    /// Listens for every event, for a plugin that logs or forwards them all.
    /// The only place a raw `sweeppp_event_t` is handled, because "everything"
    /// has no single payload type.
    [[nodiscard]] std::uint64_t subscribeAll(sweeppp_event_fn_t callback,
                                             void* user = nullptr) const {
        if (m_api == nullptr || m_api->subscribe_event == nullptr) {
            return 0;
        }
        return m_api->subscribe_event(m_api->host, str({}), callback, user);
    }

    void unsubscribe(std::uint64_t subscription) const {
        if (m_api != nullptr && m_api->unsubscribe_event != nullptr) {
            m_api->unsubscribe_event(m_api->host, subscription);
        }
    }

    // ---- the host's chrome -------------------------------------------------

    /// Whether the spectrum is painting contributions of this type right now.
    ///
    /// Read per frame, not cached: the operator's B and C keys and the Display
    /// ticks flip the same switch, and a button holding its own copy would sit
    /// lit over a plot with nothing on it.
    [[nodiscard]] bool contributionsShown(sweeppp_contribution_type_t type) const noexcept {
        return m_api != nullptr && m_api->contributions_shown != nullptr &&
               m_api->contributions_shown(m_api->host, type) != 0;
    }

    void setContributionsShown(sweeppp_contribution_type_t type, bool shown) const noexcept {
        if (m_api != nullptr && m_api->set_contributions_shown != nullptr) {
            m_api->set_contributions_shown(m_api->host, type, shown ? 1 : 0);
        }
    }

    /// Whether this host can ask the operator where to put a file.
    ///
    /// False in a headless build and in a host older than this call, which is
    /// a plugin's cue to write somewhere of its own rather than to refuse.
    [[nodiscard]] bool canChooseFile() const noexcept {
        // The `struct_size` rule, spelled out: the macro that does this for
        // the host lives in a private header a plugin cannot include, and a
        // host older than this member never wrote it -- so reading the pointer
        // without checking would be reading past the end of its struct.
        constexpr std::size_t needed =
            offsetof(sweeppp_host_api_t, save_file_dialog) + sizeof(m_api->save_file_dialog);
        return m_api != nullptr && m_api->struct_size >= needed &&
               m_api->save_file_dialog != nullptr;
    }

    /// Asks the operator where to save something, through the host's own
    /// native dialog. Empty when they cancelled, or when there is no dialog.
    ///
    /// `extension` is bare: "csv", not "*.csv". Blocks until answered, so call
    /// it from the UI thread and never from a worker.
    [[nodiscard]] std::filesystem::path chooseFile(std::string_view title,
                                                   std::string_view suggestedName,
                                                   std::string_view extension) const {
        if (!canChooseFile()) {
            return {};
        }
        // Copied immediately: the host's answer is valid only until this
        // thread's next host call.
        return std::filesystem::path(std::string(view(
            m_api->save_file_dialog(m_api->host, str(title), str(suggestedName), str(extension)))));
    }

    /// The glyph when the host's icon font is loaded, the words when it is not.
    /// The plugin owns the glyph -- the host has no idea what its button means.
    [[nodiscard]] std::string icon(const char* glyph, const char* fallback) const {
        const bool available = m_api != nullptr && m_api->icons_available != nullptr &&
                               m_api->icons_available(m_api->host) != 0;
        return available ? std::string(glyph) : std::string(fallback);
    }

    // ---- ImGui -----------------------------------------------------------

    [[nodiscard]] const sweeppp_imgui_binding_t* imgui() const noexcept {
        return m_api != nullptr ? m_api->imgui : nullptr;
    }

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    /// Adopts the host's ImGui, and refuses if this plugin's copy does not
    /// agree with it byte for byte.
    ///
    /// Allocators first, then the context: both libraries allocate on first
    /// use, and memory taken from one copy's allocator and returned to the
    /// other's is a heap corruption that surfaces days later somewhere else
    /// entirely.
    ///
    /// The layout check is not a formality. `sweeppp_imconfig.h` makes
    /// `ImDrawIdx` and `ImWchar` 32-bit; a plugin compiled without it has a
    /// different `ImDrawVert` and writes malformed vertices into a draw list
    /// the host will later hand to the GPU. That failure has no exception, no
    /// assertion and no obvious symptom -- which is why the sizes are in the
    /// ABI at all.
    ///
    /// `reason` receives what went wrong, so the plugin can hand it to the
    /// host as a facet failure rather than a silent absence.
    [[nodiscard]] bool adoptImGui(std::string& reason) const {
        const sweeppp_imgui_binding_t* binding = imgui();
        if (binding == nullptr) {
            reason = "this build of Sweep++ has no user interface";
            return false;
        }

        const auto mismatch = [&reason](std::string_view field, std::size_t theirs,
                                        std::size_t ours) {
            reason =
                std::format("ImGui {} is {} bytes in the host and {} here", field, theirs, ours);
            return false;
        };

        // Checked here as well as by ImGui's own routine below, because
        // ImGui's reports through IM_ASSERT -- which aborts in a build with
        // assertions on. A plugin with the wrong ImGui should be listed with
        // the reason, not take the application down with it.
        if (binding->size_of_io != sizeof(ImGuiIO)) {
            return mismatch("ImGuiIO", binding->size_of_io, sizeof(ImGuiIO));
        }
        if (binding->size_of_style != sizeof(ImGuiStyle)) {
            return mismatch("ImGuiStyle", binding->size_of_style, sizeof(ImGuiStyle));
        }
        if (binding->size_of_vec2 != sizeof(ImVec2)) {
            return mismatch("ImVec2", binding->size_of_vec2, sizeof(ImVec2));
        }
        if (binding->size_of_vec4 != sizeof(ImVec4)) {
            return mismatch("ImVec4", binding->size_of_vec4, sizeof(ImVec4));
        }
        if (binding->size_of_draw_vert != sizeof(ImDrawVert)) {
            return mismatch("ImDrawVert", binding->size_of_draw_vert, sizeof(ImDrawVert));
        }
        if (binding->size_of_draw_idx != sizeof(ImDrawIdx)) {
            return mismatch("ImDrawIdx", binding->size_of_draw_idx, sizeof(ImDrawIdx));
        }

        const std::string hostImGui(view(binding->imgui_version));
        if (hostImGui != IMGUI_VERSION) {
            reason =
                std::format("built against ImGui {}, the host has {}", IMGUI_VERSION, hostImGui);
            return false;
        }

        const std::string hostImPlot(view(binding->implot_version));
        if (!hostImPlot.empty() && hostImPlot != IMPLOT_VERSION) {
            // ImPlot has no data-layout check of its own, so its version
            // string is all there is to compare.
            reason =
                std::format("built against ImPlot {}, the host has {}", IMPLOT_VERSION, hostImPlot);
            return false;
        }

        ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(binding->alloc_fn),
                                     reinterpret_cast<ImGuiMemFreeFunc>(binding->free_fn),
                                     binding->allocator_user);
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(binding->imgui_context));

        if (!ImGui::DebugCheckVersionAndDataLayout(
                hostImGui.c_str(), binding->size_of_io, binding->size_of_style,
                binding->size_of_vec2, binding->size_of_vec4, binding->size_of_draw_vert,
                binding->size_of_draw_idx)) {
            reason = "ImGui refused this plugin's data layout";
            return false;
        }

        if (binding->implot_context != nullptr) {
            ImPlot::SetImGuiContext(ImGui::GetCurrentContext());
            ImPlot::SetCurrentContext(static_cast<ImPlotContext*>(binding->implot_context));
        }
        return true;
    }
#endif

private:
    [[nodiscard]] sweeppp_value_t profileValue(std::string_view key) const {
        sweeppp_value_t value{};
        value.struct_size = sizeof(value);
        value.type = SWEEPPP_VALUE_ABSENT;

        if (m_api != nullptr && m_api->profile_get != nullptr) {
            m_api->profile_get(m_api->host, str(key), &value);
        }
        return value;
    }

    template <typename Fn>
    [[nodiscard]] sweeppp_plugin_status_t call(Fn function, const sweeppp_facet_t& declared,
                                               void* instance) const {
        if (function == nullptr) {
            return SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
        }
        return function(m_api->host, str(m_id), &declared, instance);
    }

    const sweeppp_host_api_t* m_api = nullptr;
    std::string_view m_id;
};

// -------------------------------------------------------------- streaming

/// One block borrowed from the host's pool.
///
/// Move-only, and the destructor gives it back. The same discipline the host's
/// own `BlockRef` has, for the same reason: a block leaked on an error path
/// shrinks the pool permanently, and the symptom appears much later as
/// unexplained drops with nothing pointing at the cause.
///
/// An invalid Block is what an exhausted pool answers with, and that is a
/// normal answer -- see `Stream::acquire`.
class Block {
public:
    Block() = default;

    Block(const sweeppp_sdr_stream_t* stream, sweeppp_sdr_block_t raw) noexcept
        : m_stream(stream), m_raw(raw) {}

    ~Block() { release(); }

    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;

    Block(Block&& other) noexcept : m_stream(other.m_stream), m_raw(other.m_raw) {
        other.m_raw = sweeppp_sdr_block_t{};
    }

    Block& operator=(Block&& other) noexcept {
        if (this != &other) {
            release();
            m_stream = other.m_stream;
            m_raw = other.m_raw;
            other.m_raw = sweeppp_sdr_block_t{};
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return m_raw.handle != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] std::uint8_t* data() const noexcept { return m_raw.data; }
    [[nodiscard]] std::size_t capacityBytes() const noexcept { return m_raw.capacity_bytes; }

    /// The block's storage as the sample type actually being written into it.
    /// The pool guarantees cache-line alignment, which exceeds the alignment
    /// of every sample type, so this is always well-founded.
    template <typename Sample>
    [[nodiscard]] Sample* as() const noexcept {
        return reinterpret_cast<Sample*>(m_raw.data);
    }

    /// Hands it back unfilled. Also what the destructor does.
    void release() noexcept {
        if (m_raw.handle != nullptr && m_stream != nullptr && m_stream->release != nullptr) {
            m_stream->release(m_stream->stream, m_raw.handle);
        }
        m_raw = sweeppp_sdr_block_t{};
    }

private:
    friend class Stream;

    /// Relinquishes ownership to a publish, which consumes the handle.
    [[nodiscard]] void* take() noexcept {
        void* handle = m_raw.handle;
        m_raw = sweeppp_sdr_block_t{};
        return handle;
    }

    const sweeppp_sdr_stream_t* m_stream = nullptr;
    sweeppp_sdr_block_t m_raw{};
};

/// The host's pool and telemetry, as a driver sees them.
///
/// Nothing here blocks, allocates or takes a contended lock, which is what
/// makes it callable from inside a libusb callback -- which is where a USB
/// radio's driver publishes from.
class Stream {
public:
    Stream() = default;
    explicit Stream(const sweeppp_sdr_stream_t* abi) noexcept : m_abi(abi) {}

    [[nodiscard]] bool valid() const noexcept {
        return m_abi != nullptr && m_abi->acquire != nullptr && m_abi->publish != nullptr;
    }

    /// Borrows a block, or answers with an invalid one when the pool is empty.
    ///
    /// An empty pool is an ANSWER, not a failure, and the only correct
    /// response is to drop the data and call `reportPoolExhausted`. Waiting
    /// for a block on a transfer thread turns a host-side backlog into a
    /// device-side overrun, which is both worse and misleading.
    [[nodiscard]] Block acquire() const noexcept {
        if (!valid()) {
            return {};
        }
        return Block(m_abi, m_abi->acquire(m_abi->stream));
    }

    /// Hands a filled block back. `delivery.handle` is filled in from `block`,
    /// so a caller cannot publish one block's data under another's handle.
    ///
    /// Serialised by the driver: deliveries must not overlap, and their
    /// sequence numbers must ascend.
    void publish(Block&& block, sweeppp_sdr_delivery_t delivery) const noexcept {
        if (!valid() || !block.valid()) {
            return;
        }
        delivery.struct_size = sizeof(sweeppp_sdr_delivery_t);
        delivery.handle = block.take();
        m_abi->publish(m_abi->stream, &delivery);
    }

    /// Samples the DEVICE lost -- its own FIFO overran.
    void reportDeviceOverrun(std::uint64_t samples) const noexcept {
        if (m_abi != nullptr && m_abi->report_device_overrun != nullptr) {
            m_abi->report_device_overrun(m_abi->stream, samples);
        }
    }

    /// Samples dropped because `acquire` came back empty.
    void reportPoolExhausted(std::uint64_t samples) const noexcept {
        if (m_abi != nullptr && m_abi->report_pool_exhausted != nullptr) {
            m_abi->report_pool_exhausted(m_abi->stream, samples);
        }
    }

private:
    const sweeppp_sdr_stream_t* m_abi = nullptr;
};

// ---------------------------------------------------------------- drawing

/// A plot's geometry, as an overlay sees it.
///
/// The arithmetic is reimplemented rather than called across the ABI: it is
/// four lines, and an indirect call per point on a trace that can be a million
/// points wide is not a trade worth making. The definitions are in
/// `PluginAbi.h` so the two cannot drift.
class PlotContext {
public:
    explicit PlotContext(const sweeppp_plot_context_t& raw) noexcept : m_raw(raw) {}

    [[nodiscard]] const sweeppp_plot_context_t& raw() const noexcept { return m_raw; }
    [[nodiscard]] sweeppp_ui_spot_t spot() const noexcept { return m_raw.spot; }
    [[nodiscard]] sweeppp_ui_layer_t layer() const noexcept { return m_raw.layer; }

    [[nodiscard]] double fromHz() const noexcept { return m_raw.from_hz; }
    [[nodiscard]] double toHz() const noexcept { return m_raw.to_hz; }
    [[nodiscard]] double markerHz() const noexcept { return m_raw.marker_hz; }
    [[nodiscard]] float overlayAlpha() const noexcept { return m_raw.overlay_alpha; }
    [[nodiscard]] float minDb() const noexcept { return m_raw.min_db; }
    [[nodiscard]] float maxDb() const noexcept { return m_raw.max_db; }

    [[nodiscard]] float left() const noexcept { return m_raw.origin_x; }
    [[nodiscard]] float top() const noexcept { return m_raw.origin_y; }
    [[nodiscard]] float width() const noexcept { return m_raw.size_x; }
    [[nodiscard]] float height() const noexcept { return m_raw.size_y; }
    [[nodiscard]] float right() const noexcept { return m_raw.origin_x + m_raw.size_x; }
    [[nodiscard]] float bottom() const noexcept { return m_raw.origin_y + m_raw.size_y; }

    [[nodiscard]] float xForHz(double hz) const noexcept {
        const double span = m_raw.to_hz - m_raw.from_hz;
        const double t = span > 0.0 ? (hz - m_raw.from_hz) / span : 0.0;
        return m_raw.origin_x + (static_cast<float>(t) * m_raw.size_x);
    }

    [[nodiscard]] double hzForX(float x) const noexcept {
        const float t = m_raw.size_x > 0.0F ? (x - m_raw.origin_x) / m_raw.size_x : 0.0F;
        return m_raw.from_hz + (static_cast<double>(t) * (m_raw.to_hz - m_raw.from_hz));
    }

    [[nodiscard]] float yForDb(float db) const noexcept {
        const float span = m_raw.max_db - m_raw.min_db;
        const float t = span > 0.0F ? (db - m_raw.min_db) / span : 0.0F;
        return m_raw.origin_y + ((1.0F - t) * m_raw.size_y);
    }

    [[nodiscard]] float dbForY(float y) const noexcept {
        const float t = m_raw.size_y > 0.0F ? (y - m_raw.origin_y) / m_raw.size_y : 0.0F;
        return m_raw.max_db - (t * (m_raw.max_db - m_raw.min_db));
    }

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    /// Already clipped to the plot rectangle by the host. Do not call
    /// `ImPlot::BeginPlot` inside an overlay -- this list *is* the plot.
    [[nodiscard]] ImDrawList* drawList() const noexcept {
        return static_cast<ImDrawList*>(m_raw.draw_list);
    }
#endif

private:
    sweeppp_plot_context_t m_raw;
};

#if defined(SWEEPPP_PLUGIN_HAS_UI)

/// Builds a UI vtable that forwards to `T`'s member functions.
///
/// `T` supplies whichever of these it wants, and only those:
///
///     void drawOverlay(const PlotContext&)
///     bool drawSettings()          // false means "nothing to show"
///     void drawStatusChip()
///     void drawStatusAction()      // a button in the status bar
///     void drawToolbar()
///     void drawWindow()
///     void openPanel()             // the keyboard asked for the toolbar panel
///
/// A slot `T` does not define is left null rather than made a compile error,
/// so a plugin that wants one spot writes one function instead of four empty
/// ones -- and gains nothing to write when a spot is added here later.
///
/// `governs` names the contribution type this facet's toolbar button switches,
/// which is what the host routes `openPanel` by. Leave it alone for a facet
/// with no button, or one whose button is about something else.
///
/// Store the result in something with static storage duration: the host keeps
/// the pointer.
template <typename T>
[[nodiscard]] sweeppp_ui_vtable_t
makeUiVtable(std::uint32_t spots, sweeppp_ui_layer_t layer = SWEEPPP_UI_LAYER_UNDER,
             sweeppp_contribution_type_t governs = static_cast<sweeppp_contribution_type_t>(0)) {
    sweeppp_ui_vtable_t vtable{};
    vtable.struct_size = sizeof(sweeppp_ui_vtable_t);
    vtable.spots = spots;
    vtable.layer = layer;
    vtable.toolbar_governs = governs;

    // PushID on the instance pointer around every call. Two plugins with a
    // "Reset" button would otherwise share one ImGui id and fight over which
    // of them was clicked.
    if constexpr (requires(T& t, const PlotContext& c) { t.drawOverlay(c); }) {
        vtable.draw_overlay = [](void* instance, const sweeppp_plot_context_t* context) {
            detail::guard([&] {
                ImGui::PushID(instance);
                static_cast<T*>(instance)->drawOverlay(PlotContext{*context});
                ImGui::PopID();
            });
        };
    }

    if constexpr (requires(T& t) {
                      { t.drawSettings() } -> std::convertible_to<bool>;
                  }) {
        vtable.draw_settings = [](void* instance) -> std::int32_t {
            std::int32_t drew = 0;
            detail::guard([&] {
                ImGui::PushID(instance);
                drew = static_cast<T*>(instance)->drawSettings() ? 1 : 0;
                ImGui::PopID();
            });
            return drew;
        };
    }

    if constexpr (requires(T& t) { t.drawStatusChip(); }) {
        vtable.draw_status_chip = [](void* instance) {
            detail::guard([&] {
                ImGui::PushID(instance);
                static_cast<T*>(instance)->drawStatusChip();
                ImGui::PopID();
            });
        };
    }

    if constexpr (requires(T& t) { t.drawToolbar(); }) {
        vtable.draw_toolbar = [](void* instance) {
            detail::guard([&] {
                ImGui::PushID(instance);
                static_cast<T*>(instance)->drawToolbar();
                ImGui::PopID();
            });
        };
    }

    if constexpr (requires(T& t) { t.drawWindow(); }) {
        vtable.draw_window = [](void* instance) {
            detail::guard([&] {
                ImGui::PushID(instance);
                static_cast<T*>(instance)->drawWindow();
                ImGui::PopID();
            });
        };
    }

    if constexpr (requires(T& t) { t.drawStatusAction(); }) {
        vtable.draw_status_action = [](void* instance) {
            detail::guard([&] {
                ImGui::PushID(instance);
                static_cast<T*>(instance)->drawStatusAction();
                ImGui::PopID();
            });
        };
    }

    // No PushID here, and that is the point: this one submits nothing. It runs
    // outside the toolbar's window, where an id would be hashed against the
    // wrong stack -- all it may do is record that the panel was asked for.
    if constexpr (requires(T& t) { t.openPanel(); }) {
        vtable.open_panel = [](void* instance) {
            detail::guard([&] { static_cast<T*>(instance)->openPanel(); });
        };
    }

    return vtable;
}

// ------------------------------------------------------------- tick trees

/// What the operator asked for by clicking a row's tick.
///
/// Three answers rather than two, because a row standing over other rows has a
/// third state and a plain toggle cannot express what a click on it means:
///
///   - something under it is on   -> hide this branch, and leave every tick
///                                   inside it alone, so bringing it back
///                                   brings back the selection rather than
///                                   everything;
///   - nothing under it is on, and this row's own tick is off
///                                -> unhide, and the inner ticks decide what
///                                   actually appears;
///   - nothing under it is on and this row's own tick is already on
///                                -> the branch is on but empty, so the only
///                                   thing the click can mean is "show me all
///                                   of it".
enum class TickAction : std::uint8_t { Hide, Unhide, ShowAll };

/// The tri-state tick every row of a plugin's settings tree carries.
///
/// Here rather than in each plugin because two plugins drawing the same
/// control with the same three-way meaning must not be able to disagree about
/// it -- and they did not disagree by accident, they disagreed by being
/// written twice.
///
/// `anyOn` decides whether it reads as ticked, `allOn` whether it reads as a
/// dash, and `ownOn` is the row's own flag, which is what tells "off" apart
/// from "on but everything inside it is off".
[[nodiscard]] inline std::optional<TickAction> drawTick(bool anyOn, bool allOn, bool ownOn) {
    const bool mixed = anyOn && !allOn;
    if (mixed) {
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    }
    bool ticked = anyOn;
    const bool clicked = ImGui::Checkbox("##on", &ticked);
    if (mixed) {
        ImGui::PopItemFlag();
    }

    if (!clicked) {
        return std::nullopt;
    }
    if (anyOn) {
        return TickAction::Hide;
    }
    return ownOn ? TickAction::ShowAll : TickAction::Unhide;
}

#endif

// ----------------------------------------------------------- contributions

/// One contribution, filled in. Beside `str` and `facet` so that no
/// contributor repeats the field-by-field assignment.
[[nodiscard]] constexpr sweeppp_contribution_t
contribution(sweeppp_contribution_type_t type, std::string_view name, std::string_view category,
             double startHz, double stopHz, const float (&color)[4],
             std::string_view description = {}) noexcept {
    return sweeppp_contribution_t{.struct_size = sizeof(sweeppp_contribution_t),
                                  .type = type,
                                  .name = str(name),
                                  .description = str(description),
                                  .category = str(category),
                                  .start_hz = startHz,
                                  .stop_hz = stopHz,
                                  .color = {color[0], color[1], color[2], color[3]}};
}

/// Builds a contributor vtable that forwards to `T`'s member functions:
///
///     std::uint32_t datasetCount() const
///     std::string_view datasetName(std::uint32_t) const
///     std::string_view datasetDescription(std::uint32_t) const
///     std::uint32_t activeDataset() const
///     bool selectDataset(std::uint32_t)
///     std::uint32_t contributionsIn(double fromHz, double toHz,
///                                   sweeppp_contribution_t* out,
///                                   std::uint32_t capacity) const
///     std::uint32_t contributionsAt(double hz, sweeppp_contribution_t* out,
///                                   std::uint32_t capacity) const
///
/// `render` defaults to HOST, which is the point of the facet: a contributor
/// that says nothing about drawing gets spans, labels and insets it did not
/// have to write.
template <typename T>
[[nodiscard]] sweeppp_contributor_vtable_t
makeContributorVtable(sweeppp_contribution_render_t render = SWEEPPP_CONTRIBUTION_RENDER_HOST) {
    sweeppp_contributor_vtable_t vtable{};
    vtable.struct_size = sizeof(sweeppp_contributor_vtable_t);
    vtable.render = render;

    vtable.dataset_count = [](void* instance) -> std::uint32_t {
        std::uint32_t count = 0;
        detail::guard([&] { count = static_cast<const T*>(instance)->datasetCount(); });
        return count;
    };

    vtable.dataset_name = [](void* instance, std::uint32_t index) -> sweeppp_str_t {
        sweeppp_str_t name = str({});
        detail::guard([&] { name = str(static_cast<const T*>(instance)->datasetName(index)); });
        return name;
    };

    vtable.dataset_description = [](void* instance, std::uint32_t index) -> sweeppp_str_t {
        sweeppp_str_t text = str({});
        detail::guard(
            [&] { text = str(static_cast<const T*>(instance)->datasetDescription(index)); });
        return text;
    };

    vtable.active_dataset = [](void* instance) -> std::uint32_t {
        std::uint32_t index = 0;
        detail::guard([&] { index = static_cast<const T*>(instance)->activeDataset(); });
        return index;
    };

    vtable.select_dataset = [](void* instance, std::uint32_t index) -> sweeppp_plugin_status_t {
        bool selected = false;
        detail::guard([&] { selected = static_cast<T*>(instance)->selectDataset(index); });
        return selected ? SWEEPPP_PLUGIN_OK : SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
    };

    vtable.contributions_in = [](void* instance, double fromHz, double toHz,
                                 sweeppp_contribution_t* out,
                                 std::uint32_t capacity) -> std::uint32_t {
        std::uint32_t count = 0;
        detail::guard([&] {
            count = static_cast<const T*>(instance)->contributionsIn(fromHz, toHz, out, capacity);
        });
        return count;
    };

    vtable.contributions_at = [](void* instance, double hz, sweeppp_contribution_t* out,
                                 std::uint32_t capacity) -> std::uint32_t {
        std::uint32_t count = 0;
        detail::guard(
            [&] { count = static_cast<const T*>(instance)->contributionsAt(hz, out, capacity); });
        return count;
    };

    // Optional, and wired only when `T` has it:
    //
    //     bool hide(const sweeppp_contribution_t&)
    //
    // A contributor whose entries are not individually dismissable says so by
    // not defining it, rather than by defining one that returns false.
    if constexpr (requires(T& t, const sweeppp_contribution_t& c) {
                      { t.hide(c) } -> std::convertible_to<bool>;
                  }) {
        vtable.hide = [](void* instance,
                         const sweeppp_contribution_t* which) -> sweeppp_plugin_status_t {
            bool hidden = false;
            detail::guard([&] {
                if (which != nullptr) {
                    hidden = static_cast<T*>(instance)->hide(*which);
                }
            });
            return hidden ? SWEEPPP_PLUGIN_OK : SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        };
    }

    return vtable;
}

// ----------------------------------------------------------------- plugin

/// A profile being saved, as a plugin writes into it.
class ProfileWriter {
public:
    explicit ProfileWriter(const sweeppp_profile_writer_t* writer) noexcept : m_writer(writer) {}

    void set(std::string_view key, bool value) const {
        sweeppp_value_t held{};
        held.struct_size = sizeof(held);
        held.type = SWEEPPP_VALUE_BOOL;
        held.boolean = value ? 1 : 0;
        write(key, held);
    }

    void set(std::string_view key, std::int64_t value) const {
        sweeppp_value_t held{};
        held.struct_size = sizeof(held);
        held.type = SWEEPPP_VALUE_INT;
        held.integer = value;
        write(key, held);
    }

    void set(std::string_view key, double value) const {
        sweeppp_value_t held{};
        held.struct_size = sizeof(held);
        held.type = SWEEPPP_VALUE_FLOAT;
        held.number = value;
        write(key, held);
    }

    void set(std::string_view key, std::string_view value) const {
        sweeppp_value_t held{};
        held.struct_size = sizeof(held);
        held.type = SWEEPPP_VALUE_STRING;
        held.text = str(value);
        write(key, held);
    }

private:
    void write(std::string_view key, const sweeppp_value_t& value) const {
        if (m_writer != nullptr && m_writer->set != nullptr) {
            m_writer->set(m_writer->sink, str(key), &value);
        }
    }

    const sweeppp_profile_writer_t* m_writer = nullptr;
};

/// A profile being applied, as a plugin reads it back.
class ProfileReader {
public:
    explicit ProfileReader(const sweeppp_profile_reader_t* reader) noexcept : m_reader(reader) {}

    [[nodiscard]] bool getBool(std::string_view key, bool fallback) const {
        const sweeppp_value_t value = read(key);
        return value.type == SWEEPPP_VALUE_BOOL ? value.boolean != 0 : fallback;
    }

    [[nodiscard]] std::int64_t getInt(std::string_view key, std::int64_t fallback) const {
        const sweeppp_value_t value = read(key);
        return value.type == SWEEPPP_VALUE_INT ? value.integer : fallback;
    }

    [[nodiscard]] double getDouble(std::string_view key, double fallback) const {
        const sweeppp_value_t value = read(key);
        if (value.type == SWEEPPP_VALUE_FLOAT) {
            return value.number;
        }
        return value.type == SWEEPPP_VALUE_INT ? static_cast<double>(value.integer) : fallback;
    }

    [[nodiscard]] std::string getString(std::string_view key,
                                        std::string_view fallback = {}) const {
        const sweeppp_value_t value = read(key);
        return value.type == SWEEPPP_VALUE_STRING ? std::string(view(value.text))
                                                  : std::string(fallback);
    }

    /// Every key this profile carries for this plugin, so one that stores a
    /// variable set -- a list of named regions, say -- can find them.
    [[nodiscard]] std::vector<std::string> keys() const {
        if (m_reader == nullptr || m_reader->keys == nullptr) {
            return {};
        }

        std::vector<sweeppp_str_t> raw(m_reader->keys(m_reader->source, nullptr, 0));
        if (raw.empty()) {
            return {};
        }
        raw.resize(
            m_reader->keys(m_reader->source, raw.data(), static_cast<std::uint32_t>(raw.size())));

        std::vector<std::string> names;
        names.reserve(raw.size());
        for (const sweeppp_str_t& key : raw) {
            names.emplace_back(view(key));
        }
        return names;
    }

private:
    [[nodiscard]] sweeppp_value_t read(std::string_view key) const {
        sweeppp_value_t value{};
        value.struct_size = sizeof(value);
        value.type = SWEEPPP_VALUE_ABSENT;

        if (m_reader != nullptr && m_reader->get != nullptr) {
            m_reader->get(m_reader->source, str(key), &value);
        }
        return value;
    }

    const sweeppp_profile_reader_t* m_reader = nullptr;
};

/// What a plugin overrides.
class Plugin {
public:
    virtual ~Plugin() = default;

    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    /// Register facets here. False means the plugin could not come up at all,
    /// and is listed as failed; a facet that individually could not register
    /// is not a reason to return false.
    [[nodiscard]] virtual bool activate(Host& host) = 0;

    /// Stop everything of your own -- threads, timers, files. The host has
    /// already withdrawn every facet by the time this runs, and it will not
    /// call back into you afterwards.
    virtual void deactivate() {}

    /// Put whatever belongs in the profile being saved.
    ///
    /// A profile is a setup an operator names and comes back to; the settings
    /// file is how this installation is configured. State that should follow
    /// "2.4 GHz survey" around goes here, and the plugin's default stays in
    /// its settings file.
    virtual void saveProfile(const ProfileWriter& writer) { (void)writer; }

    /// Read it back. Not called at all for a profile that carries none of this
    /// plugin's keys, so a plugin added to an existing setup keeps its own
    /// defaults rather than being reset by a profile that never knew about it.
    virtual void loadProfile(const ProfileReader& reader) { (void)reader; }

protected:
    Plugin() = default;
};

} // namespace sweeppp::plugin

/// Emits the two entry points and wires them to `Type`.
///
/// `descriptorFn` is a function returning `const sweeppp_plugin_desc_t&` with
/// static storage duration; the macro fills in its `activate` and `deactivate`
/// for you, so the descriptor should leave both null.
///
/// The instance is a function-local static rather than a `new`: nothing across
/// this boundary owns memory the other side allocated, and a plugin is a
/// singleton within its own image by construction.
#define SWEEPPP_PLUGIN_MAIN(Type, descriptorFn)                                                    \
    namespace {                                                                                    \
    Type& sweepppPluginInstance() {                                                                \
        static Type instance;                                                                      \
        return instance;                                                                           \
    }                                                                                              \
                                                                                                   \
    sweeppp::plugin::Host& sweepppPluginHost() {                                                   \
        static sweeppp::plugin::Host host;                                                         \
        return host;                                                                               \
    }                                                                                              \
                                                                                                   \
    sweeppp_plugin_status_t sweepppPluginActivate(const sweeppp_host_api_t* api,                   \
                                                  void** instance) {                               \
        try {                                                                                      \
            sweepppPluginHost() =                                                                  \
                sweeppp::plugin::Host(api, sweeppp::plugin::view((descriptorFn)().manifest.id));   \
            if (!sweepppPluginInstance().activate(sweepppPluginHost())) {                          \
                return SWEEPPP_PLUGIN_ERR_UNAVAILABLE;                                             \
            }                                                                                      \
            if (instance != nullptr) {                                                             \
                *instance = &sweepppPluginInstance();                                              \
            }                                                                                      \
            return SWEEPPP_PLUGIN_OK;                                                              \
        } catch (...) {                                                                            \
            return SWEEPPP_PLUGIN_ERR_UNKNOWN;                                                     \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    void sweepppPluginDeactivate(void* instance) {                                                 \
        (void)instance;                                                                            \
        sweeppp::plugin::detail::guard([] { sweepppPluginInstance().deactivate(); });              \
    }                                                                                              \
                                                                                                   \
    void sweepppPluginSaveProfile(void* instance, const sweeppp_profile_writer_t* writer) {        \
        (void)instance;                                                                            \
        sweeppp::plugin::detail::guard([writer] {                                                  \
            sweepppPluginInstance().saveProfile(sweeppp::plugin::ProfileWriter(writer));           \
        });                                                                                        \
    }                                                                                              \
                                                                                                   \
    void sweepppPluginLoadProfile(void* instance, const sweeppp_profile_reader_t* reader) {        \
        (void)instance;                                                                            \
        sweeppp::plugin::detail::guard([reader] {                                                  \
            sweepppPluginInstance().loadProfile(sweeppp::plugin::ProfileReader(reader));           \
        });                                                                                        \
    }                                                                                              \
    } /* namespace */                                                                              \
                                                                                                   \
    extern "C" SWEEPPP_PLUGIN_API std::uint32_t sweeppp_plugin_abi_version(void) {                 \
        return SWEEPPP_PLUGIN_ABI_VERSION;                                                         \
    }                                                                                              \
                                                                                                   \
    extern "C" SWEEPPP_PLUGIN_API const sweeppp_plugin_desc_t* sweeppp_plugin_query(               \
        std::uint32_t hostAbi) {                                                                   \
        if (hostAbi != SWEEPPP_PLUGIN_ABI_VERSION) {                                               \
            return nullptr;                                                                        \
        }                                                                                          \
        static sweeppp_plugin_desc_t desc = (descriptorFn)();                                      \
        desc.activate = &sweepppPluginActivate;                                                    \
        desc.deactivate = &sweepppPluginDeactivate;                                                \
        desc.profile_save = &sweepppPluginSaveProfile;                                             \
        desc.profile_load = &sweepppPluginLoadProfile;                                             \
        return &desc;                                                                              \
    }
