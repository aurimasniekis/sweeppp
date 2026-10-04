// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";

import { type ColorMap, findTheme, themeStore } from "../../../state/theme";
import { setView, viewStore } from "../../../state/view";
import { Field, Select } from "../../controls";

function gradient(map: ColorMap | undefined): string {
  if (!map || map.stops.length === 0) {
    return "var(--color-button)";
  }
  const stops = map.stops.map((s) => `${s.color} ${(s.position * 100).toFixed(1)}%`);
  return `linear-gradient(to right, ${stops.join(", ")})`;
}

function MapRow({ label, map, selected, onPick }: { label: string; map: ColorMap | undefined; selected: boolean; onPick: () => void }) {
  return (
    <button
      className={`flex w-full items-center gap-2 rounded px-1 py-0.5 text-left hover:bg-button-hover ${selected ? "bg-button-active" : ""}`}
      onClick={onPick}
    >
      <span className="h-3.5 w-30 shrink-0 rounded-sm" style={{ background: gradient(map) }} />
      <span className={`truncate ${selected ? "text-accent" : ""}`}>{label}</span>
    </button>
  );
}

export function ThemeSection() {
  const { themes, colorMaps, loaded } = useSelector(themeStore, (t) => t);
  const themeName = useSelector(viewStore, (v) => v.themeName);
  const colorMap = useSelector(viewStore, (v) => v.colorMap);
  if (!loaded) {
    return <p className="caption">Themes have not arrived from the server.</p>;
  }
  const theme = findTheme(themeName);
  const themesMap = colorMaps.find((m) => m.name === theme?.waterfall.colorMap);
  return (
    <div>
      <Field label="Theme">
        <Select
          value={theme?.name ?? ""}
          choices={themes.map((t) => ({ value: t.name, label: t.name }))}
          onChange={(name) => setView({ themeName: name })}
        />
      </Field>
      <div className="section-title">Waterfall colours</div>
      <MapRow
        label={`The theme's${themesMap ? ` (${themesMap.name})` : ""}`}
        map={themesMap}
        selected={colorMap === ""}
        onPick={() => setView({ colorMap: "" })}
      />
      {colorMaps.map((map) => (
        <MapRow
          key={map.name}
          label={map.name}
          map={map}
          selected={colorMap === map.name}
          onPick={() => setView({ colorMap: map.name })}
        />
      ))}
      <p className="caption mt-1">Saving themes and editing gradients is done in the desktop app.</p>
    </div>
  );
}
