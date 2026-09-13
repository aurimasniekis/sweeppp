// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/EventBus.hpp"
#include "sweeppp/core/Result.hpp"
#include "sweeppp/history/SessionReader.hpp"
#include "sweeppp/pipeline/FrameBus.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace sweeppp {

using session::SessionReader;
using session::SessionSummary;

/// Where frames come from.
///
/// Three implementations -- LivePipeline, SessionReplay and RemoteSource --
/// all feed the same FrameBus. The UI, plugins, history store and server bind
/// to the bus and **cannot tell which is behind it**. That is the whole
/// abstraction: replaying a session and watching a radio are the same code
/// path, and so is driving a radio on another machine.
class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    IFrameSource(const IFrameSource&) = delete;
    IFrameSource& operator=(const IFrameSource&) = delete;

    [[nodiscard]] virtual std::string_view sourceName() const noexcept = 0;

    [[nodiscard]] virtual Status start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;

    /// True for a source with a finite, seekable extent -- a session file.
    /// A live radio and a remote link are not seekable, and the transport
    /// controls hide themselves accordingly.
    [[nodiscard]] virtual bool seekable() const noexcept { return false; }

    /// Extent in seconds, or 0 for a live source.
    [[nodiscard]] virtual double durationSeconds() const noexcept { return 0.0; }
    [[nodiscard]] virtual double positionSeconds() const noexcept { return 0.0; }

    virtual void seek(double /*seconds*/) {}
    virtual void setPaused(bool /*paused*/) {}
    [[nodiscard]] virtual bool paused() const noexcept { return false; }

    /// Playback rate multiplier. 1.0 is real time.
    virtual void setSpeed(double /*multiplier*/) {}
    [[nodiscard]] virtual double speed() const noexcept { return 1.0; }

    /// Advance exactly one frame while paused.
    virtual void step() {}

protected:
    IFrameSource() = default;
};

/// Plays a `.sweeps` file back onto a FrameBus.
///
/// Reproduces the frames *and* the event stream, so on playback the RBW / FFT
/// size / gain / span readouts change at the same moments they did live. That
/// is the difference between a video of a waterfall and a session that behaves
/// like a radio.
class SessionReplay final : public IFrameSource {
public:
    static constexpr double kMinSpeed = 0.25;
    static constexpr double kMaxSpeed = 16.0;

    [[nodiscard]] static Result<std::unique_ptr<SessionReplay>>
    open(const std::filesystem::path& path, FrameBus& output, EventBus& events);

    ~SessionReplay() override;

    [[nodiscard]] std::string_view sourceName() const noexcept override { return "replay"; }

    [[nodiscard]] Status start() override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override {
        return m_running.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool seekable() const noexcept override { return true; }
    [[nodiscard]] double durationSeconds() const noexcept override;
    [[nodiscard]] double positionSeconds() const noexcept override;

    void seek(double seconds) override;
    void setPaused(bool paused) override;
    [[nodiscard]] bool paused() const noexcept override {
        return m_paused.load(std::memory_order_relaxed);
    }

    void setSpeed(double multiplier) override;
    [[nodiscard]] double speed() const noexcept override {
        return m_speed.load(std::memory_order_relaxed);
    }

    void step() override;

    [[nodiscard]] const SessionReader& reader() const noexcept { return *m_reader; }

    /// Fires once playback reaches the end of the session.
    void setCompletionCallback(std::function<void()> callback) {
        m_onComplete = std::move(callback);
    }

private:
    SessionReplay(std::unique_ptr<SessionReader> reader, FrameBus& output, EventBus& events);

    void replayLoop(std::stop_token stop);
    void emitLine(std::uint32_t segmentId, std::uint64_t lineNs);
    void emitLineForPosition(std::uint64_t positionNs);
    void applyEventsUpTo(std::uint64_t monotonicNs);

    std::unique_ptr<SessionReader> m_reader;
    FrameBus& m_output;
    EventBus& m_events;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<double> m_speed{1.0};
    std::atomic<std::uint64_t> m_positionNs{0};
    std::atomic<std::int64_t> m_pendingSteps{0};
    std::atomic<std::uint64_t> m_seekTargetNs{0};
    std::atomic<bool> m_seekRequested{false};
    std::atomic<std::uint64_t> m_frameSequence{0};

    std::size_t m_nextEventIndex = 0;
    std::function<void()> m_onComplete;
    std::jthread m_thread;
};

/// Wraps the live pipeline as an IFrameSource, so the UI's source selector
/// treats a radio, a session file and a remote node identically.
class LivePipelineSource final : public IFrameSource {
public:
    LivePipelineSource(std::function<Status()> startFn, std::function<void()> stopFn,
                       std::function<bool()> runningFn)
        : m_start(std::move(startFn)), m_stop(std::move(stopFn)), m_running(std::move(runningFn)) {}

    [[nodiscard]] std::string_view sourceName() const noexcept override { return "live"; }
    [[nodiscard]] Status start() override { return m_start(); }
    void stop() override { m_stop(); }
    [[nodiscard]] bool running() const noexcept override { return m_running(); }

private:
    std::function<Status()> m_start;
    std::function<void()> m_stop;
    std::function<bool()> m_running;
};

} // namespace sweeppp
