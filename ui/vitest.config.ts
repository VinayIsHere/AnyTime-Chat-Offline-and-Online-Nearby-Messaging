import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

// Separate from vite.config.ts so the two build targets there stay about
// building, and nothing test-only can leak into a shipped bundle.
//
// Screens are rendered in jsdom against a mocked `api` and `onEvent` — the
// C++ core is covered by its own suite (tests/ServiceTests.cpp), so these
// assert what the USER sees and which calls a tap makes, not the protocol.
export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'jsdom',
    setupFiles: ['./src/test/setup.ts'],
    include: ['src/**/*.test.{ts,tsx}'],
    restoreMocks: true,
  },
});
