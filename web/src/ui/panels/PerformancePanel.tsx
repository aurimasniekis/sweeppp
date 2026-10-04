// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import * as Tabs from "@radix-ui/react-tabs";
import { useState } from "react";

import { BenchmarkTab } from "./performance/BenchmarkTab";
import { PerformanceTab } from "./performance/PerformanceTab";

type Tab = "performance" | "benchmark";

let lastTab: Tab = "performance";

const kTrigger =
  "-mb-px border-b-2 border-transparent px-3 py-1.5 text-dim hover:text-text data-[state=active]:border-accent data-[state=active]:text-text";

/** Stream, processing and link figures, and the server's FFT benchmark. */
export function PerformancePanel() {
  const [tab, setTab] = useState<Tab>(lastTab);
  return (
    <Tabs.Root
      value={tab}
      onValueChange={(value) => {
        lastTab = value as Tab;
        setTab(lastTab);
      }}
    >
      <Tabs.List className="mb-2 flex border-b border-separator">
        <Tabs.Trigger value="performance" className={kTrigger}>
          Performance
        </Tabs.Trigger>
        <Tabs.Trigger value="benchmark" className={kTrigger}>
          Benchmark
        </Tabs.Trigger>
      </Tabs.List>
      {/* Kept mounted so the graphs go on filling behind the benchmark. */}
      <Tabs.Content value="performance" forceMount className="outline-none data-[state=inactive]:hidden">
        <PerformanceTab />
      </Tabs.Content>
      <Tabs.Content value="benchmark" className="outline-none">
        <BenchmarkTab />
      </Tabs.Content>
    </Tabs.Root>
  );
}
