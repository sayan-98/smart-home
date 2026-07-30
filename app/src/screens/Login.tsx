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
import { ApiError, getApiBase } from '../api/transport.js';

interface Props {
  onConnected: () => void;
}

export function Login({ onConnected }: Props): JSX.Element {
  const [tab, setTab] = useState<'direct' | 'account'>('direct');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // direct
  const [hint, setHint] = useState('');

  // account
  const [base, setBase] = useState('');
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [mode, setMode] = useState<'login' | 'register'>('login');

  useEffect(() => {
    void getApiBase().then((stored) => setBase(stored || ''));
  }, []);

  async function connectDirect(): Promise<void> {
    setError(null);
    setBusy(true);
    try {
      const ok = await store.connectDirect(hint);
      if (ok) onConnected();
      else setError(store.error ?? 'No device answered.');
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
            className={`tab ${tab === 'account' ? 'act' : ''}`}
            onClick={() => {
              setTab('account');
              setError(null);
            }}
          >
            Account
          </button>
        </div>

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
