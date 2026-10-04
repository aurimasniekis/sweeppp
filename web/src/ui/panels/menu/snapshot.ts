// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { NoticeKind } from "../../../protocol/messages";
import { toast } from "../../toasts";

/** "20261004-153012", local time, as the desktop names its snapshots. */
function compactNow(): string {
  const now = new Date();
  const two = (n: number) => String(n).padStart(2, "0");
  return (
    `${now.getFullYear()}${two(now.getMonth() + 1)}${two(now.getDate())}` +
    `-${two(now.getHours())}${two(now.getMinutes())}${two(now.getSeconds())}`
  );
}

function backgroundOf(root: HTMLElement): string {
  const own = getComputedStyle(root).backgroundColor;
  if (own && own !== "transparent" && own !== "rgba(0, 0, 0, 0)") {
    return own;
  }
  return getComputedStyle(document.documentElement).getPropertyValue("--color-window").trim() || "#000";
}

/** The panels' canvases composited where they sit on screen, at the screen's
 * pixel density, downloaded as a PNG. */
export function saveSnapshot(): void {
  const root = document.querySelector<HTMLElement>("[data-snapshot-root]") ?? document.body;
  const area = root.getBoundingClientRect();
  const scale = window.devicePixelRatio || 1;
  const width = Math.round(area.width * scale);
  const height = Math.round(area.height * scale);
  const canvases = [...root.querySelectorAll("canvas")].filter((c) => {
    const r = c.getBoundingClientRect();
    return c.width > 0 && c.height > 0 && r.width > 0 && r.height > 0;
  });
  if (canvases.length === 0 || width <= 1 || height <= 1) {
    toast(NoticeKind.Error, "there is nothing on screen to snapshot yet");
    return;
  }

  const out = document.createElement("canvas");
  out.width = width;
  out.height = height;
  const ctx = out.getContext("2d");
  if (!ctx) {
    toast(NoticeKind.Error, "this browser cannot compose the snapshot");
    return;
  }
  ctx.fillStyle = backgroundOf(root);
  ctx.fillRect(0, 0, width, height);
  for (const canvas of canvases) {
    const r = canvas.getBoundingClientRect();
    ctx.drawImage(
      canvas,
      0,
      0,
      canvas.width,
      canvas.height,
      (r.left - area.left) * scale,
      (r.top - area.top) * scale,
      r.width * scale,
      r.height * scale,
    );
  }

  out.toBlob((blob) => {
    if (!blob) {
      toast(NoticeKind.Error, "the snapshot could not be encoded");
      return;
    }
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `sweeppp-${compactNow()}.png`;
    document.body.append(link);
    link.click();
    link.remove();
    // Revoked later rather than at once: some browsers start the download
    // only after the click handler has returned.
    window.setTimeout(() => URL.revokeObjectURL(url), 10_000);
    toast(NoticeKind.Success, `${width}x${height} snapshot saved`);
  }, "image/png");
}
