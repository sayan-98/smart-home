import type { CapacitorConfig } from '@capacitor/cli';

const config: CapacitorConfig = {
  appId: 'com.smarthome.os',
  appName: 'Smart Home',
  webDir: 'dist',

  android: {
    // The app talks directly to the ESP32 over plain HTTP on the LAN, which
    // Android blocks by default from API 28 onward. This permits it; the
    // network-security config in android/ narrows it to private ranges so it is
    // not a blanket "allow all cleartext".
    allowMixedContent: true,
  },

  server: {
    // Required for the direct-to-device path: without it the WebView refuses
    // http:// requests to 192.168.x.x while the app itself is served over
    // https://. See docs/API.md - LAN traffic is plain HTTP by design in v1.
    cleartext: true,
    androidScheme: 'https',
  },

  plugins: {
    SplashScreen: {
      launchShowDuration: 800,
      backgroundColor: '#0e1116',
      showSpinner: false,
      androidSplashResourceName: 'splash',
    },
  },
};

export default config;
