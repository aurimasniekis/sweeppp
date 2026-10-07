# Supported devices

Sweep++ works with these radios:

- [HackRF One and HackRF Pro](#hackrf-one-and-hackrf-pro)
- [bladeRF](#bladerf)
- [RTL-SDR](#rtl-sdr)
- [Fobos SDR](#fobos-sdr)
- [Synthetic signal generator](#synthetic-signal-generator), which needs no
  hardware
- [IQ file](#iq-file), which replays a recording of raw samples

## How radios work in Sweep++

- **Each radio is a plugin.** If a radio's plugin is missing or its library
  isn't installed, that radio simply isn't offered. Everything else keeps
  working. `sweeppp-cli info` lists the plugins that failed to load, and why.
- **Sweeping is done in software.** Sweep++ tunes the radio step by step and
  stitches the steps together. It doesn't use any radio's built-in sweep mode,
  so every radio sweeps the same way and all the Analysis settings apply to all
  of them.
- **Several radios at once.** When more than one radio is plugged in, each is
  listed by its serial number, so you can pick the right one.
- **The settings panel comes from the driver.** Each radio shows its own
  settings in the Device panel. Settings marked as needing a stop are greyed out
  while the radio runs.
- **More than one input.** Radios with several receive inputs let you
  [assign an antenna to each](user-guide.md#antennas-and-inputs) and switch
  between them automatically during a sweep.

## At a glance

|                 | HackRF One/Pro | bladeRF                                                        | RTL-SDR                                                                        | Fobos SDR                              |
|-----------------|----------------|----------------------------------------------------------------|--------------------------------------------------------------------------------|----------------------------------------|
| Frequency range | 1 MHz – 6 GHz  | 47 MHz – 6 GHz                                                 | depends on the tuner, about 24 – 1766 MHz; 0.5 – 28.8 MHz with direct sampling | 50 MHz – 6.9 GHz                       |
| Sample rates    | 2 – 20 MS/s    | 0.52 – 61.44 MS/s, plus 8-bit rates above that where supported | 0.25 – 3.2 MS/s                                                                | 8 – 80 MS/s (8 – 64 on agile firmware) |
| Sample format   | 8-bit          | 16-bit, or 8-bit above 61.44 MS/s                              | 8-bit                                                                          | floating point                         |
| Inputs          | 1              | 1, or 2 on the 2.0 micro                                       | tuner, plus HF on most tuners                                                  | 1                                      |
| Bias-T          | yes, 3.3 V     | yes                                                            | yes, if your librtlsdr supports it                                             | —                                      |
| Health readings | —              | yes (2.0 micro)                                                | —                                                                              | —                                      |

## HackRF One and HackRF Pro

**Driver name:** `hackrf`. Also works with HackRF-compatible radios.

- **Frequency:** 1 MHz – 6 GHz.
- **Sample rate:** 2, 4, 8, 10, 12.5, 16 or 20 MS/s; 10 MS/s by default. 8 to
  20 MS/s is the useful range for sweeping.
- **Filter bandwidth:** automatic (75% of the sample rate) by default, up to
  28 MHz.
- **Gain:**
  - **RF amp:** a +14 dB amplifier, on or off;
  - **LNA gain:** 0 – 40 dB in 8 dB steps;
  - **VGA gain:** 0 – 62 dB in 2 dB steps.
- **Bias-T:** 3.3 V on the antenna connector, for powering an active antenna or
  preamp.
- **Inputs:** one.

**If the HackRF isn't found or won't open:**

- On Linux, install the HackRF udev rules so your user can access the device.
- Close any other program that is using the HackRF. Only one program can use
  it at a time.

## bladeRF

**Driver name:** `bladerf`. Works with the bladeRF x40 and x115, and the
bladeRF 2.0 micro xA4, xA5 and xA9.

- **Frequency:** read from the radio when it opens, so it matches your board.
  Before a radio is opened it is listed as 47 MHz – 6 GHz.
- **Sample rate:** 520 kS/s up to 61.44 MS/s, in 2 MS/s steps; 20 MS/s by
  default.
  - Where the FPGA image supports it, 110 MS/s and the board's maximum (for
    example 122.88 MS/s) are also offered, marked **(8-bit)**. These run the
    radio's oversampling mode: twice the rate through the same USB link, but
    half the dynamic range.
- **Sample format:** 16-bit, or 8-bit at the oversampling rates.
- **Packed link:** sends 12-bit samples over USB to fit more data through the
  link. The radio can't report overruns in this mode.
- **Filter bandwidth:** automatic by default, up to 56 MHz.
- **Gain:**
  - **Gain mode:** Manual, or one of the automatic modes the board offers
    (automatic by default);
  - **Manual gain:** −15 – 60 dB.
- **Bias-T:** yes. Untested on the x40 and x115.
- **Inputs:** RX1 and RX2 on the bladeRF 2.0 micro. Switching between them
  briefly stops the radio (estimated at about 0.2 s), and Sweep++ includes that
  in the predicted sweep time.
- **Firmware:** the Device panel shows the firmware and FPGA versions, and
  highlights one newer than Sweep++ has been tested with. It also shows whether
  the radio is on USB 2 or USB 3. Use USB 3 for high sample rates.
- **Health readings** (bladeRF 2.0 micro), shown in the Performance window:
  - RF chip temperature, with an alarm above 85 °C;
  - power source;
  - signal strength (RSSI);
  - bus voltage, current and power;
  - oscillator trim.
- **Artefacts:** the floor rises towards each step's LO, so a wide sweep shows
  a hump every step; narrow spurs sit at multiples of the 38.4 MHz reference
  clock. Analysis → Corrections → **Learn** with no antenna removes both. See
  [Corrections](user-guide.md#advanced-settings).

## RTL-SDR

**Driver name:** `rtlsdr`. Works with RTL2832U dongles.

- **Frequency:** depends on the tuner chip in your dongle:

  | Tuner        | Range          |
  |--------------|----------------|
  | R820T, R828D | 24 – 1766 MHz  |
  | E4000        | 52 – 2200 MHz  |
  | FC0012       | 22 – 948.6 MHz |
  | FC0013       | 22 – 1100 MHz  |
  | FC2580       | 146 – 924 MHz  |

- **HF (direct sampling):** 0.5 – 28.8 MHz, on dongles wired for it. It appears
  as a second input called **HF (direct sampling)**, next to the **Tuner**
  input. Not available with the R828D tuner. Switching inputs briefly stops the
  radio.
- **Sample rate:** 0.25, 1.024, 1.4, 1.8, 1.92, 2.048, 2.4, 2.56, 2.88 or
  3.2 MS/s; 2.4 MS/s by default. Because the bandwidth is narrow, wide sweeps
  take many steps and are slower than on the other radios.
- **Tuner bandwidth:** automatic by default, up to 8 MHz, if the librtlsdr
  Sweep++ was built against supports it.
- **Gain:**
  - **Gain mode:** Manual or Automatic;
  - **Tuner gain:** in the steps your tuner supports;
  - **RTL2832 AGC:** the chip's digital automatic gain.
- **Frequency correction:** ±100 ppm, to correct a dongle whose frequency is a
  little off.
- **Bias-T:** yes, if the librtlsdr Sweep++ was built against supports it.

## Fobos SDR

**Driver name:** `fobos`. Works with both the standard and the agile firmware.
Sweep++ detects which firmware is installed and shows **(agile)** after the
radio's name when it's the agile one.

- **Frequency:** 50 MHz – 6.9 GHz.
- **Sample rate:** from 8 MS/s up to 80 MS/s on the standard firmware, or up to
  64 MS/s on the agile firmware; 20 MS/s by default. With the agile firmware on
  hardware and firmware revision 4 or later, any rate is accepted.
- **Sample format:** floating point, with DC and IQ corrections already applied
  by the Fobos library.
- **Filter bandwidth:** 80% of the sample rate by default.
- **Gain:**
  - **LNA gain:** steps 0 – 3 (0, 0, +16 or +33 dB);
  - **VGA gain:** 0 – 31, in 2 dB steps.
- **Clock source:** internal, or external if you connect a reference clock.
- **Bias-T:** not available.
- **Inputs:** one.

On Linux, the Sweep++ `.deb` package installs the udev rule that lets your
user open the radio. If you built Sweep++ yourself, install
`fobos-sdr.rules` from the Fobos library.

## Synthetic signal generator

**Driver name:** `synthetic`. Always listed, needs no hardware.

It produces made-up signals so you can try Sweep++, or test settings, without a
radio:

- **Frequency:** 1 MHz – 6 GHz.
- **Sample rate:** presets from 2 to 122.88 MS/s; accepts 1 – 200 MS/s.
- **Emitters:** 0 to 32 fake signals: continuous, drifting, pulsed and
  wideband.
- **Gain** and **Noise floor** (−120 to −20 dBFS).
- **Sample format:** selectable, to imitate different radios.
- **Simulate overruns:** makes it drop data now and then, to see how Sweep++
  reports it.
- **Inputs:** two, RX1 (1 MHz – 2 GHz) and RX2 (1.5 – 6 GHz), so you can try
  antenna routing. Bias-T is simulated on RX1.

## IQ file

**Driver name:** `iqfile`. Replays a file of raw IQ samples as if it were a
radio.

It isn't listed in the Device panel. Use it from the command line, with the
file's path after the driver name:

```sh
sweeppp-cli sweep --device iqfile:/path/to/capture.cf32 --sample-rate 2.4M -o out.csv
```

It can also be set as the device in a profile or `settings.toml` (driver
`iqfile`, id set to the file path).

- **The file is raw samples with no header.** The sample format is guessed from
  the file name: `cs8`, `cu8`, `cs16`, `cf32`, `cf64` and similar. If the name
  doesn't say, 32-bit float (`cf32`) is assumed.
- **Sample rate:** set it to the rate the file was recorded at. It sets the
  frequency axis and the playback speed.
- **Center frequency:** set it to the frequency the file was recorded at. It
  only labels the axis; it doesn't change what's in the file.
- **Loop:** start again at the end (on by default).
- **Realtime:** play at the recorded speed (on by default). Turn it off to
  process the file as fast as possible.

## Installing radio libraries

The released packages include the drivers. What else you need depends on your
system.

| Radio     | macOS (Homebrew)          | Debian / Ubuntu, to build | Build option to leave it out |
|-----------|---------------------------|---------------------------|------------------------------|
| HackRF    | `brew install hackrf`     | `libhackrf-dev`           | `SWEEPPP_WITH_HACKRF=OFF`    |
| bladeRF   | `brew install libbladerf` | `libbladerf-dev`          | `SWEEPPP_WITH_BLADERF=OFF`   |
| RTL-SDR   | `brew install librtlsdr`  | `librtlsdr-dev`           | `SWEEPPP_WITH_RTLSDR=OFF`    |
| Fobos SDR | `brew install libusb`     | `libusb-1.0-0-dev`        | `SWEEPPP_WITH_FOBOS=OFF`     |

- **macOS:** when Sweep++ starts, it checks which of these are missing and tells
  you the `brew install` command. You only need the ones for radios you own.
- **Debian and Ubuntu:** installing the `.deb` with `apt` pulls in the libraries
  it needs to run. The `-dev` packages are only needed to build Sweep++
  yourself.
- **Fobos:** the Fobos libraries themselves are built into the plugin, so only
  libusb is needed.

For building from source, see [Building from source](build.md).

## Want another radio?

Pull requests are welcome, see
[Writing a driver](plugin-development.md#writing-a-driver). Or, if you can lend
or send the hardware, open an issue and I'll develop support for it.
