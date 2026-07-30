// ---------------------------------------------------------------------------
//  Login.tsx - sign in, and pick which server to talk to.
//
//  The server URL is editable because there are three realistic deployments:
//  a laptop on the LAN during development, a Render instance, and eventually
//  something self-hosted. Hard-coding it would mean rebuilding the APK to move.
// ---------------------------------------------------------------------------
import { useEffect, useState } from 'react';
import type { JSX } from 'react';

import { ApiError, getApiBase } from '../api/transport.js';

interface Props {
  onLogin: (base: string, token: string) => Promise<void>;
}

export function Login({ onLogin }: Props): JSX.Element {
  const [base, setBase] = useState('');
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [mode, setMode] = useState<'login' | 'register'>('login');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    void getApiBase().then((stored) => setBase(stored || 'https://your-app.onrender.com'));
  }, []);

  async function submit(): Promise<void> {
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
      await onLogin(url, body.token);
    } catch (err) {
      // Render's free tier cold-starts. A first-attempt failure is usually just
      // the service waking up, and saying so prevents a pointless retry loop.
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
        <p className="sub">{mode === 'login' ? 'Sign in to your home' : 'Create an account'}</p>

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
          onKeyDown={(e) => e.key === 'Enter' && void submit()}
        />

        {error && <div className="err">{error}</div>}

        <button disabled={busy || !email || !password || !base} onClick={() => void submit()}>
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

        <p className="sub hint">
          No server yet? You can still control a device directly: put this tablet on the same
          Wi-Fi and open <code>http://smarthome-XXXX.local</code> in a browser.
        </p>
      </div>
    </div>
  );
}
