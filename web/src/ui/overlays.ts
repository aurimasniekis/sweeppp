// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { useEffect, useState } from "react";

import { type Span, spanWidth } from "../model/layout";
import { section } from "../protocol/messages";
import type { Overlay } from "../render/spectrum";
import { session, sessionStore } from "../state/session";

interface Fetched {
  span: Span;
  generation: number;
  overlays: Overlay[];
}

let cache: Fetched | null = null;

/** The server's counter for its band and channel labels. */
export function useOverlayGeneration(): number {
  return useSelector(sessionStore, () => session.section(section.overlays).getInt("generation", 0));
}

/** Bands and channels over `view`, from the server's plugins, in rank order.
 * Fetched for half a view either side, so a pan within that needs nothing new,
 * and again whenever the server says they changed. */
export function useOverlays(view: Span, bands: boolean, channels: boolean): Overlay[] {
  const generation = useOverlayGeneration();
  const [fetched, setFetched] = useState<Fetched | null>(cache);
  const covered =
    fetched &&
    fetched.generation === generation &&
    fetched.span.startHz <= view.startHz &&
    fetched.span.stopHz >= view.stopHz;

  useEffect(() => {
    if ((!bands && !channels) || covered || !(view.stopHz > view.startHz)) {
      return;
    }
    const margin = spanWidth(view) / 2;
    const span = { startHz: Math.max(view.startHz - margin, 0), stopHz: view.stopHz + margin };
    const controller = new AbortController();
    const timer = window.setTimeout(() => {
      fetch(`/api/overlays?from=${span.startHz}&to=${span.stopHz}`, { signal: controller.signal })
        .then((r) => (r.ok ? (r.json() as Promise<Overlay[]>) : []))
        .then((overlays) => {
          cache = { span, generation, overlays };
          setFetched(cache);
        })
        .catch(() => undefined);
    }, 250);
    return () => {
      window.clearTimeout(timer);
      controller.abort();
    };
  }, [view.startHz, view.stopHz, bands, channels, covered, generation]);

  if (!fetched) {
    return [];
  }
  return fetched.overlays.filter(
    (o) => (o.type === "band" ? bands : channels) && o.stopHz > view.startHz && o.startHz < view.stopHz,
  );
}

/** Everything at `hz`, ranked, from what the plots last fetched -- the
 * marker's "At" chip. Narrowest first within one contributor, as the server's
 * own answer is. */
export function overlaysAt(hz: number): Overlay[] {
  if (!cache) {
    return [];
  }
  const rank = new Map<string, number>();
  for (const o of cache.overlays) {
    if (!rank.has(o.plugin)) {
      rank.set(o.plugin, rank.size);
    }
  }
  return cache.overlays
    .filter((o) => (o.stopHz > o.startHz ? hz >= o.startHz && hz < o.stopHz : hz === o.startHz))
    .sort((a, b) => rank.get(a.plugin)! - rank.get(b.plugin)! || a.stopHz - a.startHz - (b.stopHz - b.startHz));
}
