// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import { defineConfig } from "vitest/config";

// The C++ build passes SWEEPPP_WEB_OUT so the bundle lands in its own tree;
// `pnpm dev` proxies to a `sweeppp-cli serve --web 8080` running beside it.
const backend = process.env.SWEEPPP_WEB_BACKEND ?? "http://127.0.0.1:8080";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  build: {
    outDir: process.env.SWEEPPP_WEB_OUT ?? "dist",
    emptyOutDir: true,
    assetsDir: "assets",
    sourcemap: false,
  },
  server: {
    proxy: {
      "/api": backend,
      "/login": backend,
      "/logout": backend,
      "/ws": { target: backend.replace(/^http/, "ws"), ws: true },
    },
  },
  test: {
    include: ["src/**/*.test.ts"],
  },
});
