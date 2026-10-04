# Writing a Sweep++ plugin

A plugin is a shared object in a directory Sweep++ looks in. It declares what
it provides, the host decides whether it can run, and everything it does goes
through one C function table.

The authoritative documents are the two headers, and they are meant to be read:

- `lib/libsweeppp/include/sweeppp/plugin/PluginAbi.h` — the ABI. C99, and the
  only real boundary.
- `lib/libsweeppp/include/sweeppp/plugin/Plugin.hpp` — a header-only C++
  wrapper over it, so plugins are written as ordinary classes.

This file is the part that does not belong in either: why it is shaped this
way, and the rules that will bite you if you ignore them.

## The one rule

> A plugin may link `libsweeppp` for **values** and must never touch a
> **singleton**.

`libsweeppp` is a *static* library. A plugin that links it and calls
`SdrDeviceManager::instance()` gets its own private registry, registers into
it, and registers into nothing the host can see. `Paths::instance()` gets its
own copy that ignores `--config-dir`. `Log`
gets its own sink, so `logInfo()` in a plugin writes to somewhere the
application never installed — the message does not appear in the log file, and
looks like it vanished.

None of that fails loudly. It all *appears* to work.

So: `Color`, `toml_util`, `Result`, `PluginSettings`, `BandPlan` — anything
that is pure computation over its arguments — are fine to link and use. For
anything else there is `sweeppp::plugin::Host`, which forwards to the host's
copy across the ABI.

The band-plan plugin takes this trade deliberately: it links `libsweeppp` for
the TOML helpers, and takes its directories from `Host` rather than from
`Paths`. Without the first the port would have been a rewrite of the loader;
without the second it would silently ignore `--config-dir`.

## Building one

```cmake
sweeppp_add_plugin(my_plugin
    NAME    thing                       # -> sweeppp-plugin-thing.so
    UI                                  # only if it draws
    SOURCES src/MyPlugin.cpp
    LINK    sweeppp::sweeppp)           # optional; values only
```

`cmake/SweepppPlugin.cmake` is what gets the six easy-to-miss things right:
`MODULE`, no `lib` prefix, output into `dist/plugins`, hidden visibility, the
host's ImGui headers *with its compile definitions*, and
`SWEEPPP_PLUGIN_HAS_UI`.

The compile definitions are the load-bearing half. `sweeppp_imconfig.h` makes
`ImDrawIdx` and `ImWchar` 32-bit; a plugin built without it has a different
`ImDrawVert` and corrupts every draw list it touches — silently, and not at the
call that did it. See *Drawing* below for what stops that reaching the screen.

## The shape of a plugin

```cpp
#include <sweeppp/plugin/Plugin.hpp>

namespace {
namespace plugin = sweeppp::plugin;

class MyPlugin final : public plugin::Plugin {
public:
    bool activate(plugin::Host& host) override
    {
        host.info("coming up");
        return host.registerFacet(facets()[0], this) == SWEEPPP_PLUGIN_OK;
    }
};

const sweeppp_plugin_desc_t& describe() { /* static storage */ }
} // namespace

SWEEPPP_PLUGIN_MAIN(MyPlugin, describe)
```

`plugins/bandplan/` is the worked example, and is a real plugin rather than a
demo: a TOML loader and a contributor facet, storing its plan choice both in
its own settings file and in a saved profile. It does not draw its spans — the
host paints them, names them, and gives it a dataset selector, which is what a
contributor facet is for. Its one UI facet is the **Bands** button on the
toolbar and the tree behind it, which decides which services are handed over at
all.

Everything reachable from the descriptor — every string, every array, every
vtable — must have **static storage duration**. The host reads them long after
`sweeppp_plugin_query` returns, and keeps reading them after `deactivate`.

## Facets

A facet is one thing a plugin contributes. One plugin may provide several, and
each is listed and can fail independently — a plugin whose FFT backend lost a
name race still draws its overlay.

| Facet           | Runs on                                  | Notes                                                                                                           |
|-----------------|------------------------------------------|-----------------------------------------------------------------------------------------------------------------|
| Contributor     | the caller's thread, rarely              | What is known about a frequency. Band plans, channel lists, beacon tables. The host draws it.                   |
| Frame processor | a host-owned worker, one frame at a time | May be slow; the cost is its own dropped frames.                                                                |
| SDR device      | its own transfer thread                  | Push: it fills blocks from the host's pool.                                                                     |
| RF path         | the sweep thread, a few times a pass     | An antenna switcher between a receive port and several antennas. Not a radio: no samples, no tuning, no stream. |
| FFT backend     | any pipeline worker, concurrently        | Only if it says `thread_safe_execute`.                                                                          |
| UI extension    | the UI thread, once per frame            | Never concurrently.                                                                                             |

Declare them in the manifest so the Plugins panel can say what a plugin does
before it has ever been activated; register them in `activate`, because whether
you can actually provide one may depend on the host. A facet you declare and
then decline should be reported with `Host::reportFacet`, or the listing shows
"inactive" with nothing said about it.

## Contributing frequency data

Several plugins answer the same frequency at once and **all** of their answers
are readable — a band plan saying *"2.4 GHz ISM, an allocation"* alongside a
channel list saying *"Wi-Fi channel 6, a specific 20 MHz channel"*. Which of
them titles the marker is the operator's ordering, not an accident of which
span happens to be narrower.

One contribution is a name, a description, a category, a span and a colour —
plus a **type**, which is what makes a wide allocation and a specific channel
distinguishable rather than both being "a range":

| Type                           | Means                                                                    |
|--------------------------------|--------------------------------------------------------------------------|
| `SWEEPPP_CONTRIBUTION_BAND`    | A wide allocation. Indicative: *"this region is for X"*.                 |
| `SWEEPPP_CONTRIBUTION_CHANNEL` | A defined channel with a width: Wi-Fi channel 6, FPV raceband F4.        |
| `SWEEPPP_CONTRIBUTION_SPOT`    | One known frequency: a beacon, a repeater output. `stop_hz == start_hz`. |

`category` stays a free string — it is what the colour encodes, and the host
does not need to know your vocabulary to draw it.

```cpp
std::uint32_t contributionsAt(double hz, sweeppp_contribution_t* out,
                              std::uint32_t capacity) const
{
    // Most specific first: you know your own nesting, and the host keeps the
    // order you return rather than re-deriving it from the widths.
    const std::vector<const Band*> found = plan->bandsAt(hz);
    for (std::uint32_t i = 0; i < std::min(capacity, (std::uint32_t)found.size()); ++i) {
        out[i] = plugin::contribution(SWEEPPP_CONTRIBUTION_BAND, found[i]->name,
                                      found[i]->group, found[i]->startHz,
                                      found[i]->stopHz, color, found[i]->description);
    }
    return (std::uint32_t)found.size();   // how many there ARE, not how many fitted
}
```

Both queries return how many there **are**, which may exceed `capacity`, so the
host sizes a buffer with one call and fills it with a second.

### The host draws it

By default, and that is the point. Spans, edges, per-type styling, label
collision and the inset that clears the readout row have one implementation in
the host; a contributor ships no drawing code and links no ImGui.

The **type decides the drawing**, and the difference is one of scale rather
than of taste:

- A **band** is a bar the width of the allocation, with its name inside it,
  stacked so an allocation nested in another reads as two bars rather than one
  hiding the other. An allocation narrower than a few pixels at the current
  span is not drawn at all — five hundred of them at 6 GHz is a row of coloured
  dashes that says nothing — and comes back on zooming in. The marker's chip
  names it meanwhile, so nothing is unreachable.
- A **channel** and a **spot** are *flags*: a named chip and nothing else,
  stacked into as many rows as it takes to name every one of them. A channel
  list carries hundreds, and painting those as filled spans — or even as a
  vertical each — is a wash over the trace they are annotating.
- Where even the stack runs out, the remainder collapses into a `…` chip. Hover
  it and a panel opens flush beneath, listing every one of them — a panel
  rather than a tooltip, so it can be moved onto and scrolled when there are
  sixty of them. "Nothing here" and "sixty things here and no room" are never
  the same picture.

Hovering a chip describes what it names and drops a line to where it actually
is. **Clicking** it pins that line and adds its width as a stripe under the
chip; clicking a bar instead tints its extent over the trace. Clicking it
again, or clicking the plot, puts it away. One at a time, deliberately: a line
under every chip and a fill under every bar read as a texture rather than as a
measurement.

**Ctrl-click** (or command-click) dismisses the contribution instead. That is
not something the host does behind your back — it calls your optional `hide`
slot, so your own settings can agree about it:

```cpp
bool hide(const sweeppp_contribution_t& which);   // wired only if T defines it
```

A contributor that leaves it undefined simply cannot be dismissed that way,
which is the right answer for one whose entries are not individually
switchable.

Reach for `plugin::makeContributorVtable<T>()` and you get all of it:

```cpp
plugin::facet(SWEEPPP_FACET_CONTRIBUTOR, "bands", "Band allocations",
              "Named frequency allocations, by service", &contributorVtable())
```

A plugin whose visual the host has no vocabulary for declines with
`makeContributorVtable<T>(SWEEPPP_CONTRIBUTION_RENDER_NONE)` and draws through
a UI facet instead — and still contributes its data to the ranked answer.
Contributing and drawing are separate questions.

### Datasets

One per band-plan file, per channel table. The host owns the selector, which is
the other half of a contributor shipping no UI: report `dataset_count`,
`dataset_name`, `dataset_description` and `active_dataset`, honour
`select_dataset`, and the combo appears in **Menu → Data contributors**. Zero
datasets is legal for a contributor with a single fixed one.

### Tick trees

A contributor whose entries are switched on and off one by one reports them as
rows, so a host without the plugin's own UI — the browser — can draw the tree:

```cpp
const std::vector<plugin::TreeRow>& treeRows();     // wired only if T defines both
bool toggleTreeRow(std::string_view key);
```

Rows come in drawing order, a row's children after it one level deeper.
`toggleTreeRow` is a click on that row's tick; `plugin::tickAction(row)` says
what it means. `plugin::drawTreeRows(rows)` draws the same rows in your own
ImGui popover, so the two cannot drift.

### Priority

The operator's order lives in `<configDir>/plugins.toml` beside
`plugins.disabled`, for the same reason enablement does: it is an arrangement
of the application, not a measurement setup, so loading a profile must not
silently change who names the band under the marker.

    [plugins]
    contributors = ["org.sweeppp.channels", "org.sweeppp.bandplan"]
    contributors_hidden = ["org.sweeppp.detections"]

The rank is that position first; every plugin the list does not name comes
after every plugin it does, in load order. Within one plugin, the order that
plugin returned. `contributors_hidden` is drawing only — a hidden contributor
still answers the marker's chip.

### Channel files

The format the **Channels** plugin reads is user data rather than plugin code,
so it is documented in [Plugins → Channel files](plugins.md#channel-files).

### Tick trees

The **Channels** plugin turns each channel file into a branch of the tree
behind the channels button on the toolbar. Its ticks live in
`<configDir>/plugins/org.sweeppp.channels.toml` as a list of *disabled* ids, so
a group added by a later data file appears rather than hiding. Group ids use
`/` rather than `.` as the path separator, because the id is also the
persistence key and a profile is flattened to dotted keys.

Every row of the tree — the file, each group, each individual channel — carries
its own tick, and a tick only ever sets its own row. Unticking a group
therefore *hides* its subtree without disturbing what is ticked inside it, so
ticking it again brings back the selection the operator built rather than
everything the file contains. A parent whose descendants disagree draws as a
dash; clicking a branch that is on but empty is the one case that means "show
me all of this".

The **Band plan** plugin's section works the same way, over service categories
and the allocations inside them. Both trees use `plugin::drawTick`, which is in
`<sweeppp/plugin/Plugin.hpp>` rather than in either plugin: two controls with
the same three-way meaning must not be able to disagree about it, and written
twice they eventually would. Reach for it if you build a tree of your own —

```cpp
if (const auto action = plugin::drawTick(anyOn, allOn, ownOn)) {
    switch (*action) {
    case plugin::TickAction::Hide:    setKey(key, false); break;  // keep what is inside
    case plugin::TickAction::Unhide:  setKey(key, true);  break;
    case plugin::TickAction::ShowAll: enableSubtree(index); break; // on, but empty
    }
}
```

## Events

Typed, and open. The type is a reverse-DNS **name**, the payload is the struct
that name denotes, and both cross together:

```cpp
struct PlanChanged {
    std::uint32_t struct_size;                       // first, always
    sweeppp_str_t name;

    static constexpr plugin::EventName kEventType{"org.sweeppp.bandplan.plan_changed"};
    static constexpr std::uint32_t kSchemaVersion = 1;
};

host.publish(PlanChanged{.name = plugin::str(m_planName)});
host.subscribe<sweeppp_retune_event_t>(+[](void*, const sweeppp_retune_event_t& e) { ... });
```

Two things follow from the name being the key rather than an enum:

- **Adding an event type is not an ABI change.** Any plugin can define one and
  any other plugin can listen for it. The host does not have to have heard of
  it.
- **A payload can grow.** Each carries `struct_size`, and a subscriber built
  against a smaller version is dropped rather than reading past the end.

`sweeppp.` is the host's namespace. A plugin may publish only names beginning
with its own id — a plugin able to publish `sweeppp.retune` could make a
session file say the radio moved when it did not, with nothing downstream able
to tell the difference.

Handlers run on the **publishing** thread. A retune comes off the sweep thread
thousands of times a second, so a handler must return promptly; anything slow
belongs behind a frame-processor facet, which already has a thread and a queue.

## Settings, and profiles

Two places, and the distinction matters:

- `<configDir>/plugins/<id>.toml`, through `PluginSettings` — how *this
  installation* is configured. The plugin owns the file; the host only names it.
- A **profile**, through `saveProfile` / `loadProfile` — a setup an operator
  names and comes back to. Keys land under `plugins.<id>.` and are carried
  through a profile verbatim, so a profile saved on a machine with a plugin
  still loads on one without it and hands the keys back if the plugin returns.

A plugin's enablement lives in `<configDir>/plugins.toml`, which is neither of
those: `settings.toml` and every named profile are the same `Profile` struct,
so enablement living there would mean loading a profile silently swapping the
plugin set.

The running configuration is readable through `Host::profileKeys` /
`profileString` and friends, under the same dotted keys a profile is written
with — `sweep.start`, `display.grid`, `device.driver`. Read-only: a plugin that
wants something changed asks on the event channel, where the request is
addressed and refusable.

### Files the operator chooses

`Host::chooseFile(title, suggestedName, extension)` opens the application's own
native save dialog and returns what the operator picked, or an empty path if
they cancelled. `Host::canChooseFile()` says whether there is one at all —
false in a headless build, and false on a host older than the call.

The dialog belongs to the application rather than to this library, and a plugin
linking its own copy would put two native file-dialog implementations in one
process. So it is lent across the ABI instead. It blocks until the operator
answers, which makes it a UI-thread call and never a worker one.

```cpp
if (host.canChooseFile()) {
    const std::filesystem::path path = host.chooseFile("Export", "peaks.csv", "csv");
    if (path.empty()) { return; }          // cancelled
    write(path);
} else {
    write(host.dataDir() / "peaks.csv");   // nowhere to ask; say where it went
}
```

## Recording

A plugin can write into the same `.sweeps` file as the sweep it is analysing:

```cpp
if (host.sessionOpen()) {
    host.writeSessionRecord("peaks", 1, blob.data(), blob.size());
}
```

The container has reserved a record type and an event kind for plugins since
its first release, precisely so this did not have to become a sidecar file. An
unknown plugin record reads back as opaque bytes rather than corrupting
anything, so a recording remains readable by someone who does not have your
plugin.

## Drawing

The plugin links its **own** copy of ImGui and adopts the host's context and
allocators — ImGui's documented arrangement for a dynamically loaded module.
Call `Host::adoptImGui` during activation and do not draw if it refuses:

```cpp
if (std::string reason; !host.adoptImGui(reason)) {
    host.reportFacet(uiFacet, reason);
    return true;                       // still worth coming up for its other facets
}
```

Allocators go over before the context, deliberately: both libraries allocate on
first use, and memory taken from one copy's allocator and returned to the
other's is a heap corruption that surfaces days later somewhere unrelated.

Reach for `plugin::makeUiVtable<T>(spots)` and define only the members for the
spots you named — a slot your class does not define is left null rather than
made a compile error:

| Spot                                     | Where it draws                                                                              |
|------------------------------------------|---------------------------------------------------------------------------------------------|
| `SPECTRUM_OVERLAY` / `WATERFALL_OVERLAY` | Inside the plot, clipped to it. `void drawOverlay(const PlotContext&)`                      |
| `SETTINGS`                               | The plugin's own section in **Menu**. `bool drawSettings()` — false means "nothing to show" |
| `STATUS_CHIP`                            | A readout among the status bar's chips, at its left. `void drawStatusChip()`                |
| `STATUS_ACTION`                          | A button in the status bar, after the host's own panel buttons. `void drawStatusAction()`   |
| `TOOLBAR`                                | A button on the toolbar, beside the panel launchers. `void drawToolbar()`                   |
| `WINDOW`                                 | A floating window of your own. `void drawWindow()`                                          |
| —                                        | `void openPanel()` — the keyboard asked for your toolbar panel; see below                   |

`WINDOW` is dispatched once per frame at the top level, beside the
application's own floating windows and outside every popup and child the other
spots draw inside. It exists because the alternatives are broken rather than
merely inelegant: `SETTINGS` is called only while the menu popup is open, so a
window begun there vanishes the moment the popup closes, and `TOOLBAR` is
dispatched inside the toolbar's own child window. The plugin owns whether its
window is open — return early when it is not.

### A button on the bar

`TOOLBAR` is dispatched inside the toolbar's own child window, after the host's
launchers, with the bar's frame padding pushed. Three things follow from that,
and `<sweeppp/plugin/PluginChrome.hpp>` is where they live:

- the bar lays its controls out in a row and does not know your button was
  submitted, so end the draw with `ImGui::SameLine()`;
- a popover opened from the bar inherits the bar's metrics — every checkbox in
  it comes out a 29px square — so open it inside a `chrome::PanelMetrics` scope
  and place it with a `chrome::PopoverAnchor`;
- `chrome::toolbarToggle(label, on)` draws a button that is the state as well
  as the switch: lit while what it governs is on screen, dimmed while it is not.

`STATUS_ACTION` is the same button one bar down: it is dispatched in the status
bar after the host's own History and Performance buttons, and the first two
rules above apply to it unchanged — end with `ImGui::SameLine()`, and qualify
every id you submit, because that bar shares one id stack with the host's
controls and with every other plugin's. It is the spot for a button that opens
a panel of your own, where `STATUS_CHIP` is for a number being read.

What such a button usually governs is the host's own drawing — which **kinds**
of contribution the spectrum paints, the same switch the operator's B and C
keys flip:

```cpp
const bool shown = host.contributionsShown(SWEEPPP_CONTRIBUTION_BAND);

if (chrome::toolbarToggle(host.icon(kIcon, "Bands").append("##bands").c_str(), shown)) {
    host.setContributionsShown(SWEEPPP_CONTRIBUTION_BAND, !shown);
}
if (ImGui::IsItemHovered()) { ImGui::SetTooltip("..."); }

m_popover.openOnItemClick("##tree");    // right-click: under the button
m_popover.place();
{
    const chrome::PanelMetrics metrics;
    if (ImGui::BeginPopup("##tree")) { drawTree(); ImGui::EndPopup(); }
}
ImGui::SameLine();
```

Read it every frame rather than caching it: the keys reach the same flag, and a
button lit from its own copy sits lit over a plot with nothing on it. `SPOT` rides with `CHANNEL`, because that is how the host paints it — a
beacon is a channel of no width.

The keyboard reaches the panel too — **shift+B** and **shift+C** — and the host
routes that by what the button governs rather than by any plugin's name. Say
which type yours switches, and define `openPanel()`:

```cpp
plugin::makeUiVtable<T>(SWEEPPP_UI_SPOT_BIT(SWEEPPP_UI_SPOT_TOOLBAR),
                        SWEEPPP_UI_LAYER_UNDER, SWEEPPP_CONTRIBUTION_BAND);

void openPanel() { m_panelRequested = true; }   // recorded, not acted on
```

`openPanel` runs before `drawToolbar` in the same frame and **must not open the
popup itself**: an ImGui popup id is hashed against the id stack of the window
it is opened in, and yours lives inside the toolbar's child window under the
per-instance id `makeUiVtable` pushes. `PopoverAnchor::takeRequest(id, flag)` is
the other half:

```cpp
const bool shut = m_popover.takeRequest("##tree", m_panelRequested);
...
if (ImGui::BeginPopup("##tree")) {
    if (shut) { ImGui::CloseCurrentPopup(); }
    drawTree();
    ImGui::EndPopup();
}
```

It opens the popover when it is shut and returns "close yourself" when the same
keystroke arrives while it is already open — a key that only ever opens cannot
be taken back the way it was made.

The anchor is a **member**, not a local, because where the panel hangs from has
to outlive the frame that decided it. A right-click opens it under the button,
where the operator is already looking; a keystroke opens it at the pointer,
because the eye is on the spectrum rather than on a bar the hand never went
near. Placing it under the button on the second frame would snap it back the
moment it appeared.

**This is the only host state a plugin may write**, and it is display only. The
running configuration beside it stays read-only for the reason given under
[Settings, and profiles](#settings-and-profiles): a plugin that could write
there could retune the radio as a side effect of a settings panel. A drawing
switch cannot.

`Host::icon(glyph, fallback)` gives the glyph when the host's icon font is in
the atlas and the words when it is not — the same fallback the host's own
buttons make, so your button is not the one control on the bar drawn as an
empty box. The glyph is the plugin's: the host has no idea what the button
means.

Inside an overlay you are already within the host's `PushPlotClipRect()` scope
and inside its `BeginPlot`/`EndPlot`. Use the supplied `ImDrawList*`; do not
open a plot of your own. `PlotContext` gives you `xForHz`, `hzForX` and
`yForDb`, reimplemented from the same numbers the host uses rather than called
back per point.

An exception escaping a draw disables that facet with the reason shown in the
panel — the same containment `FrameBus::publish` gives frame consumers. One
misbehaving plugin must not take down the instrument.

## Where plugins are found

Highest precedence first, first id wins:

| Directory                                                       | What counts as a plugin |
|-----------------------------------------------------------------|-------------------------|
| `$SWEEPPP_PLUGIN_PATH` (`:`-separated, `;` on Windows)          | anything                |
| `<configDir>/plugins`                                           | anything                |
| `<exeDir>/plugins`, or `Contents/PlugIns` inside a macOS bundle | anything                |
| `<libdir>/<appId>/plugins` (`sweeppp`, or `sweeppp-nightly`)    | anything                |
| `<libdir>`                                                      | `sweeppp-plugin-*` only |

`<libdir>` is each system library directory in turn:

- Linux: `/usr/local/lib/<triplet>`, `/usr/lib/<triplet>`, `/usr/local/lib`,
  `/usr/lib`. The multiarch directories come first because that is where a
  packaged library actually lands on Debian and its derivatives, so leaving
  them out would mean a `.deb`'s plugin was never found.
- macOS: `/opt/homebrew/lib`, `/usr/local/lib`, `/usr/lib`.
- Windows: `%ProgramFiles%\sweeppp` and `%ProgramW6432%\sweeppp`.

[Plugins → Installing plugins](plugins.md#installing-plugins) is the same list
for people installing a plugin rather than writing one.

**The two rules differ on purpose.** A directory that exists to hold plugins is
a statement of intent, so anything in it is a candidate whatever it is called.
A directory shared with the rest of the system is not: `/usr/lib` holds
thousands of shared objects, and the only way to find out whether one is a
Sweep++ plugin is to `dlopen` it — which runs its initialisers. Doing that to
every library on the machine to find the two that are ours is not a cost or a
risk worth taking. The filename is what makes the question answerable without
asking it.

So a plugin that will ever be **packaged** should be called
`sweeppp-plugin-<name>.<ext>`; `sweeppp_add_plugin`'s `NAME` argument produces
exactly that. `libsweeppp-plugin-<name>` is accepted too, for build systems
that insist on the `lib` prefix. A plugin that only ever lives in a directory
of ours can be called anything.

`.so`, `.dylib` and `.dll` are accepted **on every platform**. That is not
sloppiness: CMake's `add_library(MODULE)` with `PREFIX ""` emits a `.so` on
macOS, so a loader that guessed the extension from the platform would fail to
find the plugins it had just built.

`sweeppp-cli info` lists what was found, what was not, and why — including
which directories are prefix-gated, so "my plugin is not being found" and "my
plugin is not called `sweeppp-plugin-*`" are distinguishable without reading
this file.

## Writing a driver

`<sweeppp/plugin/PluginSdr.hpp>` gives you two base classes that mirror the
application's own interfaces member for member, so a driver that was built in
becomes a plugin by changing what it derives from:

```cpp
class MyRadio : public sweeppp::plugin::Radio {
    Status start(plugin::Stream& stream, const StreamConfig& config) override;
    // info, parameters, get/setParameter, nativeFormat, stop, streaming,
    // retune -- and everything else is optional, with a default behind it.
};

class MyDriver : public sweeppp::plugin::Driver {
    std::vector<SdrDeviceInfo> enumerate() const override;
    Result<std::unique_ptr<plugin::Radio>> open(std::string_view id) override;
};

const auto kVtable = sweeppp::plugin::makeSdrDriverVtable<MyDriver>();
// facet(SWEEPPP_FACET_SDR_DEVICE, "myradio", ...) -> host.registerFacet(...)
```

This header is allowed to include libsweeppp's SDR *value* headers under
[the one rule](#the-one-rule): `SdrParameter::coerce`, `SampleFormat` and
`SdrDeviceInfo` are pure computation over their arguments.

**Samples are pushed, in the radio's own format.** The host lends you a block
from its pool; you fill it and hand it back. Nothing converts on the way, which
is what makes 100 MS/s reachable — a `float` conversion on a transfer thread is
800 MB/s, more than the link delivers.

```cpp
plugin::Block block = stream.acquire();
if (!block) {                        // the pool is empty -- see below
    stream.reportPoolExhausted(framesLost);
    return;
}
std::memcpy(block.as<std::int8_t>(), transfer, bytes);

sweeppp_sdr_delivery_t delivery{};
delivery.frames = frames;
delivery.format = plugin::toAbiFormat(SampleFormat::Cs8);
delivery.sequence = m_sequence++;
delivery.center_hz = m_centerHz;
stream.publish(std::move(block), delivery);
```

**Never wait for a block.** An empty pool means the consumer side is behind,
and it is a normal answer rather than a failure. Blocking on a transfer thread
turns a host-side backlog into a device-side overrun, which is both a worse
failure and one that points at the wrong half of the system. Drop the data,
report it, return. `Block` is move-only and releases in its destructor, so an
early return on any other path cannot shrink the pool.

**`parameters()` is the one exception to static storage.** Its result, and
every string reachable from it, must stay valid until the next call to it or
until the radio is destroyed — not for ever. That exception exists because a
real radio needs it: a bladeRF's gain modes are read from the hardware at open,
so no static table could express them. Everything else you hand the host still
has to outlive the process. `rxPorts()` below follows the same rule.

**A radio with one connector implements none of the receive-port entries.**

```cpp
std::span<const SdrRxPort> rxPorts() const noexcept override;   // rx_ports
std::string_view selectedRxPort() const noexcept override;      // selected_rx_port
Status selectRxPort(std::string_view id) override;              // select_rx_port
```

Leave all three alone and the host treats the radio as having a single
implicit input: no port chooser in the panel, and nothing for a routed sweep to
choose between. That is the right answer for a HackRF and the wrong one for a
bladeRF 2.0, which has two RX connectors and reports them here.

A port is not an enum parameter spelling `"RX1"`. It carries frequency limits,
whether it can supply bias-T, and what a switch costs — and the sweep planner
reads all three, because a routed pass sends each part of the plan through the
connector whose antenna covers it and has to predict what the switching costs.
The `id` is what an antenna assignment is stored under, so it must be stable
across firmware updates that reorder the list; index into a list is not.

Declare `requires_stop` if the port cannot be selected on a live stream — the
host stops the device's stream, calls you, and starts it again, leaving its
workers, pool and FFT plan alone. Charge the whole cost of that in
`switch_seconds`: the Analysis panel prints the predicted pass time, and a plan
whose antennas interleave twenty times spends real seconds a pass switching
that the operator needs to see *before* pressing Start.

**`deactivate` must not touch an open device.** `shutdown()` withdraws every
facet whether or not a radio is still open, so closing your library there pulls
it out from under one that is live. Release in the device's own destructor
instead. This is the exception to "the host will not withdraw a facet in use"
below — an application must close its radios before shutting the host down, and
the ones in this repository do.

## Writing an antenna switcher

A 2/4/8/16/32-way box sitting behind one receive port, whose inputs each carry
an antenna of their own. The chain the host routes through becomes
port → switcher → input → antenna.

It is a facet of its own — `SWEEPPP_FACET_RF_PATH`, with
`sweeppp_rf_path_factory_vtable_t` and `sweeppp_rf_path_vtable_t` — and not an
SDR device wearing a hat. A switcher has no samples, no tuning and no stream,
and listing one among the radios would be a lie the device chooser cannot
recover from: an operator picking it would get a receiver that never delivers
a block.

```c
uint32_t (*inputs)(void* path, sweeppp_rf_path_input_t* out, uint32_t capacity);
sweeppp_plugin_status_t (*select_input)(void* path, uint32_t index);
uint32_t (*selected_input)(void* path);
```

`select_input` takes an index rather than an id because this one *is* on the
sweep path: the engine may drive it several times a pass and must not be
comparing strings to do it. The `id` in `inputs` is what the operator's
assignment is stored under, so it has to be stable across firmware updates.

Discovery and opening mirror the SDR factory exactly, error channel included —
a switcher is found and opened like a radio even though it is not one. Report
`switch_seconds` honestly: the host charges it per transition when it predicts
a pass, and moving between two antennas on one box pays only that, because the
receiver's connector never changes.

## Writing an FFT backend

`<sweeppp/plugin/PluginFft.hpp>` has no base classes to offer, because there is
nothing to mirror: a backend implements `IFftBackend` and `IFftPlan`, the
application's own interfaces, and the header is the vtable over them.

```cpp
class MyPlan : public sweeppp::IFftPlan {
    void execute(const std::complex<float>* in, std::complex<float>* out) noexcept override;
    // executeBatch, size, inverse
};

class MyFft : public sweeppp::IFftBackend {
    Result<std::unique_ptr<IFftPlan>> createPlan(const FftPlanConfig&) override;
    // name, displayName, capabilities -- and snapSize/supportsSize have defaults
    // derived from what capabilities() declared
};

const auto kVtable = sweeppp::plugin::makeFftBackendVtable<MyFft>();
// facet(SWEEPPP_FACET_FFT_BACKEND, "myfft", ...) -> host.registerFacet(...)
```

Like `PluginSdr.hpp` this header includes libsweeppp's own headers under
[the one rule](#the-one-rule): `IFftBackend`, `IFftPlan` and `FftCapabilities`
are pure computation over their arguments. `FftBackendManager::instance()` is
the singleton it must not touch, and the host registers for you.

**The facet id is the backend name the operator sees and a profile stores.**
`--fft-backend fftw` and the saved backend preference are that string, so it is
not a label to be tidied later.

**`thread_safe_execute` is the capability that matters.** It says one plan may
be executed from several threads at once with per-thread buffers, and that is
exactly what the pipeline does: it builds one plan per size and hands it to
every worker. Declare it false and the transform still works; declare it true
when it is not and the failure is a data race under load, which is the hardest
kind of bug this ABI can produce. `thread_safe_planning` is separate and
usually false — FFTW's planner, for one, mutates global state.

**Plans are executed on the caller's buffers.** Nothing is owned across the
boundary: `execute` receives an input and an output pointer, so a plan holding
scratch of its own would force one plan per worker and multiply planning cost
by the worker count.

**What `createPlan` says does not cross.** The ABI's `create_plan` answers with
a status code, so the sentence in your `Result` stops at the boundary — the host
turns the code into "FFT backend 'x' could not plan size N: <status>". Log
anything more specific yourself, through `Host::as()`.

**Sizes come from resolution bandwidth, not from a menu.** The sweep planner
divides sample rate by RBW and calls `snapSize` on the result, so a backend
declaring `FftSizeConstraint::Any` will be handed arbitrary sizes and has to
mean it. Declaring `PowerOfTwo` is honest and costs nothing but a rounded-up
bandwidth.

**`supportsSize` does not cross the ABI, so do not out-clever it.** The vtable
carries `snap_size` but not `supports_size`: the host answers the latter from
your declared `FftCapabilities` alone. A backend accepting some third set of
lengths — vDSP takes `f · 2ⁿ`, which the enum cannot express — must therefore
declare the constraint it can *describe*, not the one it can serve. Declare
`Any` and snap to your real lattice and the host will plan a size its own
`supportsSize` then rejects, which surfaces far from the cause.

**Null-check every setup, whatever the vendor header promises.** Accelerate's
interleaved DFT documents a set of accepted lengths and then declines
everything above 4096, returning `NULL` with no error code — so
`plugins/fft-accelerate/` keeps a second plan strategy and picks between them
at plan time. If your library has more than one way to run a transform, choose
per plan rather than per execute, and log which one a plan took.

## Two things the host will not do

**It will never `dlclose` a module.** Disabling a plugin withdraws every facet
and stops every callback, but the image stays mapped until the process exits.
Unmapping means unmapping every `std::function` target, vtable, `shared_ptr`
deleter and exception type the module ever handed out, and unlike a leaked
allocation a stale pointer into an unmapped page is a crash with no useful
stack. Re-enabling re-activates the module that is already there.

**It will not withdraw a facet that is in use.** An FFT backend the pipeline
has acquired, or a driver with an open radio, refuses — and that refusal is
what becomes the "restart needed" marker in the panel. The preference is saved
either way, so the restart does what was asked.
