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

/**
 * Where we stand with a peer, derived server-side from the contact list and the
 * newest outgoing request. Survives navigating away and restarting the app,
 * which component state could not.
 */
export type PeerStatus = 'none' | 'requested' | 'declined' | 'contact';

export interface NearbyPeer {
  peerId: string;
  nickname: string;
  handle: string;
  signal: SignalBand;
  status: PeerStatus;
  /**
   * Whether the button is actionable. Separate from `status` on purpose: an
   * unanswered request stays `requested` — so the UI keeps saying it was sent —
   * but becomes resendable once the cooldown passes.
   */
  canResend: boolean;
  /** Present only when `status` is 'contact'. */
  chatId?: string;
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
  /**
   * We blocked this contact. The core refuses sends to them, so the chat
   * screen shows an unblock prompt in place of the composer.
   */
  blocked: boolean;
}

/** One row of Profile › Blocked users. */
export interface BlockedPeer {
  peerId: string;
  /** Best name the core holds — may be empty for a peer never named. */
  nickname: string;
  /** Always present: falls back to the fingerprint suffix when unnamed. */
  handle: string;
  /** Relative, e.g. "3d". */
  at: string;
}

/**
 * Radio state as the PLATFORM sees it — not the same question as whether the
 * mesh is currently carrying traffic.
 *
 * `unsupported` is the desktop and browser-development answer: there is no
 * radio to gate on, so the UI must treat it as usable rather than as a fault.
 * Mirrors anytime::RadioAvailability in core/domain/Enums.h.
 */
export type RadioAvailability = 'unknown' | 'unsupported' | 'unauthorized' | 'off' | 'on';

export interface BluetoothStatus {
  state: RadioAvailability;
  /** Android can show an in-app enable dialog; iOS can only open Settings. */
  canPromptInApp: boolean;
}

export type DeliveryStatus = 'queued' | 'sent' | 'delivered' | 'read' | 'failed';

/** Mirrors support::ReportReason in adapters/support/SupportLinks.h. */
export type ReportReason = 'child_safety' | 'sexual_content' | 'harassment' | 'spam' | 'other';

export interface ReportOutcome {
  /** False when the phone had no mail app to hand the draft to. */
  emailOpened: boolean;
  /** Where reports go, so the UI never has its own copy of the address. */
  email: string;
}

/** The pages on our website the app may open, by id. The core holds the URLs. */
export type ExternalLink = 'privacy' | 'child-safety';

/**
 * An image attached to a message. A REFERENCE, never the bytes — the chat list
 * would otherwise be a multi-megabyte JSON document. Fetch the image itself
 * from `api.mediaUrl(id)`.
 */
export interface MediaRef {
  id: string;
  mimeType: string;
  bytes: number;
}

export interface Message {
  id: string;
  chatId: string;
  clientMessageId: string;
  /** For a media message this is the caption, and may be empty. */
  body: string;
  mine: boolean;
  at: string;
  status: DeliveryStatus;
  /** Present only on image messages. */
  media?: MediaRef;
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

  // ── Bluetooth ─────────────────────────────────────────────────────────────
  /** Asks the platform fresh each time, so it is cheap to call on an event. */
  getBluetooth: () => request<BluetoothStatus>('bluetooth'),

  /**
   * Raises the system prompt and returns immediately — NOT when the user
   * answers, which may be seconds away or never. Watch `radio.state` instead.
   */
  enableBluetooth: () => post<void>('bluetooth/enable'),

  /** The fallback when the in-app dialog cannot help (permanently denied). */
  openBluetoothSettings: () => post<void>('bluetooth/settings'),

  // ── Discovery ─────────────────────────────────────────────────────────────
  /**
   * `active` scans continuously and is only justified while the Nearby screen is
   * on screen. `passive` duty-cycles in the background, which is what keeps the
   * mesh reachable without flattening the battery.
   */
  startDiscovery: (mode: 'active' | 'passive' = 'active') =>
    post<void>('discovery', { mode }),
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
  /** One chat row, so ChatScreen can name its peer without loading the list. */
  getChat: (chatId: string) => request<ChatSummary>(`chats/${chatId}`),
  getMessages: (chatId: string, before?: string) =>
    request<MessagePage>(`chats/${chatId}/messages${before ? `?before=${encodeURIComponent(before)}` : ''}`),
  /** Returns immediately at status 'queued'; later transitions arrive over SSE. */
  sendMessage: (chatId: string, body: string, clientMessageId: string) =>
    post<Message>(`chats/${chatId}/messages`, { body, clientMessageId }),
  markRead: (chatId: string, messageId: string) =>
    post<void>(`chats/${chatId}/read`, { messageId }),
  /**
   * Resends a message the core marked `failed`. Only valid for a stored row —
   * a bubble whose original POST never reached the core has no server id, and
   * is resent with sendMessage and the same clientMessageId instead.
   */
  retryMessage: (messageId: string) => post<void>(`messages/${messageId}/retry`),

  // ── Block list ────────────────────────────────────────────────────────────
  getBlocked: () => request<BlockedPeer[]>('blocked'),
  /** Also clears any request they have waiting. Idempotent. */
  blockPeer: (peerId: string) => post<void>('blocked', { peerId }),
  unblockPeer: (peerId: string) =>
    request<void>(`blocked/${peerId}`, { method: 'DELETE' }),

  // ── Safety ────────────────────────────────────────────────────────────────
  /**
   * Blocks the peer at once, then opens the phone's mail app with a report to
   * us filled in. There is no server to send a report to, so the email is the
   * report — and offline, the mail app sends it later. See ReportSheet.
   */
  reportPeer: (peerId: string, reason: ReportReason) =>
    post<ReportOutcome>('reports', { peerId, reason }),

  /** Opens one of our own pages in the phone's browser, never in this WebView. */
  openLink: (link: ExternalLink) =>
    post<{ opened: boolean; url: string }>('links/open', { link }),

  // ── Media ─────────────────────────────────────────────────────────────────

  /**
   * Sends an image. The image IS the request body — base64 inside JSON would
   * cost a third more memory and parse time for nothing on a loopback socket.
   *
   * No Content-Type is set deliberately: the core identifies the format by
   * inspecting the bytes and refuses anything that is not an image, so a value
   * sent from here would be ignored at best and misleading at worst.
   *
   * The caller must downscale first — the core rejects anything over its limit.
   */
  sendMedia: (chatId: string, image: Blob, caption: string, clientMessageId: string) => {
    const query = new URLSearchParams({ clientMessageId, caption });
    return request<Message>(`chats/${chatId}/media?${query.toString()}`, {
      method: 'POST',
      headers: {}, // not JSON; overrides the default in request()
      body: image,
    });
  },

  /** Same-origin URL for a stored image, usable directly as an `<img src>`. */
  mediaUrl: (mediaId: string) => apiUrl(`media/${mediaId}`),
};
