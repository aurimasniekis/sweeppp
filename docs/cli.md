# Command line

Sweep++ comes with three command-line programs besides the GUI:

- [`sweeppp-cli`](#sweeppp-cli) sweeps, records and inspects without a window;
- [`sweeps`](#sweeps) reads `.sweeps` recordings, with nothing else from
  Sweep++ needed;
- [`sweeppp-server`](#sweeppp-server) is a placeholder that doesn't do anything
  yet.

When built from source they're in `build/<preset>/dist/`. The `.deb` package
installs them to `/usr/bin`.

## `sweeppp` (the GUI)

```sh
sweeppp                           # start normally
sweeppp recording.sweeps          # open a recording in the History viewer
sweeppp --history recording.sweeps
sweeppp --history                 # open an empty History viewer
sweeppp --config-dir ~/sweeppp-bench  # keep settings in another folder
```

`--config-dir <folder>` (or `--config-dir=<folder>`) uses that folder instead
of the [default one](user-guide.md#where-settings-are-stored). It is created if
it doesn't exist, and plugins and the History viewer use it too.

On macOS, run the binary inside the bundle:
`Sweep++.app/Contents/MacOS/Sweep++` (or `"Sweep++ Nightly.app/Contents/MacOS/Sweep++ Nightly"`).

## `sweeppp-cli`

```sh
sweeppp-cli <command> [options]
```

| Command               | Does                                                                     |
|-----------------------|--------------------------------------------------------------------------|
| [`sweep`](#sweep)     | Sweep or tune, and write the spectrum as CSV.                            |
| [`record`](#record)   | Record to a `.sweeps` file.                                              |
| [`replay`](#replay)   | Play a `.sweeps` file back as CSV.                                       |
| [`info`](#info)       | Describe this build (FFT engines, radios, plugins), or a `.sweeps` file. |
| [`extract`](#extract) | Cut a time and frequency range out of a `.sweeps` file.                  |
| `serve`               | **Not available yet.** Exits with an error.                              |
| `help`                | Print the usage.                                                         |
| `version`             | Print the version and build details.                                     |

Options can be written as `--option value` or `--option=value`. A word with no
`--` in front of it is taken as the input file.

### Values

- **Frequencies** take a suffix: `100M`, `2.4G`, `10k`, `433.92MHz`, or plain
  hertz such as `100e6`.
- **Durations** take a suffix: `500ms`, `30s`, `2min`, `1h`, or plain seconds.
- **Devices** are written `driver` or `driver:id`:
  - `synthetic` (the default), `hackrf`, `bladerf`, `rtlsdr`, `fobos`;
  - `hackrf:<serial>` to pick one of several radios. `sweeppp-cli info` lists
    the ids;
  - `iqfile:/path/to/capture.cf32` to replay a raw IQ file. See
    [IQ file](devices.md#iq-file).

### Options

**Source**

| Option                    | Default     | Meaning                                                                                      |
|---------------------------|-------------|----------------------------------------------------------------------------------------------|
| `--device <spec>`         | `synthetic` | Which radio. See [Values](#values).                                                          |
| `-p`, `--param key=value` |             | Set any radio setting by its key. Repeat for several. See [Radio settings](#radio-settings). |
| `--rx-port <id>`          |             | Which input to listen on. `info --device …` lists them.                                      |
| `--route-antennas`        | off         | Sweep each band through the antenna that covers it, using your saved antenna assignments.    |
| `--sample-rate <freq>`    | `20M`       | Sample rate.                                                                                 |
| `--center <freq>`         | `100M`      | Centre frequency when not sweeping.                                                          |

**Sweep**

| Option           | Meaning                                                              |
|------------------|----------------------------------------------------------------------|
| `--start <freq>` | Sweep start. With `--stop`, sweeps instead of staying on `--center`. |
| `--stop <freq>`  | Sweep stop.                                                          |
| `--rbw <freq>`   | Target resolution bandwidth. Sets the FFT size.                      |

**Analysis**

| Option                 | Default         | Meaning                                                                      |
|------------------------|-----------------|------------------------------------------------------------------------------|
| `--fft-backend <name>` | first available | `pocketfft`, `fftw` or `accelerate`.                                         |
| `--fft-size <n>`       | `4096`          | FFT points, when `--rbw` isn't given.                                        |
| `--window <name>`      | `hann`          | `rectangular`, `hann`, `hamming`, `blackman-harris`, `flat-top` or `kaiser`. |
| `--overlap <fraction>` |                 | Overlap between FFTs, 0 to 0.95.                                             |
| `--average <n>`        | `1`             | FFTs averaged per output frame.                                              |
| `--workers <n>`        | `0`             | Analysis threads; 0 means all cores but two.                                 |

**Throughput**

| Option              | Default | Meaning                                               |
|---------------------|---------|-------------------------------------------------------|
| `--throttle <mode>` | `auto`  | `auto`, `every-nth` or `all-samples`.                 |
| `--every-nth <n>`   |         | With `--throttle every-nth`: analyse one buffer in N. |
| `--stats`           | off     | Print statistics when finished.                       |

**Output**

| Option                         | Default | Meaning                                                         |
|--------------------------------|---------|-----------------------------------------------------------------|
| `--duration <time>`            | `5`     | How long to run.                                                |
| `-o`, `--output <path>`        | stdout  | Where to write. `-` means stdout.                               |
| `-i`, `--input <path>`         |         | The `.sweeps` file to read.                                     |
| `--from <time>`, `--to <time>` |         | For `extract`: the time range, from the start of the recording. |
| `-q`, `--quiet`                |         | Less output.                                                    |
| `-v`, `--verbose`              |         | More output.                                                    |

**Config**

| Option               | Default                                                      | Meaning                                                              |
|----------------------|--------------------------------------------------------------|----------------------------------------------------------------------|
| `--config-dir <dir>` | [the config folder](user-guide.md#where-settings-are-stored) | Use this folder for settings, antennas, plugins and the log instead. |

`--listen`, `--port` and `--token` are accepted but unused until `serve`
exists.

### Radio settings

`--param` takes the same settings the GUI's Device panel shows, by key.
Numbers accept the same suffixes as frequencies (`bandwidth=5M`). On/off
settings take `true` or `false` (`on`/`off`, `yes`/`no` and `1`/`0` work too).

| Radio     | Keys                                                                    |
|-----------|-------------------------------------------------------------------------|
| HackRF    | `bandwidth`, `amp_enable`, `lna_gain`, `vga_gain`, `bias_tee`           |
| bladeRF   | `bandwidth`, `gain_mode`, `gain`, `bias_tee`, `packed_link`             |
| RTL-SDR   | `bandwidth`, `gain_mode`, `gain`, `rtl_agc`, `ppm`, `bias_tee`          |
| Fobos SDR | `bandwidth`, `lna_gain`, `vga_gain`, `clock_source`                     |
| Synthetic | `gain`, `noise_floor`, `emitters`, `sample_format`, `simulate_overruns` |
| IQ file   | `loop`, `realtime`                                                      |

Every radio also has `center_hz` and `sample_rate`, which `--center` and
`--sample-rate` already set.

```sh
sweeppp-cli sweep --device hackrf -p amp_enable=true -p lna_gain=24 -p vga_gain=20 \
    --start 88M --stop 108M --rbw 10k -o fm.csv
```

### `sweep`

Writes the spectrum as CSV.

- **With `--start` and `--stop`** it sweeps the range, and writes one row per
  completed pass.
- **Without them** it stays on `--center` and writes a row per frame.

```sh
# Sweep 2.4 GHz Wi-Fi at 10 kHz resolution for 30 seconds
sweeppp-cli sweep --device hackrf --start 2.4G --stop 2.5G --rbw 10k --duration 30 -o wifi.csv

# A quick test with the synthetic radio, printing statistics
sweeppp-cli sweep --start 88M --stop 108M --rbw 10k --stats -o /dev/null
```

The file starts with comment lines describing the radio and settings, then a
header row:

```
# sweeppp 0.1.0
# device: ...
# center_hz: ...
# sample_rate: ...
# fft_size: ...
# window: hann
# rbw_hz: ...
# enbw_bins: ...
sequence,host_time_ns,sweep_pass,sweep_step,start_hz,bin_width_hz,bin_count,bins_dbfs...
```

Each row gives the frequency of the first bin (`start_hz`), the spacing
(`bin_width_hz`) and the number of bins, followed by one level in dBFS per
bin.

`--stats` prints, when the run ends, the sample rate held, samples dropped and
where, and for a sweep, how much of the range was measured.

From a source checkout, `make sweep ARGS="…"` builds and runs a 5-second
synthetic sweep with `--stats`:

```sh
make sweep ARGS="--start 88M --stop 108M --rbw 10k"
```

### `record`

Records to a `.sweeps` file, like the GUI does.

```sh
sweeppp-cli record --device rtlsdr --center 433.92M --sample-rate 2.4M --duration 10min -o ism.sweeps
```

- `-o` is required.
- It records at a single centre frequency only. `--start`, `--stop`, `--rbw`
  and `--throttle` are ignored.
- A `--param` with an unknown key or a bad value is skipped without an error.

### `replay`

Plays a `.sweeps` file back and writes it as CSV, in the same format as
`sweep`.

```sh
sweeppp-cli replay ism.sweeps --duration 0 -o ism.csv
```

Pass `--duration 0` to replay the whole file at its recorded speed. With any
other `--duration` (including the default of 5) it plays at 16× speed.

### `info`

With no file, describes what this build can see:

```sh
sweeppp-cli info
sweeppp-cli info --device bladerf    # also lists the radio's inputs and antenna assignments
```

- the FFT engines, and why any are unavailable;
- the radios connected, with their `driver:id`, frequency range and sample
  rates;
- every plugin found, including ones that failed to load and the reason, and
  what each one adds;
- the datasets plugins provide;
- every folder searched for plugins, marking the ones that only accept
  `sweeppp-plugin-*` files and the ones that don't exist.

This is the first thing to run when a radio or plugin doesn't show up.

With a file, describes the recording, the same as `sweeps info`:

```sh
sweeppp-cli info ism.sweeps
```

### `extract`

Copies part of a recording into a new `.sweeps` file, without re-encoding
anything.

```sh
sweeppp-cli extract ism.sweeps --from 1min --to 2min --start 433M --stop 435M -o burst.sweeps
```

`--from` and `--to` are times from the start of the recording; `--start` and
`--stop` are frequencies. Leave any of them out to keep everything in that
direction.

## `sweeps`

`sweeps` reads `.sweeps` files. It is part of the MIT-licensed `.sweeps` library
and needs nothing else from Sweep++, so it can be built and shipped on its own.
See [`.sweeps` files](sweeps-files.md).

```sh
sweeps <command> <file> [options]
```

| Command                                                                                            | Does                                                                |
|----------------------------------------------------------------------------------------------------|---------------------------------------------------------------------|
| `info <file> [--events N]`                                                                         | Metadata, segments and the first N events (20 by default).          |
| `verify <file> [--quiet]`                                                                          | Check every record's checksum. Exits with 1 if the file is damaged. |
| `extract <file> -o <out> [--from S] [--to S] [--start HZ] [--stop HZ]`                             | Copy a time and frequency range to a new file.                      |
| `events <file> [--kind KIND]`                                                                      | The events as JSON Lines, one object per line.                      |
| `manifest <file>`                                                                                  | The recording's metadata as JSON.                                   |
| `plugins <file> [--plugin ID]`                                                                     | The plugin data stored in the recording.                            |
| `dump <file> [--segment N] [--lod N] [--from S] [--to S] [--start HZ] [--stop HZ] [--max-lines N]` | Levels as CSV, one row per waterfall line.                          |
| `version`                                                                                          | Library and format versions.                                        |

Unlike `sweeppp-cli`:

- **Numbers are plain.** Times are seconds and frequencies are hertz: write
  `--start 433000000`, not `--start 433M`.
- **Put the file first.** `-o` must come after the file name
  (`sweeps extract in.sweeps -o out.sweeps`). `--output` can go anywhere.

```sh
sweeps verify capture.sweeps
sweeps manifest capture.sweeps | jq -r .name
sweeps events capture.sweeps | jq -s 'group_by(.kind) | map({kind: .[0].kind, n: length})'
sweeps dump capture.sweeps --start 88000000 --stop 108000000 --max-lines 100 > fm.csv
```

## `sweeppp-server`

A placeholder for a future web interface. It prints its version and exits.

## Environment variables

| Variable              | Effect                                                                                          |
|-----------------------|-------------------------------------------------------------------------------------------------|
| `SWEEPPP_PLUGIN_PATH` | Extra folders to load plugins from, searched first. Separate several with `:` (`;` on Windows). |
| `XDG_CONFIG_HOME`     | On Linux, the config folder is `$XDG_CONFIG_HOME/sweeppp` when this is set.                     |

## Files

`sweeppp-cli` uses the same [config folder](user-guide.md#where-settings-are-stored)
as the GUI: plugins and plugin settings, antennas and antenna assignments (for
`--route-antennas`), and the log file `sweeppp.log`.
