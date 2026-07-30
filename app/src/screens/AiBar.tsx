// ---------------------------------------------------------------------------
//  AiBar.tsx - type a sentence, get an action.
//
//  The confirmation step is the important part of this component. When the
//  backend says a request was ambiguous, or that it would change several
//  sockets at once, nothing happens until you tap Confirm and can see exactly
//  what it intends to do. The other end of this is mains wiring; one extra tap
//  is a cheap price for never being surprised.
// ---------------------------------------------------------------------------
import { useState } from 'react';
import type { JSX } from 'react';

import { store } from '../api/store.js';
import { ApiError, cloud } from '../api/transport.js';

interface CommandResponse {
  understood: boolean;
  reply: string;
  actions: string[];
  rejected?: string[];
  executed: number;
  needsConfirmation?: boolean;
  reason?: string;
}

export function AiBar({ homeId }: { homeId: string }): JSX.Element {
  const [text, setText] = useState('');
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState<CommandResponse | null>(null);
  const [pendingText, setPendingText] = useState<string | null>(null);

  async function send(confirm: boolean): Promise<void> {
    const utterance = confirm ? (pendingText ?? text) : text;
    if (!utterance.trim()) return;

    setBusy(true);
    try {
      const res = await cloud<CommandResponse>('/api/ai/command', {
        method: 'POST',
        body: JSON.stringify({ homeId, text: utterance, confirm }),
      });
      setResult(res);

      if (res.needsConfirmation) {
        setPendingText(utterance);
      } else {
        setPendingText(null);
        setText('');
        if (res.executed > 0) await store.refreshDevices();
      }
    } catch (err) {
      setResult({
        understood: false,
        reply:
          err instanceof ApiError && err.status === 429
            ? 'Too many requests - wait a minute.'
            : 'The assistant is unavailable. The controls below still work normally.',
        actions: [],
        executed: 0,
      });
    } finally {
      setBusy(false);
    }
  }

  return (
    <section className="card ai">
      <div className="row">
        <input
          value={text}
          placeholder='e.g. "turn off everything in the bedroom"'
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => e.key === 'Enter' && void send(false)}
          disabled={busy}
        />
        <button disabled={busy || !text.trim()} onClick={() => void send(false)}>
          {busy ? '...' : 'Ask'}
        </button>
      </div>

      {result && (
        <div className={`ai-result ${result.understood ? '' : 'miss'}`}>
          <p>{result.reply}</p>

          {result.actions.length > 0 && (
            <ul>
              {result.actions.map((a) => (
                <li key={a}>{a}</li>
              ))}
            </ul>
          )}

          {result.needsConfirmation && (
            <div className="confirm">
              <p className="sub">{result.reason} - confirm before this runs.</p>
              <div className="row">
                <button disabled={busy} onClick={() => void send(true)}>
                  Confirm
                </button>
                <button
                  className="sec"
                  onClick={() => {
                    setResult(null);
                    setPendingText(null);
                  }}
                >
                  Cancel
                </button>
              </div>
            </div>
          )}

          {result.rejected && result.rejected.length > 0 && (
            <p className="sub">Skipped: {result.rejected.join('; ')}</p>
          )}

          {result.executed > 0 && (
            <p className="sub">
              {result.executed} command{result.executed === 1 ? '' : 's'} sent.
            </p>
          )}
        </div>
      )}
    </section>
  );
}
