// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/sdr/ISdrDevice.hpp"

#include "sweeppp/core/BlockPool.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/dsp/Convert.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace sweeppp {
namespace {

/// Pull-mode adapter state.
///
/// Deliberately per-call rather than a member: readSamples() is a convenience
/// for tests and simple tools, and giving it permanent state on every device
/// would suggest it is the main path. It is not -- it converts to float and
/// cannot carry a high-rate stream.
struct PullState {
    std::mutex mutex;
    std::condition_variable ready;
    std::complex<float>* out = nullptr;
    std::size_t wanted = 0;
    std::size_t written = 0;
    bool done = false;
};

} // namespace

Result<std::size_t> ISdrDevice::readSamples(std::complex<float>* out, std::size_t frames) {
    if (out == nullptr || frames == 0) {
        return fail<std::size_t>(ErrorCode::InvalidArgument,
                                 "readSamples needs a destination and a non-zero count");
    }
    if (streaming()) {
        return fail<std::size_t>(ErrorCode::Unavailable,
                                 "readSamples cannot be used while a callback stream is running");
    }

    // A pool just big enough to keep the device fed while this call drains it.
    constexpr std::size_t kFramesPerBlock = 65'536;
    constexpr std::uint32_t kBlocks = 8;

    auto pool = BlockPool::create(kFramesPerBlock * bytesPerFrame(nativeFormat()), kBlocks);
    if (!pool) {
        return std::unexpected(pool.error());
    }

    PullState state;
    state.out = out;
    state.wanted = frames;

    const StreamConfig config{
        .framesPerBlock = kFramesPerBlock, .blockCount = kBlocks, .format = nativeFormat()};

    auto started = start(**pool, config, [&state](IqBlock&& block) {
        const std::lock_guard lock(state.mutex);
        if (state.done) {
            return;
        }

        const std::size_t take = std::min(block.frames, state.wanted - state.written);
        dsp::convertToComplexFloat(block.data(), block.format, state.out + state.written, take);
        state.written += take;

        if (state.written >= state.wanted) {
            state.done = true;
            state.ready.notify_one();
        }
    });
    if (!started) {
        return std::unexpected(started.error());
    }

    {
        std::unique_lock lock(state.mutex);
        // Bounded so a device that stops delivering cannot hang the caller
        // forever; a partial read is reported as such.
        const bool complete =
            state.ready.wait_for(lock, std::chrono::seconds(10), [&state] { return state.done; });
        state.done = true;
        if (!complete) {
            logWarn("sdr", "readSamples timed out with {}/{} frames", state.written, state.wanted);
        }
    }

    stop();
    return state.written;
}

SdrDeviceManager& SdrDeviceManager::instance() {
    static SdrDeviceManager manager;
    return manager;
}

void SdrDeviceManager::registerFactory(std::unique_ptr<ISdrDeviceFactory> factory) {
    if (!factory) {
        return;
    }

    std::shared_ptr<ISdrDeviceFactory> shared(std::move(factory));

    const std::lock_guard lock(m_mutex);
    const auto existing =
        std::ranges::find_if(m_factories, [&shared](const std::shared_ptr<ISdrDeviceFactory>& c) {
            return c->driver() == shared->driver();
        });
    if (existing != m_factories.end()) {
        *existing = std::move(shared);
        return;
    }
    m_factories.push_back(std::move(shared));
}

bool SdrDeviceManager::hasDriver(std::string_view driver) const {
    const std::lock_guard lock(m_mutex);
    return std::ranges::any_of(m_factories,
                               [driver](const std::shared_ptr<ISdrDeviceFactory>& candidate) {
                                   return candidate->driver() == driver;
                               });
}

Status SdrDeviceManager::unregisterFactory(std::string_view driver) {
    const std::lock_guard lock(m_mutex);

    const auto found =
        std::ranges::find_if(m_factories, [driver](const std::shared_ptr<ISdrDeviceFactory>& c) {
            return c->driver() == driver;
        });
    if (found == m_factories.end()) {
        return fail(ErrorCode::NotFound, "no SDR driver named '{}'", driver);
    }

    if (const std::string blocker = (*found)->withdrawalBlocker(); !blocker.empty()) {
        return fail(ErrorCode::Unavailable, "SDR driver '{}' is still in use: {}", driver, blocker);
    }

    m_factories.erase(found);
    return ok();
}

std::vector<SdrDeviceInfo> SdrDeviceManager::enumerateAll() const {
    // Snapshot under the lock, enumerate outside it. Each of these is a USB bus
    // scan; holding the registry mutex across all of them would block every
    // other caller for the sum of them, and the UI asks on a timer.
    std::vector<std::shared_ptr<ISdrDeviceFactory>> factories;
    {
        const std::lock_guard lock(m_mutex);
        factories = m_factories;
    }

    std::vector<SdrDeviceInfo> devices;
    for (const std::shared_ptr<ISdrDeviceFactory>& factory : factories) {
        // One driver failing to enumerate -- a permissions problem, a wedged
        // USB device -- must not hide every other radio on the machine.
        try {
            std::vector<SdrDeviceInfo> found = factory->enumerate();
            devices.insert(devices.end(), std::make_move_iterator(found.begin()),
                           std::make_move_iterator(found.end()));
        } catch (const std::exception& error) {
            logWarn("sdr", "driver '{}' failed to enumerate: {}", factory->driver(), error.what());
        }
    }
    return devices;
}

Result<std::unique_ptr<ISdrDevice>> SdrDeviceManager::open(std::string_view driver,
                                                           std::string_view id) {
    std::shared_ptr<ISdrDeviceFactory> factory;
    {
        const std::lock_guard lock(m_mutex);
        const auto found = std::ranges::find_if(
            m_factories, [driver](const std::shared_ptr<ISdrDeviceFactory>& c) {
                return c->driver() == driver;
            });
        if (found == m_factories.end()) {
            return fail<std::unique_ptr<ISdrDevice>>(ErrorCode::NotFound,
                                                     "no SDR driver named '{}'", driver);
        }
        factory = *found;
    }

    // Opening can be slow (USB enumeration, firmware handshake) and must not
    // hold the registry lock -- the UI enumerates on a timer. The copied
    // shared_ptr is what makes calling out here safe: a plugin withdrawn on
    // another thread meanwhile drops its entry from the vector, not the object
    // this call is standing on.
    return factory->open(id);
}

Result<std::unique_ptr<ISdrDevice>> SdrDeviceManager::openSpecifier(std::string_view specifier) {
    const std::size_t colon = specifier.find(':');
    const std::string_view driver = specifier.substr(0, colon);
    const std::string_view id =
        colon == std::string_view::npos ? std::string_view{} : specifier.substr(colon + 1);
    return open(driver, id);
}

std::vector<std::string> SdrDeviceManager::drivers() const {
    const std::lock_guard lock(m_mutex);
    std::vector<std::string> names;
    names.reserve(m_factories.size());
    for (const std::shared_ptr<ISdrDeviceFactory>& factory : m_factories) {
        names.emplace_back(factory->driver());
    }
    return names;
}

} // namespace sweeppp
