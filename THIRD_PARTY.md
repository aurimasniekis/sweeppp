# Third-party dependencies

Sweep++ is licensed **GPL-3.0-or-later** ([`LICENSE`](LICENSE)), except
[`lib/libsweepsfile/`](lib/libsweepsfile/), which is **MIT**
([`lib/libsweepsfile/LICENSE`](lib/libsweepsfile/LICENSE)).

This file lists everything Sweep++ uses that it did not write: where each one
comes from, its licence, and which part of Sweep++ uses it.

## Installed on the system

These are found with pkg-config or CMake when you configure. Nothing is
bundled. If one is missing, configuration stops and prints the install command.

| Library                                                  | Licence              | Used by                     | Turn off with                   |
|----------------------------------------------------------|----------------------|-----------------------------|---------------------------------|
| [GLFW](https://github.com/glfw/glfw) ≥ 3.3               | Zlib                 | GUI                         | `SWEEPPP_BUILD_GUI=OFF`         |
| [GTK 3](https://www.gtk.org/) (Linux only)               | LGPL-2.1-or-later    | GUI: file dialog, clipboard | `SWEEPPP_BUILD_GUI=OFF`         |
| [libcurl](https://curl.se/libcurl/)                      | curl (MIT-style)     | GUI: update check           | `SWEEPPP_WITH_UPDATE_CHECK=OFF` |
| [FFTW](https://www.fftw.org/) (`fftw3f`)                 | **GPL-2.0-or-later** | `fft-fftw` plugin           | `SWEEPPP_WITH_FFTW=OFF`         |
| Accelerate (macOS only)                                  | Part of macOS        | `fft-accelerate` plugin     | `SWEEPPP_WITH_ACCELERATE=OFF`   |
| [libhackrf](https://github.com/greatscottgadgets/hackrf) | **GPL-2.0-only**     | `sdr-hackrf` plugin         | `SWEEPPP_WITH_HACKRF=OFF`       |
| [librtlsdr](https://gitea.osmocom.org/sdr/rtl-sdr)       | **GPL-2.0-or-later** | `sdr-rtlsdr` plugin         | `SWEEPPP_WITH_RTLSDR=OFF`       |
| [libbladeRF](https://github.com/Nuand/bladeRF)           | LGPL-2.1-or-later    | `sdr-bladerf` plugin        | `SWEEPPP_WITH_BLADERF=OFF`      |
| [libusb](https://libusb.info/)                           | LGPL-2.1-or-later    | `sdr-fobos` plugin          | `SWEEPPP_WITH_FOBOS=OFF`        |

If libcurl is missing, the build turns the update check off and prints a
warning instead of failing.

## Downloaded at build time

These are downloaded as source archives and built with Sweep++. The versions
are pinned in [`cmake/Dependencies.cmake`](cmake/Dependencies.cmake), and the
downloads go into `build/<preset>/_deps`.

| Project                                                                        | Version            | Licence              | Used by                               |
|--------------------------------------------------------------------------------|--------------------|----------------------|---------------------------------------|
| [Dear ImGui](https://github.com/ocornut/imgui) (docking)                       | `v1.92.9b-docking` | MIT                  | GUI                                   |
| [ImPlot](https://github.com/epezent/implot)                                    | `7eeb9168`         | MIT                  | GUI                                   |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | `v1.2.1`           | Zlib                 | GUI: file dialog                      |
| [stb_image_write](https://github.com/nothings/stb)                             | `2c980bb5`         | MIT or public domain | GUI: PNG snapshots                    |
| [Material Design Icons](https://github.com/Templarian/MaterialDesign-Webfont)  | `v7.4.47`          | Apache-2.0           | GUI: icon font file                   |
| [nlohmann/json](https://github.com/nlohmann/json)                              | `v3.12.0`          | MIT                  | Server, GUI update check              |
| [toml++](https://github.com/marzer/tomlplusplus)                               | `v3.4.0`           | MIT                  | `libsweeppp`: config files            |
| [Monocypher](https://github.com/LoupVaillant/Monocypher)                       | `4.0.2`            | CC0-1.0 or BSD-2-Clause | `libsweeppp`: remote link encryption |
| [PocketFFT](https://github.com/mreineck/pocketfft) (`cpp` branch)              | `c90e55b3`         | BSD-3-Clause         | `fft-pocketfft` plugin                |
| [libfobos](https://github.com/rigexpert/libfobos)                              | `v2.4.0`           | LGPL-2.1             | `sdr-fobos` plugin, linked statically |
| [libfobos-sdr-agile](https://github.com/rigexpert/libfobos-sdr-agile)          | `v.3.3.0`          | LGPL-2.1             | `sdr-fobos` plugin, linked statically |
| [doctest](https://github.com/doctest/doctest)                                  | `v2.4.12`          | MIT                  | Tests only                            |

## Copied into the repository

| Project                                                 | Version  | Licence | Where                                                  |
|---------------------------------------------------------|----------|---------|--------------------------------------------------------|
| [tl::expected](https://github.com/TartanLlama/expected) | `v1.1.0` | CC0-1.0 | `lib/libsweepsfile/include/sweeps/vendor/expected.hpp` |

The copy is renamed to `sweeps::vendor` so that it cannot clash with a real
tl::expected in a program that uses both. It is only used when building below
C++23. At C++23 the library uses `std::expected` instead.

`third_party/gl/` and `third_party/imgui_config/` are Sweep++'s own code, not
third-party code.

## Licence notes

- **GPL libraries are only linked by their own plugins.** FFTW, libhackrf and
  librtlsdr are each linked by a single plugin. `libsweeppp`, `sweeppp`,
  `sweeppp-cli` and `sweeppp-server` link none of them, and you can confirm
  this with `otool -L` or `ldd`. Plugins are loaded at runtime. The app needs
  an FFT plugin to run, though, so leaving FFTW out means another FFT plugin
  has to provide the transform.
- **libhackrf is GPL-2.0-only.** It has no "or later" clause, so it is not
  compatible with GPL-3.0. That is why it must stay in its own plugin.
- **Installing a library from the system does not change its licence.** A
  Homebrew build of a library carries the same licence as one you compile
  yourself.
- **The Fobos libraries are linked statically**, because no package manager
  ships them. LGPL-2.1 then requires that users can relink the plugin against
  a modified copy. The plugin's source is in this repository, and configuring
  with `-DFETCHCONTENT_SOURCE_DIR_LIBFOBOS=<dir>` or
  `-DFETCHCONTENT_SOURCE_DIR_LIBFOBOS_AGILE=<dir>` builds it against your
  copy. If you distribute a binary, you must pass that option on.
  libbladeRF and libusb are linked as shared libraries, so this does not apply
  to them.
- **A build with no GPL components** is possible on every platform:
  `-DSWEEPPP_WITH_FFTW=OFF -DSWEEPPP_WITH_HACKRF=OFF -DSWEEPPP_WITH_RTLSDR=OFF`.
  The PocketFFT plugin supplies the FFT, and on macOS the Accelerate plugin
  does too, so the app still works.
- **`lib/libsweepsfile/` is MIT so that anyone can read `.sweeps` files.** It
  depends on nothing, and GPL code can use MIT code, so `libsweeppp` links it
  without any conflict. Nothing GPL may be added to that directory.

This is not legal advice.

## Updating a pinned version

1. Change the version in `cmake/Dependencies.cmake` and update the table above.
2. Delete the old download: `rm -rf build/<preset>/_deps/<name>-*`.
3. Configure and build from clean: `rm -rf build/dev && cmake --preset dev && cmake --build --preset dev`.

ImGui and ImPlot must be updated together. ImPlot is pinned to a commit rather
than the `v1.0` tag because `v1.0` does not build against this ImGui. Read the
comment above those pins in `cmake/Dependencies.cmake` before changing either.
