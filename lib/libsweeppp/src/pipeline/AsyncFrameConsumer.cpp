// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/pipeline/AsyncFrameConsumer.hpp"

#include "sweeppp/core/Log.hpp"

namespace sweeppp {

AsyncFrameConsumer::AsyncFrameConsumer(std::string name, std::size_t queueDepth)
    : m_name(std::move(name)), m_queueDepth(queueDepth == 0 ? 1 : queueDepth) {
}

AsyncFrameConsumer::~AsyncFrameConsumer() {
    AsyncFrameConsumer::shutdown();
}

void AsyncFrameConsumer::startWorker() {
    if (m_thread.joinable()) {
        return;
    }
    m_thread = std::jthread([this](std::stop_token stop) { workerLoop(stop); });
}

void AsyncFrameConsumer::onFrame(const SpectrumFramePtr& frame) noexcept {
    if (!frame) {
        return;
    }

    {
        const std::lock_guard lock(m_mutex);
        if (m_stopping) {
            return;
        }
        if (m_queue.size() >= m_queueDepth) {
            // Full. Drop the newest and count it, rather than blocking the
            // publishing worker -- the whole point of this class.
            //
            // Dropping the *newest* rather than the oldest keeps the frames
            // this consumer does process contiguous, which matters when it is
            // writing a session file: a contiguous run with a gap at the end
            // is recoverable, an interleaved one is not.
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_queue.push_back(frame);
    }
    m_notEmpty.notify_one();
}

void AsyncFrameConsumer::workerLoop(std::stop_token stop) {
    onStart();

    while (true) {
        SpectrumFramePtr frame;
        {
            std::unique_lock lock(m_mutex);
            m_notEmpty.wait(lock, [this, &stop] {
                return !m_queue.empty() || m_stopping || stop.stop_requested();
            });

            if (m_queue.empty()) {
                if (m_stopping || stop.stop_requested()) {
                    break;
                }
                continue;
            }

            frame = std::move(m_queue.front());
            m_queue.pop_front();
        }

        try {
            processFrame(frame);
            m_processed.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& error) {
            // A consumer failing is that consumer's problem. Log it and keep
            // going; the alternative is a recorder that dies silently.
            logError("consumer", "{}: {}", m_name, error.what());
        } catch (...) {
            logError("consumer", "{}: unknown error", m_name);
        }

        // Wake anyone waiting in flush() once the backlog is gone.
        {
            const std::lock_guard lock(m_mutex);
            if (m_queue.empty()) {
                m_drained.notify_all();
            }
        }
    }

    onStop();
}

std::size_t AsyncFrameConsumer::queueDepth() const {
    const std::lock_guard lock(m_mutex);
    return m_queue.size();
}

void AsyncFrameConsumer::flush() {
    if (!m_thread.joinable()) {
        return;
    }
    std::unique_lock lock(m_mutex);
    m_drained.wait(lock, [this] { return m_queue.empty(); });
}

void AsyncFrameConsumer::shutdown() {
    if (!m_thread.joinable()) {
        return;
    }

    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_thread.request_stop();
    m_notEmpty.notify_all();
    m_thread.join();
}

} // namespace sweeppp
