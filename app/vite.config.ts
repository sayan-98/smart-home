import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  build: {
    // Capacitor ships the bundle inside the APK, so there is no download cost
    // to worry about and a readable stack trace on a real device is worth more.
    sourcemap: true,
    target: 'es2020',
  },
  server: {
    host: true, // reachable from the tablet during `npm run dev`
    port: 5173,
  },
});
