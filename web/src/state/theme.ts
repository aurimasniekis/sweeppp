// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { Store } from "@tanstack/react-store";

/** A theme as /api/themes describes it: the desktop's own, colours as hex. */
export interface Theme {
  name: string;
  dark: boolean;
  chrome: Record<string, string | number>;
  spectrum: Record<string, string | number>;
  waterfall: { colorMap: string };
}

export interface ColorMap {
  name: string;
  stops: { position: number; color: string }[];
}

export interface ThemeState {
  themes: Theme[];
  colorMaps: ColorMap[];
  loaded: boolean;
}

export const themeStore = new Store<ThemeState>({ themes: [], colorMaps: [], loaded: false });

export async function loadThemes(): Promise<void> {
  const [themes, colorMaps] = await Promise.all([
    fetch("/api/themes").then((r) => r.json() as Promise<Theme[]>),
    fetch("/api/colormaps").then((r) => r.json() as Promise<ColorMap[]>),
  ]);
  themeStore.setState(() => ({ themes, colorMaps, loaded: true }));
}

export function findTheme(name: string): Theme | undefined {
  const { themes } = themeStore.state;
  return themes.find((t) => t.name === name) ?? themes[0];
}

/** The chrome's colours as the CSS variables Tailwind's theme reads. */
export function applyTheme(theme: Theme | undefined): void {
  if (!theme) {
    return;
  }
  const root = document.documentElement.style;
  const c = theme.chrome;
  const set = (name: string, value: string | number | undefined) => {
    if (typeof value === "string") {
      root.setProperty(name, value);
    }
  };
  set("--color-window", c.windowBackground);
  set("--color-panel", c.panelBackground);
  set("--color-header", c.headerBackground);
  set("--color-text", c.text);
  set("--color-dim", c.textDim);
  set("--color-accent", c.accent);
  set("--color-accent-hover", c.accentHover);
  set("--color-border", c.border);
  set("--color-separator", c.separator);
  set("--color-button", c.buttonBackground);
  set("--color-button-hover", c.buttonHover);
  set("--color-button-active", c.buttonActive);
  set("--color-input", c.inputBackground);
  set("--color-input-hover", c.inputHover);
  set("--color-start", c.start);
  set("--color-stop", c.stop);
  set("--color-record", c.record);
  set("--color-ok", c.ok);
  set("--color-warning", c.warning);
  set("--color-danger", c.danger);
  set("--color-plot", theme.spectrum.background);
  set("--color-grid", theme.spectrum.grid);
  set("--color-axis", theme.spectrum.axisText);
  document.documentElement.style.colorScheme = theme.dark ? "dark" : "light";
}

export function parseHex(hex: string): [number, number, number, number] {
  const text = hex.replace(/^#/, "");
  const value = (at: number) => parseInt(text.slice(at, at + 2), 16) / 255;
  return [value(0), value(2), value(4), text.length >= 8 ? value(6) : 1];
}

/** 256 RGBA colours from a map's stops, linear between them, as
 * ColorMap::bake does. */
export function bakeColorMap(map: ColorMap | undefined): Uint8Array {
  const lut = new Uint8Array(256 * 4);
  const stops = (map?.stops ?? []).map((s) => ({ position: s.position, rgba: parseHex(s.color) }));
  for (let i = 0; i < 256; ++i) {
    const position = i / 255;
    let rgba: [number, number, number, number] = [0, 0, 0, 1];
    if (stops.length > 0) {
      const first = stops[0]!;
      const last = stops[stops.length - 1]!;
      if (position <= first.position) {
        rgba = first.rgba;
      } else if (position >= last.position) {
        rgba = last.rgba;
      } else {
        for (let s = 1; s < stops.length; ++s) {
          const to = stops[s]!;
          if (position <= to.position) {
            const from = stops[s - 1]!;
            const span = to.position - from.position;
            const t = span > 0 ? (position - from.position) / span : 0;
            rgba = from.rgba.map((v, k) => v + (to.rgba[k]! - v) * t) as [number, number, number, number];
            break;
          }
        }
      }
    }
    lut.set(rgba.map((v) => Math.round(v * 255)), i * 4);
  }
  return lut;
}
