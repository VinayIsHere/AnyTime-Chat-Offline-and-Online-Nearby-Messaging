import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// Two build targets, selected by mode:
//
//   npm run build:native   (alias: build:android)  ->  dist/
//   npm run build:web                              ->  dist-web/
//
// Android and iOS share the NATIVE bundle: both are shells that serve it from
// the C++ loopback server. Web is a genuinely different artifact, not a rename.
//
// The difference that matters is `base`:
//
//   native  './'  The bundle is served under a SECRET PATH PREFIX (AD-08),
//                 http://127.0.0.1:<port>/<64-hex-token>/, so asset URLs must be
//                 relative. An absolute "/assets/index.js" would resolve outside
//                 the token prefix and 404 — silently breaking the phone build.
//
//   web     '/'   A hosted app uses client-side routes like /chats/abc. With a
//                 relative base, reloading that URL resolves ./assets against
//                 /chats/ and 404s. Absolute paths are correct there.
//
// CMake's copy_ui_assets target packages dist/ into the APK, so the native
// output directory must stay `dist`.
export default defineConfig(({ mode }) => {
  const isWeb = mode === 'web';

  return {
    base: isWeb ? '/' : './',
    plugins: [react()],
    build: {
      outDir: isWeb ? 'dist-web' : 'dist',
      emptyOutDir: true,
      target: 'es2020',
      modulePreload: false,
      rollupOptions: {
        output: {
          // Stable file names, so the C++ asset server has nothing to guess about.
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
  };
});
