// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

/** One icon from `icons.ts`, in the text's colour and sized to it. */
export function Icon({ path, size = "1.2em", className = "" }: { path: string; size?: string; className?: string }) {
  return (
    <svg viewBox="0 0 24 24" width={size} height={size} aria-hidden="true" className={`inline-block shrink-0 ${className}`}>
      <path d={path} fill="currentColor" />
    </svg>
  );
}
