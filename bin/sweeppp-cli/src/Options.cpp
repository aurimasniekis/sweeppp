// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Options.hpp"

#include <print>
#include <string_view>
#include <sweeppp/core/Toml.hpp>

namespace sweeppp::cli {
namespace {

Result<Command> commandFromString(std::string_view name) {
    if (name == "sweep") {
        return Command::Sweep;
    }
    if (name == "record") {
        return Command::Record;
    }
    if (name == "serve") {
        return Command::Serve;
    }
    if (name == "replay") {
        return Command::Replay;
    }
    if (name == "calibrate") {
        return Command::Calibrate;
    }
    if (name == "info") {
        return Command::Info;
    }
    if (name == "extract") {
        return Command::Extract;
    }
    if (name == "help" || name == "--help" || name == "-h") {
        return Command::Help;
    }
    if (name == "version" || name == "--version") {
        return Command::Version;
    }

    return fail<Command>(ErrorCode::InvalidArgument, "unknown command '{}'", name);
}

/// Parses `--key value` and `--key=value` alike, since both are muscle memory.
struct ArgumentReader {
    int argc = 0;
    char** argv = nullptr;
    int index = 0;

    [[nodiscard]] bool done() const noexcept { return index >= argc; }
    [[nodiscard]] std::string_view peek() const { return argv[index]; }
    std::string_view take() { return argv[index++]; }

    [[nodiscard]] Result<std::string> value(std::string_view flag, std::string_view inlineValue) {
        if (!inlineValue.empty()) {
            return std::string(inlineValue);
        }
        if (done()) {
            return fail<std::string>(ErrorCode::InvalidArgument, "{} needs a value", flag);
        }
        return std::string(take());
    }
};

} // namespace

Result<Options> parseArguments(int argc, char** argv) {
    Options options;

    if (argc < 2) {
        options.command = Command::Help;
        return options;
    }

    auto command = commandFromString(argv[1]);
    if (!command) {
        return std::unexpected(command.error());
    }
    options.command = *command;

    ArgumentReader reader{.argc = argc, .argv = argv, .index = 2};

    while (!reader.done()) {
        std::string_view argument = reader.take();

        // A bare word after the command is the positional input path, which is
        // what `sweeppp-cli info session.sweeps` should obviously mean.
        //
        // Tested against "-", not "--": short flags such as -o and -p must not
        // fall into this branch. A lone "-" is the stdout convention and is a
        // value, not a flag.
        if (argument == "-" || !argument.starts_with("-")) {
            options.inputPath = std::string(argument);
            continue;
        }

        std::string_view inlineValue;
        if (const std::size_t equals = argument.find('='); equals != std::string_view::npos) {
            inlineValue = argument.substr(equals + 1);
            argument = argument.substr(0, equals);
        }

        const auto stringValue = [&]() { return reader.value(argument, inlineValue); };

        const auto frequencyValue = [&]() -> Result<double> {
            auto text = stringValue();
            if (!text) {
                return std::unexpected(text.error());
            }
            return toml_util::parseFrequency(*text);
        };

        const auto durationValue = [&]() -> Result<double> {
            auto text = stringValue();
            if (!text) {
                return std::unexpected(text.error());
            }
            return toml_util::parseDuration(*text);
        };

        const auto intValue = [&]() -> Result<std::int64_t> {
            auto text = stringValue();
            if (!text) {
                return std::unexpected(text.error());
            }
            auto parsed = toml_util::parseFrequency(*text);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            return static_cast<std::int64_t>(*parsed);
        };

        if (argument == "--device") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.device = *value;
            options.deviceGiven = true;
        } else if (argument == "--param" || argument == "-p") {
            // --param key=value, repeatable. Applied through the generic
            // parameter model, so any device's full surface is reachable from
            // the command line with no per-device flags.
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            const std::size_t equals = value->find('=');
            if (equals == std::string::npos) {
                return fail<Options>(ErrorCode::InvalidArgument,
                                     "--param expects key=value, got '{}'", *value);
            }
            options.deviceParameters[value->substr(0, equals)] = value->substr(equals + 1);
        } else if (argument == "--route-antennas") {
            options.antennaRouting = true;
        } else if (argument == "--rx-port") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.rxPort = *value;
        } else if (argument == "--fft-backend") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.fftBackend = *value;
        } else if (argument == "--fft-size") {
            auto value = intValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.fftSize = static_cast<std::uint32_t>(*value);
        } else if (argument == "--window") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.window = *value;
        } else if (argument == "--overlap") {
            auto value = frequencyValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.overlap = *value;
        } else if (argument == "--average") {
            auto value = intValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.averageCount = static_cast<std::uint32_t>(*value);
        } else if (argument == "--workers") {
            auto value = intValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.workerCount = static_cast<std::uint32_t>(*value);
        } else if (argument == "--no-dc-removal") {
            options.dcRemoval = false;
        } else if (argument == "--flatten") {
            options.flatten = true;
        } else if (argument == "--spur-mask") {
            options.spurMask = true;
        } else if (argument == "--calibration") {
            auto value = reader.value(argument, inlineValue);
            if (!value) {
                return std::unexpected(value.error());
            }
            options.calibrationPath = *value;
        } else if (argument == "--sample-rate") {
            auto value = frequencyValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.sampleRate = *value;
        } else if (argument == "--center") {
            auto value = frequencyValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.centerHz = *value;
        } else if (argument == "--start") {
            auto value = frequencyValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.startHz = *value;
        } else if (argument == "--stop") {
            auto value = frequencyValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.stopHz = *value;
        } else if (argument == "--rbw") {
            auto value = frequencyValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.rbwHz = *value;
        } else if (argument == "--throttle") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.throttle = *value;
        } else if (argument == "--every-nth") {
            auto value = intValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.everyNth = static_cast<std::uint32_t>(*value);
        } else if (argument == "--duration") {
            auto value = durationValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.durationSeconds = *value;
        } else if (argument == "--output" || argument == "-o") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.outputPath = *value;
        } else if (argument == "--input" || argument == "-i") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.inputPath = *value;
        } else if (argument == "--listen") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.listenAddress = *value;
        } else if (argument == "--port") {
            auto value = intValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.port = static_cast<std::uint16_t>(*value);
        } else if (argument == "--token") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.token = *value;
        } else if (argument == "--token-file") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.tokenFile = *value;
        } else if (argument == "--from") {
            auto value = durationValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.fromSeconds = *value;
        } else if (argument == "--to") {
            auto value = durationValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.toSeconds = *value;
        } else if (argument == "--config-dir") {
            auto value = stringValue();
            if (!value) {
                return std::unexpected(value.error());
            }
            options.configDir = *value;
        } else if (argument == "--stats") {
            options.stats = true;
        } else if (argument == "--quiet" || argument == "-q") {
            options.quiet = true;
        } else if (argument == "--verbose" || argument == "-v") {
            options.verbose = true;
        } else if (argument == "--help" || argument == "-h") {
            options.command = Command::Help;
            return options;
        } else {
            return fail<Options>(ErrorCode::InvalidArgument, "unknown option '{}'", argument);
        }
    }

    return options;
}

void printUsage() {
    std::println(R"(sweeppp-cli -- headless sweeping, recording and remote serving

USAGE
  sweeppp-cli <command> [options]

COMMANDS
  sweep      Sweep and write CSV. The throughput and correctness harness.
  calibrate  Learn the receiver's floor and spurs. Disconnect the antenna first.
  record     Capture to a .sweeps session file, no GUI.
  serve      Run the engine for a remote desktop to connect to.
  replay     Play back a .sweeps file.
  info       Describe devices and FFT backends, or a .sweeps file.
  extract    Cut a time+frequency range out of a .sweeps file.
  help       Print this usage.
  version    Print the version and build details.

SOURCE
  --device <spec>          driver or driver:id (default: synthetic)
                           e.g. synthetic, hackrf, iqfile:/path/capture.cf32
  --param key=value        set any device parameter; repeatable
  --rx-port <id>           which RF input to listen on; `info` lists them
  --route-antennas         sweep each band through the antenna that covers it
  --sample-rate <freq>     e.g. 20M, 100e6
  --center <freq>          fixed-tune centre frequency

SWEEP
  --start <freq>           sweep start; with --stop, sweeps instead of fixed-tune
  --stop <freq>            sweep stop
  --rbw <freq>             target resolution bandwidth; sets FFT size

ANALYSIS
  --fft-backend <name>     default: the suggested backend (see `info`)
  --fft-size <n>           default: 4096
  --window <name>          rectangular, hann, hamming, blackman-harris,
                           flat-top, kaiser (default: hann)
  --overlap <fraction>     0..0.95
  --average <n>            FFTs averaged per emitted frame
  --workers <n>            0 = cores - 2
  --no-dc-removal          keep the LO leak; DC removal is on by default
  --flatten                subtract the learned floor shape
  --spur-mask              interpolate across the learned spurs
  --calibration <file>     calibration to read (sweep) or write (calibrate);
                           default: the radio's own in the config folder

THROUGHPUT
  --throttle <mode>        auto | every-nth | all-samples (default: auto)
  --every-nth <n>          with --throttle=every-nth
  --stats                  print the telemetry summary on exit

OUTPUT
  --duration <time>        e.g. 30, 500ms, 2min (default: 5)
  -o, --output <path>      CSV or .sweeps destination; '-' means stdout
  -i, --input <path>       source .sweeps file for replay/info/extract

SERVE
  --listen <address>       default 127.0.0.1; 0.0.0.0 for every interface
  --port <n>               default 7332; 0 picks a free one
  --token <secret>         required for any non-loopback listener
  --token-file <path>      the token from a file; or set SWEEPPP_REMOTE_TOKEN

EXTRACT
  --from <time>            offset from session start
  --to <time>

CONFIG
  --config-dir <dir>       use this folder instead of the default config folder

EXAMPLES
  # Throughput acceptance test: must sustain the rate, and the sample
  # accounting must reconcile exactly.
  sweeppp-cli sweep --device synthetic --sample-rate 100e6 --duration 30 --stats

  # Sweep 2.4 GHz for Wi-Fi, 10 kHz RBW, to CSV
  sweeppp-cli sweep --start 2.4G --stop 2.5G --rbw 10k -o wifi.csv

  # Learn a bladeRF's floor and spurs with no antenna, then sweep through them
  sweeppp-cli calibrate --device bladerf --start 2300M --stop 3400M --sample-rate 61.44M
  sweeppp-cli sweep --device bladerf --start 2300M --stop 3400M --flatten --spur-mask

  # Serve a HackRF to desktops on the local network
  sweeppp-cli serve --device hackrf --listen 0.0.0.0 --token-file ~/.sweeppp-token

  # What is available in this build?
  sweeppp-cli info)");
}

} // namespace sweeppp::cli
