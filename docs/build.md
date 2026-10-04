# Building from source

- [Requirements](#requirements)
- [Quick build](#quick-build)
- [Presets](#presets)
- [Options](#options)
- [Makefile targets](#makefile-targets)
- [Tests](#tests)
- [Code quality](#code-quality)
- [Windows](#windows)
- [Packages](#packages)
- [Updating a pinned dependency](#updating-a-pinned-dependency)

## Requirements

- **CMake 3.24** or newer
- **Ninja**
- **pkg-config** (or pkgconf)
- **A C++23 compiler:** GCC 14+, Clang 18+, or Apple clang from Xcode 26+
- **Node.js 20+ and pnpm**, for the browser UI (`corepack enable pnpm`), unless
  built with `SWEEPPP_WITH_WEB_UI=OFF`

CMake doesn't check the compiler version, so an older compiler fails partway
through the build rather than at configure time:

- **GCC 13 and older** lack `std::println` and `std::expected`, which the tree
  uses throughout. This is why Ubuntu 22.04 and Debian 12 aren't supported;
  on Ubuntu 24.04, install `g++-14`.
- **Xcode 16's libc++** has no `<stop_token>` or `std::jthread`. CI pins
  Xcode 26.3.

The first configure downloads the remaining dependencies (ImGui, ImPlot,
toml++, PocketFFT and others) into `build/<preset>/_deps`.
[`THIRD_PARTY.md`](../THIRD_PARTY.md) lists all of them.

### System libraries

**macOS** (Homebrew):

```sh
brew install cmake ninja pkgconf fftw glfw hackrf libbladerf librtlsdr libusb
```

libcurl, for the update check, comes with the macOS SDK.

**Debian / Ubuntu:**

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
                 libfftw3-dev libglfw3-dev libgl1-mesa-dev libgtk-3-dev libcurl4-openssl-dev \
                 libhackrf-dev libbladerf-dev librtlsdr-dev libusb-1.0-0-dev
```

CI installs the same list, plus the tools its packaging step needs; see
`LINUX_PACKAGES` in
[`.github/workflows/build.yml`](../.github/workflows/build.yml).

| Library               | Needed for                                      | Turn off with                   |
|-----------------------|-------------------------------------------------|---------------------------------|
| GLFW ≥ 3.3            | the GUI                                         | `SWEEPPP_BUILD_GUI=OFF`         |
| OpenGL, GTK 3 (Linux) | the GUI (GTK for the file dialog and clipboard) | `SWEEPPP_BUILD_GUI=OFF`         |
| libcurl               | the update check                                | `SWEEPPP_WITH_UPDATE_CHECK=OFF` |
| Node.js, pnpm         | the browser UI for `serve --web`                | `SWEEPPP_WITH_WEB_UI=OFF`       |
| FFTW (`fftw3f`)       | the FFTW plugin                                 | `SWEEPPP_WITH_FFTW=OFF`         |
| libhackrf             | the HackRF plugin                               | `SWEEPPP_WITH_HACKRF=OFF`       |
| libbladeRF            | the bladeRF plugin                              | `SWEEPPP_WITH_BLADERF=OFF`      |
| librtlsdr             | the RTL-SDR plugin                              | `SWEEPPP_WITH_RTLSDR=OFF`       |
| libusb-1.0            | the Fobos SDR plugin                            | `SWEEPPP_WITH_FOBOS=OFF`        |

You only need the libraries for the parts you build. Turn off what you don't
need rather than installing it.

When something is missing, configuration behaves differently depending on
what:

- **FFTW, libhackrf, libbladeRF, librtlsdr, libusb and GLFW** stop
  configuration with the `brew` and `apt` command that installs them.
- **GTK 3, OpenGL and pkg-config** stop with CMake's own "not found" error,
  with no install hint.
- **libcurl** only prints a warning, and the update check is turned off.

## Quick build

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Or with the Makefile:

```sh
make build    # configure and build the dev preset
make run      # build and start the GUI
make test     # build and run the tests
```

Everything lands in `build/<preset>/dist/`:

- `sweeppp` (on macOS, `Sweep++ Nightly.app`, or `Sweep++.app` from the
  `release` preset), `sweeppp-cli` and `sweeps`;
- the plugins, in `dist/plugins/` as `sweeppp-plugin-<name>.so`, `.dylib` or
  `.dll`;
- the test executables.

Nothing is copied into `dist/` from `resources/`. The binaries find the
source tree's `resources/` by walking up from their own directory, so they run
straight from the build tree.

## Presets

All presets use Ninja and build into `build/<preset>/`. Compiler warnings are on
in every preset; none of them turns warnings into errors.

| Preset     | Build type     | What's built                        | Notes                                                                    |
|------------|----------------|-------------------------------------|--------------------------------------------------------------------------|
| `dev`      | RelWithDebInfo | everything, with tests              | The default.                                                             |
| `debug`    | Debug          | everything, with tests              |                                                                          |
| `release`  | Release        | everything except tests             |                                                                          |
| `asan`     | Debug          | no GUI or server; plugins and tests | AddressSanitizer and UndefinedBehaviorSanitizer.                         |
| `tsan`     | Debug          | no GUI or server; plugins and tests | ThreadSanitizer, mainly for the acquisition pipeline.                    |
| `headless` | Release        | no GUI, server or tests             | For machines without a display, such as one running `sweeppp-cli serve`. |

- The sanitizer presets still build the plugins, so they still need FFTW and
  the radio libraries.
- `ctest --preset` works only for `dev`, `asan` and `tsan`. For any other
  preset, use `ctest --test-dir build/<preset>`.

## Options

Pass these to CMake with `-D<option>=ON|OFF`.

| Option                      | Default | Effect                                                                                                     |
|-----------------------------|---------|------------------------------------------------------------------------------------------------------------|
| `SWEEPPP_BUILD_GUI`         | `ON`    | The desktop GUI, `sweeppp`.                                                                                |
| `SWEEPPP_WITH_WEB_UI`       | `ON`    | The browser UI for `serve --web`, built with pnpm and compiled into `sweeppp-cli`. Needs Node 20 and pnpm. |
| `SWEEPPP_BUILD_PLUGINS`     | `ON`    | All bundled plugins. Off skips every plugin's system library.                                              |
| `SWEEPPP_BUILD_TESTS`       | `ON`    | The test suites, including libsweepsfile's.                                                                |
| `SWEEPPP_WITH_FFTW`         | `ON`    | The FFTW plugin. Needs `fftw3f`.                                                                           |
| `SWEEPPP_WITH_ACCELERATE`   | `ON`    | The Accelerate plugin. macOS only; ignored elsewhere.                                                      |
| `SWEEPPP_WITH_POCKETFFT`    | `ON`    | The PocketFFT plugin. Downloaded at build time; builds everywhere.                                         |
| `SWEEPPP_WITH_HACKRF`       | `ON`    | The HackRF plugin. Needs `libhackrf`.                                                                      |
| `SWEEPPP_WITH_BLADERF`      | `ON`    | The bladeRF plugin. Needs `libbladeRF`.                                                                    |
| `SWEEPPP_WITH_RTLSDR`       | `ON`    | The RTL-SDR plugin. Needs `librtlsdr`.                                                                     |
| `SWEEPPP_WITH_FOBOS`        | `ON`    | The Fobos SDR plugin. Needs `libusb-1.0`; the two Fobos libraries are downloaded and linked in statically. |
| `SWEEPPP_WITH_UPDATE_CHECK` | `ON`    | The GUI's update check. Needs libcurl; turned off automatically, with a warning, when libcurl isn't found. |
| `SWEEPPP_WERROR`            | `OFF`   | Treat warnings as errors.                                                                                  |
| `SWEEPPP_SANITIZER`         | *empty* | `address`, `thread`, `undefined` or `address+undefined`. Not supported with MSVC.                          |

On macOS the minimum system version defaults to 13.3
(`CMAKE_OSX_DEPLOYMENT_TARGET`).

### Building without GPL components

Sweep++ itself is GPL-3.0-or-later, but three third-party libraries it can use
are GPL too. Each is used only by its own plugin, so leaving out those plugins
gives a build with no GPL third-party code:

```sh
cmake --preset release -DSWEEPPP_WITH_FFTW=OFF -DSWEEPPP_WITH_HACKRF=OFF -DSWEEPPP_WITH_RTLSDR=OFF
```

PocketFFT (and Accelerate on macOS) still provide the FFT, so the app works.
[`THIRD_PARTY.md`](../THIRD_PARTY.md) explains the licences.

## Makefile targets

Run `make` on its own for the full list. Variables: `PRESET` (default `dev`),
`JOBS`, `ARGS`, `WITHOUT` and `BUILD_TYPE`.

| Target                                                                             | Does                                                                  |
|------------------------------------------------------------------------------------|-----------------------------------------------------------------------|
| `make build`                                                                       | Configure and build `PRESET`.                                         |
| `make debug`, `make release`, `make headless`                                      | Build that preset.                                                    |
| `make all`                                                                         | Build dev, debug and release.                                         |
| `make run`                                                                         | Build and start the GUI.                                              |
| `make cli ARGS="…"`                                                                | Build and run `sweeppp-cli` with those arguments.                     |
| `make info`                                                                        | List the FFT engines, radios and plugins this build can see.          |
| `make sweep ARGS="…"`                                                              | A 5-second sweep of the synthetic radio, with statistics.             |
| `make throughput`                                                                  | A 30-second, 100 MS/s synthetic run that checks no samples are lost.  |
| `make serve ARGS="…"`                                                              | Serve the synthetic radio on loopback, for the desktop to connect to. |
| `make test`                                                                        | Build and run the tests.                                              |
| `make asan`, `make tsan`                                                           | Run the tests under the sanitizers.                                   |
| `make sweepsfile`                                                                  | Build, test and install-check libsweepsfile on its own.               |
| `make check`                                                                       | Everything CI runs: `test`, `sweepsfile`, `asan` and `tsan`.          |
| `make modules`                                                                     | List the names `make without` accepts.                                |
| `make without WITHOUT="…"`                                                         | Build with some parts turned off.                                     |
| `make format`, `make format-check`, `make tidy`, `make check-headers`, `make lint` | See [Code quality](#code-quality).                                    |
| `make compile-commands`                                                            | Point `compile_commands.json` at `PRESET`'s build, for clangd.        |
| `make clean`                                                                       | Delete `build/`, downloaded dependencies included.                    |
| `make rebuild`                                                                     | `clean`, then `build`.                                                |

### Building with parts turned off

```sh
make without WITHOUT="bladerf gui"
```

turns off `SWEEPPP_WITH_BLADERF` and `SWEEPPP_BUILD_GUI`, and builds into
`build/without-bladerf-gui/`. Each combination gets its own directory, so a
turned-off part never lingers in a stale cache. The names are the option names
in lowercase with dashes: `tests`, `gui`, `server`, `plugins`, `fftw`,
`accelerate`, `pocketfft`, `hackrf`, `bladerf`, `rtlsdr`, `fobos` and
`update-check`. `BUILD_TYPE` sets the build type (RelWithDebInfo by default).

## Tests

```sh
ctest --preset dev                          # everything
ctest --test-dir build/dev -R MpmcQueue     # tests whose name matches a regex
```

The tests are split into several executables in `build/<preset>/dist/`:

- `sweeppp-tests`, for the core library;
- `sweeppp-plugin-<name>-tests`, one per plugin;
- `sweepsfile-tests`, for the `.sweeps` library.

Each is a doctest binary, so it can also be run directly, with doctest's own
filters such as `-tc="name*"`.

The remote link's throughput report is skipped by default; run it with
`sweeppp-tests -tc="throughput*" --no-skip`.

`make check` runs everything CI runs, including both sanitizer builds and the
standalone libsweepsfile checks.

## Code quality

| Target               | Does                                                                                                                         |
|----------------------|------------------------------------------------------------------------------------------------------------------------------|
| `make format`        | Apply `.clang-format` to the tree. CI uses clang-format 22.                                                                  |
| `make format-check`  | Check formatting without changing anything.                                                                                  |
| `make tidy`          | Run clang-tidy, using `PRESET`'s compile database.                                                                           |
| `make check-headers` | Check every source file has the right SPDX licence header: MIT under `lib/libsweepsfile/`, GPL-3.0-or-later everywhere else. |
| `make lint`          | `format-check` and `check-headers`, without building.                                                                        |

The scripts behind them are in `tools/`. `CLANG_FORMAT` and `CLANG_TIDY` pick a
specific binary.

## Windows

Windows builds are experimental. The only tested recipe is the CI job in
[`.github/workflows/build.yml`](../.github/workflows/build.yml), which:

- uses MSVC;
- installs `fftw3`, `glfw3`, `pkgconf` and `curl` from vcpkg;
- installs `libhackrf` and `rtl-sdr` from conda-forge, which also brings
  libusb;
- unpacks libbladeRF from Nuand's Windows installer;
- writes `.pc` files for the libraries that don't ship one, and configures with
  the vcpkg toolchain file and vcpkg's `pkgconf.exe`;
- copies the DLLs next to the executables.

Follow that job if you want to build locally. MSYS2 and MinGW aren't supported.

`register-sweeps.cmd`, configured from `packaging/windows/register-sweeps.cmd.in`
into `dist/`, associates `.sweeps` files with the `sweeppp.exe` next to it, for
the current user. `register-sweeps.cmd /u` removes the association. Each
channel registers its own ProgId, so the release's and the nightly's can both
be registered; the last one registered is the double-click default.

## Packages

There are no install rules, and no CPack. Packages are built only by CI, from
the `release` preset:

- a `.tar.gz` (or `.zip` on Windows) with the binaries, plugins and resources;
- on Linux, a `.deb` that installs to `/usr/bin`, `/usr/lib/<triplet>/sweeppp/plugins`
  and `/usr/share/sweeppp/resources`, with a desktop entry, the `.sweeps` file
  type, icons, and the Fobos udev rule;
- on macOS, `Sweep++.app` inside the tarball.

Every push to `main` updates the `nightly` pre-release. `pages.yml` publishes
the site and a signed apt repository (`main` = the last 5 releases, `nightly` =
the current and archived nightlies) to GitHub Pages after every `build` run.
`homebrew.yml` rewrites the `sweeppp` and `sweeppp-nightly` casks in the
`aurimasniekis/homebrew-sweeppp` tap from `packaging/homebrew/cask.sh` at the
same point.

## Channels

Every build is Sweep++ Nightly except the `release` preset, which sets
`SWEEPPP_CHANNEL=release`; CI uses the release channel only for a `v*` tag.

|              | release                                                         | nightly                                                                                                 |
|--------------|-----------------------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| Name         | Sweep++                                                         | Sweep++ Nightly                                                                                         |
| Identifier   | `sweeppp`                                                       | `sweeppp-nightly`                                                                                       |
| macOS bundle | `Sweep++.app`, `org.sweeppp.app`                                | `Sweep++ Nightly.app`, `org.sweeppp.app.nightly`                                                        |
| Settings     | `~/.config/sweeppp`                                             | `~/.config/sweeppp-nightly`                                                                             |
| `.deb`       | `sweeppp`: `sweeppp`, `sweeppp-cli`, `sweeps`                   | `sweeppp-nightly`: `sweeppp-nightly`, `sweeppp-nightly-cli`, `sweeps-nightly`                           |

*Share settings with the Sweep++ release* in the nightly's settings switches
it to the release's directory from the next start; the choice is the empty
file `use-release-config` in the nightly's own directory.

Icons live in `branding/` as `<identifier>.svg`, plus `sweeps.svg` for
`.sweeps` files, each with the `.ico` and `.icns` rendered from it; CI renders
the Linux PNG ladders from the SVGs, dropping `class="detail"` groups below
32 px.

## Updating a pinned dependency

See [Updating a pinned version](../THIRD_PARTY.md#updating-a-pinned-version) in
`THIRD_PARTY.md`.
