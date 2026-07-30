// ---------------------------------------------------------------------------
//  App.tsx - tablet-first control surface.
//
//  Laid out for an 11" Redmi Pad 2: a two-pane view in landscape, one pane in
//  portrait. Tiles are large because this is a wall-mounted panel as much as a
//  handheld app, and because a mis-tap switches a real appliance.
// ---------------------------------------------------------------------------
import { useEffect, useSyncExternalStore, useState } from 'react';
import type { JSX } from 'react';

import { store, type Device, type Relay } from './api/store.js';
import { setToken } from './api/transport.js';
import { Login } from './screens/Login.js';
import { DeviceDetail } from './screens/DeviceDetail.js';
import { AiBar } from './screens/AiBar.js';
import { Help } from './screens/Help.js';

/** Re-renders on every store change. See Store.snapshot for why it must be a counter. */
function useStore(): void {
  useSyncExternalStore(
    (cb) => store.subscribe(cb),
    () => store.snapshot,
  );
}

function ModeBadge(): JSX.Element {
  const label =
    store.mode === 'lan'
      ? 'Direct - on your Wi-Fi'
      : store.mode === 'remote'
        ? 'Remote - via broker'
        : store.mode === 'cloud'
          ? 'Cloud'
          : 'Offline';
  return <span className={`badge ${store.mode}`}>{label}</span>;
}

/** How a change is described to the user - "why did this turn on?" */
const SOURCE_LABEL: Record<string, string> = {
  physical: 'wall switch',
  app: 'app',
  alexa: 'Alexa',
  schedule: 'schedule',
  automation: 'automation',
  timer: 'timer',
  restore: 'restored',
  cloud: 'remote',
};

function RelayTile({ device, relay }: { device: Device; relay: Relay }): JSX.Element {
  const [pending, setPending] = useState(false);

  async function onTap(): Promise<void> {
    if (!relay.enabled || pending) return;
    setPending(true);
    await store.toggle(device.id, relay.channel);
    setPending(false);
  }

  return (
    <button
      className={`tile ${relay.state ? 'on' : ''} ${relay.enabled ? '' : 'disabled'}`}
      onClick={() => void onTap()}
      disabled={!relay.enabled || !device.online}
    >
      <div className="tile-top">
        <span className="tile-name">{relay.name}</span>
        <span className="switch" aria-hidden>
          <i />
        </span>
      </div>
      <div className="tile-meta">
        {!device.online
          ? 'device offline'
          : !relay.enabled
            ? 'disabled'
            : `${relay.state ? 'ON' : 'OFF'} - ${SOURCE_LABEL[relay.source] ?? relay.source}`}
      </div>
    </button>
  );
}

function DeviceCard({ device, onOpen }: { device: Device; onOpen: () => void }): JSX.Element {
  return (
    <section className="card">
      <header className="card-head">
        <div>
          <h2>{device.name}</h2>
          <p className="sub">
            {device.online
              ? `online - ${device.rssi ?? '--'} dBm - fw ${device.firmwareVersion}`
              : 'offline'}
            {device.room ? ` - ${device.room.name}` : ''}
          </p>
          {/* The device reports its own address in every heartbeat, so its web
              page is one tap away no matter how often DHCP reassigns it. You
              should never have to go hunting for an IP. */}
          {device.lastIp && (
            <p className="sub">
              web page:{' '}
              <a href={`http://${device.lastIp}`} target="_blank" rel="noreferrer">
                http://{device.lastIp}
              </a>
            </p>
          )}
        </div>
        <div className="row">
          <button className="sec" onClick={() => void store.allOff(device.id)}>
            All off
          </button>
          <button className="sec" onClick={onOpen}>
            Details
          </button>
        </div>
      </header>

      {/* The single most misdiagnosed hardware fault on this board, surfaced
          where it will actually be seen rather than buried in a log. */}
      {device.lastBrownout && (
        <div className="warn">
          <b>This device browned out.</b> Its 5 V supply is sagging. Check that the relay
          board&apos;s JD-VCC jumper is removed and the coils have their own 2 A supply. This
          is a power problem, not a firmware fault.
        </div>
      )}

      <div className="grid">
        {device.relays.map((relay) => (
          <RelayTile key={relay.id} device={device} relay={relay} />
        ))}
      </div>
    </section>
  );
}

export function App(): JSX.Element {
  useStore();
  const [ready, setReady] = useState(false);
  const [authed, setAuthed] = useState(false);
  const [detail, setDetail] = useState<Device | null>(null);
  const [help, setHelp] = useState(false);

  useEffect(() => {
    void (async () => {
      setAuthed(await store.init());
      setReady(true);
    })();
    return () => store.dispose();
  }, []);

  async function onLogout(): Promise<void> {
    if (store.remote) {
      await store.forgetRemote();
    } else if (store.direct) {
      await store.forgetDirect();
    } else {
      await setToken(null);
      store.dispose();
    }
    setAuthed(false);
  }

  if (!ready) {
    return (
      <div className="center">
        <p className="sub">Starting...</p>
      </div>
    );
  }

  // The guide is reachable before sign-in too - "how do I get this on Wi-Fi"
  // is exactly the question people have at that point.
  if (help) return <Help onClose={() => setHelp(false)} />;

  if (!authed) {
    return (
      <>
        <Login onConnected={() => setAuthed(true)} />
        <button className="help-fab" onClick={() => setHelp(true)}>
          Help
        </button>
      </>
    );
  }

  const home = store.homes.find((h) => h.id === store.activeHomeId);
  const onlineCount = store.devices.filter((d) => d.online).length;

  return (
    <div className="app">
      <header className="top">
        <div>
          <h1>{home?.name ?? 'Smart Home'}</h1>
          <p className="sub">
            {store.devices.length} device{store.devices.length === 1 ? '' : 's'} - {onlineCount}{' '}
            online
          </p>
        </div>
        <div className="row">
          <ModeBadge />
          <button className="sec" onClick={() => setHelp(true)}>
            Help
          </button>
          <button className="sec" onClick={() => void store.refreshDevices()}>
            Refresh
          </button>
          <button className="sec" onClick={() => void onLogout()}>
            {store.direct ? 'Disconnect' : 'Sign out'}
          </button>
        </div>
      </header>

      {store.error && (
        <div className="err" onClick={() => store.clearError()}>
          {store.error} <span className="sub">(tap to dismiss)</span>
        </div>
      )}

      {store.mode === 'offline' && store.queuedCount > 0 && (
        <div className="warn">
          {store.queuedCount} command{store.queuedCount === 1 ? '' : 's'} waiting. They will be
          sent when the connection comes back.
        </div>
      )}

      {store.aiEnabled && store.activeHomeId && <AiBar homeId={store.activeHomeId} />}

      <main className={detail ? 'split' : ''}>
        <div className="pane">
          {store.devices.length === 0 ? (
            <section className="card empty">
              <h2>No devices yet</h2>
              <p className="sub">
                Power on the ESP32, connect it to your Wi-Fi through its setup page, then add
                it here using the claim code shown on that page.
              </p>
            </section>
          ) : (
            store.devices.map((device) => (
              <DeviceCard key={device.id} device={device} onOpen={() => setDetail(device)} />
            ))
          )}
        </div>

        {detail && (
          <div className="pane detail">
            <DeviceDetail device={detail} onClose={() => setDetail(null)} />
          </div>
        )}
      </main>
    </div>
  );
}
