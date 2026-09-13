/// <reference types="vitest/config" />
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

// Plan section 4: the application is served by the device, works without the
// internet, and fits a 512 KiB gzip budget. Nothing here reaches outside the
// origin: no CDN, no web fonts, and assets are files, not inline data URIs, so
// the page's Content-Security-Policy can stay at 'self'.
export default defineConfig({
  plugins: [react()],
  build: {
    target: 'es2020',
    assetsInlineLimit: 0,
    sourcemap: false,
    cssCodeSplit: false,
    reportCompressedSize: true,
  },
  server: {
    // `npm run dev` against the mock server: the API is same-origin through
    // the proxy, exactly as it is when the device serves the page.
    proxy: { '/api': 'http://127.0.0.1:8080' },
  },
  test: {
    environment: 'jsdom',
    setupFiles: ['./src/test/setup.ts'],
    include: ['src/**/*.test.{ts,tsx}'],
    restoreMocks: true,
  },
});
