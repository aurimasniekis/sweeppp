// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { type ReactNode, useState } from "react";

import { Icon } from "../../Icon";
import { icon } from "../../icons";

/** A collapsing header, as the desktop's menu sections are. */
export function Section({ title, open, children }: { title: string; open?: boolean; children: ReactNode }) {
  return (
    <details open={open} className="group mb-1">
      <summary className="flex cursor-pointer list-none select-none items-center gap-1.5 rounded bg-header px-2 py-1.5 font-semibold hover:bg-button-hover [&::-webkit-details-marker]:hidden">
        <span className="inline-flex text-dim transition-transform group-open:rotate-90">
          <Icon path={icon.expand} />
        </span>
        {title}
      </summary>
      <div className="px-1.5 pb-2 pt-1">{children}</div>
    </details>
  );
}

/** A button that asks once more before doing something that cannot be undone. */
export function ConfirmButton({
  children,
  prompt,
  onConfirm,
  title,
  disabled,
  className = "btn",
}: {
  children: ReactNode;
  prompt: string;
  onConfirm: () => void;
  title?: string;
  disabled?: boolean;
  className?: string;
}) {
  const [armed, setArmed] = useState(false);
  if (armed && !disabled) {
    return (
      <span className="flex shrink-0 items-center gap-1">
        <span className="text-xs text-warning">{prompt}</span>
        <button
          className="btn h-7 border-danger px-2 text-danger"
          onClick={() => {
            setArmed(false);
            onConfirm();
          }}
        >
          Yes
        </button>
        <button className="btn h-7 px-2" onClick={() => setArmed(false)}>
          No
        </button>
      </span>
    );
  }
  return (
    <button className={className} title={title} disabled={disabled} onClick={() => setArmed(true)}>
      {children}
    </button>
  );
}
