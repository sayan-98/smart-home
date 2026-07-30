import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { SplashScreen } from '@capacitor/splash-screen';
import { StatusBar, Style } from '@capacitor/status-bar';
import { Capacitor } from '@capacitor/core';

import { App } from './App.js';
import './styles.css';

async function initNative(): Promise<void> {
  if (!Capacitor.isNativePlatform()) return;
  try {
    // Follow the tablet's own light/dark setting rather than forcing one.
    const dark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    await StatusBar.setStyle({ style: dark ? Style.Dark : Style.Light });
    await SplashScreen.hide();
  } catch {
    // Not fatal - the app works fine without either plugin.
  }
}

void initNative();

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
