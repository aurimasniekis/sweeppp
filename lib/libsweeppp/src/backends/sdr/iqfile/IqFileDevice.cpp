// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/backends/sdr/SyntheticDevice.hpp"
#include "sweeppp/core/BlockPool.hpp"
#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Paths.hpp"
#include "sweeppp/core/Telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

namespace sweeppp {
namespace {

namespace fs = std::filesystem;

/// Replays a raw IQ file as if it were a radio.
///
/// The point is reproducibility: a recorded capture, or a generated fixture,
/// re-enters the pipeline through exactly the same path a live radio uses, so
/// a regression test exercises the real code rather than a shortcut. Format is
/// taken from the file extension when it is recognisable, since that is the
/// convention every SDR tool already writes.
class IqFileDevice final : public ISdrDevice {
public:
    IqFileDevice(SdrDeviceInfo info, fs::path path, SampleFormat format)
        : m_info(std::move(info)), m_path(std::move(path)), m_format(format) {
        buildParameters();
    }

    ~IqFileDevice() override { IqFileDevice::stop(); }

    [[nodiscard]] const SdrDeviceInfo& info() const noexcept override { return m_info; }

    [[nodiscard]] std::span<const SdrParameter> parameters() const noexcept override {
        return m_parameters;
    }

    [[nodiscard]] Result<SdrValue> getParameter(std::string_view key) const override {
        if (key == "center_hz") {
            return SdrValue{m_centerHz.load()};
        }
        if (key == "sample_rate") {
            return SdrValue{m_sampleRate.load()};
        }
        if (key == "loop") {
            return SdrValue{m_loop.load()};
        }
        if (key == "realtime") {
            return SdrValue{m_realtime.load()};
        }
        if (key == "path") {
            return SdrValue{m_path.string()};
        }
        return fail<SdrValue>(ErrorCode::NotFound, "no parameter '{}'", key);
    }

    [[nodiscard]] Status setParameter(std::string_view key, const SdrValue& value) override {
        const auto parameter = std::ranges::find_if(
            m_parameters, [key](const SdrParameter& p) { return p.key == key; });
        if (parameter == m_parameters.end()) {
            return fail(ErrorCode::NotFound, "no parameter '{}'", key);
        }
        if (parameter->readOnly) {
            return fail(ErrorCode::InvalidArgument, "'{}' is read-only", key);
        }

        auto coerced = parameter->coerce(value);
        if (!coerced) {
            return std::unexpected(coerced.error());
        }

        if (key == "center_hz") {
            m_centerHz.store(asDouble(*coerced));
        } else if (key == "sample_rate") {
            m_sampleRate.store(asDouble(*coerced));
        } else if (key == "loop") {
            m_loop.store(asBool(*coerced));
        } else if (key == "realtime") {
            m_realtime.store(asBool(*coerced));
        }
        return ok();
    }

    [[nodiscard]] SampleFormat nativeFormat() const noexcept override { return m_format; }

    [[nodiscard]] Status start(BlockPool& pool, const StreamConfig& config,
                               IqCallback callback) override {
        if (m_streaming.load()) {
            return fail(ErrorCode::AlreadyExists, "already streaming");
        }

        m_stream.open(m_path, std::ios::binary);
        if (!m_stream) {
            return fail(ErrorCode::IoError, "could not open {}", m_path.string());
        }

        m_pool = &pool;
        m_callback = std::move(callback);
        m_framesPerBlock = config.framesPerBlock;
        m_sequence = 0;
        m_streaming.store(true);

        m_thread = std::jthread([this](std::stop_token stop) { replay(stop); });
        return ok();
    }

    void stop() override {
        if (!m_streaming.exchange(false)) {
            return;
        }
        m_thread.request_stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_stream.close();
        m_callback = nullptr;
        m_pool = nullptr;
    }

    [[nodiscard]] bool streaming() const noexcept override { return m_streaming.load(); }

    [[nodiscard]] Status retune(double centerHz) override {
        // A file has one centre frequency; retuning only relabels it. Accepted
        // rather than refused so a sweep plan can be replayed against a file
        // without special-casing the device.
        m_centerHz.store(centerHz);
        return ok();
    }

    void attachTelemetry(StreamCounters* counters) noexcept override {
        m_telemetry.store(counters);
    }

private:
    void buildParameters() {
        m_parameters = {
            SdrParameter{.key = "path",
                         .label = "File",
                         .group = "Source",
                         .type = SdrParameterType::String,
                         .unit = {},
                         .min = 0.0,
                         .max = 0.0,
                         .step = 0.0,
                         .enumValues = {},
                         .readOnly = true,
                         .gridAffecting = false,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "IQ file being replayed.",
                         .defaultValue = std::string{}},
            SdrParameter{.key = "center_hz",
                         .label = "Center frequency",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "Hz",
                         .min = 0.0,
                         .max = 6e9,
                         .step = 1.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "Frequency this capture was taken at. Metadata only -- "
                                        "the file's contents do not change.",
                         .defaultValue = 100e6},
            SdrParameter{.key = "sample_rate",
                         .label = "Sample rate",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "S/s",
                         .min = 1e3,
                         .max = 200e6,
                         .step = 0.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "Rate the capture was taken at. Sets the frequency axis "
                                        "and, with Realtime on, the replay pace.",
                         .defaultValue = 20e6},
            SdrParameter{.key = "loop",
                         .label = "Loop",
                         .group = "Playback",
                         .type = SdrParameterType::Bool,
                         .unit = {},
                         .min = 0.0,
                         .max = 0.0,
                         .step = 0.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "Restart at end of file instead of stopping.",
                         .defaultValue = true},
            SdrParameter{.key = "realtime",
                         .label = "Realtime",
                         .group = "Playback",
                         .type = SdrParameterType::Bool,
                         .unit = {},
                         .min = 0.0,
                         .max = 0.0,
                         .step = 0.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "Pace replay to the sample rate. Off replays as fast as "
                                        "the pipeline accepts, which is what a batch regression "
                                        "run wants.",
                         .defaultValue = true},
        };
    }

    void replay(std::stop_token stop) {
        const std::uint64_t startNs = monotonicNs();
        std::uint64_t framesRead = 0;

        while (!stop.stop_requested()) {
            const double sampleRate = m_sampleRate.load();

            if (m_realtime.load()) {
                const double elapsed = nsToSeconds(monotonicNs() - startNs);
                const double due = static_cast<double>(framesRead) / sampleRate;
                if (due > elapsed + 0.002) {
                    std::this_thread::sleep_for(
                        std::chrono::duration<double>(std::min(due - elapsed, 0.05)));
                    continue;
                }
            }

            BlockRef ref = m_pool->acquire();
            if (!ref.valid()) {
                if (StreamCounters* counters = m_telemetry.load(); counters != nullptr) {
                    counters->poolExhaustedEvents.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::yield();
                continue;
            }

            const std::size_t wanted = m_framesPerBlock * bytesPerFrame(m_format);
            m_stream.read(reinterpret_cast<char*>(ref.data()),
                          static_cast<std::streamsize>(wanted));
            auto got = static_cast<std::size_t>(m_stream.gcount());

            if (got == 0) {
                if (!m_loop.load()) {
                    logInfo("iqfile", "reached end of {}", m_path.string());
                    break;
                }
                m_stream.clear();
                m_stream.seekg(0);
                continue;
            }

            // A short final read is truncated to whole frames -- half a sample
            // would shift the I/Q interleaving for everything after it.
            const std::size_t frames = got / bytesPerFrame(m_format);
            if (frames == 0) {
                continue;
            }

            IqBlock block;
            block.block = std::move(ref);
            block.frames = frames;
            block.format = m_format;
            block.sequence = m_sequence++;
            block.hostTimeNs = monotonicNs();
            block.centerHz = m_centerHz.load();
            block.sampleRate = sampleRate;

            if (m_callback) {
                m_callback(std::move(block));
            }
            framesRead += frames;
        }
    }

    SdrDeviceInfo m_info;
    std::vector<SdrParameter> m_parameters;
    fs::path m_path;
    SampleFormat m_format = SampleFormat::Cf32;

    std::atomic<double> m_centerHz{100e6};
    std::atomic<double> m_sampleRate{20e6};
    std::atomic<bool> m_loop{true};
    std::atomic<bool> m_realtime{true};

    std::atomic<bool> m_streaming{false};
    std::jthread m_thread;
    std::ifstream m_stream;
    BlockPool* m_pool = nullptr;
    IqCallback m_callback;
    std::size_t m_framesPerBlock = 262'144;
    std::uint64_t m_sequence = 0;
    std::atomic<StreamCounters*> m_telemetry{nullptr};
};

/// Guesses the sample format from the filename.
///
/// Every SDR tool encodes it in the extension or a suffix, and honouring that
/// convention avoids making the user restate what the filename already says.
/// Falls back to cf32, the most common interchange format.
SampleFormat formatFromPath(const fs::path& path) {
    const std::string name = path.filename().string();

    // Longest first: "cs16" contains no shorter entry, but "cf32" would match
    // a hypothetical "f32" before it if the shorter one came first, and
    // "cu16"/"cu8" differ only in the tail.
    for (const std::string_view candidate :
         {"cs16", "sc16", "cu16", "cs12", "cu12", "cf16", "cf32", "fc32", "cf64", "fc64", "cs32",
          "sc32", "cu32", "cs8", "sc8", "cu8"}) {
        if (name.find(candidate) != std::string::npos) {
            if (const auto parsed = sampleFormatFromString(candidate)) {
                return *parsed;
            }
        }
    }
    return SampleFormat::Cf32;
}

class IqFileFactory final : public ISdrDeviceFactory {
public:
    [[nodiscard]] std::string_view driver() const noexcept override { return "iqfile"; }
    [[nodiscard]] std::string displayName() const override { return "IQ file replay"; }

    [[nodiscard]] std::vector<SdrDeviceInfo> enumerate() const override {
        // Not enumerable: the user supplies a path. Returning nothing keeps it
        // out of the device chip's list until explicitly opened.
        return {};
    }

    [[nodiscard]] Result<std::unique_ptr<ISdrDevice>> open(std::string_view id) override {
        if (id.empty()) {
            return fail<std::unique_ptr<ISdrDevice>>(
                ErrorCode::InvalidArgument,
                "the iqfile driver needs a path, e.g. iqfile:/path/to/capture.cf32");
        }

        const fs::path path(id);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) {
            return fail<std::unique_ptr<ISdrDevice>>(ErrorCode::NotFound, "no such file: {}",
                                                     path.string());
        }

        const SampleFormat format = formatFromPath(path);
        const std::uint64_t size = fs::file_size(path, ec);

        SdrDeviceInfo info{.driver = "iqfile",
                           .id = path.string(),
                           .label = std::format("IQ file: {}", path.filename().string()),
                           .serial = {},
                           .hardwareRevision = std::string(toString(format)),
                           .firmware = {},
                           .minFrequencyHz = 0.0,
                           .maxFrequencyHz = 6e9,
                           .minSampleRate = 1e3,
                           .maxSampleRate = 200e6,
                           .linkCapacityBytesPerSec = 0,
                           .linkDescription =
                               std::format("file, {} frames", size / bytesPerFrame(format))};

        return std::make_unique<IqFileDevice>(std::move(info), path, format);
    }
};

} // namespace

void registerIqFileDevice() {
    SdrDeviceManager::instance().registerFactory(std::make_unique<IqFileFactory>());
}

void registerBuiltinSdrDevices() {
    static std::once_flag once;
    std::call_once(once, [] {
        registerSyntheticDevice();
        registerIqFileDevice();
    });
}

} // namespace sweeppp
