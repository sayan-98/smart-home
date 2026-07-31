// ---------------------------------------------------------------------------
//  VoiceBar.tsx - speak or type, in any language.
//
//  "পাখাটা ৫ মিনিটের জন্য চালাও", "pankha band karo", "fan on for 5 minutes" -
//  all the same to this bar. The status line narrates every stage so nothing
//  is a mystery: Listening (with the live transcript as you speak), then
//  Thinking, then the model's reply in your own language.
//
//  Voice input uses the browser's SpeechRecognition when the WebView provides
//  it. When it does not (many Android WebViews), the mic button hides and the
//  keyboard's own mic key does the same job through the text field - Gboard
//  dictation handles every language the keyboard does.
// ---------------------------------------------------------------------------
import { useEffect, useRef, useState } from 'react';
import type { JSX } from 'react';
import { Capacitor } from '@capacitor/core';
import { SpeechRecognition as NativeSpeech } from '@capacitor-community/speech-recognition';

import { store } from '../api/store.js';
import { NlError } from '../api/nl.js';
import { getGroqKey, setGroqKey } from '../api/transport.js';

type Phase = 'idle' | 'listening' | 'thinking';

/** Minimal typing for the vendor-prefixed API; absent in many WebViews. */
interface SpeechRecognitionLike {
  continuous: boolean;
  interimResults: boolean;
  onresult: ((ev: {
    resultIndex: number;
    results: ArrayLike<{ isFinal: boolean; 0: { transcript: string } }>;
  }) => void) | null;
  onend: (() => void) | null;
  onerror: ((ev: { error?: string }) => void) | null;
  start(): void;
  stop(): void;
}

function getRecognizer(): SpeechRecognitionLike | null {
  const w = window as unknown as {
    SpeechRecognition?: new () => SpeechRecognitionLike;
    webkitSpeechRecognition?: new () => SpeechRecognitionLike;
  };
  const Ctor = w.SpeechRecognition ?? w.webkitSpeechRecognition;
  if (!Ctor) return null;
  try {
    return new Ctor();
  } catch {
    return null;
  }
}

export function VoiceBar(): JSX.Element {
  const [hasKey, setHasKey] = useState<boolean | null>(null);
  const [keyDraft, setKeyDraft] = useState('');
  const [text, setText] = useState('');
  const [phase, setPhase] = useState<Phase>('idle');
  const [interim, setInterim] = useState('');
  const [reply, setReply] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [micAvailable, setMicAvailable] = useState(false);
  const recRef = useRef<SpeechRecognitionLike | null>(null);

  /** Native recognizer (real Android speech) beats the WebView shim. */
  const nativeRef = useRef(false);

  useEffect(() => {
    void getGroqKey().then((k) => setHasKey(Boolean(k)));

    void (async () => {
      // Prefer the OS recognizer: it exists on every Android phone, handles
      // every keyboard language, and streams partial results for the live
      // "Listening: ..." line. The WebView's SpeechRecognition rarely exists.
      if (Capacitor.isNativePlatform()) {
        try {
          const { available } = await NativeSpeech.available();
          if (available) {
            nativeRef.current = true;
            setMicAvailable(true);
            return;
          }
        } catch {
          /* fall through to the web API check */
        }
      }
      setMicAvailable(getRecognizer() !== null);
    })();

    return () => {
      recRef.current?.stop();
      if (nativeRef.current) void NativeSpeech.stop().catch(() => {});
    };
  }, []);

  async function startNative(): Promise<void> {
    try {
      const perm = await NativeSpeech.requestPermissions();
      if (perm.speechRecognition !== 'granted') {
        setError('Microphone permission was refused.');
        return;
      }

      setError(null);
      setReply(null);
      setPhase('listening');
      setInterim('');

      let lastHeard = '';
      await NativeSpeech.removeAllListeners();
      await NativeSpeech.addListener('partialResults', (data: { matches: string[] }) => {
        if (data.matches?.[0]) {
          lastHeard = data.matches[0];
          setInterim(lastHeard); // the live transcript while you speak
        }
      });
      await NativeSpeech.addListener('listeningState', (data: { status: string }) => {
        if (data.status === 'stopped') {
          setPhase('idle');
          setInterim('');
          if (lastHeard.trim()) {
            setText(lastHeard);
            void run(lastHeard);
          }
        }
      });

      // No language pinned: the OS recognizer follows the device's speech
      // settings, which is what makes Bengali/Hindi/anything work.
      await NativeSpeech.start({ partialResults: true, popup: false });
    } catch {
      setPhase('idle');
      setError('Could not start the microphone - try typing instead.');
    }
  }

  function stopNative(): void {
    void NativeSpeech.stop().catch(() => {});
  }

  async function saveKey(): Promise<void> {
    const k = keyDraft.trim();
    if (!k.startsWith('gsk_') || k.length < 20) {
      setError('That does not look like a Groq key (they start with gsk_).');
      return;
    }
    await setGroqKey(k);
    setKeyDraft('');
    setHasKey(true);
    setError(null);
  }

  async function run(utterance: string): Promise<void> {
    const spoken = utterance.trim();
    if (!spoken || phase === 'thinking') return;

    setPhase('thinking');
    setReply(null);
    setError(null);
    try {
      const res = await store.runNl(spoken);
      if (!res.understood || res.executed === 0) {
        setError(res.reply || 'I could not match that to a socket. Try using its name.');
      } else {
        setReply(res.reply);
        setText('');
      }
    } catch (err) {
      setError(err instanceof NlError ? err.message : 'Something went wrong.');
    } finally {
      setPhase('idle');
      setInterim('');
    }
  }

  function startListening(): void {
    const rec = getRecognizer();
    if (!rec) return;
    recRef.current?.stop();
    recRef.current = rec;

    rec.continuous = false;
    rec.interimResults = true; // this is what makes the live transcript appear

    let finalText = '';
    rec.onresult = (ev) => {
      let interimText = '';
      for (let i = ev.resultIndex; i < ev.results.length; i++) {
        const r = ev.results[i];
        if (r.isFinal) finalText += r[0].transcript;
        else interimText += r[0].transcript;
      }
      setInterim(interimText || finalText);
      if (finalText) setText(finalText);
    };
    rec.onend = () => {
      setPhase('idle');
      setInterim('');
      if (finalText.trim()) void run(finalText);
    };
    rec.onerror = (ev) => {
      setPhase('idle');
      setInterim('');
      setError(
        ev.error === 'not-allowed'
          ? 'Microphone permission was refused.'
          : 'Could not hear that - try again, or type instead.',
      );
    };

    setError(null);
    setReply(null);
    setPhase('listening');
    rec.start();
  }

  function stopListening(): void {
    recRef.current?.stop();
  }

  if (hasKey === null) return <></>;

  if (!hasKey) {
    return (
      <section className="card ai">
        <p className="sub">
          Voice and any-language control needs your Groq API key (free at console.groq.com).
          It is stored only on this device.
        </p>
        <div className="row">
          <input
            value={keyDraft}
            onChange={(e) => setKeyDraft(e.target.value)}
            placeholder="gsk_..."
            autoCapitalize="off"
            autoCorrect="off"
            type="password"
            onKeyDown={(e) => e.key === 'Enter' && void saveKey()}
          />
          <button disabled={!keyDraft.trim()} onClick={() => void saveKey()}>
            Save
          </button>
        </div>
        {error && <div className="err">{error}</div>}
      </section>
    );
  }

  return (
    <section className="card ai">
      <div className="row">
        <input
          value={text}
          placeholder='say or type - "পাখা চালাও", "fan on for 5 minutes"...'
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => e.key === 'Enter' && void run(text)}
          disabled={phase !== 'idle'}
        />
        {micAvailable && (
          <button
            className={phase === 'listening' ? 'danger' : 'sec'}
            onClick={() => {
              if (phase === 'listening') {
                nativeRef.current ? stopNative() : stopListening();
              } else {
                nativeRef.current ? void startNative() : startListening();
              }
            }}
            disabled={phase === 'thinking'}
          >
            {phase === 'listening' ? 'Stop' : '🎤 Speak'}
          </button>
        )}
        <button disabled={phase !== 'idle' || !text.trim()} onClick={() => void run(text)}>
          {phase === 'thinking' ? '...' : 'Ask'}
        </button>
      </div>

      {/* The narration line - this is the "better visualisation" part. */}
      {phase === 'listening' && (
        <p className="nl-status listening">
          Listening{interim ? `: "${interim}"` : '...'}
        </p>
      )}
      {phase === 'thinking' && <p className="nl-status">Understanding...</p>}
      {reply && <p className="nl-status ok">{reply}</p>}
      {error && <p className="nl-status err-text">{error}</p>}

      {!micAvailable && (
        <p className="sub hint">
          Tip: tap the text box and use your keyboard&apos;s mic key to dictate in any
          language.
        </p>
      )}
    </section>
  );
}
