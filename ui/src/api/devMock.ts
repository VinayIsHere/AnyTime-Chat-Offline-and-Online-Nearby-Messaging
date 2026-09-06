/**
 * A browser-side stand-in for the C++ core, active ONLY under `vite dev`.
 *
 * Why this exists: phase 0 is being built UI-first, so the local HTTP server
 * does not answer yet. Without a mock the sign-in flow cannot be run or
 * reviewed at all. With it, `npm run dev` gives a working account-creation
 * flow in an ordinary browser today.
 *
 * It is NOT a test double for the protocol and never will be — the real
 * multi-device testing story is the loopback transport (AD-07), which exercises
 * the actual C++ core with two desktop instances. This only fakes HTTP shapes so
 * screens can be built before the backend lands.
 *
 * Guarded three ways so it can never reach a shipped build:
 *   1. `import.meta.env.DEV` — stripped from production bundles entirely
 *   2. `IS_NATIVE_SHELL` — inert the moment the C++ server serves the page
 *   3. dynamic import from main.tsx, so it is not in the production graph
 */

import { BASE_PATH, IS_NATIVE_SHELL } from './base';
import type { Account, ChatRequest, ChatSummary, Gender, NearbyPeer } from './client';

const STORAGE_KEY = 'anytime.devmock.account';

/** Deterministic 16-hex peer id, so the handle is stable across reloads. */
function fakePeerId(seed: string): string {
  let h1 = 0x811c9dc5;
  for (let i = 0; i < seed.length; i++) {
    h1 ^= seed.charCodeAt(i);
    h1 = Math.imul(h1, 0x01000193) >>> 0;
  }
  let h2 = Math.imul(h1 ^ 0x9e3779b9, 0x85ebca6b) >>> 0;
  return (h1.toString(16).padStart(8, '0') + h2.toString(16).padStart(8, '0')).slice(0, 16);
}

function handleFor(username: string, peerId: string): string {
  return '@' + username + '·' + peerId.slice(-4);
}

function loadAccount(): Account | null {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw ? (JSON.parse(raw) as Account) : null;
  } catch {
    return null;
  }
}

function saveAccount(a: Account): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(a));
  } catch {
    /* private window — the flow still works for one session */
  }
}

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });

const errorBody = (code: string, message: string, status: number) =>
  json({ code, message }, status);

/** Simulates the core taking a moment to generate a keypair. */
const delay = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function handle(path: string, init: RequestInit | undefined): Promise<Response | null> {
  const method = (init?.method ?? 'GET').toUpperCase();
  const body = init?.body ? JSON.parse(String(init.body)) : undefined;

  // ── Account ───────────────────────────────────────────────────────────────
  if (path === 'account') {
    if (method === 'GET') {
      const account = loadAccount();
      return account
        ? json(account)
        : errorBody('not_found', 'No account on this device yet.', 404);
    }

    if (method === 'POST') {
      await delay(600); // keypair generation is not instant on a phone
      const peerId = fakePeerId(body.username + '|' + body.dob);
      const account: Account = {
        peerId,
        username: body.username,
        handle: handleFor(body.username, peerId),
        dob: body.dob,
        gender: body.gender as Gender,
        discoverable: true,
        acceptFrom: 'everyone',
      };
      saveAccount(account);
      return json(account, 201);
    }

    if (method === 'PATCH') {
      const current = loadAccount();
      if (!current) return errorBody('not_found', 'No account.', 404);
      const next: Account = { ...current, ...body };
      if (body.username) next.handle = handleFor(body.username, current.peerId);
      saveAccount(next);
      return json(next);
    }
  }

  // ── Everything else: empty, well-shaped responses ─────────────────────────
  // Enough for the shell to render without errors. Real behaviour arrives with
  // the loopback transport.
  if (path === 'discovery' || path === 'chats/read') return new Response(null, { status: 204 });
  if (path === 'discovery/peers') return json([] as NearbyPeer[]);
  if (path === 'requests') return json([] as ChatRequest[]);
  if (path === 'chats') return json([] as ChatSummary[]);
  if (/^chats\/[^/]+\/messages$/.test(path) && method === 'GET') {
    return json({ messages: [], nextCursor: null, hasMore: false });
  }

  return null; // not mocked — let it fall through to a real fetch
}

/** Installs the fetch interceptor. Safe to call more than once. */
export function installDevMock(): void {
  if (!import.meta.env.DEV || IS_NATIVE_SHELL) return;

  const realFetch = window.fetch.bind(window);
  const apiPrefix = BASE_PATH + 'api/';

  window.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url;

    if (url.startsWith(apiPrefix)) {
      const path = url.slice(apiPrefix.length).split('?')[0];
      const response = await handle(path, init);
      if (response) return response;
    }
    return realFetch(input as RequestInfo, init);
  };

  // EventSource cannot be intercepted the same way; the SSE stream simply never
  // connects in dev, which is correct — there is no core to push anything.
  console.info(
    '[AnyTime] dev mock active — account endpoints are faked in localStorage.\n' +
      'Clear it with: localStorage.removeItem("%s")',
    STORAGE_KEY,
  );
}

/** Wipes the mock account, so the welcome flow can be replayed. */
export function resetDevMock(): void {
  localStorage.removeItem(STORAGE_KEY);
}
