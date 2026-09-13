// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <sweeppp/core/Result.hpp>
#include <vector>

namespace sweeppp::cli {

/// Subcommands. All of them work except `serve`, which reports that it is not
/// available yet rather than failing obscurely.
enum class Command {
    Sweep,   ///< Headless sweep to CSV. The perf and CI harness.
    Record,  ///< Headless capture to .sweeps.
    Serve,   ///< Remote server: engine plus protocol endpoint.
    Replay,  ///< Play back a .sweeps file.
    Info,    ///< Describe devices, backends, or a .sweeps file.
    Extract, ///< Cut a time+frequency range out of a .sweeps file.
    Help,
    Version,
};

struct Options {
    Command command = Command::Help;

    std::string device = "synthetic";

    /// Whether `--device` was actually given. `info` opens the named radio to
    /// describe its RF inputs, and doing that to the default would claim a
    /// device the operator never mentioned.
    bool deviceGiven = false;

    std::map<std::string, std::string> deviceParameters;

    /// Which RF input to listen on, by `SdrRxPort::id`. Empty leaves whichever
    /// the driver came up on.
    ///
    /// Its own flag rather than `--param`, because a receive port is not a
    /// parameter: it has frequency limits and a switching cost the generic
    /// model has nowhere to put, and `info` lists the ids this accepts.
    std::string rxPort;

    /// Sweep each band through the antenna that covers it. Needs antennas
    /// assigned to the radio's connectors, which the desktop writes and this
    /// reads -- see `sweeppp-cli info --device ...`.
    bool antennaRouting = false;

    std::string fftBackend;
    std::uint32_t fftSize = 4096;
    std::string window = "hann";
    double overlap = 0.0;
    std::uint32_t averageCount = 1;
    std::uint32_t workerCount = 0;

    double sampleRate = 20e6;
    double centerHz = 100e6;
    double startHz = 0.0;
    double stopHz = 0.0;
    double rbwHz = 0.0;

    std::string throttle = "auto";
    std::uint32_t everyNth = 1;

    double durationSeconds = 5.0;
    std::string outputPath;
    std::string inputPath;

    /// Print the telemetry summary. The acceptance test for the throughput
    /// requirement reads its numbers from here.
    bool stats = false;
    bool quiet = false;
    bool verbose = false;

    /// Config root to use instead of the platform default. Empty keeps it.
    std::string configDir;

    /// serve
    std::string listenAddress = "127.0.0.1";
    std::uint16_t port = 7332;
    std::string token;

    /// extract
    double fromSeconds = 0.0;
    double toSeconds = 0.0;
};

[[nodiscard]] Result<Options> parseArguments(int argc, char** argv);

void printUsage();

} // namespace sweeppp::cli
