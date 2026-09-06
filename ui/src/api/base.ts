/**
 * Resolves the secret path prefix the whole app is served under.
 *
 * AD-08 / docs/ARCHITECTURE.md section 10 — Android's loopback interface is
 * shared by every app on the device, so anything installed can probe
 * 127.0.0.1. The gate is a 256-bit path prefix generated per launch:
 *
 *     http://127.0.0.1:<ephemeral-port>/<64-hex-token>/
 *
 * The C++ server substitutes it into index.html while serving, so the page
 * knows its credential from the first byte — no injection race, unlike a
 * bearer token pushed in after load.
 *
 * Everything else in this module exists so the rest of the UI never has to
 * think about any of that.
 */

/**
 * The literal placeholder in index.html, left unreplaced by `vite dev`.
 *
 * Spelled differently from the `__AT_BASE__` global on purpose — if the two
 * matched, the server's substitution would also rewrite the property name and
 * produce `window./abc123/`, a syntax error. See the comment in index.html.
 */
const PLACEHOLDER = '%%AT_BASE%%';

declare global {
  interface Window {
    __AT_BASE__?: string;
  }
}

function resolveBase(): string {
  const injected = typeof window === 'undefined' ? undefined : window.__AT_BASE__;

  // Unreplaced placeholder means we are in `vite dev`, served from the root.
  if (!injected || injected === PLACEHOLDER) return '/';

  return injected.endsWith('/') ? injected : injected + '/';
}

/**
 * Path prefix with leading and trailing slashes — "/" in dev, "/<token>/" when
 * served by the C++ shell. Captured once at module load, BEFORE the router
 * starts rewriting `location.pathname`.
 */
export const BASE_PATH = resolveBase();

/** True when the C++ shell is serving us, false under `vite dev`. */
export const IS_NATIVE_SHELL = BASE_PATH !== '/';

/**
 * Router basename, so `<Link to="/nearby">` resolves inside the token prefix.
 * React Router wants no trailing slash.
 */
export const ROUTER_BASENAME = BASE_PATH === '/' ? '/' : BASE_PATH.slice(0, -1);

/** Absolute URL for a local API path. `apiUrl('account')` -> "/<token>/api/account". */
export function apiUrl(path: string): string {
  return BASE_PATH + 'api/' + path.replace(/^\/+/, '');
}
