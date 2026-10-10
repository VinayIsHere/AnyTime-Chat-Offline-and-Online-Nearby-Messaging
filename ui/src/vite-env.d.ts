/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** Set by .env.native or .env.web; unset under `vite dev`. */
  readonly VITE_PLATFORM?: 'native' | 'web';
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
