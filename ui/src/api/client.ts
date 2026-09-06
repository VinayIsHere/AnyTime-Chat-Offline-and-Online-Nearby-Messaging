/**
 * The whole local API surface. Mirrors docs/ARCHITECTURE.md section 05.
 *
 * Every call goes to the C++ core over loopback HTTP under the secret path
 * prefix. There is no remote server anywhere in this app.
 *
 * NOTE ON WHAT IS GONE (AD-05): there is no username-availability check. With
 * no server there is no global registry to check against, so usernames are
 * display labels that may collide. Identity is the keypair; the UI
 * disambiguates with `peerId` / `handle`.
 */

import { apiUrl } from './base';

// ─────────────────────────────────────────────────────────────────────────────
// Types — these mirror the C++ domain in core/domain/Entities.h
// ─────────────────────────────────────────────────────────────────────────────

export type Gender = 'Male' | 'Female' | 'Other' | 'Prefer not to say';
export type AcceptFrom = 'everyone' | 'nobody';

export interface Account {
  /** 16 hex characters — first 8 bytes of the key fingerprint. */
  peerId: string;
  username: string;
  /** "@vinay·4f2a" — username plus the fingerprint discriminator. */
  handle: string;
  dob: string;
  gender: Gender;
  discoverable: boolean;
  acceptFrom: AcceptFrom;
}

export type SignalBand = 'very strong' | 'strong' | 'weak' | 'unknown';

export interface NearbyPeer {
  peerId: string;
  nickname: string;
  handle: string;
  signal: SignalBand;
}

export interface ChatRequest {
  id: string;
  peerId: string;
  nickname: string;
  handle: string;
  greeting: string;
  at: string;
}

export interface ChatSummary {
  id: string;
  peerId: string;
  handle: string;
  lastMessage: string;
  at: string;
  unread: number;
  reachable: boolean;
}

export type DeliveryStatus = 'queued' | 'sent' | 'delivered' | 'read' | 'failed';

export interface Message {
  id: string;
  chatId: string;
  clientMessageId: string;
  body: string;
  mine: boolean;
  at: string;
  status: DeliveryStatus;
}

export interface MessagePage {
  messages: Message[];
  nextCursor: string | null;
  hasMore: boolean;
}

// ─────────────────────────────────────────────────────────────────────────────
// Transport
// ─────────────────────────────────────────────────────────────────────────────

/** Thrown for any non-2xx response, carrying the core's ErrorCode string. */
export class ApiError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    message: string,
  ) {
    super(message);
    this.name = 'ApiError';
  }
}

/** Distinguishes "no account yet" from a real failure, so App can route to /welcome. */
export const NOT_FOUND = 'not_found';

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let res: Response;
  try {
    res = await fetch(apiUrl(path), {
      headers: { 'Content-Type': 'application/json' },
      ...init,
    });
  } catch (cause) {
    // The core is not up yet, or the shell is still starting the server.
    throw new ApiError(0, 'unreachable', 'Cannot reach the app core.');
  }

  if (!res.ok) {
    let code = 'internal';
    let message = res.statusText;
    try {
      const body = await res.json();
      code = body.code ?? code;
      message = body.message ?? message;
    } catch {
      /* non-JSON error body — keep the status text */
    }
    throw new ApiError(res.status, code, message);
  }

  if (res.status === 204) return undefined as T;
  return (await res.json()) as T;
}

const post = <T>(path: string, body?: unknown) =>
  request<T>(path, { method: 'POST', body: body === undefined ? undefined : JSON.stringify(body) });

// ─────────────────────────────────────────────────────────────────────────────

export const api = {
  // ── Account ───────────────────────────────────────────────────────────────
  /** Rejects with ApiError(404, 'not_found') before setup. */
  getAccount: () => request<Account>('account'),

  /** Generates the identity keypair. This call IS account creation. */
  createAccount: (p: { username: string; dob: string; gender: Gender }) =>
    post<Account>('account', p),

  updateAccount: (
    p: Partial<Pick<Account, 'username' | 'gender' | 'discoverable' | 'acceptFrom'>>,
  ) => request<Account>('account', { method: 'PATCH', body: JSON.stringify(p) }),

  // ── Discovery ─────────────────────────────────────────────────────────────
  startDiscovery: () => post<void>('discovery'),
  stopDiscovery: () => request<void>('discovery', { method: 'DELETE' }),
  /** Peers inside the 60 s reachability window, strongest signal first. */
  getPeers: () => request<NearbyPeer[]>('discovery/peers'),

  // ── Requests ──────────────────────────────────────────────────────────────
  sendRequest: (peerId: string, greeting = '') =>
    post<ChatRequest>('requests', { peerId, greeting }),
  getRequests: () => request<ChatRequest[]>('requests'),
  acceptRequest: (id: string) => post<ChatSummary>(`requests/${id}/accept`),
  declineRequest: (id: string, block = false) => post<void>(`requests/${id}/decline`, { block }),

  // ── Chats ─────────────────────────────────────────────────────────────────
  getChats: () => request<ChatSummary[]>('chats'),
  getMessages: (chatId: string, before?: string) =>
    request<MessagePage>(`chats/${chatId}/messages${before ? `?before=${encodeURIComponent(before)}` : ''}`),
  /** Returns immediately at status 'queued'; later transitions arrive over SSE. */
  sendMessage: (chatId: string, body: string, clientMessageId: string) =>
    post<Message>(`chats/${chatId}/messages`, { body, clientMessageId }),
  markRead: (chatId: string, messageId: string) =>
    post<void>(`chats/${chatId}/read`, { messageId }),
};
