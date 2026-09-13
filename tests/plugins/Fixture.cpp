// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Deliberately-shaped plugins, for the paths that are otherwise only asserted.
//
// Written against the raw C ABI rather than the C++ wrapper, on purpose: the
// host must not depend on anything the wrapper happens to do, and the wrapper
// is exercised by the real plugin instead. One source, parameterised by the
// defines below, because six near-identical files is six places for a change
// to be applied five times.
//
//   FIXTURE_ID            the manifest id
//   FIXTURE_ABI           what sweeppp_plugin_abi_version() reports
//   FIXTURE_MIN_HOST      min_host_version
//   FIXTURE_DEPENDENCY    a required plugin id, or "" for none
//   FIXTURE_CONTRIBUTION  the contribution type this build hands over
//   FIXTURE_CONTRIBUTOR_ONLY  1 to declare nothing but the contributor facet
//
// The last two exist because ordering is a question about *several*
// contributors, and one plugin cannot answer it: two builds of this file, with
// different ids and different types over the same frequencies, are what make
// "who titles the marker" a thing a test can change and observe.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <sweeppp/plugin/PluginAbi.h>
#include <thread>

#ifndef FIXTURE_ID
#define FIXTURE_ID "test.sweeppp.fixture"
#endif
#ifndef FIXTURE_ABI
#define FIXTURE_ABI SWEEPPP_PLUGIN_ABI_VERSION
#endif
#ifndef FIXTURE_MIN_HOST
#define FIXTURE_MIN_HOST "0.0.1"
#endif
#ifndef FIXTURE_DEPENDENCY
#define FIXTURE_DEPENDENCY ""
#endif
#ifndef FIXTURE_CONTRIBUTION
#define FIXTURE_CONTRIBUTION SWEEPPP_CONTRIBUTION_BAND
#endif
#ifndef FIXTURE_CONTRIBUTOR_ONLY
#define FIXTURE_CONTRIBUTOR_ONLY 0
#endif

namespace {

/// A literal's length, from the literal. Hand-counted lengths in a table this
/// size are a bug waiting for someone to edit one of the strings.
template <std::size_t N>
constexpr sweeppp_str_t lit(const char (&text)[N]) {
    return sweeppp_str_t{.data = text, .len = N - 1};
}

struct FixtureRange {
    const char* name;
    const char* category;
    double startHz;
    double stopHz;
};

/// Two ranges, and the second nests inside the first: `contributions_in` then
/// has something to truncate, and `contributions_at` has a nesting of its own
/// to report narrowest first.
constexpr FixtureRange kRanges[]{
    {.name = "wide", .category = "test", .startHz = 80e6, .stopHz = 120e6},
    {.name = "narrow", .category = "test", .startHz = 95e6, .stopHz = 105e6},
};

constexpr std::uint32_t kRangeCount = 2;

sweeppp_str_t borrow(const char* text) {
    std::size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return sweeppp_str_t{.data = text, .len = length};
}

void fillContribution(std::uint32_t index, sweeppp_contribution_t& out) {
    out.struct_size = sizeof(sweeppp_contribution_t);
    out.type = FIXTURE_CONTRIBUTION;
    out.name = borrow(kRanges[index].name);
    out.description = lit("what the fixture says about this range");
    out.category = borrow(kRanges[index].category);
    out.start_hz = kRanges[index].startHz;
    out.stop_hz = kRanges[index].stopHz;
    out.color[0] = 0.2F;
    out.color[1] = 0.6F;
    out.color[2] = 0.9F;
    out.color[3] = 1.0F;
}

std::uint32_t datasetCount(void*) {
    return 1;
}

sweeppp_str_t datasetName(void*, std::uint32_t index) {
    return index == 0 ? lit("fixture") : lit("");
}

sweeppp_str_t datasetDescription(void*, std::uint32_t index) {
    return index == 0 ? lit("the fixture's only dataset") : lit("");
}

std::uint32_t activeDataset(void*) {
    return 0;
}

sweeppp_plugin_status_t selectDataset(void*, std::uint32_t index) {
    return index == 0 ? SWEEPPP_PLUGIN_OK : SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
}

std::uint32_t contributionsIn(void*, double fromHz, double toHz, sweeppp_contribution_t* out,
                              std::uint32_t capacity) {
    std::uint32_t found = 0;
    for (std::uint32_t i = 0; i < kRangeCount; ++i) {
        if (kRanges[i].stopHz <= fromHz || kRanges[i].startHz >= toHz) {
            continue;
        }
        if (found < capacity) {
            fillContribution(i, out[found]);
        }
        ++found;
    }
    return found;
}

/// Everything containing `hz`, narrowest first -- this plugin's own nesting,
/// which the host preserves rather than re-deriving.
std::uint32_t contributionsAt(void*, double hz, sweeppp_contribution_t* out,
                              std::uint32_t capacity) {
    std::uint32_t order[kRangeCount]{};
    std::uint32_t found = 0;

    for (std::uint32_t i = 0; i < kRangeCount; ++i) {
        if (hz >= kRanges[i].startHz && hz < kRanges[i].stopHz) {
            order[found++] = i;
        }
    }

    std::sort(order, order + found, [](std::uint32_t a, std::uint32_t b) {
        return (kRanges[a].stopHz - kRanges[a].startHz) < (kRanges[b].stopHz - kRanges[b].startHz);
    });

    for (std::uint32_t i = 0; i < found && i < capacity; ++i) {
        fillContribution(order[i], out[i]);
    }
    return found;
}

const sweeppp_contributor_vtable_t kContributorVtable{
    .struct_size = sizeof(sweeppp_contributor_vtable_t),
    .render = SWEEPPP_CONTRIBUTION_RENDER_HOST,
    .dataset_count = datasetCount,
    .dataset_name = datasetName,
    .dataset_description = datasetDescription,
    .active_dataset = activeDataset,
    .select_dataset = selectDataset,
    .contributions_in = contributionsIn,
    .contributions_at = contributionsAt,
};

// ---------------------------------------------------------- frame processor

/// What the last frame carried, republished as this plugin's own event type.
///
/// A processor that only counted would prove the callback fired and nothing
/// about what it was handed; the sequence and the bin count are what say the
/// frame survived the crossing intact. Declared here, in the plugin, which is
/// the point of the event channel being name-keyed rather than an enum: the
/// host has never heard of this type and does not need to.
struct FrameReport {
    std::uint32_t struct_size;
    std::uint64_t sequence;
    std::size_t bin_count;
    double start_hz;
    float first_bin;
};

/// The name is the plugin's own id and then the event, which is the rule the
/// host enforces on publish.
#define FIXTURE_FRAME_EVENT FIXTURE_ID ".frame"

const sweeppp_host_api_t* g_host = nullptr;

void onFrame(void*, const sweeppp_frame_t* frame) {
    if (frame == nullptr || g_host == nullptr || g_host->publish_event == nullptr) {
        return;
    }

    const FrameReport report{.struct_size = sizeof(FrameReport),
                             .sequence = frame->sequence,
                             .bin_count = frame->bin_count,
                             .start_hz = frame->start_hz,
                             .first_bin = frame->bin_count > 0 ? frame->bins_dbfs[0] : 0.0F};

    sweeppp_event_t event{};
    event.struct_size = sizeof(sweeppp_event_t);
    event.type = lit(FIXTURE_FRAME_EVENT);
    event.schema_version = 1;
    event.monotonic_ns = frame->host_time_ns;
    event.wall_ns = frame->wall_time_ns;
    event.payload = &report;
    event.payload_size = sizeof(report);

    g_host->publish_event(g_host->host, lit(FIXTURE_ID), &event);
}

const sweeppp_frame_processor_vtable_t kFrameVtable{
    .struct_size = sizeof(sweeppp_frame_processor_vtable_t),
    .on_frame = onFrame,
};

// --------------------------------------------------------------- fft backend

/// A transform that is not one: it copies, which is all the plumbing needs to
/// prove. What is being tested is that a plan crosses the boundary, executes
/// on the caller's buffers and is destroyed once -- not that anyone can write
/// a DFT.
void fftExecute(void* plan, const float* input, float* output) {
    const auto size = *static_cast<const std::size_t*>(plan);
    for (std::size_t i = 0; i < size * 2; ++i) {
        output[i] = input[i] * 2.0F;
    }
}

void fftExecuteBatch(void* plan, const float* input, float* output, std::size_t count) {
    const auto size = *static_cast<const std::size_t*>(plan);
    for (std::size_t i = 0; i < count; ++i) {
        fftExecute(plan, input + (i * size * 2), output + (i * size * 2));
    }
}

std::size_t g_planSize = 0;
int g_livePlans = 0;

void fftDestroy(void*) {
    --g_livePlans;
}

const sweeppp_fft_plan_vtable_t kFftPlanVtable{
    .struct_size = sizeof(sweeppp_fft_plan_vtable_t),
    .destroy = fftDestroy,
    .execute = fftExecute,
    .execute_batch = fftExecuteBatch,
};

sweeppp_plugin_status_t fftCapabilities(void*, sweeppp_fft_capabilities_t* out) {
    out->struct_size = sizeof(sweeppp_fft_capabilities_t);
    out->is_gpu = 0;
    out->min_size = 2;
    out->max_size = 65536;
    out->power_of_two_only = 1;
    out->supports_batch = 1;
    out->supports_in_place = 0;
    out->thread_safe_execute = 1;
    out->thread_safe_planning = 0;
    return SWEEPPP_PLUGIN_OK;
}

sweeppp_plugin_status_t fftCreatePlan(void*, const sweeppp_fft_plan_config_t* config, void** plan,
                                      const sweeppp_fft_plan_vtable_t** vtable) {
    g_planSize = config->size;
    ++g_livePlans;
    *plan = &g_planSize;
    *vtable = &kFftPlanVtable;
    return SWEEPPP_PLUGIN_OK;
}

std::size_t fftSnapSize(void*, std::size_t desired) {
    std::size_t size = 2;
    while (size < desired) {
        size *= 2;
    }
    return size;
}

const sweeppp_fft_backend_vtable_t kFftVtable{
    .struct_size = sizeof(sweeppp_fft_backend_vtable_t),
    .capabilities = fftCapabilities,
    .create_plan = fftCreatePlan,
    .snap_size = fftSnapSize,
    .reset = nullptr,
};

// -------------------------------------------------------------------- sdr
//
// Deliberately not a minimal driver. The facet is generated into a UI panel
// and fed straight into the acquisition path, so the fixture exercises the
// widths that actually matter: every SdrValue alternative, an enum with
// appliesWhen, all four flags, a native format that is NOT float, and a real
// producer thread that borrows blocks from the host's pool and hands them back.

int g_liveDevices = 0;
double g_centerHz = 100e6;
double g_sampleRate = 20e6;
std::int64_t g_lnaGain = 16;
bool g_biasTee = false;
const char* g_gainMode = "manual";

/// The one sample a block carries at a given position.
///
/// Keyed on the sequence number so a test can recompute exactly what should
/// have arrived and compare the bytes -- which is the proof that nothing
/// converted or copied on the way through.
constexpr std::int16_t rampSample(std::uint64_t sequence, std::size_t frame, bool quadrature) {
    const auto value = static_cast<std::int16_t>((sequence * 1000U) + frame);
    return quadrature ? static_cast<std::int16_t>(-value) : value;
}

/* ---- parameters ------------------------------------------------------- */

const sweeppp_sdr_enum_value_t kGainModes[]{
    {
        .struct_size = sizeof(sweeppp_sdr_enum_value_t),
        .value = lit("manual"),
        .label = lit("Manual"),
        .description = lit("The gain below applies"),
    },
    {
        .struct_size = sizeof(sweeppp_sdr_enum_value_t),
        .value = lit("automatic"),
        .label = lit("Automatic"),
        .description = lit("The radio decides, and the gain below does not apply"),
    },
};

/// The gain only applies in manual mode, which is the one thing a generated
/// panel cannot work out for itself.
const sweeppp_str_t kGainAppliesWhen[]{lit("manual")};

void fillParameter(sweeppp_sdr_parameter_t& out) {
    out = sweeppp_sdr_parameter_t{};
    out.struct_size = sizeof(sweeppp_sdr_parameter_t);
}

std::uint32_t sdrParameters(void*, sweeppp_sdr_parameter_t* out, std::uint32_t capacity) {
    if (out == nullptr || capacity < 4) {
        return 4;
    }

    // Double, and the one the sweep engine drives.
    fillParameter(out[0]);
    out[0].key = lit("center_hz");
    out[0].label = lit("Centre frequency");
    out[0].group = lit("Tuning");
    out[0].type = SWEEPPP_SDR_PARAM_DOUBLE;
    out[0].unit = lit("Hz");
    out[0].minimum = 1e6;
    out[0].maximum = 6e9;
    out[0].step = 1.0;
    out[0].grid_affecting = 1;
    out[0].description = lit("Where the fixture pretends to be tuned.");
    out[0].default_value.struct_size = sizeof(sweeppp_value_t);
    out[0].default_value.type = SWEEPPP_VALUE_FLOAT;
    out[0].default_value.number = 100e6;

    // Int, with a step the host must snap to.
    fillParameter(out[1]);
    out[1].key = lit("lna_gain");
    out[1].label = lit("LNA gain");
    out[1].group = lit("Gain");
    out[1].type = SWEEPPP_SDR_PARAM_INT;
    out[1].unit = lit("dB");
    out[1].minimum = 0.0;
    out[1].maximum = 40.0;
    out[1].step = 8.0;
    out[1].calibration_affecting = 1;
    out[1].description = lit("Moves in 8 dB steps, as a real front end does.");
    out[1].default_value.struct_size = sizeof(sweeppp_value_t);
    out[1].default_value.type = SWEEPPP_VALUE_INT;
    out[1].default_value.integer = 16;
    out[1].applies_when_key = lit("gain_mode");
    out[1].applies_when_values = kGainAppliesWhen;
    out[1].applies_when_value_count = 1;

    // Enum, as a string, and what the one above depends on.
    fillParameter(out[2]);
    out[2].key = lit("gain_mode");
    out[2].label = lit("Gain mode");
    out[2].group = lit("Gain");
    out[2].type = SWEEPPP_SDR_PARAM_ENUM;
    out[2].enum_values = kGainModes;
    out[2].enum_value_count = 2;
    out[2].requires_stop = 1;
    out[2].description = lit("Whether the manual gain applies at all.");
    out[2].default_value.struct_size = sizeof(sweeppp_value_t);
    out[2].default_value.type = SWEEPPP_VALUE_STRING;
    out[2].default_value.text = lit("manual");

    // Bool, and read-only, so both flags have a case.
    fillParameter(out[3]);
    out[3].key = lit("bias_tee");
    out[3].label = lit("Bias tee");
    out[3].group = lit("Front end");
    out[3].type = SWEEPPP_SDR_PARAM_BOOL;
    out[3].read_only = 1;
    out[3].description = lit("Reported, never set: the fixture has no such supply.");
    out[3].default_value.struct_size = sizeof(sweeppp_value_t);
    out[3].default_value.type = SWEEPPP_VALUE_BOOL;
    out[3].default_value.boolean = 0;

    return 4;
}

sweeppp_plugin_status_t sdrGetParameter(void*, sweeppp_str_t key, sweeppp_value_t* out) {
    const std::string_view name(key.data, key.len);
    out->struct_size = sizeof(sweeppp_value_t);

    if (name == "center_hz") {
        out->type = SWEEPPP_VALUE_FLOAT;
        out->number = g_centerHz;
        return SWEEPPP_PLUGIN_OK;
    }
    if (name == "lna_gain") {
        out->type = SWEEPPP_VALUE_INT;
        out->integer = g_lnaGain;
        return SWEEPPP_PLUGIN_OK;
    }
    if (name == "gain_mode") {
        out->type = SWEEPPP_VALUE_STRING;
        out->text = sweeppp_str_t{.data = g_gainMode, .len = std::strlen(g_gainMode)};
        return SWEEPPP_PLUGIN_OK;
    }
    if (name == "bias_tee") {
        out->type = SWEEPPP_VALUE_BOOL;
        out->boolean = g_biasTee ? 1 : 0;
        return SWEEPPP_PLUGIN_OK;
    }
    if (name == "sample_rate") {
        out->type = SWEEPPP_VALUE_FLOAT;
        out->number = g_sampleRate;
        return SWEEPPP_PLUGIN_OK;
    }
    return SWEEPPP_PLUGIN_ERR_NOT_FOUND;
}

sweeppp_plugin_status_t sdrSetParameter(void*, sweeppp_str_t key, const sweeppp_value_t* value) {
    const std::string_view name(key.data, key.len);

    if (name == "center_hz") {
        g_centerHz = value->number;
        return SWEEPPP_PLUGIN_OK;
    }
    if (name == "lna_gain") {
        g_lnaGain = value->integer;
        return SWEEPPP_PLUGIN_OK;
    }
    if (name == "gain_mode") {
        // Compared against the literals so the pointer stays valid after the
        // borrowed string goes: the ABI lends this only for the call.
        const std::string_view text(value->text.data, value->text.len);
        g_gainMode = text == "automatic" ? "automatic" : "manual";
        return SWEEPPP_PLUGIN_OK;
    }
    if (name == "sample_rate") {
        g_sampleRate = value->number;
        return SWEEPPP_PLUGIN_OK;
    }
    return SWEEPPP_PLUGIN_ERR_NOT_FOUND;
}

/* ---- streaming -------------------------------------------------------- */

sweeppp_sdr_stream_t g_stream{};
std::thread g_producer;
std::atomic<bool> g_streaming{false};
std::atomic<std::uint64_t> g_sequence{0};
std::atomic<std::size_t> g_framesPerBlock{0};
std::atomic<std::uint64_t> g_exhausted{0};

sweeppp_sdr_format_t sdrNativeFormat(void*) {
    // Cs16, and deliberately not a float format: nothing in the host may
    // assume the one the old pull-mode facet could carry.
    return SWEEPPP_SDR_FORMAT_CS16;
}

std::uint32_t sdrSampleRates(void*, double* out, std::uint32_t capacity) {
    const double rates[]{2e6, 8e6, 20e6};
    for (std::uint32_t i = 0; i < 3 && i < capacity; ++i) {
        out[i] = rates[i];
    }
    return 3;
}

void produce() {
    while (g_streaming.load()) {
        const sweeppp_sdr_block_t block = g_stream.acquire(g_stream.stream);
        if (block.handle == nullptr) {
            // Exhausted is a normal answer: drop it and say so, never wait.
            // Waiting here would turn a host backlog into a device overrun.
            const std::size_t frames = g_framesPerBlock.load();
            g_stream.report_pool_exhausted(g_stream.stream, frames);
            g_exhausted.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const std::uint64_t sequence = g_sequence.fetch_add(1);
        const std::size_t frames = std::min(g_framesPerBlock.load(), block.capacity_bytes / 4);

        auto* samples = reinterpret_cast<std::int16_t*>(block.data);
        for (std::size_t i = 0; i < frames; ++i) {
            samples[i * 2] = rampSample(sequence, i, false);
            samples[(i * 2) + 1] = rampSample(sequence, i, true);
        }

        sweeppp_sdr_delivery_t delivery{};
        delivery.struct_size = sizeof(delivery);
        delivery.handle = block.handle;
        delivery.frames = frames;
        delivery.format = SWEEPPP_SDR_FORMAT_CS16;
        delivery.sequence = sequence;
        delivery.center_hz = g_centerHz;
        delivery.sample_rate = g_sampleRate;
        g_stream.publish(g_stream.stream, &delivery);

        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

sweeppp_plugin_status_t sdrStart(void*, const sweeppp_sdr_stream_config_t* config,
                                 const sweeppp_sdr_stream_t* stream) {
    if (config == nullptr || stream == nullptr || g_streaming.load()) {
        return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
    }

    g_stream = *stream;
    g_framesPerBlock.store(config->frames_per_block);
    g_sequence.store(0);
    g_exhausted.store(0);
    g_streaming.store(true);
    g_producer = std::thread(produce);
    return SWEEPPP_PLUGIN_OK;
}

void sdrStop(void*) {
    // Must not return until the producer has stopped touching the stream
    // table: the host tears it down the moment this returns.
    g_streaming.store(false);
    if (g_producer.joinable()) {
        g_producer.join();
    }
}

int sdrStreaming(void*) {
    return g_streaming.load() ? 1 : 0;
}

/* ---- the rest --------------------------------------------------------- */

void sdrDestroy(void*) {
    sdrStop(nullptr);
    --g_liveDevices;
}

sweeppp_plugin_status_t sdrRetune(void*, double centerHz) {
    g_centerHz = centerHz;
    return SWEEPPP_PLUGIN_OK;
}

double sdrSettle(void*) {
    return 0.001;
}
double sdrGranularity(void*, double sampleRate) {
    return 65536.0 / sampleRate;
}

std::uint32_t sdrHealth(void*, sweeppp_sdr_health_t* out, std::uint32_t capacity) {
    if (out == nullptr || capacity < 2) {
        return 2;
    }

    out[0].struct_size = sizeof(sweeppp_sdr_health_t);
    out[0].label = lit("Temperature");
    out[0].value = lit("71.5 C");
    out[0].numeric = 71.5F;
    out[0].minimum = 0.0F;
    out[0].maximum = 70.0F;
    out[0].alarm = 1; // over its own maximum, so the alarm path has a case

    // Text only: a maximum at or below the minimum means there is no scale to
    // plot this against.
    out[1].struct_size = sizeof(sweeppp_sdr_health_t);
    out[1].label = lit("Power source");
    out[1].value = lit("USB bus");
    out[1].numeric = 0.0F;
    out[1].minimum = 0.0F;
    out[1].maximum = 0.0F;
    out[1].alarm = 0;

    return 2;
}

void fillInfo(sweeppp_sdr_device_info_t& out) {
    out.struct_size = sizeof(sweeppp_sdr_device_info_t);
    out.id = lit("fixture-0");
    out.label = lit("Fixture radio");
    out.serial = lit("0000");
    out.hardware_revision = lit("r0");
    out.min_frequency_hz = 1e6;
    out.max_frequency_hz = 6e9;
    out.min_sample_rate = 1e6;
    out.max_sample_rate = 20e6;

    out.firmware.struct_size = sizeof(sweeppp_sdr_version_t);
    out.firmware.version = lit("2.1.0");
    out.firmware.known_latest = lit("2.3.0");
    out.firmware.ahead_of_driver = 0;

    out.fpga.struct_size = sizeof(sweeppp_sdr_version_t);
    out.fpga.version = lit("0.15.0");
    out.fpga.known_latest = lit("0.15.0");
    out.fpga.ahead_of_driver = 1;

    out.link_capacity_bytes_per_sec = 400000000;
    out.link_description = lit("USB 3.0 SuperSpeed");
}

sweeppp_plugin_status_t sdrInfo(void*, sweeppp_sdr_device_info_t* out) {
    if (out == nullptr) {
        return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
    }
    fillInfo(*out);
    return SWEEPPP_PLUGIN_OK;
}

const sweeppp_sdr_device_vtable_t kSdrDeviceVtable{
    .struct_size = sizeof(sweeppp_sdr_device_vtable_t),
    .destroy = sdrDestroy,
    .info = sdrInfo,
    .parameters = sdrParameters,
    .get_parameter = sdrGetParameter,
    .set_parameter = sdrSetParameter,
    .native_format = sdrNativeFormat,
    .supported_sample_rates = sdrSampleRates,
    .start = sdrStart,
    .stop = sdrStop,
    .streaming = sdrStreaming,
    .retune = sdrRetune,
    .retune_settle_seconds = sdrSettle,
    .delivery_granularity_seconds = sdrGranularity,
    .health_readings = sdrHealth,
};

std::uint32_t sdrEnumerate(void*, sweeppp_sdr_device_info_t* out, std::uint32_t capacity) {
    if (capacity >= 1 && out != nullptr) {
        fillInfo(out[0]);
    }
    return 1;
}

sweeppp_plugin_status_t sdrOpen(void*, sweeppp_str_t id, void** device,
                                const sweeppp_sdr_device_vtable_t** vtable, sweeppp_str_t* error) {
    // A failure worth explaining, which is the whole reason `error` exists:
    // ERR_DEVICE on its own tells an operator nothing they can act on.
    if (std::string_view(id.data, id.len) == "boom") {
        if (error != nullptr) {
            *error = lit("the fixture was asked to fail (this is the error channel working)");
        }
        return SWEEPPP_PLUGIN_ERR_DEVICE;
    }

    ++g_liveDevices;
    *device = &g_liveDevices;
    *vtable = &kSdrDeviceVtable;
    return SWEEPPP_PLUGIN_OK;
}

const sweeppp_sdr_factory_vtable_t kSdrVtable{
    .struct_size = sizeof(sweeppp_sdr_factory_vtable_t),
    .enumerate = sdrEnumerate,
    .open = sdrOpen,
};

// ---------------------------------------------------------------- rf path

// A four-way antenna switcher. Not a radio: no samples, no tuning, no stream,
// which is the whole reason it is its own facet kind.
//
// `g_selectedInput` is what a routing test reads to prove the pass drove the
// box in the order the plan says it should.
std::uint32_t g_selectedInput = 0;
int g_livePaths = 0;

const struct {
    const char* id;
    const char* label;
    double minHz;
    double maxHz;
} kRfInputs[]{
    {"in1", "J1", 0.0, 0.0},
    // One filtered input, so the host's "the switcher narrows it too" path has
    // something to narrow.
    {"in2", "J2", 1.0e9, 2.0e9},
    {"in3", "J3", 0.0, 0.0},
    {"in4", "J4", 0.0, 0.0},
};

std::uint32_t rfInputs(void*, sweeppp_rf_path_input_t* out, std::uint32_t capacity) {
    constexpr auto kCount = static_cast<std::uint32_t>(std::size(kRfInputs));
    if (out == nullptr || capacity < kCount) {
        return kCount;
    }
    for (std::uint32_t i = 0; i < kCount; ++i) {
        out[i] = sweeppp_rf_path_input_t{};
        out[i].struct_size = sizeof(sweeppp_rf_path_input_t);
        out[i].id = borrow(kRfInputs[i].id);
        out[i].label = borrow(kRfInputs[i].label);
        out[i].min_hz = kRfInputs[i].minHz;
        out[i].max_hz = kRfInputs[i].maxHz;
    }
    return kCount;
}

void fillRfPathInfo(sweeppp_rf_path_info_t& out) {
    out = sweeppp_rf_path_info_t{};
    out.struct_size = sizeof(sweeppp_rf_path_info_t);
    out.id = lit("switch-1");
    out.label = lit("Fixture 4-way switch");
    out.model = lit("FX-4");
    out.serial = lit("SW0001");
    out.input_count = static_cast<std::uint32_t>(std::size(kRfInputs));
    out.requires_stop = 0;
    out.switch_seconds = 0.01;
}

sweeppp_plugin_status_t rfPathInfo(void*, sweeppp_rf_path_info_t* out) {
    if (out == nullptr) {
        return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
    }
    fillRfPathInfo(*out);
    return SWEEPPP_PLUGIN_OK;
}

sweeppp_plugin_status_t rfSelectInput(void*, std::uint32_t index) {
    if (index >= std::size(kRfInputs)) {
        return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
    }
    g_selectedInput = index;
    return SWEEPPP_PLUGIN_OK;
}

std::uint32_t rfSelectedInput(void*) {
    return g_selectedInput;
}

std::uint32_t rfHealth(void*, sweeppp_sdr_health_t* out, std::uint32_t capacity) {
    if (out == nullptr || capacity < 1) {
        return 1;
    }
    out[0] = sweeppp_sdr_health_t{};
    out[0].struct_size = sizeof(sweeppp_sdr_health_t);
    out[0].label = lit("Supply");
    out[0].value = lit("12.0 V");
    out[0].numeric = 12.0F;
    out[0].minimum = 10.0F;
    out[0].maximum = 14.0F;
    return 1;
}

void rfDestroy(void*) {
    if (g_livePaths > 0) {
        --g_livePaths;
    }
}

const sweeppp_rf_path_vtable_t kRfPathVtable{
    .struct_size = sizeof(sweeppp_rf_path_vtable_t),
    .destroy = rfDestroy,
    .info = rfPathInfo,
    .inputs = rfInputs,
    .select_input = rfSelectInput,
    .selected_input = rfSelectedInput,
    .health_readings = rfHealth,
};

std::uint32_t rfEnumerate(void*, sweeppp_rf_path_info_t* out, std::uint32_t capacity) {
    if (capacity >= 1 && out != nullptr) {
        fillRfPathInfo(out[0]);
    }
    return 1;
}

sweeppp_plugin_status_t rfOpen(void*, sweeppp_str_t id, void** path,
                               const sweeppp_rf_path_vtable_t** vtable, sweeppp_str_t* error) {
    if (std::string_view(id.data, id.len) == "boom") {
        if (error != nullptr) {
            *error = lit("the fixture switcher was asked to fail");
        }
        return SWEEPPP_PLUGIN_ERR_DEVICE;
    }

    ++g_livePaths;
    *path = &g_livePaths;
    *vtable = &kRfPathVtable;
    return SWEEPPP_PLUGIN_OK;
}

const sweeppp_rf_path_factory_vtable_t kRfPathFactoryVtable{
    .struct_size = sizeof(sweeppp_rf_path_factory_vtable_t),
    .enumerate = rfEnumerate,
    .open = rfOpen,
};

#define FIXTURE_WINDOW_EVENT FIXTURE_ID ".window"

/// A count of the window spot's dispatches, published.
///
/// The one UI spot a headless test can exercise: every other one is drawn
/// inside something -- a popup, a toolbar child, a plot's clip rect -- and
/// means nothing without a frame, while a window is dispatched at the top
/// level and so is just a call the host either makes or does not. Reported on
/// the event channel because the counter lives in this module and the test
/// lives in another.
int g_windowDraws = 0;

struct WindowReport {
    std::uint32_t struct_size;
    std::int32_t draws;
};

void fixtureDrawWindow(void*) {
    ++g_windowDraws;
    if (g_host == nullptr || g_host->publish_event == nullptr) {
        return;
    }

    const WindowReport report{.struct_size = sizeof(WindowReport), .draws = g_windowDraws};

    sweeppp_event_t event{};
    event.struct_size = sizeof(sweeppp_event_t);
    event.type = lit(FIXTURE_WINDOW_EVENT);
    event.schema_version = 1;
    event.payload = &report;
    event.payload_size = sizeof(report);

    g_host->publish_event(g_host->host, lit(FIXTURE_ID), &event);
}

const sweeppp_ui_vtable_t kUiVtable{
    .struct_size = sizeof(sweeppp_ui_vtable_t),
    .spots = SWEEPPP_UI_SPOT_BIT(SWEEPPP_UI_SPOT_WINDOW),
    .layer = SWEEPPP_UI_LAYER_UNDER,
    .draw_overlay = nullptr,
    .draw_settings = nullptr,
    .draw_status_chip = nullptr,
    .draw_toolbar = nullptr,
    .draw_window = fixtureDrawWindow,
};

const sweeppp_facet_t kFacets[]{
    {
        .struct_size = sizeof(sweeppp_facet_t),
        .kind = SWEEPPP_FACET_CONTRIBUTOR,
        .id = lit("ranges"),
        .name = lit("Fixture ranges"),
        .description = lit("Two overlapping ranges, for the host's tests"),
        .vtable = &kContributorVtable,
    },
#if !FIXTURE_CONTRIBUTOR_ONLY
    {
        .struct_size = sizeof(sweeppp_facet_t),
        .kind = SWEEPPP_FACET_FRAME_PROCESSOR,
        .id = lit("frames"),
        .name = lit("Fixture frame processor"),
        .description = lit("Republishes what it was handed, on the event channel"),
        .vtable = &kFrameVtable,
    },
    {
        .struct_size = sizeof(sweeppp_facet_t),
        .kind = SWEEPPP_FACET_FFT_BACKEND,
        .id = lit(FIXTURE_ID ".fft"),
        .name = lit("Fixture FFT"),
        .description = lit("Doubles its input; enough to prove a plan crosses the boundary"),
        .vtable = &kFftVtable,
    },
    {
        .struct_size = sizeof(sweeppp_facet_t),
        .kind = SWEEPPP_FACET_SDR_DEVICE,
        .id = lit(FIXTURE_ID ".sdr"),
        .name = lit("Fixture radio"),
        .description = lit("Delivers a ramp, on the host's own pump"),
        .vtable = &kSdrVtable,
    },
    {
        .struct_size = sizeof(sweeppp_facet_t),
        .kind = SWEEPPP_FACET_RF_PATH,
        .id = lit(FIXTURE_ID ".switch"),
        .name = lit("Fixture switcher"),
        .description = lit("A four-way antenna switch, for the routing tests"),
        .vtable = &kRfPathFactoryVtable,
    },
    {
        .struct_size = sizeof(sweeppp_facet_t),
        .kind = SWEEPPP_FACET_UI_EXTENSION,
        .id = lit("window"),
        .name = lit("Fixture window"),
        .description = lit("Counts the times the host dispatches the window spot"),
        .vtable = &kUiVtable,
    },
#endif
};

#if FIXTURE_CONTRIBUTOR_ONLY
constexpr std::uint32_t kFacetCount = 1;
#else
constexpr std::uint32_t kFacetCount = 6;
#endif

const sweeppp_dependency_t kDependencies[]{{
    .struct_size = sizeof(sweeppp_dependency_t),
    .plugin_id = lit(FIXTURE_DEPENDENCY),
    .min_version = lit(""),
    .max_version = lit(""),
    .optional = 0,
}};

const sweeppp_author_t kAuthors[]{{
    .struct_size = sizeof(sweeppp_author_t),
    .name = lit("Sweep++ tests"),
    .email = lit(""),
}};

/// Set by activate, so a test can tell "registered" from "merely declared".
int g_activated = 0;

sweeppp_plugin_status_t fixtureActivate(const sweeppp_host_api_t* host, void** instance) {
    if (host == nullptr || host->register_contributor == nullptr) {
        return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
    }

    g_host = host;
    g_activated = 1;
    if (instance != nullptr) {
        *instance = &g_activated;
    }

    // Every facet the manifest declared. Each result is ignored on purpose:
    // losing a driver-name race or landing in a host with no frame bus is a
    // listed reason, not a reason to fail activation.
    host->register_contributor(host->host, lit(FIXTURE_ID), &kFacets[0], &g_activated);
#if !FIXTURE_CONTRIBUTOR_ONLY
    if (host->frame_subscribe != nullptr) {
        host->frame_subscribe(host->host, lit(FIXTURE_ID), &kFacets[1], &g_activated);
    }
    if (host->register_fft_backend != nullptr) {
        host->register_fft_backend(host->host, lit(FIXTURE_ID), &kFacets[2], &g_activated);
    }
    if (host->register_sdr_driver != nullptr) {
        host->register_sdr_driver(host->host, lit(FIXTURE_ID), &kFacets[3], &g_activated);
    }
    if (host->register_rf_path != nullptr) {
        host->register_rf_path(host->host, lit(FIXTURE_ID), &kFacets[4], &g_activated);
    }
    if (host->register_ui_extension != nullptr) {
        host->register_ui_extension(host->host, lit(FIXTURE_ID), &kFacets[5], &g_activated);
    }
#endif
    return SWEEPPP_PLUGIN_OK;
}

void fixtureDeactivate(void*) {
    g_activated = 0;
}

const sweeppp_plugin_desc_t kDesc{
    .struct_size = sizeof(sweeppp_plugin_desc_t),
    .manifest =
        sweeppp_manifest_t{
            .struct_size = sizeof(sweeppp_manifest_t),
            .id = lit(FIXTURE_ID),
            .version = lit("1.2.3"),
            .name = lit("Fixture"),
            .description = lit("A plugin that exists so the host's failure paths are exercised"),
            .authors = kAuthors,
            .author_count = 1,
            .links = nullptr,
            .link_count = 0,
            .dependencies = kDependencies,
            .dependency_count = sizeof(FIXTURE_DEPENDENCY) > 1 ? 1U : 0U,
            .min_host_version = lit(FIXTURE_MIN_HOST),
            .requires_restart_to_disable = 0,
        },
    .facets = kFacets,
    .facet_count = kFacetCount,
    .activate = fixtureActivate,
    .deactivate = fixtureDeactivate,
    .profile_save = nullptr,
    .profile_load = nullptr,
};

} // namespace

extern "C" SWEEPPP_PLUGIN_API std::uint32_t sweeppp_plugin_abi_version() {
    return FIXTURE_ABI;
}

extern "C" SWEEPPP_PLUGIN_API const sweeppp_plugin_desc_t*
sweeppp_plugin_query(std::uint32_t hostAbi) {
    return hostAbi == SWEEPPP_PLUGIN_ABI_VERSION ? &kDesc : nullptr;
}
