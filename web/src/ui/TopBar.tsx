// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import type { ReactNode } from "react";

import { focused, resolveView, spanCentre, zoomAbout } from "../model/layout";
import { frequencyShort } from "../render/format";
import { start, stop } from "../state/actions";
import { type Contributor, contributorsStore } from "../state/contributors";
import { useInstrument } from "../state/instrument";
import { setPanel, setView, viewStore } from "../state/view";
import { ContributorTree } from "./ContributorTree";
import { Popover, Switch } from "./controls";
import { Icon } from "./Icon";
import { icon } from "./icons";
import { sweptSpan } from "./PanelGrid";
import { ChartPopover } from "./popovers/ChartPopover";
import { MarkersPopover } from "./popovers/MarkersPopover";
import { PanelsPopover } from "./popovers/PanelsPopover";
import { RangePopover } from "./popovers/RangePopover";
import { WaterfallPopover } from "./popovers/WaterfallPopover";

/** The panels the bar opens, built by the workspace. */
export interface BarPanels {
  device: ReactNode;
  analysis: ReactNode;
  menu: ReactNode;
}

export function BarButton({
  children,
  title,
  active,
  ...rest
}: { children: ReactNode; title: string; active?: boolean } & React.ButtonHTMLAttributes<HTMLButtonElement>) {
  return (
    <button className={`btn gap-1.5 ${active ? "border-accent bg-accent/25" : ""}`} title={title} {...rest}>
      {children}
    </button>
  );
}

/** What the bar knows about the contributors that ship with Sweep++: their
 * glyph, and which of the view's switches their button carries. */
const kKnown: Record<string, { glyph: string; label: string; flag: "showBands" | "showChannels"; key: string }> = {
  "org.sweeppp.bandplan": { glyph: icon.bands, label: "Bands", flag: "showBands", key: "B" },
  "org.sweeppp.channels": { glyph: icon.channels, label: "Channels", flag: "showChannels", key: "C" },
};

/** One contributor's button: lit while what it contributes is drawn, and its
 * tree behind it. */
function ContributorButton({ contributor, disabled }: { contributor: Contributor; disabled: boolean }) {
  const known = kKnown[contributor.id];
  const shown = useSelector(viewStore, (v) => (known ? v[known.flag] : true));
  const label = known?.label ?? contributor.name;
  return (
    <Popover
      trigger={
        <BarButton title={`${label}: what is drawn on the spectrum, and the tree behind it`} active={shown}>
          <Icon path={known?.glyph ?? icon.settings} />
        </BarButton>
      }
      title={contributor.name}
      width={440}
    >
      {known && (
        <div className="mb-2">
          <Switch
            label={`Show on the spectrum (${known.key})`}
            checked={shown}
            onChange={(on) => setView({ [known.flag]: on })}
          />
        </div>
      )}
      <ContributorTree contributor={contributor} disabled={disabled} />
    </Popover>
  );
}

export function TopBar({ panels }: { panels: BarPanels }) {
  const instrument = useInstrument();
  const layout = useSelector(viewStore, (v) => v.layout);
  const contributors = useSelector(contributorsStore, (c) => c.contributors);
  const panel = focused(layout);
  const plan = instrument.plan;
  const running = instrument.run.running;
  const label = instrument.device.descriptor
    ? instrument.device.displayLabel || instrument.device.descriptor.info.label
    : "Select device";

  const lowest = Math.min(...plan.segments.map((s) => s.startHz));
  const highest = Math.max(...plan.segments.map((s) => s.stopHz));
  const rangeCaption = !instrument.run.sweeping
    ? "Fixed tune"
    : plan.segments.length > 1
      ? `${plan.segments.length} ranges   ${frequencyShort(lowest)} - ${frequencyShort(highest)}`
      : plan.segments[0]
        ? `Start ${frequencyShort(lowest)}   Stop ${frequencyShort(highest)}`
        : "No range";

  const zoom = (factor: number) => {
    const view = resolveView(panel, sweptSpan());
    const next = zoomAbout(view, spanCentre(view), factor);
    setPanel(panel.id, { viewStartHz: next.startHz, viewStopHz: next.stopHz });
  };

  return (
    <div className="flex min-h-11 shrink-0 flex-wrap items-center gap-1.5 border-b border-border bg-header px-2 py-1.5">
      <Popover
        trigger={
          <BarButton title="Device settings and the server's recordings">
            <Icon path={icon.device} />
            <span className="max-w-48 truncate">{label}</span>
          </BarButton>
        }
        title={label}
        width={420}
      >
        {panels.device}
      </Popover>
      <Popover
        trigger={
          <BarButton title="Resolution, window and throughput">
            <Icon path={icon.analysis} />
          </BarButton>
        }
        title="Analysis"
        width={420}
      >
        {panels.analysis}
      </Popover>
      <Popover
        trigger={
          <BarButton title="Display, theme, profiles, antennas and data contributors">
            <Icon path={icon.menu} />
          </BarButton>
        }
        title="Menu"
        width={400}
      >
        {panels.menu}
      </Popover>
      <Popover
        trigger={
          <BarButton title="Markers and what is under them">
            <Icon path={icon.marker} />
          </BarButton>
        }
        title="Markers"
        width={470}
      >
        <MarkersPopover />
      </Popover>
      {contributors
        .filter((c) => c.tree.length > 0)
        .map((c) => (
          <ContributorButton key={c.id} contributor={c} disabled={!instrument.canControl} />
        ))}

      <div className="mx-auto">
        <Popover
          trigger={
            <button
              className="btn min-w-72 justify-center whitespace-pre tabular-nums"
              title={
                "Click to edit the swept range.\n\nDrag: pan\nShift+drag: zoom\nShift+Ctrl+drag: sweep that band"
              }
            >
              {rangeCaption}
            </button>
          }
          width={520}
        >
          <RangePopover />
        </Popover>
      </div>

      <BarButton title="Fit the whole span in the focused panel" onClick={() => setPanel(panel.id, { viewStartHz: 0, viewStopHz: 0 })}>
        <Icon path={icon.resetZoom} />
      </BarButton>
      <BarButton title="Zoom out" onClick={() => zoom(1.5)}>
        <Icon path={icon.zoomOut} />
      </BarButton>
      <BarButton title="Zoom in" onClick={() => zoom(0.5)}>
        <Icon path={icon.zoomIn} />
      </BarButton>
      <Popover
        trigger={
          <BarButton title="Spectrum display: points, traces, fill and levels">
            <Icon path={icon.chart} />
          </BarButton>
        }
        title="Chart"
      >
        <ChartPopover />
      </Popover>
      <Popover
        trigger={
          <BarButton title="Waterfall: pause, history and gradient">
            <Icon path={icon.waterfall} />
          </BarButton>
        }
        title="Waterfall"
      >
        <WaterfallPopover />
      </Popover>
      <Popover
        trigger={
          <BarButton title="Panels: layout, mirror or spans">
            <Icon path={icon.layoutGrid} />
          </BarButton>
        }
        title="Panels"
      >
        <PanelsPopover />
      </Popover>
      <button
        className="btn ml-1 w-24 justify-center border-transparent font-semibold text-white"
        style={{ background: running ? "var(--color-stop)" : "var(--color-start)" }}
        disabled={!instrument.canControl || !instrument.device.descriptor}
        onClick={() => (running ? stop() : start())}
      >
        {running ? "Stop" : "Start"}
      </button>
    </div>
  );
}
