import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// The bundle is served by the C++ local HTTP server from a SECRET PATH PREFIX
// (AD-08): http://127.0.0.1:<ephemeral>/<64-hex-token>/
//
// So every asset URL must be RELATIVE — an absolute "/assets/index.js" would
// resolve outside the token prefix and 404. `base: './'` is what makes that work,
// and it is the one setting here that will silently break the Android build if
// it is ever changed.
export default defineConfig({
  base: './',
  plugins: [react()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    // The Android WebView chokes on <script type="module" crossorigin> when the
    // page is served from a plain local server; a single classic bundle avoids
    // module-preload and CORS entirely.
    target: 'es2020',
    modulePreload: false,
    rollupOptions: {
      output: {
        // One file each, so the C++ asset server has nothing to guess about.
        entryFileNames: 'assets/[name].js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name].[ext]',
      },
    },
  },
  server: {
    port: 5173,
    strictPort: false,
  },
});
