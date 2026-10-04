// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { Store, useSelector } from "@tanstack/react-store";

import { NoticeKind, type NoticeKindId } from "../protocol/messages";
import { Icon } from "./Icon";
import { icon } from "./icons";

export interface Toast {
  id: number;
  kind: NoticeKindId;
  text: string;
}

export const toastStore = new Store<{ toasts: Toast[]; condition: string }>({ toasts: [], condition: "" });
let nextId = 1;

/** A message for a few seconds; a condition stays until cleared, as the
 * desktop's latched error does. */
export function toast(kind: NoticeKindId, text: string): void {
  if (kind === NoticeKind.Condition) {
    toastStore.setState((s) => ({ ...s, condition: text }));
    return;
  }
  if (kind === NoticeKind.ClearCondition) {
    toastStore.setState((s) => ({ ...s, condition: "" }));
    return;
  }
  const id = nextId++;
  toastStore.setState((s) => ({ ...s, toasts: [...s.toasts.slice(-4), { id, kind, text }] }));
  window.setTimeout(
    () => toastStore.setState((s) => ({ ...s, toasts: s.toasts.filter((t) => t.id !== id) })),
    kind === NoticeKind.Error ? 8000 : 4000,
  );
}

const colour: Record<number, string> = {
  [NoticeKind.Info]: "border-border",
  [NoticeKind.Success]: "border-ok",
  [NoticeKind.Warning]: "border-warning",
  [NoticeKind.Error]: "border-danger",
};

const glyph: Record<number, [string, string]> = {
  [NoticeKind.Info]: [icon.info, "text-accent"],
  [NoticeKind.Success]: [icon.success, "text-ok"],
  [NoticeKind.Warning]: [icon.warning, "text-warning"],
  [NoticeKind.Error]: [icon.error, "text-danger"],
};

export function Toasts() {
  const toasts = useSelector(toastStore, (s) => s.toasts);
  return (
    <div className="pointer-events-none fixed bottom-10 right-3 z-[70] flex max-w-[min(420px,calc(100vw-24px))] flex-col gap-2">
      {toasts.map((t) => (
        <div
          key={t.id}
          className={`pointer-events-auto flex items-start gap-2 rounded border-l-4 bg-panel px-3 py-2 shadow-lg ${colour[t.kind] ?? "border-border"}`}
        >
          {glyph[t.kind] && <Icon path={glyph[t.kind]![0]} className={`mt-0.5 ${glyph[t.kind]![1]}`} />}
          <span>{t.text}</span>
        </div>
      ))}
    </div>
  );
}
