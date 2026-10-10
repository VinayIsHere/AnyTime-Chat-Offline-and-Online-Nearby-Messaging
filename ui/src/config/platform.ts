/**
 * Build-time target, set by the Vite mode (see vite.config.ts and .env.*).
 *
 * The single place the UI should branch on native vs web. Components ask
 * `IS_WEB` rather than sniffing user agents or checking for the secret prefix,
 * so adding the web app later means filling in branches, not hunting for them.
 *
 * `vite dev` runs in development mode, which sets no VITE_PLATFORM; that is
 * treated as native, because the dev server stands in for the local C++ core.
 * Web must be asked for explicitly.
 */
export type Platform = 'native' | 'web';

export const PLATFORM: Platform = import.meta.env.VITE_PLATFORM === 'web' ? 'web' : 'native';

export const IS_WEB = PLATFORM === 'web';
export const IS_NATIVE = PLATFORM === 'native';
