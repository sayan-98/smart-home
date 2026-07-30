// ---------------------------------------------------------------------------
//  DeviceDetail.tsx - rename sockets, set restore behaviour, read health.
//
//  Restore mode gets prominence here because it decides what happens when a
//  master wall switch is flipped back on, and the safe default is not always
//  the one people want.
// ---------------------------------------------------------------------------
import { useEffect, useState } from 'react';
import type { JSX } from 'react';

import { store, type Device } from '../api/store.js';
import { cloud } from '../api/transport.js';

interface Props {
  device: Device;
  onClose: () => void;
}

type Restore = 'off' | 'on' | 'last';

const RESTORE_LABEL: Record<Restore, string> = {
  off: 'Stay off',
  last: 'As before',
  on: 'Turn on',
};

export function DeviceDetail({ device, onClose }: Props): JSX.Element {
  const [names, setNames] = useState<Record<number, string>>({});
  const [restore, setRestore] = useState<Record<number, Restore>>({});
  const [saving, setSaving] = useState(false);
  const [note, setNote] = useState<string | null>(null);
  const [diag, setDiag] = useState<string | null>(null);

  useEffect(() => {
    setNames(Object.fromEntries(device.relays.map((r) => [r.channel, r.name])));
    setRestore({});
    setNote(null);
  }, [device.id, device.relays]);

  async function saveName(channel: number): Promise<void> {
    const name = names[channel]?.trim();
    if (!name) return;
    setSaving(true);
    try {
      await cloud(`/api/devices/${device.id}/relay/${channel}`, {
        method: 'PATCH',
        body: JSON.stringify({ name }),
      });
      await store.refreshDevices();
      // Renaming changes what you say to Alexa, and Alexa caches the old name.
      setNote('Saved. Say "Alexa, discover devices" so the new name works by voice.');
    } catch {
      setNote('Could not save that name.');
    } finally {
      setSaving(false);
    }
  }

  async function saveRestore(channel: number, mode: Restore): Promise<void> {
    setRestore({ ...restore, [channel]: mode });
    try {
      // The device owns this setting - it has to work when the device boots
      // with no network at all - so the backend only forwards it.
      await cloud(`/api/devices/${device.id}/relay/${channel}`, {
        method: 'PATCH',
        body: JSON.stringify({ restore: mode }),
      });
      setNote(`Channel ${channel + 1} will "${RESTORE_LABEL[mode]}" after power returns.`);
    } catch {
      setNote('Could not reach the device.');
    }
  }

  async function runDiagnose(): Promise<void> {
    setDiag('Checking...');
    try {
      const res = await cloud<{ summary: string }>('/api/ai/diagnose', {
        method: 'POST',
        body: JSON.stringify({ deviceId: device.id }),
      });
      setDiag(res.summary);
    } catch {
      setDiag('Diagnosis is unavailable right now.');
    }
  }

  const weakSignal = device.rssi !== null && device.rssi < -78;

  return (
    <section className="card">
      <header className="card-head">
        <div>
          <h2>{device.name}</h2>
          <p className="sub">{device.uuid}</p>
        </div>
        <button className="sec" onClick={onClose}>
          Close
        </button>
      </header>

      {note && <div className="warn">{note}</div>}

      <h3>Sockets</h3>
      {device.relays.map((relay) => (
        <div className="field" key={relay.id}>
          <label>Channel {relay.channel + 1}</label>
          <div className="row">
            <input
              value={names[relay.channel] ?? ''}
              maxLength={23}
              onChange={(e) => setNames({ ...names, [relay.channel]: e.target.value })}
            />
            <button className="sec" disabled={saving} onClick={() => void saveName(relay.channel)}>
              Save
            </button>
          </div>
          <div className="row restore">
            <span className="sub">After power returns:</span>
            {(['off', 'last', 'on'] as Restore[]).map((mode) => (
              <button
                key={mode}
                className={`chip ${restore[relay.channel] === mode ? 'sel' : ''}`}
                onClick={() => void saveRestore(relay.channel, mode)}
              >
                {RESTORE_LABEL[mode]}
              </button>
            ))}
          </div>
        </div>
      ))}

      <p className="sub hint">
        <b>&quot;As before&quot;</b> is usually what you want when a single wall switch feeds
        the whole board, so flipping it back on returns each socket to how you left it. Keep
        anything with a heating element on <b>&quot;Stay off&quot;</b>.
      </p>

      <h3>Health</h3>
      <table>
        <tbody>
          <tr>
            <td>Status</td>
            <td>{device.online ? 'online' : 'offline'}</td>
          </tr>
          <tr>
            <td>Signal</td>
            <td>
              {device.rssi ?? '--'} dBm
              {weakSignal ? ' (weak - expect dropped commands)' : ''}
            </td>
          </tr>
          <tr>
            <td>Firmware</td>
            <td>{device.firmwareVersion}</td>
          </tr>
          <tr>
            <td>Local address</td>
            <td>{device.lastIp ?? '--'}</td>
          </tr>
        </tbody>
      </table>

      {device.lastBrownout && (
        <div className="warn">
          <b>Last restart was a brownout.</b> The 5 V rail sagged. Remove the relay
          board&apos;s JD-VCC jumper and give the coils their own 2 A supply. This is a power
          problem, not a software one.
        </div>
      )}

      {store.aiEnabled && (
        <>
          <button className="sec" onClick={() => void runDiagnose()}>
            Explain this device&apos;s health
          </button>
          {diag && <p className="sub">{diag}</p>}
        </>
      )}
    </section>
  );
}
