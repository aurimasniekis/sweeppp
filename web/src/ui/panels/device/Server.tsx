// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useState } from "react";

import { bytes, count } from "../../../render/format";
import { releaseControl, takeControl } from "../../../state/actions";
import { deleteRecording, type InstrumentView, startRecording, stopRecording } from "../../../state/instrument";
import { Select } from "../../controls";
import { Icon } from "../../Icon";
import { icon } from "../../icons";

const kResolutions = [
  { value: "16777216", label: "Full" },
  { value: "65536", label: "65 536 bins" },
  { value: "16384", label: "16 384 bins" },
  { value: "4096", label: "4 096 bins" },
];

export function Recordings({ instrument, server }: { instrument: InstrumentView; server: string }) {
  const [bins, setBins] = useState("65536");
  const [deleting, setDeleting] = useState<string | null>(null);
  const recordings = instrument.recordings;
  const watching = !instrument.canControl;

  const files = recordings.files.filter((f) => !(recordings.active && f.name === recordings.current));
  return (
    <>
      <div className="section-title">Recordings on {server}</div>
      {!recordings.available ? (
        <p className="caption">This server has nowhere to record to.</p>
      ) : (
        <>
          {recordings.active ? (
            <div className="py-1">
              <div className="break-all text-record">Recording {recordings.current}</div>
              <div className="caption">
                {count(recordings.lines)} lines, {bytes(recordings.bytes)}
              </div>
              <button className="btn mt-1 w-full" disabled={watching} onClick={stopRecording}>
                Stop recording
              </button>
            </div>
          ) : (
            <div className="py-1">
              <div className="mb-1 text-xs text-dim">Resolution</div>
              <div className="flex gap-2">
                <div className="grow">
                  <Select value={bins} choices={kResolutions} onChange={setBins} />
                </div>
                <button className="btn" disabled={watching} onClick={() => startRecording(Number(bins))}>
                  Record on server
                </button>
              </div>
              <div className="caption mt-1">Bins a line. Full keeps every bin and can be gigabytes an hour.</div>
            </div>
          )}

          {files.map((file) =>
            deleting === file.name ? (
              <div key={file.name} className="py-1">
                <div className="break-all text-xs">
                  Delete {file.name} from {server}?
                </div>
                <div className="mt-1 flex gap-2">
                  <button
                    className="btn text-danger"
                    disabled={watching}
                    onClick={() => {
                      deleteRecording(file.name);
                      setDeleting(null);
                    }}
                  >
                    Delete
                  </button>
                  <button className="btn" onClick={() => setDeleting(null)}>
                    Cancel
                  </button>
                </div>
              </div>
            ) : (
              <div key={file.name} className="flex items-center gap-2 py-1">
                <div className="min-w-0 grow">
                  <div className="truncate" title={file.name}>
                    {file.name}
                  </div>
                  <div className="caption">{bytes(file.bytes)}</div>
                </div>
                <a
                  className="btn-icon"
                  href={`/api/recordings/${encodeURIComponent(file.name)}`}
                  download={file.name}
                  title="Download"
                >
                  <Icon path={icon.download} />
                </a>
                <button
                  className="btn-icon"
                  disabled={watching}
                  title={`Delete it from ${server}`}
                  onClick={() => setDeleting(file.name)}
                >
                  <Icon path={icon.delete} />
                </button>
              </div>
            ),
          )}
          {recordings.files.length === 0 && <p className="caption">Nothing recorded yet.</p>}
        </>
      )}
    </>
  );
}

export function Clients({ instrument, server }: { instrument: InstrumentView; server: string }) {
  const control = instrument.control;
  return (
    <>
      <div className="section-title">Connected to {server}</div>
      <div className="grid grid-cols-[1fr_auto] gap-x-3 gap-y-0.5 text-xs">
        {instrument.clients.map((client) => (
          <div key={client.id} className="contents" title={`${client.kind} at ${client.address}`}>
            <div className="truncate">
              {client.name}
              {client.you && <span className="text-dim"> (this page)</span>}
            </div>
            <div className={client.controls ? "text-accent" : "text-dim"}>
              {client.controls ? "in control" : "watching"}
            </div>
          </div>
        ))}
      </div>
      <div className="mt-2 flex gap-2">
        <button className="btn grow" disabled={control.you} onClick={takeControl}>
          Take control
        </button>
        <button className="btn grow" disabled={!control.you} onClick={releaseControl}>
          Release control
        </button>
      </div>
      {instrument.canControl && <p className="caption mt-1">Anyone connected can take control.</p>}
    </>
  );
}
