// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import type { ReactNode } from "react";

import { focused, resolveView, spanCentre } from "../model/layout";
import { decodeControl, section } from "../protocol/messages";
import { byteRate, frequencyShort } from "../render/format";
import { session, sessionStore } from "../state/session";
import { viewStore } from "../state/view";
import { Popover } from "./controls";
import { Icon } from "./Icon";
import { icon } from "./icons";
import { overlaysAt } from "./overlays";
import { sweptSpan } from "./PanelGrid";
import { saveSnapshot } from "./panels/menu/snapshot";
import { toastStore } from "./toasts";

/** The status bar's panels, built by the workspace. */
export interface StatusPanels {
  performance: ReactNode;
  history: ReactNode;
}

const css = ([r, g, b]: number[]) => `rgb(${r! * 255},${g! * 255},${b! * 255})`;

/** What is at the selected marker, or at the middle of the focused panel:
 * the first answer titles it, the rest are a hover away. */
function AtChip() {
  const hz = useSelector(viewStore, (v) => {
    const marker = v.markers.items.find((m) => m.id === v.markers.activeId);
    return marker?.visible ? marker.frequencyHz : spanCentre(resolveView(focused(v.layout), sweptSpan()));
  });
  // Re-read as the overlays arrive, which the session's version follows.
  useSelector(sessionStore, (s) => s.sectionsVersion);
  const found = overlaysAt(hz);
  const top = found[0];
  return (
    <span
      className="truncate text-dim"
      title={found
        .map((o) => `${o.name}\n${o.stopHz > o.startHz ? `${frequencyShort(o.startHz)} - ${frequencyShort(o.stopHz)}` : frequencyShort(o.startHz)}\n${o.pluginName} · ${o.type}${o.category ? ` · ${o.category}` : ""}`)
        .join("\n\n")}
    >
      At{" "}
      {top ? (
        <span style={{ color: css(top.color) }}>
          {top.name}
          {found.length > 1 && <span className="text-dim"> +{found.length - 1}</span>}
        </span>
      ) : (
        "-"
      )}
    </span>
  );
}

function StatusButton({ children, title, ...rest }: { children: ReactNode; title: string } & React.ButtonHTMLAttributes<HTMLButtonElement>) {
  return (
    <button className="btn h-6 px-1.5" title={title} {...rest}>
      {children}
    </button>
  );
}

export function StatusBar({ panels }: { panels: StatusPanels }) {
  const s = useSelector(sessionStore, (state) => state);
  const condition = useSelector(toastStore, (t) => t.condition);
  const values = session.section(section.values);
  const rxPort = values.getString("selectedRxPort");
  const control = decodeControl(session.section(section.control));
  const run = session.section(section.run);
  const engine = run.getHash("engine");

  return (
    <div className="flex h-8 shrink-0 items-center gap-4 overflow-hidden whitespace-nowrap border-t border-border bg-header px-3 text-xs">
      <AtChip />
      {rxPort && (
        <span className="text-dim">
          On <span className="text-text">{rxPort}</span>
        </span>
      )}
      <span className="text-dim" title="The link to the server: round trip and what arrives">
        Via{" "}
        <span className="text-text">
          {s.serverName}
          {control.shared && !control.you ? " · watching" : ""} · {s.roundTripMs.toFixed(0)} ms ·{" "}
          {byteRate(s.bytesPerSec)}
        </span>
      </span>
      <span className="flex items-center gap-1">
        <StatusButton title="Save the visible panels as a PNG" onClick={saveSnapshot}>
          <Icon path={icon.snapshot} />
        </StatusButton>
        <Popover
          trigger={
            <StatusButton title="Read a recording made on the server">
              <Icon path={icon.history} />
            </StatusButton>
          }
          title="Recordings on the server"
          width={360}
        >
          {panels.history}
        </Popover>
        <Popover
          trigger={
            <StatusButton title="Performance: input rate, drops, throttle, link and benchmark">
              <Icon path={icon.performance} />
            </StatusButton>
          }
          title="Performance"
          width={460}
        >
          {panels.performance}
        </Popover>
      </span>
      {condition && <span className="truncate text-warning">{condition}</span>}
      <span className="ml-auto hidden text-dim md:inline" title="Sweep rate measured on the server">
        <span className="text-text tabular-nums">{(engine.getFloat("measuredSweepRateHzPerSec") / 1e6).toFixed(1)}</span>{" "}
        MHz/s
      </span>
      <span className="hidden text-dim sm:inline">
        <span className="text-text tabular-nums">{s.linesPerSec.toFixed(0)}</span> lines/s
      </span>
    </div>
  );
}
