// ---------------------------------------------------------------------------
//  Login.tsx - two ways in.
//
//  DIRECT is offered first, and deliberately so. One board on your own Wi-Fi
//  does not need an account, a server, or an internet connection - the ESP32
//  serves its own API. Requiring a cloud sign-up to tap a switch in your own
//  house would be the wrong default.
//
//  The account path exists for what it is actually good for: reaching the house
//  from outside it, several devices, and the AI features.
// ---------------------------------------------------------------------------
import { useEffect, useState } from 'react';
import type { JSX } from 'react';

import { store } from '../api/store.js';
import {
  ApiError,
  getApiBase,
  getSavedCreds,
  rememberCreds,
  type SavedCreds,
} from '../api/transport.js';

interface Props {
  onConnected: () => void;
}

export function Login({ onConnected }: Props): JSX.Element {
  const [tab, setTab] = useState<'direct' | 'remote' | 'account'>('direct');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // direct
  const [hint, setHint] = useState('');

  // remote (broker)
  const [host, setHost] = useState('');
  const [port, setPort] = useState('8884');
  const [user, setUser] = useState('');
  const [pass, setPass] = useState('');

  // account
  const [base, setBase] = useState('');
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [mode, setMode] = useState<'login' | 'register'>('login');

  const [saved, setSaved] = useState<SavedCreds>({});

  useEffect(() => {
    void getApiBase().then((stored) => setBase(stored || ''));
    void getSavedCreds().then(setSaved);
  }, []);

  /** Whether anything is remembered for the tab currently showing. */
  const hasSavedForTab =
    (tab === 'direct' && Boolean(saved.directHint)) ||
    (tab === 'remote' && Boolean(saved.broker)) ||
    (tab === 'account' && Boolean(saved.account));

  function fillSaved(): void {
    if (tab === 'direct' && saved.directHint) {
      setHint(saved.directHint);
    } else if (tab === 'remote' && saved.broker) {
      setHost(saved.broker.host);
      setPort(String(saved.broker.port));
      setUser(saved.broker.username);
      setPass(saved.broker.password);
    } else if (tab === 'account' && saved.account) {
      setBase(saved.account.base);
      setEmail(saved.account.email);
      setPassword(saved.account.password);
    }
    setError(null);
  }

  /** A short reminder of what will be filled, so it is never a blind tap. */
  function savedLabel(): string {
    if (tab === 'direct') return saved.directHint ?? '';
    if (tab === 'remote' && saved.broker) {
      return `${saved.broker.username} @ ${saved.broker.host}`;
    }
    if (tab === 'account' && saved.account) return saved.account.email;
    return '';
  }

  async function connectDirect(): Promise<void> {
    setError(null);
    setBusy(true);
    try {
      const ok = await store.connectDirect(hint);
      if (ok) {
        // Only remember what actually worked. Saving on submit would preserve
        // typos and offer them back forever.
        if (hint.trim()) await rememberCreds({ directHint: hint.trim() });
        onConnected();
      } else {
        setError(store.error ?? 'No device answered.');
      }
    } finally {
      setBusy(false);
    }
  }

  async function connectRemote(): Promise<void> {
    setError(null);
    setBusy(true);
    try {
      const broker = {
        // People paste the whole URL from the broker's dashboard; strip it back
        // to a hostname rather than failing on it.
        host: host.trim().replace(/^\w+:\/\//, '').replace(/[/:].*$/, ''),
        port: Number(port) || 8884,
        username: user.trim(),
        password: pass,
        path: '/mqtt',
      };
      const ok = await store.connectRemote(broker);
      if (ok) {
        await rememberCreds({ broker });
        onConnected();
      } else {
        setError(store.remoteDetail ?? 'Could not reach the broker.');
      }
    } finally {
      setBusy(false);
    }
  }

  async function submitAccount(): Promise<void> {
    setError(null);
    setBusy(true);
    try {
      const url = base.replace(/\/+$/, '');
      const res = await fetch(`${url}/api/auth/${mode}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ email: email.trim(), password }),
      });

      if (!res.ok) {
        const body = (await res.json().catch(() => ({}))) as { error?: string };
        throw new ApiError(res.status, body.error ?? `Server returned ${res.status}`);
      }

      const body = (await res.json()) as { token: string };
      const { setApiBase, setToken } = await import('../api/transport.js');
      await setApiBase(url);
      await setToken(body.token);
      await rememberCreds({ account: { base: url, email: email.trim(), password } });
      await store.init();
      onConnected();
    } catch (err) {
      // A free-tier server cold-starts; saying so prevents a pointless retry loop.
      setError(
        err instanceof ApiError
          ? err.message
          : 'Could not reach the server. If it is on a free plan it may be waking up - try again in 30 seconds.',
      );
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="center">
      <div className="card login">
        <h1>Smart Home</h1>

        <div className="tabs">
          <button
            className={`tab ${tab === 'direct' ? 'act' : ''}`}
            onClick={() => {
              setTab('direct');
              setError(null);
            }}
          >
            This Wi-Fi
          </button>
          <button
            className={`tab ${tab === 'remote' ? 'act' : ''}`}
            onClick={() => {
              setTab('remote');
              setError(null);
            }}
          >
            Anywhere
          </button>
          <button
            className={`tab ${tab === 'account' ? 'act' : ''}`}
            onClick={() => {
              setTab('account');
              setError(null);
            }}
          >
            Account
          </button>
        </div>

        {/* Offered on whichever tab has something remembered, and labelled with
            what it will fill so it is never a blind tap. */}
        {hasSavedForTab && (
          <div className="saved-fill">
            <button className="sec" onClick={fillSaved}>
              Fill my saved details
            </button>
            <span className="sub">{savedLabel()}</span>
          </div>
        )}

        {tab === 'direct' ? (
          <>
            <p className="sub">
              Control a device directly on your own network. No account, no server, and it
              keeps working with the internet down.
            </p>

            <label>Device address (optional)</label>
            <input
              value={hint}
              onChange={(e) => setHint(e.target.value)}
              placeholder="192.168.1.42  or  smarthome-ab10.local"
              autoCapitalize="off"
              autoCorrect="off"
              onKeyDown={(e) => e.key === 'Enter' && void connectDirect()}
            />

            {error && <div className="err">{error}</div>}

            <button disabled={busy} onClick={() => void connectDirect()}>
              {busy ? 'Looking...' : 'Find my device'}
            </button>

            <p className="sub hint">
              Leave the box empty to search automatically. If the device is still in setup
              mode, join its <code>SmartHome-XXXX</code> Wi-Fi first - it answers on{' '}
              <code>192.168.4.1</code>.
            </p>
          </>
        ) : tab === 'remote' ? (
          <>
            <p className="sub">
              Control your home from anywhere - office, travel, mobile data. Your device and
              this app both connect out to a broker, so no port forwarding or fixed IP is
              needed.
            </p>

            <label>Broker host</label>
            <input
              value={host}
              onChange={(e) => setHost(e.target.value)}
              placeholder="abc123.s1.eu.hivemq.cloud"
              autoCapitalize="off"
              autoCorrect="off"
            />

            <label>WebSocket port</label>
            <input
              value={port}
              onChange={(e) => setPort(e.target.value)}
              inputMode="numeric"
              placeholder="8884"
            />

            <label>Username</label>
            <input
              value={user}
              onChange={(e) => setUser(e.target.value)}
              autoCapitalize="off"
              autoCorrect="off"
            />

            <label>Password</label>
            <input
              value={pass}
              onChange={(e) => setPass(e.target.value)}
              type="password"
              onKeyDown={(e) => e.key === 'Enter' && void connectRemote()}
            />

            {error && <div className="err">{error}</div>}

            <button disabled={busy || !host || !user || !pass} onClick={() => void connectRemote()}>
              {busy ? 'Connecting...' : 'Connect'}
            </button>

            <p className="sub hint">
              Use the <b>WebSocket</b> port, not the MQTT one - 8884 on HiveMQ Cloud, not 8883.
              The device must be given the same broker details, and it needs internet at home
              that stays on while you are out.
            </p>
          </>
        ) : (
          <>
            <p className="sub">
              Sign in to reach your home from anywhere, manage several devices, and use the
              assistant.
            </p>

            <label>Server</label>
            <input
              value={base}
              onChange={(e) => setBase(e.target.value)}
              placeholder="https://your-app.onrender.com"
              autoCapitalize="off"
              autoCorrect="off"
              inputMode="url"
            />

            <label>Email</label>
            <input
              value={email}
              onChange={(e) => setEmail(e.target.value)}
              type="email"
              autoCapitalize="off"
              autoCorrect="off"
            />

            <label>Password</label>
            <input
              value={password}
              onChange={(e) => setPassword(e.target.value)}
              type="password"
              onKeyDown={(e) => e.key === 'Enter' && void submitAccount()}
            />

            {error && <div className="err">{error}</div>}

            <button
              disabled={busy || !email || !password || !base}
              onClick={() => void submitAccount()}
            >
              {busy ? 'Working...' : mode === 'login' ? 'Sign in' : 'Create account'}
            </button>

            <button
              className="link"
              onClick={() => {
                setMode(mode === 'login' ? 'register' : 'login');
                setError(null);
              }}
            >
              {mode === 'login' ? 'Create an account instead' : 'I already have an account'}
            </button>
          </>
        )}
      </div>
    </div>
  );
}
