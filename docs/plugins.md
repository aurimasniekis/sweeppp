# Plugins

Much of Sweep++ is made of plugins: the radio drivers, the FFT engines, the band
plans, the channel lists and signal detection. This page covers the plugins that
ship with Sweep++, how to manage them, how to add your own band plans and
channels, and how to install plugins from other people.

To write a plugin, see [Plugin development](plugin-development.md).

## What a plugin is

A plugin is a file Sweep++ loads when it starts. One plugin can add several
things at once. The detections plugin, for example, adds a signal detector, the
labels it paints on the spectrum, and the Detections button and window.

Each thing a plugin adds can fail on its own. If one part can't start, the rest
of the plugin still works, and **Menu → Plugins** says what went wrong.

## Bundled plugins

| Plugin                                              | Adds                                                                  |
|-----------------------------------------------------|-----------------------------------------------------------------------|
| **Band plan**                                       | Band allocation labels, and the **Bands** button.                     |
| **Channels**                                        | Channel labels, the **Channels** button and the channel editor.       |
| **Detections**                                      | Automatic signal detection, and the **Detections** button and window. |
| **PocketFFT**                                       | An FFT engine. Works on every system.                                 |
| **FFTW**                                            | An FFT engine, using the FFTW library.                                |
| **Accelerate**                                      | An FFT engine, using Apple's Accelerate framework. macOS only.        |
| **HackRF**, **bladeRF**, **RTL-SDR**, **Fobos SDR** | Radio drivers. See [Supported devices](devices.md).                   |

The synthetic signal generator and the IQ file player are built into Sweep++
rather than being plugins.

### Band plan

Draws frequency allocations over the spectrum and names them in the status bar.
Two plans are included:

- **ECA (Europe)**, from ERC Report 25: about 550 allocations covering mobile,
  aeronautical, fixed, satellite, maritime, broadcasting, radiolocation,
  amateur, radionavigation, time signals, meteorological and more.
- **ITU Region 1**: a shorter list of the most common allocations across Europe,
  Africa and the Middle East.

Pick the plan in **Menu → Data contributors**. Right-click **Bands**, or press
**Shift+B**, to open a tree of services and allocations where you tick which
ones are shown. Anything unticked is also left out of the status bar.

### Channels

Draws specific channels and single frequencies over the spectrum. These lists
are included:

| List                         | Contains                                                     |
|------------------------------|--------------------------------------------------------------|
| **Beacons**                  | Aviation, navigation and satellite downlinks. On by default. |
| **Wi-Fi**                    | 2.4 GHz (on by default), 5 GHz, 6 GHz and HaLow channels.    |
| **Cellular and short range** | GSM, LTE, 5G NR, Bluetooth LE and Zigbee.                    |
| **LoRa and ISM**             | LoRaWAN regional plans, SRD/ISM, PMR446 and RC links.        |
| **FPV video**                | Analog bands from 1.2 to 8.8 GHz, and digital systems.       |

Most groups start switched off, so a fresh install isn't covered in labels.
Right-click **Channels**, or press **Shift+C**, to open the tree and tick what
you want. **Reset** goes back to the defaults.

**Edit channels…** in that panel opens the channel editor. Add a channel from a
marker with **Add Marker 1** (and so on), or type a name, group, centre frequency and
width. Leave the width blank for a single frequency. Your channels are saved in
**My channels**; see [Your own data](#your-own-data).

### Detections

Finds signals automatically and keeps a history of when each was active. See
[Detections](user-guide.md#detections) in the user guide.

### FFT engines

An FFT engine does the maths that turns radio samples into a spectrum. Sweep++
needs at least one, and won't start without one.

- **PocketFFT** works everywhere and handles any FFT size.
- **FFTW** is a mature, well-optimised library with fast kernels for modern
  CPUs.
- **Accelerate** uses the maths library built into macOS, and is the fastest on
  Apple silicon at most FFT sizes. It only uses power-of-two FFT sizes, so the
  resolution you ask for may be rounded slightly.

Choose one in **Analysis → Advanced → Backend**. Which is fastest depends on
your computer and the FFT size, so click **Bench** next to it: it times every
installed engine on your machine and offers to switch to the winner.

## Managing plugins

### Menu → Plugins

Lists every plugin Sweep++ found. For each one you can:

- tick or untick it to turn it on or off. A plugin that failed to load can't be
  turned on, and says why;
- open its details: its id, authors, what it adds, and links;
- **Show in folder** to find the plugin file.

Some changes can't take effect while the plugin is in use (for example turning
off the driver for the radio that is open). The plugin is then marked
**restart needed**; your choice is saved and applies when you restart.

**Open plugins folder** opens the folder for plugins you install yourself.

### Menu → Data contributors

Band plans, channel lists and detections can all name the same frequency. This
section decides which comes first, and has a dataset selector for plugins with
more than one (such as the two band plans). See
[Data contributors](user-guide.md#data-contributors) in the user guide.

### When a plugin doesn't load

Run `sweeppp-cli info`. It lists every plugin found, including the ones that
failed and why, and every folder it searched. See
[Command line](cli.md#info).

## Your own data

Sweep++ reads band plans and channel lists from your
[config folder](user-guide.md#where-settings-are-stored) as well as the built-in
ones. **Menu → Application → Open config folder** takes you there.

| Put files in       | For           |
|--------------------|---------------|
| `bandplans/*.toml` | Band plans    |
| `channels/*.toml`  | Channel lists |

A file of yours replaces a built-in one when it has the same id: the
`[bandplan]` `name` for band plans, or the `[channels]` `id` for channel lists.
With a different id, it's added alongside the built-in ones.

The easiest way to start is to copy one of the built-in files from Sweep++'s
`resources/bandplans/` or `resources/channels/` folder and edit it. Each starts
with comments explaining its format. Restart Sweep++ to load changes you make
by hand.

### Band plan files

```toml
[bandplan]
name = "My region"
description = "Allocations I care about"

[bandplan.colors]
"Broadcasting" = "#8B5CF6"
"Amateur"      = "#22C55E"

[[band]]
name = "FM broadcast"
start = "87.5 MHz"
stop = "108 MHz"
group = "Broadcasting"
description = "Shown when you hover the label"

[[band]]
name = "2 m amateur"
start = "144 MHz"
stop = "146 MHz"
group = "Amateur"
```

- `group` picks the colour from `[bandplan.colors]`.
- `type` is optional: `"band"` (the default), `"channel"` or `"spot"`. A spot is
  a single frequency and has no `stop`.

### Channel files

```toml
[channels]
id = "fpv"
name = "FPV video"
width = "12 MHz"          # default width for every channel in the file

[channels.colors]
analog = "#F97316"

[[group]]
id = "analog"
name = "Analog"
color = "analog"          # a key into [channels.colors], or a literal #RRGGBB

[[group]]
id = "analog/58/r"        # parents come from the id path
name = "Raceband (R)"
default = true            # ticked on a fresh install

  [[group.channel]]
  name = "R1 5658"
  center = "5658 MHz"
```

- **Groups form a tree through their ids.** `analog/58/r` sits under
  `analog/58`, which sits under `analog`. Parents must be declared before their
  children. Groups appear in the order they're written.
- **A channel is a `center` and a width.** The width comes from the channel,
  else its group, else the file. `color` is inherited the same way. You can
  write `start` and `stop` instead of `center`.
- **Frequencies can be written several ways:** `"5658 MHz"`, `"5.658G"` and a
  plain number of hertz all work.
- **No width anywhere makes a single frequency**, drawn as a flag with no
  width. `type = "band"`, `"channel"` or `"spot"` sets it explicitly.
- **`default` only decides the first run**, and what **Reset** goes back to.
  After that, your ticks are remembered. A group added to a file later appears
  switched on.

### My channels

Channels you add in the channel editor are saved to `channels/custom.toml`, in
the same format. You can edit that file by hand too.

## Installing plugins

To install a plugin someone else made, put its file (`.so` on Linux, `.dylib`
on macOS, `.dll` on Windows) in your plugins folder: **Menu → Plugins → Open
plugins folder**, which is `plugins/` in your
[config folder](user-guide.md#where-settings-are-stored). Then restart
Sweep++.

Sweep++ looks for plugins in these places, in order. When two files have the
same plugin id, the first one found wins.

| Folder                                                                                         | Loads                               |
|------------------------------------------------------------------------------------------------|-------------------------------------|
| Each folder in `$SWEEPPP_PLUGIN_PATH` (separated by `:`, or `;` on Windows)                    | any plugin file                     |
| `plugins/` in your config folder                                                               | any plugin file                     |
| `plugins/` next to the Sweep++ program, or `Contents/PlugIns` inside `Sweep++.app`             | any plugin file                     |
| `sweeppp/plugins/` (`sweeppp-nightly/plugins/` for the nightly) inside a system library folder | any plugin file                     |
| A system library folder itself                                                                 | only files named `sweeppp-plugin-*` |

The system library folders are:

- **Linux:** `/usr/local/lib/<arch>`, `/usr/lib/<arch>` (for example
  `x86_64-linux-gnu`), `/usr/local/lib` and `/usr/lib`;
- **macOS:** `/opt/homebrew/lib`, `/usr/local/lib` and `/usr/lib`;
- **Windows:** `%ProgramFiles%\sweeppp` and `%ProgramW6432%\sweeppp`.

In a system library folder, only files named `sweeppp-plugin-*` are loaded.
Those folders hold thousands of other libraries, and loading a file is the only
way to find out whether it's a plugin, so Sweep++ goes by the name instead.
[Plugin development](plugin-development.md#where-plugins-are-found) explains
this in more detail.

Only install plugins from people you trust. A plugin runs inside Sweep++ with
the same access to your computer as Sweep++ itself.
