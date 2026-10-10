// Bundled font files — the app has no guaranteed internet, so no Google Fonts.
// Latin subset only, and only the weights theme.ts actually uses.
import '@fontsource/manrope/latin-400.css';
import '@fontsource/manrope/latin-500.css';
import '@fontsource/manrope/latin-600.css';
import '@fontsource/manrope/latin-700.css';
import '@fontsource/manrope/latin-800.css';

import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import App from './App';

async function bootstrap() {
  // Dev-only stand-in for the C++ core. Dynamically imported so it never enters
  // the production bundle — see devMock.ts for the three layers of guarding.
  if (import.meta.env.DEV) {
    const { installDevMock } = await import('./api/devMock');
    installDevMock();
  }

  const root = document.getElementById('root');
  if (!root) throw new Error('#root missing from index.html');

  createRoot(root).render(
    <StrictMode>
      <App />
    </StrictMode>,
  );
}

void bootstrap();
