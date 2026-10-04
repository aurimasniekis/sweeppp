// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import * as RadixPopover from "@radix-ui/react-popover";
import * as RadixSelect from "@radix-ui/react-select";
import * as RadixSlider from "@radix-ui/react-slider";
import * as RadixSwitch from "@radix-ui/react-switch";
import * as RadixToggle from "@radix-ui/react-toggle-group";
import { type ReactNode, useEffect, useState } from "react";

import { Icon } from "./Icon";
import { icon } from "./icons";

/** A bar button that opens a panel under it. */
export function Popover({
  trigger,
  title,
  children,
  width = 380,
  open,
  onOpenChange,
}: {
  trigger: ReactNode;
  title?: string;
  children: ReactNode;
  width?: number;
  open?: boolean;
  onOpenChange?: (open: boolean) => void;
}) {
  return (
    <RadixPopover.Root open={open} onOpenChange={onOpenChange}>
      <RadixPopover.Trigger asChild>{trigger}</RadixPopover.Trigger>
      <RadixPopover.Portal>
        <RadixPopover.Content
          className="popover"
          style={{ width: `min(${width}px, calc(100vw - 16px))` }}
          sideOffset={6}
          collisionPadding={8}
          align="start"
        >
          {title && <div className="mb-2 text-sm font-semibold">{title}</div>}
          {children}
        </RadixPopover.Content>
      </RadixPopover.Portal>
    </RadixPopover.Root>
  );
}

export function Switch({
  checked,
  onChange,
  label,
  disabled,
}: {
  checked: boolean;
  onChange: (checked: boolean) => void;
  label: string;
  disabled?: boolean;
}) {
  return (
    <label className={`flex cursor-pointer items-center gap-2 py-1 ${disabled ? "opacity-50" : ""}`}>
      <RadixSwitch.Root
        checked={checked}
        onCheckedChange={onChange}
        disabled={disabled}
        className="relative h-4 w-7 shrink-0 rounded-full bg-button data-[state=checked]:bg-accent"
      >
        <RadixSwitch.Thumb className="block h-3 w-3 translate-x-0.5 rounded-full bg-text transition-transform data-[state=checked]:translate-x-3.5" />
      </RadixSwitch.Root>
      <span>{label}</span>
    </label>
  );
}

/** Two switches a row: the desktop's compact panel layout. */
export function SwitchRow({ children }: { children: ReactNode }) {
  return <div className="grid grid-cols-2 gap-x-3">{children}</div>;
}

export function Slider({
  value,
  min,
  max,
  step = 1,
  onChange,
  onCommit,
  disabled,
}: {
  value: number;
  min: number;
  max: number;
  step?: number;
  onChange?: (value: number) => void;
  onCommit?: (value: number) => void;
  disabled?: boolean;
}) {
  return (
    <RadixSlider.Root
      className="relative flex h-5 w-full touch-none select-none items-center"
      value={[value]}
      min={min}
      max={max}
      step={step}
      disabled={disabled}
      onValueChange={(v) => onChange?.(v[0]!)}
      onValueCommit={(v) => onCommit?.(v[0]!)}
    >
      <RadixSlider.Track className="relative h-1 grow rounded-full bg-button">
        <RadixSlider.Range className="absolute h-full rounded-full bg-accent" />
      </RadixSlider.Track>
      <RadixSlider.Thumb className="block h-3.5 w-3.5 rounded-full bg-text shadow focus:outline-none" />
    </RadixSlider.Root>
  );
}

export interface Choice<T extends string> {
  value: T;
  label: string;
  /** Shown on hover. */
  title?: string;
}

export function Select<T extends string>({
  value,
  choices,
  onChange,
  disabled,
  placeholder,
}: {
  value: T;
  choices: Choice<T>[];
  onChange: (value: T) => void;
  disabled?: boolean;
  placeholder?: string;
}) {
  return (
    <RadixSelect.Root value={value} onValueChange={(v) => onChange(v as T)} disabled={disabled}>
      <RadixSelect.Trigger className="field flex items-center justify-between text-left">
        <RadixSelect.Value placeholder={placeholder} />
        <RadixSelect.Icon className="text-dim">
          <Icon path={icon.collapse} />
        </RadixSelect.Icon>
      </RadixSelect.Trigger>
      <RadixSelect.Portal>
        <RadixSelect.Content className="popover z-[60] p-1" position="popper" sideOffset={4}>
          <RadixSelect.Viewport>
            {choices.map((choice) => (
              <RadixSelect.Item
                key={choice.value}
                value={choice.value}
                title={choice.title}
                className="cursor-pointer rounded px-2 py-1.5 outline-none data-[highlighted]:bg-button-hover data-[state=checked]:text-accent"
              >
                <RadixSelect.ItemText>{choice.label}</RadixSelect.ItemText>
              </RadixSelect.Item>
            ))}
          </RadixSelect.Viewport>
        </RadixSelect.Content>
      </RadixSelect.Portal>
    </RadixSelect.Root>
  );
}

export function Segmented<T extends string>({
  value,
  choices,
  onChange,
  disabled,
}: {
  value: T;
  choices: Choice<T>[];
  onChange: (value: T) => void;
  disabled?: boolean;
}) {
  return (
    <RadixToggle.Root
      type="single"
      value={value}
      disabled={disabled}
      onValueChange={(v) => v && onChange(v as T)}
      className="inline-flex rounded border border-border"
    >
      {choices.map((choice) => (
        <RadixToggle.Item
          key={choice.value}
          value={choice.value}
          className="h-7 px-2.5 first:rounded-l last:rounded-r hover:bg-button-hover data-[state=on]:bg-accent data-[state=on]:text-white disabled:opacity-50"
        >
          {choice.label}
        </RadixToggle.Item>
      ))}
    </RadixToggle.Root>
  );
}

/** A label above a control, with a hint underneath. */
export function Field({ label, hint, children }: { label: string; hint?: string; children: ReactNode }) {
  return (
    <div className="py-1">
      <div className="mb-1 text-xs text-dim">{label}</div>
      {children}
      {hint && <div className="caption mt-1">{hint}</div>}
    </div>
  );
}

/** A text field committed on Enter or blur, so an edit in progress -- a stop
 * below its start, half a number -- is never sent. */
export function CommitField({
  value,
  onCommit,
  disabled,
  inputMode = "decimal",
}: {
  value: string;
  onCommit: (text: string) => boolean;
  disabled?: boolean;
  inputMode?: "decimal" | "text" | "numeric";
}) {
  const [text, setText] = useState(value);
  const [bad, setBad] = useState(false);
  useEffect(() => setText(value), [value]);
  const commit = () => {
    if (text === value) {
      return;
    }
    const accepted = onCommit(text);
    setBad(!accepted);
    if (!accepted) {
      setText(value);
    }
  };
  return (
    <input
      className={`field ${bad ? "border-danger" : ""}`}
      value={text}
      disabled={disabled}
      inputMode={inputMode}
      onChange={(e) => setText(e.target.value)}
      onBlur={commit}
      onKeyDown={(e) => {
        if (e.key === "Enter") {
          commit();
          (e.target as HTMLInputElement).blur();
        }
      }}
    />
  );
}

export function Readout({ rows }: { rows: [string, ReactNode, string?][] }) {
  return (
    <div className="grid grid-cols-[auto_1fr] gap-x-3 gap-y-0.5 text-xs">
      {rows.map(([label, value, tip]) => (
        <div key={label} className="contents" title={tip}>
          <div className="text-dim">{label}</div>
          <div className="text-right tabular-nums">{value}</div>
        </div>
      ))}
    </div>
  );
}
