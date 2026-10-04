// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useEffect, useRef, useState } from "react";

import { type Contributor, toggleContributorRow } from "../state/contributors";
import { Icon } from "./Icon";
import { icon } from "./icons";

/** A tick that reads as a dash when some of what is under it is on. */
function Tick({
  anyOn,
  allOn,
  disabled,
  onClick,
}: {
  anyOn: boolean;
  allOn: boolean;
  disabled: boolean;
  onClick: () => void;
}) {
  const ref = useRef<HTMLInputElement>(null);
  useEffect(() => {
    if (ref.current) {
      ref.current.indeterminate = anyOn && !allOn;
    }
  }, [anyOn, allOn]);
  return (
    <input
      ref={ref}
      type="checkbox"
      className="size-3.5 shrink-0 accent-[var(--color-accent)]"
      checked={anyOn}
      disabled={disabled}
      onChange={onClick}
    />
  );
}

/** A contributor's tick tree, as its own popover on the desktop draws it:
 * branches closed until opened, a tick that hides a branch without forgetting
 * what is ticked inside it. */
export function ContributorTree({ contributor, disabled }: { contributor: Contributor; disabled: boolean }) {
  const [open, setOpen] = useState<Set<string>>(() => new Set());
  const rows = contributor.tree;
  if (rows.length === 0) {
    return <p className="caption">{contributor.name} has nothing to tick.</p>;
  }

  const parent = (i: number) => i + 1 < rows.length && rows[i + 1]!.depth > rows[i]!.depth;
  let leaves = 0;
  let on = 0;
  rows.forEach((row, i) => {
    if (!parent(i)) {
      ++leaves;
      on += row.anyOn ? 1 : 0;
    }
  });

  // Rows deeper than `shown` sit inside a closed branch.
  let shown = 0;
  const visible: number[] = [];
  rows.forEach((row, i) => {
    if (row.depth > shown) {
      return;
    }
    visible.push(i);
    shown = parent(i) && open.has(row.key) ? row.depth + 1 : row.depth;
  });

  const flip = (key: string) =>
    setOpen((current) => {
      const next = new Set(current);
      if (!next.delete(key)) {
        next.add(key);
      }
      return next;
    });

  return (
    <div>
      <p className="caption">
        {on} of {leaves} on. Unticked rows leave the plot and the marker's chip.
      </p>
      <div className="mt-1 max-h-80 overflow-auto rounded border border-border py-0.5">
        {visible.map((i) => {
          const row = rows[i]!;
          const branch = parent(i);
          return (
            <div
              key={row.key}
              className="flex min-h-6 items-center gap-1.5 pr-2 hover:bg-button-hover"
              style={{ paddingLeft: 4 + row.depth * 16 }}
              title={row.description || undefined}
            >
              <button
                className={`flex size-4 items-center justify-center text-dim ${branch ? "" : "invisible"}`}
                onClick={() => flip(row.key)}
                aria-label={open.has(row.key) ? "Close" : "Open"}
              >
                <Icon path={open.has(row.key) ? icon.collapse : icon.expand} />
              </button>
              <Tick
                anyOn={row.anyOn}
                allOn={row.allOn}
                disabled={disabled}
                onClick={() => toggleContributorRow(contributor.id, row.key)}
              />
              {row.color[3] > 0 && (
                <span
                  className="h-2.5 w-1.5 shrink-0 rounded-sm"
                  style={{ background: `rgb(${row.color[0] * 255},${row.color[1] * 255},${row.color[2] * 255})` }}
                />
              )}
              <span className="truncate">{row.name}</span>
              {row.detail && <span className="ml-auto shrink-0 pl-2 text-xs text-dim tabular-nums">{row.detail}</span>}
            </div>
          );
        })}
      </div>
    </div>
  );
}
