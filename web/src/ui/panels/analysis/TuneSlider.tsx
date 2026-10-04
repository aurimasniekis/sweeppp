// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useState } from "react";

import { Field, Slider } from "../../controls";

/** A slider that sends once, on release: each value sent restarts the
 * radio's stream, so the values passed on the way are only shown. */
export function TuneSlider({
  label,
  value,
  format,
  min,
  max,
  step,
  hint,
  disabled,
  onCommit,
}: {
  label: string;
  value: number;
  format: (value: number) => string;
  min: number;
  max: number;
  step: number;
  hint?: string;
  disabled?: boolean;
  onCommit: (value: number) => void;
}) {
  const [dragging, setDragging] = useState<number | null>(null);
  const shown = dragging ?? value;
  return (
    <Field label={`${label} ${format(shown)}`} hint={hint}>
      <Slider
        value={shown}
        min={min}
        max={max}
        step={step}
        disabled={disabled}
        onChange={setDragging}
        onCommit={(x) => {
          setDragging(null);
          if (x !== value) {
            onCommit(x);
          }
        }}
      />
    </Field>
  );
}
